#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstring>

// Tests must define FX_PERF_ENABLED to exercise the active macro paths
#define FX_PERF_ENABLED
#include "util/perf_counters.hpp"
#include "util/perf_macros.hpp"

namespace {

// ===== PerfCounterRegistry unit tests =====

class PerfCounterRegistryTest : public ::testing::Test {
protected:
    // Uses the singleton with reset_all() for test isolation.
    util::PerfCounterRegistry& reg_ = util::PerfCounterRegistry::instance();

    void SetUp() override {
        reg_.reset_all();
    }
};

TEST_F(PerfCounterRegistryTest, RegisterAndRetrieve) {
    auto id = reg_.register_counter("test.counter_one");
    ASSERT_NE(id, util::PerfCounterRegistry::INVALID_ID);
    EXPECT_STREQ(reg_.name(id), "test.counter_one");
}

TEST_F(PerfCounterRegistryTest, IdempotentRegistration) {
    auto id1 = reg_.register_counter("test.idempotent");
    auto id2 = reg_.register_counter("test.idempotent");
    EXPECT_EQ(id1, id2);
}

TEST_F(PerfCounterRegistryTest, EmptyNameReturnsInvalid) {
    EXPECT_EQ(reg_.register_counter(""), util::PerfCounterRegistry::INVALID_ID);
    EXPECT_EQ(reg_.register_counter(nullptr), util::PerfCounterRegistry::INVALID_ID);
}

TEST_F(PerfCounterRegistryTest, RecordAndQuery) {
    auto id = reg_.register_counter("test.record_query");
    ASSERT_NE(id, util::PerfCounterRegistry::INVALID_ID);

    reg_.record(id, 100);
    reg_.record(id, 200);
    reg_.record(id, 300);

    const auto& h = reg_.histogram(id);
    EXPECT_EQ(h.count(), 3u);
    EXPECT_EQ(h.sum(), 600u);
}

TEST_F(PerfCounterRegistryTest, ResetAllClearsHistograms) {
    auto id = reg_.register_counter("test.reset_all");
    ASSERT_NE(id, util::PerfCounterRegistry::INVALID_ID);

    reg_.record(id, 1000);
    EXPECT_EQ(reg_.histogram(id).count(), 1u);

    reg_.reset_all();
    EXPECT_EQ(reg_.histogram(id).count(), 0u);
}

TEST_F(PerfCounterRegistryTest, MultipleCountersIndependent) {
    auto id_a = reg_.register_counter("test.counter_a");
    auto id_b = reg_.register_counter("test.counter_b");
    ASSERT_NE(id_a, util::PerfCounterRegistry::INVALID_ID);
    ASSERT_NE(id_b, util::PerfCounterRegistry::INVALID_ID);
    ASSERT_NE(id_a, id_b);

    reg_.record(id_a, 100);
    reg_.record(id_b, 200);
    reg_.record(id_b, 300);

    EXPECT_EQ(reg_.histogram(id_a).count(), 1u);
    EXPECT_EQ(reg_.histogram(id_b).count(), 2u);
}

TEST_F(PerfCounterRegistryTest, DumpAllToBuffer) {
    auto id = reg_.register_counter("test.dump");
    reg_.record(id, 500);

    char buf[1024];
    std::size_t len = reg_.dump_all(buf, sizeof(buf));
    EXPECT_GT(len, 0u);

    std::string output(buf);
    EXPECT_NE(output.find("test.dump"), std::string::npos);
    EXPECT_NE(output.find("P50"), std::string::npos);
}

TEST_F(PerfCounterRegistryTest, RecordWithInvalidIdNoOp) {
    // Should not crash or corrupt data
    reg_.record(util::PerfCounterRegistry::INVALID_ID, 100);
    reg_.record(9999, 100);
}

// ===== Perf macros integration tests =====

// These test the PERF_DECLARE_COUNTER / PERF_BEGIN / PERF_END / PERF_SCOPED macros

PERF_DECLARE_COUNTER(test_macro_begin_end, "test.macro_begin_end");
PERF_DECLARE_COUNTER(test_macro_scoped, "test.macro_scoped");

TEST(PerfMacrosTest, BeginEndRecordsLatency) {
    auto& reg = util::PerfCounterRegistry::instance();
    reg.reset_all();

    PERF_BEGIN(test_macro_begin_end);
    // Simulate work
    volatile int x = 0;
    for (int i = 0; i < 100; ++i) {
        x += i;
    }
    PERF_END(test_macro_begin_end);

    const auto& h = reg.histogram(perf_cid_test_macro_begin_end);
    EXPECT_EQ(h.count(), 1u);
    EXPECT_GT(h.sum(), 0u);
}

TEST(PerfMacrosTest, ScopedGuardRecordsLatency) {
    auto& reg = util::PerfCounterRegistry::instance();
    reg.reset_all();

    {
        PERF_SCOPED(test_macro_scoped);
        volatile int x = 0;
        for (int i = 0; i < 100; ++i) {
            x += i;
        }
    }

    const auto& h = reg.histogram(perf_cid_test_macro_scoped);
    EXPECT_EQ(h.count(), 1u);
    EXPECT_GT(h.sum(), 0u);
}

// ===== Overhead measurement test =====

TEST(PerfMacrosTest, InstrumentationOverheadSubMicrosecond) {
    auto& reg = util::PerfCounterRegistry::instance();
    auto id = reg.register_counter("test.overhead_measure");
    reg.reset_all();

    constexpr int WARMUP = 1000;
    constexpr int MEASURE = 100000;

    // Warm up
    for (int i = 0; i < WARMUP; ++i) {
        reg.record(id, 42);
    }
    reg.reset_all();

    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < MEASURE; ++i) {
        reg.record(id, 42);
    }
    auto end = std::chrono::steady_clock::now();

    const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    const auto per_call_ns = elapsed_ns / MEASURE;

    // Expect sub-microsecond overhead per record call
    EXPECT_LT(per_call_ns, 1000) << "record() overhead: " << per_call_ns << " ns/call";

    std::fprintf(stderr, "[PerfMacrosTest] registry record() overhead: %lld ns/call (%d iterations)\n",
                 static_cast<long long>(per_call_ns), MEASURE);
}

} // namespace
