#include "persist/wire_capture_writer.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <ctime>
#include <cstring>
#include <cerrno>
#include <thread>
#include <mutex>
#include <array>

#ifndef _WIN32
#include <sys/statvfs.h>
#else
#include <windows.h>
#endif

#include "util/log.hpp"

namespace persist {

namespace {

constexpr std::size_t kRingCapacity = 1024;
constexpr std::size_t kSlotSize = 16 * 1024;

struct RecordBuffers {
    RecordFields fields{};
};

static bool writev_fully(IFileSink& sink,
                         struct iovec* iov,
                         int iovcnt,
                         std::size_t& total_written,
                         std::atomic<std::uint64_t>& partial_counter) {
    total_written = 0;
    int idx = 0;
    while (idx < iovcnt) {
        std::size_t bytes_written = 0;
        IoResult r = sink.writev(&iov[idx], iovcnt - idx, bytes_written);
        if (!r.ok) {
            if (r.error_code == EINTR) {
                continue;
            }
            return false;
        }
        total_written += bytes_written;
        std::size_t advance = bytes_written;
        while (advance > 0 && idx < iovcnt) {
            if (advance < iov[idx].iov_len) {
                iov[idx].iov_base = static_cast<char*>(iov[idx].iov_base) + advance;
                iov[idx].iov_len -= advance;
                advance = 0;
            } else {
                advance -= iov[idx].iov_len;
                ++idx;
            }
        }
        if (idx < iovcnt && bytes_written > 0) {
            partial_counter.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return true;
}

} // namespace

WireCaptureWriter::WireCaptureWriter(WireCaptureConfig cfg)
    : WireCaptureWriter(std::move(cfg),
                        cfg.sink_factory ? cfg.sink_factory() : std::make_unique<PosixFileSink>(),
                        cfg.steady_clock ? std::move(cfg.steady_clock) : std::make_unique<util::SteadyClock>(),
                        cfg.system_clock ? std::move(cfg.system_clock) : std::make_unique<util::SystemClock>()) {}

WireCaptureWriter::WireCaptureWriter(WireCaptureConfig cfg,
                                     std::unique_ptr<IFileSink> sink,
                                     std::unique_ptr<util::SteadyClock> steady_clock,
                                     std::unique_ptr<util::SystemClock> system_clock)
    : cfg_(std::move(cfg))
    , ring_(std::make_unique<Ring>())
    , sink_(std::move(sink))
    , steady_clock_(std::move(steady_clock))
    , system_clock_(std::move(system_clock)) {
    static_assert(Ring::capacity_static() == kRingCapacity, "Ring capacity mismatch");
    if (cfg_.ring_capacity != kRingCapacity) {
        util::log(util::LogLevel::Warn,
                  "WireCaptureWriter ring_capacity=%zu ignored; compiled capacity=%zu",
                  cfg_.ring_capacity,
                  kRingCapacity);
    }
    if (cfg_.max_payload > kSlotSize) {
        cfg_.max_payload = kSlotSize;
    }
}

WireCaptureWriter::~WireCaptureWriter() { stop(); }

bool WireCaptureWriter::try_submit(std::span<const std::byte> payload) noexcept {
    const std::size_t len = payload.size();
    if (ring_->producer_stopped()) {
        metrics_.drops_queue_full.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (len > cfg_.max_payload || len > kSlotSize) {
        metrics_.drops_payload_too_large.fetch_add(1, std::memory_order_relaxed);
        if (len > 4096) {
            metrics_.large_payload_count.fetch_add(1, std::memory_order_relaxed);
        }
    if (rate_limited_log(last_log_large_)) {
        util::log(util::LogLevel::Warn, "WireCaptureWriter dropping oversize payload len=%zu", len);
    }
        return false;
    }
    if (metrics_.degraded_mode.load(std::memory_order_acquire)) {
        metrics_.drops_degraded_mode.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const auto capture_ts = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(system_clock_->now().time_since_epoch()).count());
    if (!ring_->try_push(payload.data(), len, capture_ts)) {
        metrics_.drops_queue_full.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    metrics_.messages_submitted.fetch_add(1, std::memory_order_relaxed);
    if (len > 4096) {
        metrics_.large_payload_count.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

void WireCaptureWriter::start() {
    if (running_.exchange(true)) {
        return;
    }
    stop_writer_.store(false, std::memory_order_relaxed);
    writer_thread_ = std::thread(&WireCaptureWriter::run, this);
}

void WireCaptureWriter::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    ring_->stop_producer();
    shutdown_deadline_ = now() + cfg_.shutdown_grace;
    while (ring_->size_approx() > 0 && now() < shutdown_deadline_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    stop_writer_.store(true, std::memory_order_release);
    if (writer_thread_.joinable()) {
        writer_thread_.join();
    }
    if (sink_) {
        sink_->close();
    }
}

WireCaptureMetricsSnapshot WireCaptureWriter::snapshot_metrics() const {
    WireCaptureMetricsSnapshot snap;
    snap.messages_submitted = metrics_.messages_submitted.load(std::memory_order_relaxed);
    snap.messages_written = metrics_.messages_written.load(std::memory_order_relaxed);
    snap.bytes_written = metrics_.bytes_written.load(std::memory_order_relaxed);
    snap.drops_queue_full = metrics_.drops_queue_full.load(std::memory_order_relaxed);
    snap.drops_payload_too_large = metrics_.drops_payload_too_large.load(std::memory_order_relaxed);
    snap.drops_degraded_mode = metrics_.drops_degraded_mode.load(std::memory_order_relaxed);
    snap.io_errors_write = metrics_.io_errors_write.load(std::memory_order_relaxed);
    snap.io_errors_open = metrics_.io_errors_open.load(std::memory_order_relaxed);
    snap.io_errors_disk_full = metrics_.io_errors_disk_full.load(std::memory_order_relaxed);
    snap.partial_writes = metrics_.partial_writes.load(std::memory_order_relaxed);
    snap.files_rotated = metrics_.files_rotated.load(std::memory_order_relaxed);
    snap.large_payload_count = metrics_.large_payload_count.load(std::memory_order_relaxed);
    snap.disk_space_bytes_free = metrics_.disk_space_bytes_free.load(std::memory_order_relaxed);
    snap.degraded_mode = metrics_.degraded_mode.load(std::memory_order_relaxed);
    return snap;
}

void WireCaptureWriter::run() {
    file_open_time_ = now();
    writer_loop();
}

bool WireCaptureWriter::rate_limited_log(std::chrono::steady_clock::time_point& last) {
    const auto now_ts = now();
    if (now_ts - last >= cfg_.log_rate_limit) {
        last = now_ts;
        return true;
    }
    return false;
}

void WireCaptureWriter::writer_loop() {
    while (true) {
        if (metrics_.degraded_mode.load(std::memory_order_acquire)) {
            recover_if_due();
            CaptureDescriptor desc{};
            while (ring_->try_pop(desc)) {
                metrics_.drops_degraded_mode.fetch_add(1, std::memory_order_relaxed);
            }
            if (stop_writer_.load(std::memory_order_acquire) && ring_->size_approx() == 0) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        if (!ensure_open_file()) {
            enter_degraded_mode();
            continue;
        }

        if (!check_disk_space()) {
            enter_degraded_mode();
            continue;
        }

        if (!perform_write_batch()) {
            continue;
        }

        if (stop_writer_.load(std::memory_order_acquire) && ring_->size_approx() == 0) {
            break;
        }
    }
    if (sink_) {
        sink_->close();
    }
}

bool WireCaptureWriter::ensure_open_file() {
    if (sink_ && sink_->is_open()) {
        return true;
    }
    return rotate_file();
}

bool WireCaptureWriter::rotate_if_needed(std::size_t next_record_bytes) {
    const auto now_ts = now();
    const bool time_rotate = (now_ts - file_open_time_) >= cfg_.rotate_interval;
    const bool size_rotate = (current_file_size_ + next_record_bytes) > cfg_.rotate_max_bytes;
    if (!time_rotate && !size_rotate) {
        return true;
    }
    return rotate_file();
}

bool WireCaptureWriter::rotate_file() {
    const std::string path = make_filename();
    auto new_sink = cfg_.sink_factory ? cfg_.sink_factory() : std::make_unique<PosixFileSink>();
    auto res = new_sink->open(path);
    if (!res.ok) {
        metrics_.io_errors_open.fetch_add(1, std::memory_order_relaxed);
        if (rate_limited_log(last_log_error_)) {
            util::log(util::LogLevel::Error, "WireCaptureWriter failed to open %s: %s", path.c_str(), std::strerror(res.error_code));
        }
        return false;
    }
    WireLogHeaderFields header{};
    std::array<std::byte, wire_log_header_size> header_bytes{};
    encode_header(header, header_bytes);
    struct iovec header_iov { header_bytes.data(), header_bytes.size() };
    std::size_t header_written = 0;
    if (!writev_fully(*new_sink, &header_iov, 1, header_written, metrics_.partial_writes) ||
        header_written != header_bytes.size()) {
        metrics_.io_errors_write.fetch_add(1, std::memory_order_relaxed);
        if (rate_limited_log(last_log_error_)) {
            util::log(util::LogLevel::Error, "WireCaptureWriter failed to write header to %s", path.c_str());
        }
        new_sink->close();
        return false;
    }
    const std::uint64_t new_size = header_written;
    auto old_sink = std::move(sink_);
    sink_ = std::move(new_sink);
    current_file_size_ = new_size;
    metrics_.bytes_written.fetch_add(new_size, std::memory_order_relaxed);
    file_open_time_ = now();
    ++file_sequence_;
    metrics_.files_rotated.fetch_add(1, std::memory_order_relaxed);
    if (old_sink) {
        old_sink->close();
    }
    return true;
}

bool WireCaptureWriter::perform_write_batch() {
    struct iovec iovecs[4 * 64];
    RecordBuffers buf[64];
    CaptureDescriptor desc{};
    int iovcnt = 0;
    std::size_t batch_bytes = 0;
    int records = 0;

    std::uint64_t dropped_on_failure = 0;
    while (records < static_cast<int>(cfg_.batch_records) &&
           batch_bytes < cfg_.batch_bytes &&
           ring_->try_pop(desc)) {
        const std::size_t payload_len = desc.length;
        if (payload_len == 0 || payload_len > wire_log_max_payload_size || payload_len != wire_exec_event_wire_size) {
            metrics_.drops_payload_too_large.fetch_add(1, std::memory_order_relaxed);
            ++dropped_on_failure;
            continue;
        }
        const std::size_t record_size = framed_size(payload_len);
        if (!rotate_if_needed(record_size)) {
            enter_degraded_mode();
            ++dropped_on_failure;
            continue;
        }
        std::span<const std::byte> payload(ring_->slot_ptr(desc.slot_index), payload_len);
        encode_record(payload, desc.capture_ts_ns, buf[records].fields);

        iovecs[iovcnt++] = {buf[records].fields.length_le.data(), buf[records].fields.length_le.size()};
        iovecs[iovcnt++] = {buf[records].fields.capture_ts_le.data(), buf[records].fields.capture_ts_le.size()};
        iovecs[iovcnt++] = {const_cast<std::byte*>(payload.data()), payload.size()};
        iovecs[iovcnt++] = {buf[records].fields.checksum_le.data(), buf[records].fields.checksum_le.size()};
        batch_bytes += record_size;
        ++records;
    }

    if (records == 0) {
        if (dropped_on_failure > 0) {
            metrics_.drops_degraded_mode.fetch_add(dropped_on_failure, std::memory_order_relaxed);
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        return true;
    }

    std::size_t written = 0;
    if (!writev_fully(*sink_, iovecs, iovcnt, written, metrics_.partial_writes)) {
        metrics_.io_errors_write.fetch_add(1, std::memory_order_relaxed);
        if (rate_limited_log(last_log_error_)) {
            util::log(util::LogLevel::Error, "WireCaptureWriter write failure");
        }
        metrics_.drops_degraded_mode.fetch_add(records, std::memory_order_relaxed);
        enter_degraded_mode();
        return false;
    }
    current_file_size_ += written;
    metrics_.messages_written.fetch_add(records, std::memory_order_relaxed);
    metrics_.bytes_written.fetch_add(written, std::memory_order_relaxed);
    if (dropped_on_failure > 0) {
        metrics_.drops_degraded_mode.fetch_add(dropped_on_failure, std::memory_order_relaxed);
    }
    return true;
}

void WireCaptureWriter::enter_degraded_mode() {
    metrics_.degraded_mode.store(true, std::memory_order_release);
    next_recovery_time_ = now() + cfg_.recovery_initial;
}

static bool writev_fully(IFileSink& sink,
                         struct iovec* iov,
                         int iovcnt,
                         std::size_t& total_written,
                         std::atomic<std::uint64_t>& partial_counter) {
    total_written = 0;
    int idx = 0;
    while (idx < iovcnt) {
        std::size_t bytes_written = 0;
        IoResult r = sink.writev(&iov[idx], iovcnt - idx, bytes_written);
        if (!r.ok) {
            if (r.error_code == EINTR) {
                continue;
            }
            return false;
        }
        total_written += bytes_written;
        std::size_t advance = bytes_written;
        while (advance > 0 && idx < iovcnt) {
            if (advance < iov[idx].iov_len) {
                iov[idx].iov_base = static_cast<char*>(iov[idx].iov_base) + advance;
                iov[idx].iov_len -= advance;
                advance = 0;
            } else {
                advance -= iov[idx].iov_len;
                ++idx;
            }
        }
        if (idx < iovcnt && bytes_written > 0) {
            partial_counter.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return true;
}

void WireCaptureWriter::recover_if_due() {
    const auto now_ts = now();
    if (now_ts < next_recovery_time_) {
        return;
    }
    if (rotate_file()) {
        metrics_.degraded_mode.store(false, std::memory_order_release);
        return;
    }
    auto next = cfg_.recovery_initial * 2;
    if (next > cfg_.recovery_max) {
        next = cfg_.recovery_max;
    }
    cfg_.recovery_initial = next;
    next_recovery_time_ = now_ts + cfg_.recovery_initial;
}

bool WireCaptureWriter::check_disk_space() {
#ifdef _WIN32
    ULARGE_INTEGER free_bytes_available{}, total_bytes{}, total_free{};
    if (!GetDiskFreeSpaceExA(cfg_.output_dir.c_str(), &free_bytes_available, &total_bytes, &total_free)) {
        metrics_.io_errors_disk_full.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const std::uint64_t free_bytes = free_bytes_available.QuadPart;
#else
    struct statvfs s {};
    if (statvfs(cfg_.output_dir.c_str(), &s) != 0) {
        metrics_.io_errors_disk_full.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const std::uint64_t free_bytes = static_cast<std::uint64_t>(s.f_bavail) * s.f_bsize;
#endif
    metrics_.disk_space_bytes_free.store(free_bytes, std::memory_order_relaxed);
    if (free_bytes < cfg_.min_disk_free_bytes) {
        metrics_.io_errors_disk_full.fetch_add(1, std::memory_order_relaxed);
        if (rate_limited_log(last_log_error_)) {
            util::log(util::LogLevel::Error, "WireCaptureWriter low disk space: %llu bytes free",
                      static_cast<unsigned long long>(free_bytes));
        }
        return false;
    }
    return true;
}

std::string WireCaptureWriter::make_filename() {
    const auto now_tp = system_clock_->now();
    std::time_t t = std::chrono::system_clock::to_time_t(now_tp);
    char buf[64];
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    std::snprintf(buf, sizeof(buf), "wire_capture_%04d%02d%02d_%02d%02d%02d_seq%06llu.bin",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<unsigned long long>(file_sequence_));
    std::filesystem::create_directories(cfg_.output_dir);
    return (std::filesystem::path(cfg_.output_dir) / buf).string();
}

bool WireCaptureWriter::within_shutdown_deadline() const noexcept {
    return now() < shutdown_deadline_;
}

} // namespace persist
