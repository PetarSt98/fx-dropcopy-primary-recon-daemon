#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/divergence.hpp"
#include "core/order_state_store.hpp"
#include "core/reconciler.hpp"
#include "core/sequence_tracker.hpp"
#include "ingest/aeron_subscriber.hpp"
#include "tests/e2e/aeron_test_harness.hpp"
#include "util/arena.hpp"

using namespace std::chrono_literals;

namespace {

struct TestEnv {
    std::filesystem::path aeron_dir;
    e2e::MediaDriverGuard driver;
    std::shared_ptr<aeron::Aeron> aeron_client;
    std::unique_ptr<ingest::Ring> primary_ring;
    std::unique_ptr<ingest::Ring> dropcopy_ring;
    ingest::ThreadStats primary_stats{};
    ingest::ThreadStats dropcopy_stats{};
    core::ReconCounters counters{};
    core::DivergenceRing divergence_ring;
    core::SequenceGapRing seq_gap_ring;
    std::atomic<bool> stop_flag{false};
    util::Arena arena{util::Arena::default_capacity_bytes};
    core::OrderStateStore store{arena, 1u << 12};

    std::thread primary_thread;
    std::thread dropcopy_thread;
    std::thread recon_thread;
    std::shared_ptr<aeron::Aeron> publisher_client;
    std::shared_ptr<ingest::AeronSubscriber> primary_sub;
    std::shared_ptr<ingest::AeronSubscriber> dropcopy_sub;
    std::shared_ptr<core::Reconciler> reconciler;

    ~TestEnv() {
        teardown();
    }

    void teardown() {
        stop_flag.store(true, std::memory_order_release);
        if (primary_thread.joinable()) primary_thread.join();
        if (dropcopy_thread.joinable()) dropcopy_thread.join();
        if (recon_thread.joinable()) recon_thread.join();
        driver.stop();
    }
};

struct WaitOutcome {
    bool success{false};
    std::string message;
};

WaitOutcome wait_for_counts(const core::ReconCounters& counters,
                            std::uint64_t expected_primary,
                            std::uint64_t expected_dropcopy,
                            std::uint64_t expected_divergences,
                            std::uint64_t expected_gaps,
                            std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (counters.internal_events >= expected_primary &&
            counters.dropcopy_events >= expected_dropcopy &&
            counters.divergence_total >= expected_divergences &&
            (counters.primary_seq_gaps + counters.dropcopy_seq_gaps +
             counters.primary_seq_duplicates + counters.dropcopy_seq_duplicates +
             counters.primary_seq_out_of_order + counters.dropcopy_seq_out_of_order) >= expected_gaps) {
            return {true, ""};
        }
        std::this_thread::sleep_for(10ms);
    }

    std::ostringstream oss;
    oss << "Timed out waiting for counters. "
        << "primary=" << counters.internal_events << "/" << expected_primary << " "
        << "dropcopy=" << counters.dropcopy_events << "/" << expected_dropcopy << " "
        << "divergence_total=" << counters.divergence_total << "/" << expected_divergences << " "
        << "gap_counts=" << (counters.primary_seq_gaps + counters.dropcopy_seq_gaps +
                             counters.primary_seq_duplicates + counters.dropcopy_seq_duplicates +
                             counters.primary_seq_out_of_order + counters.dropcopy_seq_out_of_order)
        << "/" << expected_gaps << " "
        << "divergence_ring_drops=" << counters.divergence_ring_drops << " "
        << "seq_gap_ring_drops=" << counters.sequence_gap_ring_drops;
    return {false, oss.str()};
}

std::unique_ptr<TestEnv> make_env(const std::string& test_name) {
    auto env = std::make_unique<TestEnv>();
    env->primary_ring = std::make_unique<ingest::Ring>();
    env->dropcopy_ring = std::make_unique<ingest::Ring>();

    env->aeron_dir = e2e::make_unique_aeron_dir(test_name);
    std::string err;
    env->driver = e2e::MediaDriverGuard::start(env->aeron_dir, 5s, err);
    if (!env->driver.valid()) {
        ADD_FAILURE() << "Failed to start Aeron media driver: " << err;
        return env;
    }

    env->aeron_client = e2e::connect_client(env->aeron_dir, err);
    if (!env->aeron_client) {
        ADD_FAILURE() << "Failed to connect Aeron client: " << err;
        return env;
    }
    env->publisher_client = e2e::connect_client(env->aeron_dir, err);
    if (!env->publisher_client) {
        ADD_FAILURE() << "Failed to connect publisher client: " << err;
        return env;
    }

    return env;
}

