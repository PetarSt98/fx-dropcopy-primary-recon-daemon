#include <filesystem>
#include <fstream>
#include <atomic>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "util/async_log.hpp"

namespace {
using util::AsyncLogger;
using util::LogLevel;

std::filesystem::path make_temp_log_path(const std::string& name) {
    auto dir = std::filesystem::temp_directory_path();
    return dir / name;
}

AsyncLogger::Config base_config(const std::filesystem::path& path) {
    AsyncLogger::Config cfg{};
    cfg.capacity_pow2 = 1u << 10;
    cfg.flush_every = 1;
    cfg.file_path = path.string();
    cfg.use_rdtsc = false;
    cfg.consumer_sleep_ns = 1'000; // avoid busy loops in tests
    return cfg;
}
} // namespace

TEST(AsyncLoggerTests, WritesRecords) {
    auto path = make_temp_log_path("async_logger_basic.log");
    AsyncLogger logger;
    ASSERT_TRUE(logger.start(base_config(path)));

    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(logger.try_logf(LogLevel::Info, "TEST", "msg-%d", i));
    }

    // allow consumer to drain
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (logger.written() < 10 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    logger.stop();

    std::ifstream in(path);
    std::string line;
    std::size_t count = 0;
    while (std::getline(in, line)) {
        if (!line.empty()) ++count;
    }
    EXPECT_EQ(count, 10u);
}

TEST(AsyncLoggerTests, DropsOnOverflow) {
    auto path = make_temp_log_path("async_logger_drop.log");
    AsyncLogger logger;
    auto cfg = base_config(path);
    cfg.capacity_pow2 = 8;
    cfg.consumer_sleep_ns = 500'000; // slow consumer to force drops
    ASSERT_TRUE(logger.start(cfg));

    for (int i = 0; i < 200; ++i) {
        logger.try_logf(LogLevel::Info, "DROP", "event-%d", i);
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        if (logger.dropped() > 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    logger.stop();
    EXPECT_GT(logger.dropped(), 0u);
}

TEST(AsyncLoggerTests, ThreadSafetyMultiProducer) {
    auto path = make_temp_log_path("async_logger_threads.log");
    AsyncLogger logger;
    auto cfg = base_config(path);
    cfg.capacity_pow2 = 1u << 12;
    cfg.consumer_sleep_ns = 0;
    ASSERT_TRUE(logger.start(cfg));

    constexpr int producers = 4;
    constexpr int per_thread = 250;
    std::atomic<int> sent{0};
    std::vector<std::thread> threads;
    threads.reserve(producers);
    for (int p = 0; p < producers; ++p) {
        threads.emplace_back([&] {
            for (int i = 0; i < per_thread; ++i) {
                logger.try_logf(LogLevel::Debug, "MP", "p%d-%d", p, i);
                sent.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& t : threads) t.join();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (logger.written() + logger.dropped() < static_cast<std::uint64_t>(sent.load(std::memory_order_relaxed)) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    logger.stop();
    const auto total_seen = logger.written() + logger.dropped();
    EXPECT_EQ(total_seen, static_cast<std::uint64_t>(sent.load(std::memory_order_relaxed)));
}

TEST(AsyncLoggerTests, StableEncoding) {
    auto path = make_temp_log_path("async_logger_encoding.log");
    AsyncLogger logger;
    auto cfg = base_config(path);
    cfg.flush_every = 1;
    ASSERT_TRUE(logger.start(cfg));

    logger.try_log(LogLevel::Warn, "ENC", "fixed", 5, 1, 2);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (logger.written() < 1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    logger.stop();

    std::ifstream in(path);
    std::string line;
    ASSERT_TRUE(static_cast<bool>(std::getline(in, line)));
    EXPECT_NE(line.find("[WARN]"), std::string::npos);
    EXPECT_NE(line.find("[ENC]"), std::string::npos);
    EXPECT_NE(line.find("fixed"), std::string::npos);
    EXPECT_NE(line.find("a0=1"), std::string::npos);
    EXPECT_NE(line.find("a1=2"), std::string::npos);
}

TEST(AsyncLoggerTests, ReturnsFalseWhenNotStarted) {
    AsyncLogger logger;
    EXPECT_FALSE(logger.try_log(util::LogLevel::Info, "NA", "msg", 3));
    EXPECT_EQ(logger.written(), 0u);
    EXPECT_EQ(logger.dropped(), 0u);
}

