#include "util/async_log.hpp"

#include <chrono>
#include <cstdarg>
#include <cstring>

#include "util/rdtsc.hpp"

namespace util {
namespace {
AsyncLogger* global_hot_logger() {
    static AsyncLogger logger;
    return &logger;
}
}

AsyncLogger::~AsyncLogger() { stop(); }

bool AsyncLogger::start(const Config& cfg) noexcept {
    if (!is_power_of_two(cfg.capacity_pow2) || cfg.capacity_pow2 < 2) {
        return false;
    }

    if (!stop_.load(std::memory_order_acquire)) {
        return true; // already running
    }

    config_ = cfg;
    head_.store(0, std::memory_order_relaxed);
    tail_ = 0;
    mask_ = cfg.capacity_pow2 - 1;
    capacity_ = cfg.capacity_pow2;

    std::unique_ptr<Slot[]> new_slots;
    try {
        new_slots.reset(new Slot[capacity_]);
    } catch (...) {
        return false;
    }

    for (std::size_t i = 0; i < capacity_; ++i) {
        new_slots[i].sequence.store(i, std::memory_order_relaxed);
    }
    slots_ = std::move(new_slots);

    if (!cfg.file_path.empty()) {
        sink_ = std::fopen(cfg.file_path.c_str(), "a");
        if (!sink_) {
            return false;
        }
        owns_file_ = true;
    } else {
        sink_ = stderr;
        owns_file_ = false;
    }

    stop_.store(false, std::memory_order_release);
    try {
        consumer_ = std::thread([this] { consumer_loop(); });
    } catch (...) {
        stop_.store(true, std::memory_order_release);
        if (owns_file_ && sink_) {
            std::fclose(sink_);
        }
        owns_file_ = false;
        sink_ = stderr;
        return false;
    }
    return true;
}

void AsyncLogger::stop() noexcept {
    if (stop_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (consumer_.joinable()) {
        consumer_.join();
    }
    if (owns_file_ && sink_) {
        std::fclose(sink_);
    }
    sink_ = stderr;
    owns_file_ = false;
}

std::uint64_t AsyncLogger::now_ticks() const noexcept {
    if (config_.use_rdtsc) {
        return ::util::rdtsc();
    }
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

bool AsyncLogger::try_log(LogLevel lvl, const char* category, const char* msg, std::size_t len,
                          std::uint64_t arg0, std::uint64_t arg1) noexcept {
    const std::size_t cat_len = category ? std::min<std::size_t>(std::strlen(category), sizeof(LogRecord::category)) : 0;
    return try_log(lvl, category, cat_len, msg, len, arg0, arg1);
}

bool AsyncLogger::try_log(LogLevel lvl, const char* category, std::size_t category_len, const char* msg, std::size_t len,
                          std::uint64_t arg0, std::uint64_t arg1) noexcept {
    if (stop_.load(std::memory_order_acquire) || !slots_) {
        return false;
    }

    std::uint64_t pos = head_.load(std::memory_order_relaxed);
    for (;;) {
        Slot& slot = slots_[pos & mask_];
        const std::uint64_t seq = slot.sequence.load(std::memory_order_acquire);
        const std::int64_t dif = static_cast<std::int64_t>(seq) - static_cast<std::int64_t>(pos);
        if (dif == 0) {
            if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
                break;
            }
        } else if (dif < 0) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false; // full
        } else {
            pos = head_.load(std::memory_order_relaxed);
        }
    }

    Slot& slot = slots_[pos & mask_];
    LogRecord& rec = slot.record;
    static thread_local std::uint32_t tid_hash = static_cast<std::uint32_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    rec.timestamp = now_ticks();
    rec.level = lvl;
    rec.thread_id_hash = tid_hash;
    std::memset(rec.category, 0, sizeof(rec.category));
    const std::size_t cat_len = std::min<std::size_t>(sizeof(rec.category), category_len);
    if (category && cat_len > 0) {
        std::memcpy(rec.category, category, cat_len);
    }
    rec.message_len = static_cast<std::uint16_t>(std::min<std::size_t>(sizeof(rec.message), len));
    if (rec.message_len > 0) {
        std::memcpy(rec.message, msg, rec.message_len);
    }
    rec.arg0 = arg0;
    rec.arg1 = arg1;

    slot.sequence.store(pos + 1, std::memory_order_release);
    return true;
}

bool AsyncLogger::try_logf(LogLevel lvl, const char* category, const char* fmt, ...) noexcept {
    char buffer[192];
    va_list args;
    va_start(args, fmt);
    const int written = std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    const std::size_t len = written < 0 ? 0u : static_cast<std::size_t>(std::min<int>(written, static_cast<int>(sizeof(buffer))));
    return try_log(lvl, category, buffer, len);
}

bool AsyncLogger::try_pop(LogRecord& out) noexcept {
    Slot& slot = slots_[tail_ & mask_];
    const std::uint64_t seq = slot.sequence.load(std::memory_order_acquire);
    const std::int64_t dif = static_cast<std::int64_t>(seq) - static_cast<std::int64_t>(tail_ + 1);
    if (dif == 0) {
        out = slot.record;
        slot.sequence.store(tail_ + mask_ + 1, std::memory_order_release);
        ++tail_;
        return true;
    }
    return false;
}

void AsyncLogger::write_record(const LogRecord& rec) noexcept {
    if (!sink_) return;
    std::fprintf(sink_, "[%llu][%s][%u][%s] ", static_cast<unsigned long long>(rec.timestamp),
                 level_name(rec.level), static_cast<unsigned>(rec.thread_id_hash), rec.category);
    if (rec.message_len > 0) {
        std::fwrite(rec.message, 1, rec.message_len, sink_);
    }
    if (rec.arg0 || rec.arg1) {
        std::fprintf(sink_, " | a0=%llu a1=%llu", static_cast<unsigned long long>(rec.arg0),
                     static_cast<unsigned long long>(rec.arg1));
    }
    std::fputc('\n', sink_);
}

void AsyncLogger::consumer_loop() noexcept {
    std::size_t since_flush = 0;
    std::uint32_t idle_spins = 0;
    while (!stop_.load(std::memory_order_acquire) || tail_ != head_.load(std::memory_order_acquire)) {
        LogRecord rec{};
        bool had = try_pop(rec);
        if (had) {
            write_record(rec);
            written_.fetch_add(1, std::memory_order_relaxed);
            ++since_flush;
            idle_spins = 0;
            if ((config_.flush_on_warn && rec.level >= LogLevel::Warn) ||
                (config_.flush_every > 0 && since_flush >= config_.flush_every)) {
                std::fflush(sink_);
                since_flush = 0;
            }
        } else {
            since_flush = 0;
            if (idle_spins < 256) {
                ++idle_spins;
                std::this_thread::yield();
            } else if (config_.consumer_sleep_ns > 0) {
                idle_spins = 0;
                std::this_thread::sleep_for(std::chrono::nanoseconds(config_.consumer_sleep_ns));
            } else {
                std::this_thread::yield();
            }
        }
    }
    if (sink_) {
        std::fflush(sink_);
    }
}

} // namespace util

