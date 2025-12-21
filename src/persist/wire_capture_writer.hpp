#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>
#include <functional>
#include <memory>

#include "persist/capture_ring.hpp"
#include "persist/file_sink.hpp"
#include "persist/wire_log_format.hpp"
#include "util/clock.hpp"

namespace persist {

struct WireCaptureConfig {
    std::string output_dir{"./capture"};
    std::chrono::seconds rotate_interval{std::chrono::seconds{300}};
    std::uint64_t rotate_max_bytes{1ULL << 30}; // 1GB default
    std::uint64_t min_disk_free_bytes{5ULL * 1024 * 1024 * 1024ULL}; // 5GB
    std::size_t max_payload{16 * 1024};
    std::size_t ring_capacity{1024};
    std::chrono::milliseconds log_rate_limit{std::chrono::milliseconds{60000}};
    std::chrono::milliseconds recovery_initial{std::chrono::milliseconds{1000}};
    std::chrono::milliseconds recovery_max{std::chrono::milliseconds{30000}};
    std::size_t batch_records{64};
    std::size_t batch_bytes{1 * 1024 * 1024};
    std::chrono::milliseconds shutdown_grace{std::chrono::milliseconds{5000}};
    std::function<std::unique_ptr<IFileSink>()> sink_factory{};
    std::unique_ptr<util::SteadyClock> steady_clock{};
    std::unique_ptr<util::SystemClock> system_clock{};
};

struct WireCaptureMetrics {
    std::atomic<std::uint64_t> messages_submitted{0};
    std::atomic<std::uint64_t> messages_written{0};
    std::atomic<std::uint64_t> bytes_written{0};
    std::atomic<std::uint64_t> drops_queue_full{0};
    std::atomic<std::uint64_t> drops_payload_too_large{0};
    std::atomic<std::uint64_t> drops_degraded_mode{0};
    std::atomic<std::uint64_t> io_errors_write{0};
    std::atomic<std::uint64_t> io_errors_open{0};
    std::atomic<std::uint64_t> io_errors_disk_full{0};
    std::atomic<std::uint64_t> partial_writes{0};
    std::atomic<std::uint64_t> files_rotated{0};
    std::atomic<std::uint64_t> large_payload_count{0};
    std::atomic<std::uint64_t> disk_space_bytes_free{0};
    std::atomic<bool> degraded_mode{false};
};

struct WireCaptureMetricsSnapshot {
    std::uint64_t messages_submitted{0};
    std::uint64_t messages_written{0};
    std::uint64_t bytes_written{0};
    std::uint64_t drops_queue_full{0};
    std::uint64_t drops_payload_too_large{0};
    std::uint64_t drops_degraded_mode{0};
    std::uint64_t io_errors_write{0};
    std::uint64_t io_errors_open{0};
    std::uint64_t io_errors_disk_full{0};
    std::uint64_t partial_writes{0};
    std::uint64_t files_rotated{0};
    std::uint64_t large_payload_count{0};
    std::uint64_t disk_space_bytes_free{0};
    bool degraded_mode{false};
};

class WireCaptureWriter {
public:
    explicit WireCaptureWriter(WireCaptureConfig cfg);
    WireCaptureWriter(WireCaptureConfig cfg,
                      std::unique_ptr<IFileSink> sink,
                      std::unique_ptr<util::SteadyClock> steady_clock,
                      std::unique_ptr<util::SystemClock> system_clock);
    ~WireCaptureWriter();

    bool try_submit(std::span<const std::byte> payload) noexcept;

    void start();
    void stop();

    WireCaptureMetricsSnapshot snapshot_metrics() const;

private:
    using Ring = CaptureRing<1024, 16 * 1024>;

    void run();
    void writer_loop();
    bool ensure_open_file();
    bool rotate_if_needed(std::size_t next_record_bytes);
    bool rotate_file();
    bool perform_write_batch();
    void enter_degraded_mode();
    void recover_if_due();
    bool check_disk_space();
    std::string make_filename();
    std::chrono::steady_clock::time_point now() const noexcept { return steady_clock_->now(); }
    bool within_shutdown_deadline() const noexcept;
    bool rate_limited_log(std::chrono::steady_clock::time_point& last);

    WireCaptureConfig cfg_;
    WireCaptureMetrics metrics_;

    std::unique_ptr<Ring> ring_;
    std::unique_ptr<IFileSink> sink_;
    std::thread writer_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_writer_{false};

    std::unique_ptr<util::SteadyClock> steady_clock_;
    std::unique_ptr<util::SystemClock> system_clock_;

    std::uint64_t current_file_size_{0};
    std::uint64_t file_sequence_{0};
    std::chrono::steady_clock::time_point file_open_time_{};
    std::chrono::steady_clock::time_point shutdown_deadline_{};
    std::chrono::steady_clock::time_point last_log_error_{};
    std::chrono::steady_clock::time_point last_log_large_{};
    std::chrono::steady_clock::time_point next_recovery_time_{};
};

} // namespace persist
