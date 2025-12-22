#include "api/replay.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "core/exec_event.hpp"
#include "core/order_state_store.hpp"
#include "core/reconciler.hpp"
#include "ingest/spsc_ring.hpp"
#include "persist/audit_log_writer.hpp"
#include "persist/wire_log_reader.hpp"
#include "util/arena.hpp"
#include "util/log.hpp"

namespace api {

namespace {

using ExecRing = ingest::SpscRing<core::ExecEvent, 1u << 16>;

struct ReplayState {
    std::atomic<bool> stop_flag{false};
    ExecRing primary_ring;
    ExecRing dropcopy_ring;
    core::DivergenceRing divergence_ring;
    core::SequenceGapRing gap_ring;
    persist::AuditLogCounters audit_counters;
    core::ReconCounters recon_counters;
};

core::Source source_from_wire(const core::WireExecEvent& evt) noexcept {
    // Derive source deterministically from session_id parity to avoid
    // additional metadata in the wire log. Session ids used during capture
    // should therefore be stable to preserve determinism.
    return (evt.session_id % 2u == 0) ? core::Source::Primary : core::Source::DropCopy;
}

bool ensure_output_dir(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    return !ec;
}

bool rings_empty(const ReplayState& state) noexcept {
    return state.primary_ring.size_approx() == 0 && state.dropcopy_ring.size_approx() == 0;
}

void log_if(bool enabled, util::LogLevel lvl, const char* fmt, const char* arg0 = nullptr) {
    if (!enabled) {
        return;
    }
    if (arg0) {
        util::log(lvl, fmt, arg0);
    } else {
        util::log(lvl, "%s", fmt);
    }
}

bool compare_files(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::ifstream fa(a, std::ios::binary);
    std::ifstream fb(b, std::ios::binary);
    if (!fa.is_open() || !fb.is_open()) {
        return false;
    }
    std::vector<char> ba((std::istreambuf_iterator<char>(fa)), std::istreambuf_iterator<char>());
    std::vector<char> bb((std::istreambuf_iterator<char>(fb)), std::istreambuf_iterator<char>());
    return ba == bb;
}

bool compare_directories(const std::filesystem::path& lhs, const std::filesystem::path& rhs) {
    if (lhs.empty() || rhs.empty()) {
        return false;
    }
    std::error_code ec_l;
    std::error_code ec_r;
    if (!std::filesystem::exists(lhs, ec_l) || !std::filesystem::exists(rhs, ec_r)) {
        return false;
    }
    std::vector<std::filesystem::path> left_files;
    std::vector<std::filesystem::path> right_files;
    auto collect = [](const std::filesystem::path& dir, std::vector<std::filesystem::path>& out) {
        std::error_code ec;
        for (std::filesystem::directory_iterator it(dir, ec); !ec && it != std::filesystem::directory_iterator(); ++it) {
            if (it->is_regular_file(ec) && !ec) {
                out.push_back(it->path());
            }
        }
        return ec.value() == 0;
    };
    if (!collect(lhs, left_files) || !collect(rhs, right_files)) {
        return false;
    }
    std::sort(left_files.begin(), left_files.end());
    std::sort(right_files.begin(), right_files.end());
    if (left_files.size() != right_files.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left_files.size(); ++i) {
        if (!compare_files(left_files[i], right_files[i])) {
            return false;
        }
    }
    return true;
}

} // namespace

int run_replay(const ReplayConfig& cfg) {
    if (cfg.fast && cfg.speed <= 0.0) {
        // fast mode ignores speed, allow zero/negative here.
    } else if (cfg.speed <= 0.0) {
        util::log(util::LogLevel::Error, "Invalid --speed %.3f; must be > 0 unless --fast is set", cfg.speed);
        return 1;
    }

    if (cfg.input_files.empty() && cfg.input_directory.empty()) {
        util::log(util::LogLevel::Error, "No input provided; use --input <dir|file1,file2>");
        return 1;
    }

    const bool info_logs = !cfg.quiet;

    persist::WireLogReaderOptions reader_opts;
    reader_opts.files = cfg.input_files;
    reader_opts.directory = cfg.input_directory;
    reader_opts.use_time_window = cfg.use_time_window;
    reader_opts.window_start_ns = cfg.window_start_ns;
    reader_opts.window_end_ns = cfg.window_end_ns;

    persist::WireLogReader reader(std::move(reader_opts));
    if (!reader.open()) {
        util::log(util::LogLevel::Error, "Failed to open wire log input");
        return 1;
    }

    log_if(info_logs, util::LogLevel::Info, "Replay input prepared", nullptr);

    ReplayState state;
    util::Arena arena(util::Arena::default_capacity_bytes);
    constexpr std::size_t order_capacity_hint = 1u << 16;
    core::OrderStateStore store(arena, order_capacity_hint);

    core::Reconciler recon(state.stop_flag,
                           state.primary_ring,
                           state.dropcopy_ring,
                           store,
                           state.recon_counters,
                           state.divergence_ring,
                           state.gap_ring,
                           &state.audit_counters);

    persist::AuditLogConfig audit_cfg;
    if (!cfg.output_dir.empty()) {
        audit_cfg.output_dir = cfg.output_dir;
    }
    if (!ensure_output_dir(audit_cfg.output_dir)) {
        util::log(util::LogLevel::Error, "Failed to create output dir %s", audit_cfg.output_dir.string().c_str());
        return 1;
    }
    persist::AuditLogWriter audit_writer(state.divergence_ring, state.gap_ring, state.audit_counters, audit_cfg);

    audit_writer.start();
    std::thread recon_thread([&] { recon.run(); });

    std::uint64_t last_ts{0};
    std::size_t processed{0};

    core::WireExecEvent wire{};
    std::uint64_t capture_ts{0};
    bool success = true;

    while (true) {
        auto res = reader.next(wire, capture_ts);
        if (res.status == persist::WireLogReadStatus::EndOfStream) {
            break;
        }
        if (res.status != persist::WireLogReadStatus::Ok) {
            util::log(util::LogLevel::Error, "Wire log read error status=%d", static_cast<int>(res.status));
            success = false;
            break;
        }
        if (cfg.max_records > 0 && processed >= cfg.max_records) {
            break;
        }

        const core::Source src = source_from_wire(wire);
        const core::ExecEvent evt = core::from_wire(wire, src, capture_ts);
        ExecRing& ring = (src == core::Source::Primary) ? state.primary_ring : state.dropcopy_ring;

        std::uint32_t backoff = 0;
        while (!ring.try_push(evt)) {
            if (backoff < 16) {
                ++backoff;
                std::this_thread::yield();
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        }

        if (!cfg.fast && processed > 0) {
            if (capture_ts >= last_ts) {
                const std::uint64_t delta = capture_ts - last_ts;
                if (delta > 0) {
                    const double scaled = delta / cfg.speed;
                    const auto sleep_ns = static_cast<std::uint64_t>(scaled);
                    if (sleep_ns > 0) {
                        std::this_thread::sleep_for(std::chrono::nanoseconds(sleep_ns));
                    }
                }
            }
        }

        last_ts = capture_ts;
        ++processed;
    }

    // Allow the reconciler to drain remaining events before stopping.
    while (!rings_empty(state)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    state.stop_flag.store(true, std::memory_order_release);

    if (recon_thread.joinable()) {
        recon_thread.join();
    }
    audit_writer.stop();

    if (!success) {
        return 1;
    }

    if (!cfg.verify_against.empty()) {
        if (!compare_directories(audit_cfg.output_dir, cfg.verify_against)) {
            util::log(util::LogLevel::Error, "Verification failed: outputs differ from %s",
                      cfg.verify_against.string().c_str());
            return 2;
        }
    }

    if (cfg.verbose && !cfg.quiet) {
        const auto& stats = reader.stats();
        util::log(util::LogLevel::Info,
                  "Replay completed: processed=%zu ok_records=%llu filtered=%llu files=%llu bytes=%llu",
                  processed,
                  static_cast<unsigned long long>(stats.records_ok),
                  static_cast<unsigned long long>(stats.filtered_out),
                  static_cast<unsigned long long>(stats.files_opened),
                  static_cast<unsigned long long>(stats.bytes_read));
    }

    return 0;
}

} // namespace api
