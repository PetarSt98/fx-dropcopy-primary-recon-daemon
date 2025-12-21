#pragma once

#include <chrono>

namespace util {

// Thin clock abstraction to enable deterministic testing of time-based rotation/backoff.
class SteadyClock {
public:
    using time_point = std::chrono::steady_clock::time_point;

    virtual ~SteadyClock() = default;
    virtual time_point now() const noexcept { return std::chrono::steady_clock::now(); }
    virtual SteadyClock* clone() const { return new SteadyClock(); }
};

class SystemClock {
public:
    using time_point = std::chrono::system_clock::time_point;

    virtual ~SystemClock() = default;
    virtual time_point now() const noexcept { return std::chrono::system_clock::now(); }
    virtual SystemClock* clone() const { return new SystemClock(); }
};

} // namespace util
