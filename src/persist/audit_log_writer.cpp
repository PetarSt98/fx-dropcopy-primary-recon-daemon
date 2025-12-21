#include "persist/audit_log_writer.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <utility>
#include <vector>

#include "util/log.hpp"

#ifndef EINTR
#define EINTR 4
#endif

namespace persist {

namespace {

constexpr std::chrono::milliseconds kDefaultSleep{1};
constexpr std::chrono::milliseconds kMaxBackoff{30000};
constexpr std::chrono::milliseconds kLogInterval{1000};

bool should_log(std::chrono::steady_clock::time_point now,
                std::chrono::steady_clock::time_point& last) noexcept {
    if (last.time_since_epoch().count() == 0 ||
        now - last >= kLogInterval) {
        last = now;
        return true;
    }
    return false;
}

std::string format_filename(std::chrono::system_clock::time_point tp, std::uint64_t seq) {
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream oss;
    oss << audit_filename_prefix();
    oss << std::put_time(&tm, "%Y%m%d_%H%M%S") << "_seq" << std::setw(3) << std::setfill('0') << seq << ".bin";
    return oss.str();
}

} // namespace

AuditLogWriter::AuditLogWriter(DivergenceAuditRing& divergence_ring,
                               GapAuditRing& gap_ring,
                               AuditLogCounters& counters,
                               AuditLogConfig cfg,
                               std::unique_ptr<IFileSink> sink) noexcept
    : divergence_ring_(divergence_ring),
      gap_ring_(gap_ring),
      counters_(counters),
      cfg_(std::move(cfg)),
      sink_(std::move(sink)),
      staging_(cfg_.staging_buffer_bytes, std::byte{0}) {
    if (!sink_) {
        sink_ = std::make_unique<PosixFileSink>();
    }
}

AuditLogWriter::~AuditLogWriter() { stop(); }

void AuditLogWriter::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }
    stop_.store(false, std::memory_order_release);
    last_flush_time_ = std::chrono::steady_clock::now();
    last_rotate_time_ = last_flush_time_;
    thread_ = std::thread([this] { run(); });
}

void AuditLogWriter::stop() {
    stop_.store(true, std::memory_order_release);
    if (thread_.joinable()) {
        thread_.join();
    }
    running_.store(false, std::memory_order_release);
}

