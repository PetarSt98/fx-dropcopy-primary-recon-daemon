#include "api/replay_engine.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

namespace replay_engine {
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
    return (evt.session_id % 2u == 0) ? core::Source::Primary : core::Source::DropCopy;
}

bool ensure_output_dir(const std::filesystem::path& p, std::string& err) {
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    if (ec) {
        std::ostringstream oss;
        oss << "failed to create output dir " << p << ": " << ec.message();
        err = oss.str();
        return false;
    }
    return true;
}

bool parse_speed(const std::string& speed, double& out_speed, bool& out_fast) {
    const auto lower = speed;
    if (lower == "fast" || lower == "max") {
        out_fast = true;
        out_speed = 1.0;
        return true;
    }
    if (lower == "realtime") {
        out_fast = false;
        out_speed = 1.0;
        return true;
    }
    char* end = nullptr;
    const double parsed = std::strtod(lower.c_str(), &end);
    if (!end || *end != '\0') {
        return false;
    }
    out_fast = false;
    out_speed = parsed;
    return true;
}

} // namespace

ReplayResult run_replay(const ReplayConfig& config, std::string& error_msg) {
    double speed = 1.0;
    bool fast = false;
    if (!parse_speed(config.speed.empty() ? "fast" : config.speed, speed, fast)) {
        error_msg = "invalid speed: " + config.speed;
        return ReplayResult::ConfigError;
    }
    if (!fast && speed <= 0.0) {
        error_msg = "speed must be > 0 unless fast/max is set";
        return ReplayResult::ConfigError;
    }

    if (config.wire_inputs.empty()) {
        error_msg = "no input provided";
        return ReplayResult::ConfigError;
    }

    persist::WireLogReaderOptions reader_opts;
    reader_opts.files = config.wire_inputs;
    if (config.from_ns && config.to_ns) {
        reader_opts.use_time_window = true;
        reader_opts.window_start_ns = *config.from_ns;
        reader_opts.window_end_ns = *config.to_ns;
    }

    persist::WireLogReader reader(std::move(reader_opts));
    if (!reader.open()) {
        error_msg = "failed to open wire input";
        return ReplayResult::WireReadError;
    }

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
    if (!config.output_dir.empty()) {
        audit_cfg.output_dir = config.output_dir;
    }
    if (!ensure_output_dir(audit_cfg.output_dir, error_msg)) {
        return ReplayResult::OutputError;
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
                error_msg = "wire log IO error";
                success = false;
                break;
            default:
                ++loop_stats.read_errors;
                error_msg = "unexpected wire log status " + std::to_string(static_cast<int>(res.status));
                success = false;
                break;
            }
            if (!success) {
                break;
            }
        }
        if (config.max_records > 0 && loop_stats.processed_ok >= config.max_records) {
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
            error_msg = "ring backpressure exceeded while pushing event";
            success = false;
            break;
        }

        if (!fast) {
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
                if (delta > 0 && speed > 0.0) {
                    const double scaled = delta / speed;
                    const auto sleep_ns = static_cast<std::uint64_t>(scaled);
                    if (sleep_ns > 0) {
                        std::this_thread::sleep_for(std::chrono::nanoseconds(sleep_ns));
                    }
                }
            }
        }

        last_ts = capture_ts;
        ++loop_stats.processed_ok;
        if (config.max_records > 0 && loop_stats.processed_ok >= config.max_records) {
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
        return ReplayResult::ReconError;
    }

    return ReplayResult::Success;
}

} // namespace replay_engine
