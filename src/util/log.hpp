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

inline void log(LogLevel lvl, const char* fmt, ...) {
    static std::mutex mtx;
    std::lock_guard<std::mutex> lock(mtx);
    std::fprintf(stderr, "%s: ", level_name(lvl));
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fprintf(stderr, "\n");
}

} // namespace util