void AuditLogWriter::run() {
    current_path_.clear();
    if (!degraded_ && !open_new_file(std::chrono::steady_clock::now())) {
        enter_degraded("initial open failed");
    }

    while (!(stop_.load(std::memory_order_acquire) && divergence_ring_.size_approx() == 0 &&
             gap_ring_.size_approx() == 0)) {
        bool consumed = false;
        core::Divergence div{};
        while (!degraded_ && divergence_ring_.try_pop(div)) {
            append_divergence(div);
            consumed = true;
            if (batch_records_ >= cfg_.batch_max_records || staging_used_ >= cfg_.batch_max_bytes) {
                flush_batch();
            }
        }
        core::SequenceGapEvent gap{};
        while (!degraded_ && gap_ring_.try_pop(gap)) {
            append_gap(gap);
            consumed = true;
            if (batch_records_ >= cfg_.batch_max_records || staging_used_ >= cfg_.batch_max_bytes) {
                flush_batch();
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (!degraded_) {
            if (staging_used_ > 0 && now - last_flush_time_ >= cfg_.flush_idle_timeout) {
                flush_batch();
            }
            if (now - last_rotate_time_ >= cfg_.rotate_interval && sink_ && sink_->is_open()) {
                flush_batch();
                sink_->close();
                if (!open_new_file(now)) {
                    enter_degraded("periodic rotate failed");
                }
            }
        } else {
            drain_and_drop();
            maybe_recover(now);
        }

        if (!consumed) {
            std::this_thread::sleep_for(kDefaultSleep);
        }
    }

    if (!degraded_ && staging_used_ > 0) {
        flush_batch();
    }
    if (sink_ && sink_->is_open()) {
        sink_->close();
    }
    if (degraded_) {
        const auto end = std::chrono::steady_clock::now();
        counters_.audit_degraded_mode_time_ns.fetch_add(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - degraded_enter_).count(),
            std::memory_order_relaxed);
    }
}

bool AuditLogWriter::open_new_file(std::chrono::steady_clock::time_point now) {
    const auto sys_now = std::chrono::system_clock::now();
    std::error_code ec;
    std::filesystem::create_directories(cfg_.output_dir, ec);
    if (ec) {
        const auto now = std::chrono::steady_clock::now();
        if (should_log(now, last_error_log_)) {
            util::log(util::LogLevel::Error, "AuditLogWriter: failed to create dir %s: %s",
                      cfg_.output_dir.string().c_str(), ec.message().c_str());
        }
        return false;
    }
    current_path_ = cfg_.output_dir / format_filename(sys_now, file_seq_++);
    auto res = sink_->open(current_path_.string());
    if (!res.ok) {
        const auto now = std::chrono::steady_clock::now();
        if (should_log(now, last_error_log_)) {
            util::log(util::LogLevel::Error, "AuditLogWriter: open(%s) failed: %d",
                      current_path_.string().c_str(), res.error_code);
        }
        return false;
    }
    last_rotate_time_ = now;
    return true;
}

void AuditLogWriter::flush_batch() {
    if (!sink_ || !sink_->is_open() || staging_used_ == 0) {
        staging_used_ = 0;
        batch_records_ = 0;
        return;
    }

    struct iovec iov{staging_.data(), staging_used_};
    if (!writev_fully(&iov, 1)) {
        enter_degraded("write failure");
        staging_used_ = 0;
        batch_records_ = 0;
        return;
    }
    staging_used_ = 0;
    batch_records_ = 0;
    last_flush_time_ = std::chrono::steady_clock::now();
}

bool AuditLogWriter::writev_fully(const struct iovec* iov, int iovcnt) {
    std::array<struct iovec, 4> tmp{};
    std::vector<struct iovec> dynamic;
    struct iovec* cur = nullptr;
    int cur_cnt = iovcnt;

    if (iovcnt <= static_cast<int>(tmp.size())) {
        for (int i = 0; i < iovcnt; ++i) {
            tmp[i] = iov[i];
        }
        cur = tmp.data();
    } else {
        dynamic.assign(iov, iov + iovcnt);
        cur = dynamic.data();
    }

    while (cur_cnt > 0) {
        std::size_t bytes_written = 0;
        auto res = sink_->writev(cur, cur_cnt, bytes_written);
        if (!res.ok) {
            if (res.error_code == EINTR) {
                continue;
            }
            const auto now = std::chrono::steady_clock::now();
            if (should_log(now, last_error_log_)) {
                util::log(util::LogLevel::Error, "AuditLogWriter: writev failed err=%d", res.error_code);
            }
            counters_.audit_io_errors.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (bytes_written == 0) {
            const auto now = std::chrono::steady_clock::now();
            if (should_log(now, last_error_log_)) {
                util::log(util::LogLevel::Error, "AuditLogWriter: writev wrote 0 bytes");
            }
            counters_.audit_io_errors.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        std::size_t remaining = bytes_written;
        while (remaining > 0 && cur_cnt > 0) {
            if (remaining < cur[0].iov_len) {
                cur[0].iov_base = static_cast<std::byte*>(cur[0].iov_base) + remaining;
                cur[0].iov_len -= remaining;
                remaining = 0;
            } else {
                remaining -= cur[0].iov_len;
                ++cur;
                --cur_cnt;
            }
        }
    }
    return true;
}

void AuditLogWriter::enter_degraded(const char* reason) {
    if (!degraded_) {
        degraded_ = true;
        degraded_enter_ = std::chrono::steady_clock::now();
        recovery_backoff_ = std::chrono::milliseconds{1000};
        next_recovery_attempt_ = degraded_enter_ + recovery_backoff_;
        counters_.audit_io_errors.fetch_add(1, std::memory_order_relaxed);
        util::log(util::LogLevel::Error, "AuditLogWriter entering degraded mode: %s", reason);
        if (sink_ && sink_->is_open()) {
            sink_->close();
        }
    }
}

void AuditLogWriter::maybe_recover(std::chrono::steady_clock::time_point now) {
    if (!degraded_) {
        return;
    }
    if (now < next_recovery_attempt_) {
        return;
    }
    counters_.audit_recovery_attempts.fetch_add(1, std::memory_order_relaxed);
    if (open_new_file(now)) {
        const auto end = now;
        counters_.audit_degraded_mode_time_ns.fetch_add(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - degraded_enter_).count(),
            std::memory_order_relaxed);
        degraded_ = false;
        staging_used_ = 0;
        batch_records_ = 0;
        util::log(util::LogLevel::Info, "AuditLogWriter recovered from degraded mode");
        return;
    }
    recovery_backoff_ = std::min<std::chrono::milliseconds>(recovery_backoff_ * 2, kMaxBackoff);
    next_recovery_attempt_ = now + recovery_backoff_;
}

void AuditLogWriter::drain_and_drop() {
    core::Divergence div{};
    while (divergence_ring_.try_pop(div)) {
        counters_.writer_drop_divergence.fetch_add(1, std::memory_order_relaxed);
    }
    core::SequenceGapEvent gap{};
    while (gap_ring_.try_pop(gap)) {
        counters_.writer_drop_gaps.fetch_add(1, std::memory_order_relaxed);
    }
}

bool AuditLogWriter::ensure_file_ready(std::size_t next_record_size) {
    const auto now = std::chrono::steady_clock::now();
    if (!sink_ || !sink_->is_open()) {
        if (!open_new_file(now)) {
            enter_degraded("open failed");
            return false;
        }
    }
    const auto elapsed = now - last_rotate_time_;
    bool need_rotate = elapsed >= cfg_.rotate_interval;
    if (!need_rotate) {
        const std::uint64_t projected = sink_->current_size() + staging_used_ + next_record_size;
        need_rotate = cfg_.rotate_max_bytes > 0 && projected > cfg_.rotate_max_bytes;
    }
    if (need_rotate) {
        if (staging_used_ > 0) {
            flush_batch();
        }
        if (sink_->is_open()) {
            sink_->close();
        }
        if (!open_new_file(now)) {
            enter_degraded("rotate/open failed");
            return false;
        }
    }
    return true;
}

void AuditLogWriter::append_divergence(const core::Divergence& div) {
    const std::size_t record_size = record_size_from_payload(divergence_payload_v1_size);
    if (!ensure_file_ready(record_size)) {
        return;
    }
    if (staging_used_ + record_size > staging_.size()) {
        flush_batch();
        if (staging_used_ + record_size > staging_.size()) {
            const auto now = std::chrono::steady_clock::now();
            if (should_log(now, last_error_log_)) {
                util::log(util::LogLevel::Error, "AuditLogWriter: staging buffer too small for divergence record");
            }
            counters_.audit_io_errors.fetch_add(1, std::memory_order_relaxed);
            enter_degraded("staging too small");
            return;
        }
    }
    std::size_t written = 0;
    if (!encode_divergence_record_v1(div, std::span<std::byte>(staging_.data() + staging_used_, staging_.size() - staging_used_), written)) {
        counters_.audit_io_errors.fetch_add(1, std::memory_order_relaxed);
        enter_degraded("encode divergence failed");
        return;
    }
    staging_used_ += written;
    ++batch_records_;
}

void AuditLogWriter::append_gap(const core::SequenceGapEvent& gap) {
    const std::size_t record_size = record_size_from_payload(gap_payload_v1_size);
    if (!ensure_file_ready(record_size)) {
        return;
    }
    if (staging_used_ + record_size > staging_.size()) {
        flush_batch();
        if (staging_used_ + record_size > staging_.size()) {
            const auto now = std::chrono::steady_clock::now();
            if (should_log(now, last_error_log_)) {
                util::log(util::LogLevel::Error, "AuditLogWriter: staging buffer too small for gap record");
            }
            counters_.audit_io_errors.fetch_add(1, std::memory_order_relaxed);
            enter_degraded("staging too small");
            return;
        }
    }
    std::size_t written = 0;
    if (!encode_gap_record_v1(gap, std::span<std::byte>(staging_.data() + staging_used_, staging_.size() - staging_used_), written)) {
        counters_.audit_io_errors.fetch_add(1, std::memory_order_relaxed);
        enter_degraded("encode gap failed");
        return;
    }
    staging_used_ += written;
    ++batch_records_;
}

} // namespace persist
