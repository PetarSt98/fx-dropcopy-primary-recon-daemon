#pragma once

#include <cstdio>
#include <cstdarg>
#include <mutex>

namespace util {

enum class LogLevel { Trace, Debug, Info, Warn, Error, Fatal };

inline const char* level_name(LogLevel lvl) noexcept {
    switch (lvl) {
    case LogLevel::Trace: return "TRACE";
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info: return "INFO";
    case LogLevel::Warn: return "WARN";
    case LogLevel::Error: return "ERROR";
    case LogLevel::Fatal: return "FATAL";
    }
    return "UNKNOWN";
}

namespace detail {
inline std::mutex& slow_log_mutex() {
    static std::mutex mtx;
    return mtx;
}

inline void slow_log_impl(LogLevel lvl, const char* fmt, va_list args) {
    std::lock_guard<std::mutex> lock(slow_log_mutex());
    std::fprintf(stderr, "%s: ", level_name(lvl));
    std::vfprintf(stderr, fmt, args);
    std::fprintf(stderr, "\n");
}
} // namespace detail

class SyncLogger {
public:
    static void log(LogLevel lvl, const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        detail::slow_log_impl(lvl, fmt, args);
        va_end(args);
    }
};

inline void log(LogLevel lvl, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    detail::slow_log_impl(lvl, fmt, args);
    va_end(args);
}

} // namespace util

#define LOG_SLOW_TRACE(FMT, ...) ::util::SyncLogger::log(::util::LogLevel::Trace, (FMT) __VA_OPT__(, __VA_ARGS__))
#define LOG_SLOW_DEBUG(FMT, ...) ::util::SyncLogger::log(::util::LogLevel::Debug, (FMT) __VA_OPT__(, __VA_ARGS__))
#define LOG_SLOW_INFO(FMT, ...)  ::util::SyncLogger::log(::util::LogLevel::Info,  (FMT) __VA_OPT__(, __VA_ARGS__))
#define LOG_SLOW_WARN(FMT, ...)  ::util::SyncLogger::log(::util::LogLevel::Warn,  (FMT) __VA_OPT__(, __VA_ARGS__))
#define LOG_SLOW_ERROR(FMT, ...) ::util::SyncLogger::log(::util::LogLevel::Error, (FMT) __VA_OPT__(, __VA_ARGS__))
#define LOG_SLOW_FATAL(FMT, ...) ::util::SyncLogger::log(::util::LogLevel::Fatal, (FMT) __VA_OPT__(, __VA_ARGS__))