void start_threads(TestEnv& env,
                   const std::string& primary_channel,
                   std::int32_t primary_stream,
                   const std::string& dropcopy_channel,
                   std::int32_t dropcopy_stream) {
    env.primary_sub = std::make_shared<ingest::AeronSubscriber>(primary_channel,
                                                                primary_stream,
                                                                *env.primary_ring,
                                                                env.primary_stats,
                                                                core::Source::Primary,
                                                                env.aeron_client,
                                                                env.stop_flag);
    env.dropcopy_sub = std::make_shared<ingest::AeronSubscriber>(dropcopy_channel,
                                                                 dropcopy_stream,
                                                                 *env.dropcopy_ring,
                                                                 env.dropcopy_stats,
                                                                 core::Source::DropCopy,
                                                                 env.aeron_client,
                                                                 env.stop_flag);

    env.reconciler = std::make_shared<core::Reconciler>(env.stop_flag,
                                                        *env.primary_ring,
                                                        *env.dropcopy_ring,
                                                        env.store,
                                                        env.counters,
                                                        env.divergence_ring,
                                                        env.seq_gap_ring);

    env.primary_thread = std::thread([sub = env.primary_sub] { sub->run(); });
    env.dropcopy_thread = std::thread([sub = env.dropcopy_sub] { sub->run(); });
    env.recon_thread = std::thread([recon = env.reconciler] { recon->run(); });
}

TEST(E2EReconciler, HappyPathNoDivergenceNoGaps) {
#ifdef _WIN32
    GTEST_SKIP() << "Aeron media driver process launching is not supported in this harness on Windows.";
#endif
    constexpr std::int32_t primary_stream = 31001;
    constexpr std::int32_t dropcopy_stream = 31002;
    const std::string primary_channel = "aeron:udp?endpoint=localhost:31021";
    const std::string dropcopy_channel = "aeron:udp?endpoint=localhost:31022";
    constexpr std::uint16_t session_id = 42;
    constexpr std::size_t n = 6;

    auto env = make_env("happy");
    ASSERT_TRUE(env->driver.valid());
    ASSERT_NE(env->aeron_client, nullptr);
    ASSERT_NE(env->publisher_client, nullptr);

    start_threads(*env, primary_channel, primary_stream, dropcopy_channel, dropcopy_stream);

    std::vector<core::WireExecEvent> primary_msgs;
    std::vector<core::WireExecEvent> dropcopy_msgs;
    primary_msgs.reserve(n);
    dropcopy_msgs.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const auto seq = static_cast<std::uint64_t>(i + 1);
        const std::string clord = "CID-HAPPY-" + std::to_string(seq);
        const std::string oid = "OID-HAPPY-" + std::to_string(seq);
        primary_msgs.push_back(e2e::make_wire_exec(seq,
                                                   session_id,
                                                   static_cast<std::uint8_t>(core::ExecType::Fill),
                                                   static_cast<std::uint8_t>(core::OrdStatus::Filled),
                                                   1'000'000 + static_cast<std::int64_t>(seq),
                                                   100 + static_cast<std::int64_t>(seq),
                                                   100 + static_cast<std::int64_t>(seq),
                                                   1000 + seq,
                                                   1000 + seq,
                                                   clord,
                                                   oid,
                                                   "EXEC-HAPPY-P-" + std::to_string(seq)));
        dropcopy_msgs.push_back(e2e::make_wire_exec(seq,
                                                    session_id,
                                                    static_cast<std::uint8_t>(core::ExecType::Fill),
                                                    static_cast<std::uint8_t>(core::OrdStatus::Filled),
                                                    1'000'000 + static_cast<std::int64_t>(seq),
                                                    100 + static_cast<std::int64_t>(seq),
                                                    100 + static_cast<std::int64_t>(seq),
                                                    1000 + seq,
                                                    1000 + seq,
                                                    clord,
                                                    oid,
                                                    "EXEC-HAPPY-D-" + std::to_string(seq)));
    }

    const auto deadline = std::chrono::steady_clock::now() + 8s;
    const auto sent_primary = e2e::publish_events(*env->publisher_client,
                                                  primary_channel,
                                                  primary_stream,
                                                  primary_msgs,
                                                  deadline);
    const auto sent_dropcopy = e2e::publish_events(*env->publisher_client,
                                                   dropcopy_channel,
                                                   dropcopy_stream,
                                                   dropcopy_msgs,
                                                   deadline);
    ASSERT_EQ(sent_primary, n) << "Failed to publish all primary messages";
    ASSERT_EQ(sent_dropcopy, n) << "Failed to publish all dropcopy messages";

    const auto outcome = wait_for_counts(env->counters,
                                         n,
                                         n,
                                         0,
                                         0,
                                         5s);
    if (!outcome.success) {
        auto divs = e2e::drain_ring<core::Divergence>(env->divergence_ring);
        auto gaps = e2e::drain_ring<core::SequenceGapEvent>(env->seq_gap_ring);
        FAIL() << outcome.message << " divergences=" << divs.size() << " gaps=" << gaps.size();
    }

    EXPECT_EQ(env->counters.internal_events, n);
    EXPECT_EQ(env->counters.dropcopy_events, n);
    EXPECT_EQ(env->counters.divergence_total, 0);
    EXPECT_EQ(env->counters.primary_seq_gaps + env->counters.dropcopy_seq_gaps, 0);
}

