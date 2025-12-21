#include <gtest/gtest.h>

#include <chrono>
#include <cerrno>
#include <filesystem>
#include <thread>
#include <vector>

#include "persist/audit_log_writer.hpp"

namespace {

class ScriptedSink : public persist::IFileSink {
public:
    struct WriteScript {
        bool ok{true};
        int err{0};
        std::size_t bytes{0};
    };

    explicit ScriptedSink(std::vector<WriteScript> scripts = {}) : scripts_(std::move(scripts)) {}

    persist::IoResult open(const std::string&) noexcept override {
        ++open_calls_;
        opened_ = open_should_succeed_;
        if (!opened_) {
            return {false, open_error_};
        }
        size_bytes_ = 0;
        return {true, 0};
    }

    void close() noexcept override { opened_ = false; }

    persist::IoResult writev(const struct iovec* iov, int iovcnt, std::size_t& bytes_written) noexcept override {
        bytes_written = 0;
        if (!opened_) {
            return {false, EBADF};
        }
        std::size_t total = 0;
        for (int i = 0; i < iovcnt; ++i) {
            total += iov[i].iov_len;
        }
        if (script_idx_ < scripts_.size()) {
            const auto s = scripts_[script_idx_++];
            bytes_written = s.bytes > total ? total : s.bytes;
            size_bytes_ += bytes_written;
            return {s.ok, s.err};
        }
        bytes_written = total;
        size_bytes_ += total;
        return {true, 0};
    }

    std::uint64_t current_size() const noexcept override { return size_bytes_; }
    bool is_open() const noexcept override { return opened_; }

    bool open_should_succeed_{true};
    int open_error_{ENOSPC};
    int open_calls_{0};

private:
    bool opened_{false};
    std::size_t size_bytes_{0};
    std::vector<WriteScript> scripts_;
    std::size_t script_idx_{0};
};

persist::AuditLogConfig fast_cfg() {
    persist::AuditLogConfig cfg;
    cfg.flush_idle_timeout = std::chrono::milliseconds(1);
    cfg.batch_max_records = 2;
    cfg.batch_max_bytes = 1024;
    cfg.rotate_interval = std::chrono::seconds(60);
    cfg.rotate_max_bytes = 1 * 1024 * 1024;
    cfg.output_dir = std::filesystem::temp_directory_path() / "audit_writer_test";
    cfg.staging_buffer_bytes = 4096;
    return cfg;
}

core::Divergence make_div(std::uint64_t key) {
    core::Divergence d{};
    d.key = key;
    d.type = core::DivergenceType::StateMismatch;
    d.internal_status = core::OrdStatus::Working;
    d.dropcopy_status = core::OrdStatus::Filled;
    d.internal_cum_qty = 1;
    d.dropcopy_cum_qty = 2;
    d.internal_avg_px = 3;
    d.dropcopy_avg_px = 4;
    d.internal_ts = 5;
    d.dropcopy_ts = 6;
    return d;
}

TEST(AuditLogWriter, RetriesPartialAndEintr) {
    // First write EINTR, then partial, then full.
    std::vector<ScriptedSink::WriteScript> scripts = {
        {false, EINTR, 0},
        {true, 0, 8}, // partial
    };
    auto sink = std::make_unique<ScriptedSink>(scripts);
    auto* sink_raw = sink.get();
    persist::DivergenceAuditRing div_ring;
    persist::GapAuditRing gap_ring;
    persist::AuditLogCounters ctrs;
    persist::AuditLogWriter writer(div_ring, gap_ring, ctrs, fast_cfg(), std::move(sink));

    writer.start();
    ASSERT_TRUE(div_ring.try_push(make_div(1)));

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    writer.stop();

    const std::uint64_t min_expected = persist::record_size_from_payload(persist::divergence_payload_v1_size);
    EXPECT_GE(sink_raw->current_size(), min_expected);
    EXPECT_EQ(ctrs.writer_drop_divergence.load(), 0);
    EXPECT_EQ(ctrs.writer_drop_gaps.load(), 0);
}

TEST(AuditLogWriter, DegradedModeRecovers) {
    auto sink = std::make_unique<ScriptedSink>();
    auto* sink_raw = sink.get();
    sink_raw->open_should_succeed_ = false; // first open fails

    persist::DivergenceAuditRing div_ring;
    persist::GapAuditRing gap_ring;
    persist::AuditLogCounters ctrs;
    persist::AuditLogWriter writer(div_ring, gap_ring, ctrs, fast_cfg(), std::move(sink));

    writer.start();
    // First event will be dropped while degraded.
    ASSERT_TRUE(div_ring.try_push(make_div(10)));

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    sink_raw->open_should_succeed_ = true; // recovery attempt at ~1s will succeed

    std::this_thread::sleep_for(std::chrono::milliseconds(1100)); // allow recovery attempt
    // Push another event after recovery window.
    ASSERT_TRUE(div_ring.try_push(make_div(11)));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    writer.stop();

    EXPECT_GT(ctrs.writer_drop_divergence.load(), 0);
    EXPECT_GT(ctrs.audit_recovery_attempts.load(), 0);
    EXPECT_GE(sink_raw->current_size(), persist::record_size_from_payload(persist::divergence_payload_v1_size));
}

} // namespace
