#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <Aeron.h>

#include "core/wire_exec_event.hpp"

namespace e2e {

namespace chrono = std::chrono;

// RAII guard for an external Aeron media driver process.
class MediaDriverGuard {
public:
    MediaDriverGuard() = default;
    MediaDriverGuard(const MediaDriverGuard&) = delete;
    MediaDriverGuard& operator=(const MediaDriverGuard&) = delete;
    MediaDriverGuard(MediaDriverGuard&& other) noexcept;
    MediaDriverGuard& operator=(MediaDriverGuard&& other) noexcept;
    ~MediaDriverGuard();

    bool valid() const noexcept;
    void stop() noexcept;

    static MediaDriverGuard start(const std::filesystem::path& aeron_dir,
                                  chrono::milliseconds ready_timeout,
                                  std::string& error_out) noexcept;

private:
    explicit MediaDriverGuard(long pid, std::filesystem::path dir) noexcept;

    long pid_{-1};
    std::filesystem::path aeron_dir_{};
};

// Creates a unique Aeron directory for a single test run.
std::filesystem::path make_unique_aeron_dir(const std::string& test_name);

// Waits for cnc.dat to appear, signalling the media driver is ready.
bool wait_for_driver_ready(const std::filesystem::path& aeron_dir,
                           chrono::milliseconds timeout) noexcept;

// Connects an Aeron client pointed at the provided directory.
std::shared_ptr<aeron::Aeron> connect_client(const std::filesystem::path& aeron_dir,
                                             std::string& error_out) noexcept;

// Creates a publication with bounded polling. Returns nullptr on failure.
std::shared_ptr<aeron::Publication> make_publication(aeron::Aeron& client,
                                                     const std::string& channel,
                                                     std::int32_t stream_id,
                                                     chrono::steady_clock::time_point deadline) noexcept;

// Publishes all wire events in order before the deadline. Returns the number of fragments sent.
std::size_t publish_events(aeron::Aeron& client,
                           const std::string& channel,
                           std::int32_t stream_id,
                           const std::vector<core::WireExecEvent>& events,
                           chrono::steady_clock::time_point deadline) noexcept;

// Helper to copy string data into a WireExecEvent field while setting length safely.
void fill_id(char (&dest)[core::WireExecEvent::id_capacity],
             const std::string& src,
             std::uint8_t& len_out) noexcept;

// Utility to build a WireExecEvent with explicit fields for deterministic tests.
core::WireExecEvent make_wire_exec(std::uint64_t seq,
                                   std::uint16_t session,
                                   std::uint8_t exec_type,
                                   std::uint8_t ord_status,
                                   std::int64_t price_micro,
                                   std::int64_t qty,
                                   std::int64_t cum_qty,
                                   std::uint64_t sending_time,
                                   std::uint64_t transact_time,
                                   const std::string& clord_id,
                                   const std::string& order_id,
                                   const std::string& exec_id) noexcept;

// Generic ring drain helper for SPSC rings used in tests.
template <typename T, typename Ring>
std::vector<T> drain_ring(Ring& ring) {
    std::vector<T> out;
    T item{};
    while (ring.try_pop(item)) {
        out.push_back(item);
    }
    return out;
}

} // namespace e2e