TEST(E2EReconciler, DivergenceAndGapClassification) {
#ifdef _WIN32
    GTEST_SKIP() << "Aeron media driver process launching is not supported in this harness on Windows.";
#endif
    constexpr std::int32_t primary_stream = 32001;
    constexpr std::int32_t dropcopy_stream = 32002;
    const std::string primary_channel = "aeron:udp?endpoint=localhost:32021";
    const std::string dropcopy_channel = "aeron:udp?endpoint=localhost:32022";
    constexpr std::uint16_t session_id = 84;

    auto env = make_env("divergence");
    ASSERT_TRUE(env->driver.valid());
    ASSERT_NE(env->aeron_client, nullptr);
    ASSERT_NE(env->publisher_client, nullptr);

    start_threads(*env, primary_channel, primary_stream, dropcopy_channel, dropcopy_stream);

    // Scenarios:
    // 1) MissingFill: dropcopy reports fill seq 2, primary only new.
    // 2) PhantomOrder: dropcopy order never seen on primary.
    // 3) QuantityMismatch: differing cum qty.
    // 4) StateMismatch: statuses differ.
    // Also inject sequence gaps/out-of-order.
    std::vector<core::WireExecEvent> primary_msgs;
    std::vector<core::WireExecEvent> dropcopy_msgs;

    // Primary order A: New seq 1, gap (skip 2), Fill seq 3 (after gap) -> gap on primary.
    primary_msgs.push_back(e2e::make_wire_exec(1, session_id,
                                               static_cast<std::uint8_t>(core::ExecType::New),
                                               static_cast<std::uint8_t>(core::OrdStatus::New),
                                               1'000'000, 0, 0, 1000, 1000,
                                               "CL-A", "OID-A", "EXEC-A-1"));
    primary_msgs.push_back(e2e::make_wire_exec(3, session_id,
                                               static_cast<std::uint8_t>(core::ExecType::Fill),
                                               static_cast<std::uint8_t>(core::OrdStatus::Filled),
                                               1'000'000, 100, 100, 1001, 1001,
                                               "CL-A", "OID-A", "EXEC-A-3"));

    // Primary order B: New seq 4, quantity mismatch vs dropcopy.
    primary_msgs.push_back(e2e::make_wire_exec(4, session_id,
                                               static_cast<std::uint8_t>(core::ExecType::New),
                                               static_cast<std::uint8_t>(core::OrdStatus::New),
                                               1'100'000, 0, 0, 2000, 2000,
                                               "CL-B", "OID-B", "EXEC-B-4"));
    primary_msgs.push_back(e2e::make_wire_exec(5, session_id,
                                               static_cast<std::uint8_t>(core::ExecType::Fill),
                                               static_cast<std::uint8_t>(core::OrdStatus::Filled),
                                               1'100'000, 50, 50, 2001, 2001,
                                               "CL-B", "OID-B", "EXEC-B-5"));

    // Dropcopy order A: Partial seq 1 (duplicate intentional), Fill seq 2 (missing fill divergence).
    dropcopy_msgs.push_back(e2e::make_wire_exec(1, session_id,
                                                static_cast<std::uint8_t>(core::ExecType::PartialFill),
                                                static_cast<std::uint8_t>(core::OrdStatus::PartiallyFilled),
                                                1'000'000, 50, 50, 1000, 1000,
                                                "CL-A", "OID-A", "EXEC-A-DC-1"));
    dropcopy_msgs.push_back(e2e::make_wire_exec(2, session_id,
                                                static_cast<std::uint8_t>(core::ExecType::Fill),
                                                static_cast<std::uint8_t>(core::OrdStatus::Filled),
                                                1'000'000, 50, 100, 1001, 1001,
                                                "CL-A", "OID-A", "EXEC-A-DC-2"));

    // Dropcopy order B: Fill seq 3 with cum 80 -> quantity mismatch vs 50.
    dropcopy_msgs.push_back(e2e::make_wire_exec(3, session_id,
                                                static_cast<std::uint8_t>(core::ExecType::Fill),
                                                static_cast<std::uint8_t>(core::OrdStatus::Filled),
                                                1'100'000, 80, 80, 2001, 2001,
                                                "CL-B", "OID-B", "EXEC-B-DC-3"));

    // Dropcopy order C: Phantom order seq 4.
    dropcopy_msgs.push_back(e2e::make_wire_exec(4, session_id,
                                                static_cast<std::uint8_t>(core::ExecType::New),
                                                static_cast<std::uint8_t>(core::OrdStatus::New),
                                                1'200'000, 0, 0, 3000, 3000,
                                                "CL-C", "OID-C", "EXEC-C-DC-4"));

    // Dropcopy order D: State mismatch (dropcopy Canceled, primary New never canceled).
    primary_msgs.push_back(e2e::make_wire_exec(6, session_id,
                                               static_cast<std::uint8_t>(core::ExecType::New),
                                               static_cast<std::uint8_t>(core::OrdStatus::New),
                                               1'300'000, 0, 0, 4000, 4000,
                                               "CL-D", "OID-D", "EXEC-D-6"));
    dropcopy_msgs.push_back(e2e::make_wire_exec(5, session_id,
                                                static_cast<std::uint8_t>(core::ExecType::Cancel),
                                                static_cast<std::uint8_t>(core::OrdStatus::Canceled),
                                                1'300'000, 0, 0, 4000, 4000,
                                                "CL-D", "OID-D", "EXEC-D-DC-5"));

    // Out-of-order duplicate on dropcopy for order A (seq 2 again) to trigger duplicate/out-of-order.
    dropcopy_msgs.push_back(e2e::make_wire_exec(2, session_id,
                                                static_cast<std::uint8_t>(core::ExecType::Fill),
                                                static_cast<std::uint8_t>(core::OrdStatus::Filled),
                                                1'000'000, 50, 100, 1002, 1002,
                                                "CL-A", "OID-A", "EXEC-A-DC-2B"));

    const auto deadline = std::chrono::steady_clock::now() + 10s;
    const auto sent_primary = e2e::publish_events(*env->publisher_client,
                                                  primary_channel,
                                                  primary_stream,
                                                  primary_msgs,
                                                  deadline);
    const auto sent_dropcopy = e2e::publish_events(*env->publisher_client,
                                                   dropcopy_channel,
                                                   dropcopy_stream,
                                                   dropcopy_msgs,
                                                   deadline);

    ASSERT_EQ(sent_primary, primary_msgs.size()) << "Published fewer primary messages";
    ASSERT_EQ(sent_dropcopy, dropcopy_msgs.size()) << "Published fewer dropcopy messages";

    // Expected divergences: MissingFill (A), PhantomOrder (C), QuantityMismatch (B), StateMismatch (D).
    const auto expected_divergences = 4u;
    const auto expected_gaps = 2u; // primary gap (seq jump) + dropcopy duplicate/out-of-order
    const auto outcome = wait_for_counts(env->counters,
                                         sent_primary,
                                         sent_dropcopy,
                                         expected_divergences,
                                         expected_gaps,
                                         8s);
    if (!outcome.success) {
        auto divs = e2e::drain_ring<core::Divergence>(env->divergence_ring);
        auto gaps = e2e::drain_ring<core::SequenceGapEvent>(env->seq_gap_ring);
        FAIL() << outcome.message << " divergences=" << divs.size() << " gaps=" << gaps.size();
    }

    auto divergences = e2e::drain_ring<core::Divergence>(env->divergence_ring);
    ASSERT_EQ(divergences.size(), expected_divergences);

    std::size_t missing_fill = 0, phantom = 0, qty_mismatch = 0, state_mismatch = 0;
    for (const auto& d : divergences) {
        switch (d.type) {
        case core::DivergenceType::MissingFill: ++missing_fill; break;
        case core::DivergenceType::PhantomOrder: ++phantom; break;
        case core::DivergenceType::QuantityMismatch: ++qty_mismatch; break;
        case core::DivergenceType::StateMismatch: ++state_mismatch; break;
        default: break;
        }
    }
    EXPECT_EQ(missing_fill, 1u);
    EXPECT_EQ(phantom, 1u);
    EXPECT_EQ(qty_mismatch, 1u);
    EXPECT_EQ(state_mismatch, 1u);

    auto gaps = e2e::drain_ring<core::SequenceGapEvent>(env->seq_gap_ring);
    EXPECT_GE(gaps.size(), 1u);
    EXPECT_GE(env->counters.primary_seq_gaps + env->counters.dropcopy_seq_gaps +
                  env->counters.primary_seq_duplicates + env->counters.dropcopy_seq_duplicates +
                  env->counters.primary_seq_out_of_order + env->counters.dropcopy_seq_out_of_order,
              expected_gaps);
}

} // namespace
