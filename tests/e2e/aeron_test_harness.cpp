#include "tests/e2e/aeron_test_harness.hpp"

#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#include <processthreadsapi.h>
#else
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <concurrent/AtomicBuffer.h>

namespace e2e {

namespace {
using Clock = std::chrono::steady_clock;
}

MediaDriverGuard::MediaDriverGuard(long pid, std::filesystem::path dir) noexcept
    : pid_(pid), aeron_dir_(std::move(dir)) {}

MediaDriverGuard::MediaDriverGuard(MediaDriverGuard&& other) noexcept
    : pid_(other.pid_), aeron_dir_(std::move(other.aeron_dir_)) {
    other.pid_ = -1;
}

MediaDriverGuard& MediaDriverGuard::operator=(MediaDriverGuard&& other) noexcept {
    if (this != &other) {
        stop();
        pid_ = other.pid_;
        aeron_dir_ = std::move(other.aeron_dir_);
        other.pid_ = -1;
    }
    return *this;
}

MediaDriverGuard::~MediaDriverGuard() { stop(); }

bool MediaDriverGuard::valid() const noexcept {
    return pid_ > 0;
}

void MediaDriverGuard::stop() noexcept {
#ifdef _WIN32
    if (pid_ > 0) {
        HANDLE process = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(pid_));
        TerminateProcess(process, 0);
        WaitForSingleObject(process, 5000);
        CloseHandle(process);
        pid_ = -1;
    }
#else
    if (pid_ > 0) {
        kill(pid_, SIGTERM);
        int status = 0;
        waitpid(pid_, &status, 0);
        pid_ = -1;
    }
#endif

    if (!aeron_dir_.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(aeron_dir_, ec);
    }
}

MediaDriverGuard MediaDriverGuard::start(const std::filesystem::path& aeron_dir,
                                         chrono::milliseconds ready_timeout,
                                         std::string& error_out) noexcept {
    error_out.clear();
#ifdef _WIN32
    // Windows Aeron media driver is not spawned here; tests should skip.
    error_out = "External Aeron media driver launch is not supported on Windows in this harness.";
    return MediaDriverGuard{};
#else
    pid_t pid = fork();
    if (pid == 0) {
        const std::string dir_arg = "-Daeron.dir=" + aeron_dir.string();
        const char* argv[] = {"aeronmd",
                              dir_arg.c_str(),
                              "-Daeron.socket.soReusePort=true",
                              nullptr};
        execvp("aeronmd", const_cast<char* const*>(argv));
        std::cerr << "Failed to exec aeronmd" << std::endl;
        std::_Exit(127);
    }
    if (pid < 0) {
        error_out = "fork() failed launching aeronmd";
        return MediaDriverGuard{};
    }

    if (!wait_for_driver_ready(aeron_dir, ready_timeout)) {
        error_out = "aeronmd did not create cnc.dat in time";
        // Ensure the process is reaped to avoid zombies.
        MediaDriverGuard guard(pid, aeron_dir);
        guard.stop();
        return MediaDriverGuard{};
    }

    return MediaDriverGuard(pid, aeron_dir);
#endif
}

std::filesystem::path make_unique_aeron_dir(const std::string& test_name) {
    const auto now = Clock::now().time_since_epoch().count();
    std::ostringstream oss;
#ifdef _WIN32
    const auto pid = static_cast<unsigned long>(GetCurrentProcessId());
#else
    const auto pid = static_cast<unsigned long>(::getpid());
#endif
    oss << "aeron-e2e-" << test_name << "-" << now << "-" << pid;
    const auto dir = std::filesystem::temp_directory_path() / oss.str();
    std::filesystem::create_directories(dir);
    return dir;
}

bool wait_for_driver_ready(const std::filesystem::path& aeron_dir,
                           chrono::milliseconds timeout) noexcept {
    const auto cnc_path = aeron_dir / "cnc.dat";
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        if (std::filesystem::exists(cnc_path)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    return std::filesystem::exists(cnc_path);
}

std::shared_ptr<aeron::Aeron> connect_client(const std::filesystem::path& aeron_dir,
                                             std::string& error_out) noexcept {
    aeron::Context ctx;
    ctx.aeronDir(aeron_dir.string());
    try {
        return aeron::Aeron::connect(ctx);
    } catch (const std::exception& ex) {
        error_out = ex.what();
        return nullptr;
    }
}

std::shared_ptr<aeron::Publication> make_publication(aeron::Aeron& client,
                                                     const std::string& channel,
                                                     std::int32_t stream_id,
                                                     Clock::time_point deadline) noexcept {
    const auto reg_id = client.addPublication(channel, stream_id);
    while (Clock::now() < deadline) {
        auto pub = client.findPublication(reg_id);
        if (pub) {
            return pub;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return nullptr;
}

std::size_t publish_events(aeron::Aeron& client,
                           const std::string& channel,
                           std::int32_t stream_id,
                           const std::vector<core::WireExecEvent>& events,
                           Clock::time_point deadline) noexcept {
    auto pub = make_publication(client, channel, stream_id, deadline);
    if (!pub) {
        return 0;
    }

    std::size_t sent = 0;
    std::array<std::uint8_t, sizeof(core::WireExecEvent)> buffer{};
    while (sent < events.size() && Clock::now() < deadline) {
        std::memcpy(buffer.data(), &events[sent], sizeof(core::WireExecEvent));
        aeron::concurrent::AtomicBuffer atomic_buffer(buffer.data(), buffer.size());
        const auto res = pub->offer(atomic_buffer, 0, static_cast<aeron::util::index_t>(buffer.size()));
        if (res > 0) {
            ++sent;
        } else {
            std::this_thread::yield();
        }
    }
    return sent;
}

void fill_id(char (&dest)[core::WireExecEvent::id_capacity],
             const std::string& src,
             std::uint8_t& len_out) noexcept {
    const auto len = src.size() > core::WireExecEvent::id_capacity ? core::WireExecEvent::id_capacity : src.size();
    std::memcpy(dest, src.data(), len);
    len_out = static_cast<std::uint8_t>(len);
}

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
                                   const std::string& exec_id) noexcept {
    core::WireExecEvent evt{};
    evt.exec_type = exec_type;
    evt.ord_status = ord_status;
    evt.seq_num = seq;
    evt.session_id = session;
    evt.price_micro = price_micro;
    evt.qty = qty;
    evt.cum_qty = cum_qty;
    evt.sending_time = sending_time;
    evt.transact_time = transact_time;
    fill_id(evt.exec_id, exec_id, evt.exec_id_len);
    fill_id(evt.order_id, order_id, evt.order_id_len);
    fill_id(evt.clord_id, clord_id, evt.clord_id_len);
    return evt;
}

} // namespace e2e
