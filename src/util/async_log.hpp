#pragma once

#include <atomic>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#include "util/log.hpp"

namespace util {

struct LogRecord {
    std::uint64_t timestamp{0};
    LogLevel level{LogLevel::Info};
    std::uint32_t thread_id_hash{0};
    char category[8]{};
    std::uint16_t message_len{0};
    char message[192]{};
    std::uint64_t arg0{0};
    std::uint64_t arg1{0};
};

class AsyncLogger {
public:
    struct Config {
        std::size_t capacity_pow2{1u << 12};
        bool use_rdtsc{false};
        bool flush_on_warn{true};
        std::size_t flush_every{256};
        std::string file_path{}; // optional target; stderr when empty
        std::uint64_t consumer_sleep_ns{50'000};
    };

    AsyncLogger() = default;
    ~AsyncLogger();

    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;

    bool start(const Config& cfg) noexcept;
    void stop() noexcept;

    bool try_log(LogLevel lvl, const char* category, const char* msg, std::size_t len,
                 std::uint64_t arg0 = 0, std::uint64_t arg1 = 0) noexcept;

    bool try_log(LogLevel lvl, const char* category, std::size_t category_len, const char* msg, std::size_t len,
                 std::uint64_t arg0 = 0, std::uint64_t arg1 = 0) noexcept;

    bool try_logf(LogLevel lvl, const char* category, const char* fmt, ...) noexcept;

    std::uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }
    std::uint64_t written() const noexcept { return written_.load(std::memory_order_relaxed); }

private:
    struct Slot {
        std::atomic<std::uint64_t> sequence{0};
        LogRecord record{};
    };

    bool try_pop(LogRecord& out) noexcept;
    void consumer_loop() noexcept;
    void write_record(const LogRecord& rec) noexcept;
    std::uint64_t now_ticks() const noexcept;
    bool is_power_of_two(std::size_t v) const noexcept { return v != 0 && (v & (v - 1)) == 0; }

    std::atomic<std::uint64_t> head_{0};
    std::uint64_t tail_{0};
    std::size_t mask_{0};
    std::size_t capacity_{0};
    std::unique_ptr<Slot[]> slots_{};

    std::atomic<bool> stop_{true};
    std::thread consumer_{};
    Config config_{};

    FILE* sink_{stderr};
    bool owns_file_{false};

    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<std::uint64_t> written_{0};
};

AsyncLogger& hot_logger() noexcept;
bool init_hot_logger(const AsyncLogger::Config& cfg) noexcept;
void shutdown_hot_logger() noexcept;

} // namespace util

#define LOG_HOT_LITERAL(LVL, CAT_LIT, MSG_LIT, ARG0, ARG1)                                                \
    do {                                                                                                \
        constexpr std::size_t _cat_len = sizeof(CAT_LIT) > 0 ? sizeof(CAT_LIT) - 1 : 0;                 \
        constexpr std::size_t _msg_len = sizeof(MSG_LIT) > 0 ? sizeof(MSG_LIT) - 1 : 0;                 \
        ::util::hot_logger().try_log((LVL), (CAT_LIT), _cat_len, (MSG_LIT), _msg_len, (ARG0), (ARG1));  \
    } while (0)

#define LOG_HOT_TRACE(MSG_LIT, ARG0, ARG1) LOG_HOT_LITERAL(::util::LogLevel::Trace, "HOT", (MSG_LIT), (ARG0), (ARG1))
#define LOG_HOT_DEBUG(MSG_LIT, ARG0, ARG1) LOG_HOT_LITERAL(::util::LogLevel::Debug, "HOT", (MSG_LIT), (ARG0), (ARG1))
#define LOG_HOT_INFO(MSG_LIT, ARG0, ARG1)  LOG_HOT_LITERAL(::util::LogLevel::Info,  "HOT", (MSG_LIT), (ARG0), (ARG1))
#define LOG_HOT_WARN(MSG_LIT, ARG0, ARG1)  LOG_HOT_LITERAL(::util::LogLevel::Warn,  "HOT", (MSG_LIT), (ARG0), (ARG1))
#define LOG_HOT_ERROR(MSG_LIT, ARG0, ARG1) LOG_HOT_LITERAL(::util::LogLevel::Error, "HOT", (MSG_LIT), (ARG0), (ARG1))
#define LOG_HOT_FATAL(MSG_LIT, ARG0, ARG1) LOG_HOT_LITERAL(::util::LogLevel::Fatal, "HOT", (MSG_LIT), (ARG0), (ARG1))

// Formatting is not hot-path safe; reserve for diagnostics or warm paths.
#define LOG_WARM_FMT(LVL, CAT, FMT, ...) ::util::hot_logger().try_logf((LVL), (CAT), (FMT) __VA_OPT__(, __VA_ARGS__))
#define LOG_HOT_FMT(LVL, CAT, FMT, ...)  LOG_WARM_FMT((LVL), (CAT), (FMT) __VA_OPT__(, __VA_ARGS__))

