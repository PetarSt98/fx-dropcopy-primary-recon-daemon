#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/divergence.hpp"
#include "core/sequence_tracker.hpp"
#include "ingest/spsc_ring.hpp"
#include "persist/audit_log_format.hpp"
#include "persist/file_sink.hpp"

namespace persist {

struct AuditLogConfig {
    std::filesystem::path output_dir{"./audit_logs"};
    std::size_t rotate_max_bytes{128 * 1024 * 1024}; // 128MB
    std::chrono::seconds rotate_interval{std::chrono::hours(1)};
    std::size_t batch_max_records{64};
    std::size_t batch_max_bytes{1 * 1024 * 1024}; // 1MB
    std::chrono::milliseconds flush_idle_timeout{10};
    std::size_t staging_buffer_bytes{2 * 1024 * 1024};
};

using DivergenceAuditRing = ingest::SpscRing<core::Divergence, 4096>;
using GapAuditRing = ingest::SpscRing<core::SequenceGapEvent, 4096>;

class AuditLogWriter {
public:
    AuditLogWriter(DivergenceAuditRing& divergence_ring,
                   GapAuditRing& gap_ring,
                   AuditLogCounters& counters,
                   AuditLogConfig cfg,
                   std::unique_ptr<IFileSink> sink = nullptr) noexcept;
    ~AuditLogWriter();

    AuditLogWriter(const AuditLogWriter&) = delete;
    AuditLogWriter& operator=(const AuditLogWriter&) = delete;

    void start();
    void stop();
    bool is_running() const noexcept { return running_.load(std::memory_order_acquire); }

private:
    void run();
    void flush_batch();
    bool ensure_file_ready(std::size_t next_record_size);
    bool open_new_file(std::chrono::steady_clock::time_point now);
    bool writev_fully(const struct iovec* iov, int iovcnt);
    void enter_degraded(const char* reason);
    void maybe_recover(std::chrono::steady_clock::time_point now);
    void drain_and_drop();
    void append_divergence(const core::Divergence& div);
    void append_gap(const core::SequenceGapEvent& gap);

    DivergenceAuditRing& divergence_ring_;
    GapAuditRing& gap_ring_;
    AuditLogCounters& counters_;
    AuditLogConfig cfg_;
    std::unique_ptr<IFileSink> sink_;

    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    std::thread thread_;

    std::vector<std::byte> staging_;
    std::size_t staging_used_{0};
    std::size_t batch_records_{0};

    std::chrono::steady_clock::time_point last_flush_time_;
    std::chrono::steady_clock::time_point last_rotate_time_;
    std::chrono::steady_clock::time_point last_error_log_;
    std::uint64_t file_seq_{0};

    bool degraded_{false};
    std::chrono::steady_clock::time_point degraded_enter_;
    std::chrono::steady_clock::time_point next_recovery_attempt_;
    std::chrono::milliseconds recovery_backoff_{1000};

    std::filesystem::path current_path_;
};

} // namespace persist
