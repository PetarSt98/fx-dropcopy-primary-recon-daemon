#include "api/replay.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
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

struct ReplayLoopStats {
    std::size_t processed_ok{0};
    std::size_t read_errors{0};
    std::size_t corrupt_records{0};
    std::size_t push_failures{0};
    std::size_t skipped_due_to_limit{0};
    std::size_t backward_timestamps{0};
};

struct ReplayState {
    std::atomic<bool> stop_flag{false};
    std::atomic<bool> producer_done{false};
    std::unique_ptr<ExecRing> primary_ring;
    std::unique_ptr<ExecRing> dropcopy_ring;
    std::unique_ptr<core::DivergenceRing> divergence_ring;
    std::unique_ptr<core::SequenceGapRing> gap_ring;
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

struct CompareResult {
    bool match{false};
    std::string detail;
};

CompareResult compare_files_streaming(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::ifstream fa(a, std::ios::binary);
    std::ifstream fb(b, std::ios::binary);
    if (!fa.is_open() || !fb.is_open()) {
        return {false, "failed to open files"};
    }
    fa.seekg(0, std::ios::end);
    fb.seekg(0, std::ios::end);
    const auto size_a = fa.tellg();
    const auto size_b = fb.tellg();
    if (size_a != size_b) {
        std::ostringstream oss;
        oss << "size mismatch: " << a << " (" << size_a << ") vs " << b << " (" << size_b << ")";
        return {false, oss.str()};
    }
    fa.seekg(0, std::ios::beg);
    fb.seekg(0, std::ios::beg);

    std::array<char, 4096> buf_a{};
    std::array<char, 4096> buf_b{};
    std::size_t offset = 0;
    while (fa && fb) {
        fa.read(buf_a.data(), static_cast<std::streamsize>(buf_a.size()));
        fb.read(buf_b.data(), static_cast<std::streamsize>(buf_b.size()));
        const auto read_a = static_cast<std::size_t>(fa.gcount());
        const auto read_b = static_cast<std::size_t>(fb.gcount());
        const auto chunk = std::min(read_a, read_b);
        for (std::size_t i = 0; i < chunk; ++i) {
            if (buf_a[i] != buf_b[i]) {
                std::ostringstream oss;
                oss << "mismatch at offset " << (offset + i)
                    << " lhs=" << static_cast<int>(static_cast<unsigned char>(buf_a[i]))
                    << " rhs=" << static_cast<int>(static_cast<unsigned char>(buf_b[i]));
                return {false, oss.str()};
            }
        }
        offset += chunk;
    }
    return {true, ""};
}

CompareResult compare_directories(const std::filesystem::path& lhs, const std::filesystem::path& rhs) {
    if (lhs.empty() || rhs.empty()) {
        return {false, "empty directory path"};
    }
    std::error_code ec_l;
    std::error_code ec_r;
    if (!std::filesystem::exists(lhs, ec_l) || !std::filesystem::exists(rhs, ec_r)) {
        return {false, "one or both directories do not exist"};
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
        return {false, "failed to enumerate directories"};
    }
    auto normalize = [](const std::filesystem::path& p, const std::filesystem::path& root) {
        std::error_code ec;
        const auto rel = std::filesystem::relative(p, root, ec);
        return (ec ? p.filename() : rel).string();
    };
    std::sort(left_files.begin(), left_files.end(), [&](const auto& a, const auto& b) {
        return normalize(a, lhs) < normalize(b, lhs);
    });
    std::sort(right_files.begin(), right_files.end(), [&](const auto& a, const auto& b) {
        return normalize(a, rhs) < normalize(b, rhs);
    });
    if (left_files.size() != right_files.size()) {
        std::ostringstream oss;
        oss << "file count mismatch: " << left_files.size() << " vs " << right_files.size();
        return {false, oss.str()};
    }
    for (std::size_t i = 0; i < left_files.size(); ++i) {
        const auto left_rel = normalize(left_files[i], lhs);
        const auto right_rel = normalize(right_files[i], rhs);
        if (left_rel != right_rel) {
            std::ostringstream oss;
            oss << "file ordering mismatch: " << left_rel << " vs " << right_rel;
            return {false, oss.str()};
        }
        auto cmp = compare_files_streaming(left_files[i], right_files[i]);
        if (!cmp.match) {
            if (cmp.detail.empty()) {
                cmp.detail = "files differ";
            }
            return cmp;
        }
    }
    return {true, ""};
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
    state.primary_ring = std::make_unique<ExecRing>();
    state.dropcopy_ring = std::make_unique<ExecRing>();
    state.divergence_ring = std::make_unique<core::DivergenceRing>();
    state.gap_ring = std::make_unique<core::SequenceGapRing>();
    util::Arena arena(util::Arena::default_capacity_bytes);
    constexpr std::size_t order_capacity_hint = 1u << 16;
    core::OrderStateStore store(arena, order_capacity_hint);

    core::Reconciler recon(state.stop_flag,
                           *state.primary_ring,
                           *state.dropcopy_ring,
                           store,
                           state.recon_counters,
                           *state.divergence_ring,
                           *state.gap_ring,
                           &state.audit_counters,
                           &state.producer_done);

    persist::AuditLogConfig audit_cfg;
    if (!cfg.output_dir.empty()) {
        audit_cfg.output_dir = cfg.output_dir;
    }
    if (!ensure_output_dir(audit_cfg.output_dir)) {
        util::log(util::LogLevel::Error, "Failed to create output dir %s", audit_cfg.output_dir.string().c_str());
        return 1;
    }
    persist::AuditLogWriter audit_writer(*state.divergence_ring, *state.gap_ring, state.audit_counters, audit_cfg);

    audit_writer.start();
    std::thread recon_thread([&] { recon.run(); });

    std::uint64_t last_ts{0};
    ReplayLoopStats loop_stats{};
    bool first_record = true;

    core::WireExecEvent wire{};
    std::uint64_t capture_ts{0};
    bool success = true;

    constexpr int kMaxPushAttempts = 4096;

    while (true) {
        auto res = reader.next(wire, capture_ts);
        if (res.status == persist::WireLogReadStatus::EndOfStream) {
            break;
        }
        if (res.status != persist::WireLogReadStatus::Ok) {
            switch (res.status) {
            case persist::WireLogReadStatus::ChecksumMismatch:
            case persist::WireLogReadStatus::InvalidLength:
            case persist::WireLogReadStatus::Truncated:
                ++loop_stats.corrupt_records;
                util::log(util::LogLevel::Warn, "Skipping corrupt wire record status=%d",
                          static_cast<int>(res.status));
                continue;
            case persist::WireLogReadStatus::IoError:
                ++loop_stats.read_errors;
                util::log(util::LogLevel::Error, "Wire log IO error");
                success = false;
                break;
            default:
                ++loop_stats.read_errors;
                util::log(util::LogLevel::Error, "Unexpected wire log status=%d", static_cast<int>(res.status));
                success = false;
                break;
            }
            if (!success) {
                break;
            }
        }
        if (cfg.max_records > 0 && loop_stats.processed_ok >= cfg.max_records) {
            ++loop_stats.skipped_due_to_limit;
            break;
        }

        const core::Source src = source_from_wire(wire);
        const core::ExecEvent evt = core::from_wire(wire, src, capture_ts);
        ExecRing& ring = (src == core::Source::Primary) ? *state.primary_ring : *state.dropcopy_ring;

        bool pushed = false;
        for (int attempt = 0; attempt < kMaxPushAttempts; ++attempt) {
            if (ring.try_push(evt)) {
                pushed = true;
                break;
            }
            if (attempt < 32) {
                std::this_thread::yield();
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        }
        if (!pushed) {
            ++loop_stats.push_failures;
            util::log(util::LogLevel::Error, "Ring backpressure exceeded while pushing event");
            success = false;
            break;
        }

        if (!cfg.fast) {
            if (first_record) {
                first_record = false;
            } else {
                std::uint64_t delta = 0;
                if (capture_ts < last_ts) {
                    ++loop_stats.backward_timestamps;
                    util::log(util::LogLevel::Warn,
                              "Capture timestamp moved backwards prev=%llu curr=%llu",
                              static_cast<unsigned long long>(last_ts),
                              static_cast<unsigned long long>(capture_ts));
                } else {
                    delta = capture_ts - last_ts;
                }
                if (delta > 0 && cfg.speed > 0.0) {
                    const double scaled = delta / cfg.speed;
                    const auto sleep_ns = static_cast<std::uint64_t>(scaled);
                    if (sleep_ns > 0) {
                        std::this_thread::sleep_for(std::chrono::nanoseconds(sleep_ns));
                    }
                }
            }
        }

        last_ts = capture_ts;
        ++loop_stats.processed_ok;
        if (cfg.max_records > 0 && loop_stats.processed_ok >= cfg.max_records) {
            ++loop_stats.skipped_due_to_limit;
            break;
        }
    }

    state.producer_done.store(true, std::memory_order_release);
    state.stop_flag.store(true, std::memory_order_release);

    if (recon_thread.joinable()) {
        recon_thread.join();
    }
    audit_writer.stop();

    if (!success) {
        return 1;
    }

    if (!cfg.verify_against.empty()) {
        const auto cmp = compare_directories(audit_cfg.output_dir, cfg.verify_against);
        if (!cmp.match) {
            util::log(util::LogLevel::Error,
                      "Verification failed against %s: %s",
                      cfg.verify_against.string().c_str(),
                      cmp.detail.c_str());
            return 2;
        }
    }

    if (cfg.verbose && !cfg.quiet) {
        const auto& stats = reader.stats();
        util::log(util::LogLevel::Info,
                  "Replay completed: processed=%zu corrupt=%zu read_errors=%zu push_failures=%zu backward_ts=%zu limit_skips=%zu filtered=%llu files=%llu bytes=%llu",
                  loop_stats.processed_ok,
                  loop_stats.corrupt_records,
                  loop_stats.read_errors,
                  loop_stats.push_failures,
                  loop_stats.backward_timestamps,
                  loop_stats.skipped_due_to_limit,
                  static_cast<unsigned long long>(stats.filtered_out),
                  static_cast<unsigned long long>(stats.files_opened),
                  static_cast<unsigned long long>(stats.bytes_read));
    }

    return 0;
}

} // namespace api
