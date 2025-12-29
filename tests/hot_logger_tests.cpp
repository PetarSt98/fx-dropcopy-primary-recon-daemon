#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "util/hot_log.hpp"

namespace {

std::filesystem::path make_temp_path(const std::string& name) {
    auto dir = std::filesystem::temp_directory_path();
    return dir / name;
}

util::HotLogger::Config base_cfg(const std::filesystem::path& path) {
    util::HotLogger::Config cfg{};
    cfg.ring_size_pow2 = 1u << 4;
    cfg.max_rings = 8;
    cfg.file_path = path.string();
    cfg.flush_every = 1;
    cfg.consumer_sleep_ns = 1000;
    return cfg;
}

TEST(HotLoggerTests, EmitsStructuredEvents) {
    auto path = make_temp_path("hot_logger_basic.log");
    auto cfg = base_cfg(path);
    ASSERT_TRUE(util::init_hot_logger(cfg));

    for (int i = 0; i < 16; ++i) {
        EXPECT_TRUE(util::hot_logger().emit_seq_gap(1u, static_cast<std::uint64_t>(i), static_cast<std::uint64_t>(i + 1)));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    util::shutdown_hot_logger();

    std::ifstream in(path);
    std::size_t lines = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) ++lines;
    }
    EXPECT_GE(lines, 16u);
}

TEST(HotLoggerTests, DropsOnFullRingWithoutBlockingProducer) {
    auto path = make_temp_path("hot_logger_drops.log");
    auto cfg = base_cfg(path);
    cfg.ring_size_pow2 = 1u << 3;
    cfg.consumer_sleep_ns = 5'000'000; // stall consumer to fill ring
    ASSERT_TRUE(util::init_hot_logger(cfg));

    for (int i = 0; i < 200; ++i) {
        util::hot_logger().emit_latency_sample(1u, static_cast<std::uint64_t>(i));
    }

    util::shutdown_hot_logger();
    EXPECT_GT(util::hot_logger().dropped(), 0u);
}

TEST(HotLoggerTests, RegistersMultipleProducers) {
    auto path = make_temp_path("hot_logger_multi.log");
    auto cfg = base_cfg(path);
    cfg.ring_size_pow2 = 1u << 8;
    cfg.consumer_sleep_ns = 0;
    ASSERT_TRUE(util::init_hot_logger(cfg));

    constexpr int producers = 4;
    constexpr int per_thread = 64;
    std::vector<std::thread> threads;
    threads.reserve(producers);
    for (int p = 0; p < producers; ++p) {
        threads.emplace_back([p] {
            for (int i = 0; i < per_thread; ++i) {
                util::hot_logger().emit_divergence(static_cast<std::uint64_t>(p), i, i + 1, 1u, 0u);
            }
        });
    }
    for (auto& t : threads) t.join();

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    util::shutdown_hot_logger();

    std::ifstream in(path);
    std::size_t lines = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) ++lines;
    }
    EXPECT_GE(lines, static_cast<std::size_t>(producers * per_thread));
}

TEST(HotLoggerTests, SafeEmitBeforeStart) {
    util::shutdown_hot_logger();
    util::HotEvent ev{};
    ev.header.type = static_cast<std::uint16_t>(util::HotEventType::LatencySample);
    EXPECT_FALSE(util::hot_logger().emit(ev));
}

} // namespace

