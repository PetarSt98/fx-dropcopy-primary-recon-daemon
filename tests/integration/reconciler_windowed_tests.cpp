#include <gtest/gtest.h>
#include <atomic>
#include <cstring>
#include <memory>
#include <vector>

#include "core/divergence.hpp"
#include "core/order_state.hpp"
#include "core/order_state_store.hpp"
#include "core/recon_config.hpp"
#include "core/recon_state.hpp"
#include "core/recon_timer.hpp"
#include "core/reconciler.hpp"
#include "ingest/spsc_ring.hpp"
#include "util/arena.hpp"
#include "util/tsc_calibration.hpp"
#include "util/wheel_timer.hpp"

namespace core {
namespace test {

// ============================================================================
// Test Fixture and Helpers
// ============================================================================

class ReconcilerWindowedTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Ensure TSC calibration is initialized for tests (3GHz for deterministic tests)
        util::TscCalibration::instance().set_tsc_freq_hz(3'000'000'000ULL);
        stop_flag_.store(false);
        counters_ = ReconCounters{};
    }

    void TearDown() override {
        stop_flag_.store(true);
    }

    // Helper to create test ExecEvent
    ExecEvent make_event(Source src, OrdStatus status, std::int64_t cum_qty,
                         std::int64_t price_micro, std::uint64_t ingest_tsc,
                         const char* clord_id, std::uint64_t seq_num = 1,
                         const char* exec_id = nullptr) {
        ExecEvent ev{};
        ev.source = src;
        ev.ord_status = status;
        ev.cum_qty = cum_qty;
        ev.qty = cum_qty;
        ev.price_micro = price_micro;
        ev.ingest_tsc = ingest_tsc;
        ev.transact_time = ingest_tsc;
        ev.seq_num = seq_num;
        ev.set_clord_id(clord_id, std::strlen(clord_id));
        
        // Set exec_type based on status
        switch (status) {
        case OrdStatus::Filled:
            ev.exec_type = ExecType::Fill;
            break;
        case OrdStatus::PartiallyFilled:
            ev.exec_type = ExecType::PartialFill;
            break;
        case OrdStatus::Canceled:
            ev.exec_type = ExecType::Cancel;
            break;
        case OrdStatus::Rejected:
            ev.exec_type = ExecType::Rejected;
            break;
        default:
            ev.exec_type = ExecType::New;
            break;
        }
        
        // Set exec_id if provided, otherwise use a default based on seq_num
        if (exec_id) {
            ev.set_exec_id(exec_id, std::strlen(exec_id));
        } else {
            char default_exec_id[32];
            std::snprintf(default_exec_id, sizeof(default_exec_id), "EX%lu", 
                         static_cast<unsigned long>(seq_num));
            ev.set_exec_id(default_exec_id, std::strlen(default_exec_id));
        }
        
        return ev;
    }

    // Helper to drain divergence ring
    std::vector<Divergence> drain_divergences(DivergenceRing& ring) {
        std::vector<Divergence> result;
        Divergence div;
        while (ring.try_pop(div)) {
            result.push_back(div);
        }
        return result;
    }

    // Convert nanoseconds to TSC cycles for test timestamps
    std::uint64_t ns_to_tsc(std::uint64_t ns) {
        return util::ns_to_tsc(ns);
    }

    std::atomic<bool> stop_flag_{false};
    ReconCounters counters_{};
};

// ============================================================================
// Test 1: Normal flow - both sides match, no divergence
// ============================================================================
TEST_F(ReconcilerWindowedTest, BothSidesMatch_NoDivergence) {
    ReconConfig config;
    config.grace_period_ns = 500'000'000;  // 500ms
    config.enable_windowed_recon = true;

    util::Arena arena{util::Arena::default_capacity_bytes};
    OrderStateStore store{arena, 1024};
    auto primary_ring = std::make_unique<ingest::SpscRing<ExecEvent, 1u << 16>>();
    auto dropcopy_ring = std::make_unique<ingest::SpscRing<ExecEvent, 1u << 16>>();
    auto divergence_ring = std::make_unique<DivergenceRing>();
    auto seq_gap_ring = std::make_unique<SequenceGapRing>();
    auto timer_wheel = std::make_unique<util::WheelTimer>(0);

    Reconciler reconciler(stop_flag_, *primary_ring, *dropcopy_ring, store,
                          counters_, *divergence_ring, *seq_gap_ring,
                          timer_wheel.get(), config);

    // Primary arrives at t=0
    auto primary = make_event(Source::Primary, OrdStatus::Filled, 100, 5000,
                              ns_to_tsc(0), "ORD1", 1);
    reconciler.process_event_for_test(primary);

    // Dropcopy arrives at t=10ms with matching data
    auto dropcopy = make_event(Source::DropCopy, OrdStatus::Filled, 100, 5000,
                               ns_to_tsc(10'000'000), "ORD1", 1);
    reconciler.process_event_for_test(dropcopy);

    // No divergence should be emitted
    EXPECT_TRUE(drain_divergences(*divergence_ring).empty());
    EXPECT_EQ(counters_.orders_matched, 1u);
    // Note: mismatch_observed=1 because first event (primary only) triggers EXISTENCE mismatch
    // which enters grace period. When dropcopy arrives matching, it resolves as false positive.
    EXPECT_EQ(counters_.mismatch_observed, 1u);
    EXPECT_EQ(counters_.false_positive_avoided, 1u);
}

// ============================================================================
// Test 2: DropCopy leads, primary arrives within grace → NO divergence
// ============================================================================
TEST_F(ReconcilerWindowedTest, DropCopyLeads_PrimaryWithinGrace_NoDivergence) {
    ReconConfig config;
    config.grace_period_ns = 200'000'000;  // 200ms
    config.enable_windowed_recon = true;

    util::Arena arena{util::Arena::default_capacity_bytes};
    OrderStateStore store{arena, 1024};
    auto primary_ring = std::make_unique<ingest::SpscRing<ExecEvent, 1u << 16>>();
    auto dropcopy_ring = std::make_unique<ingest::SpscRing<ExecEvent, 1u << 16>>();
    auto divergence_ring = std::make_unique<DivergenceRing>();
    auto seq_gap_ring = std::make_unique<SequenceGapRing>();
    auto timer_wheel = std::make_unique<util::WheelTimer>(0);

    Reconciler reconciler(stop_flag_, *primary_ring, *dropcopy_ring, store,
                          counters_, *divergence_ring, *seq_gap_ring,
                          timer_wheel.get(), config);

    // DropCopy arrives first at t=0
    auto dropcopy = make_event(Source::DropCopy, OrdStatus::Filled, 100, 5000,
                               ns_to_tsc(0), "ORD1", 1);
    reconciler.process_event_for_test(dropcopy);

    // No divergence yet, but EXISTENCE mismatch detected (only dropcopy seen)
    // State is InGrace with timer scheduled
    EXPECT_TRUE(drain_divergences(*divergence_ring).empty());
    EXPECT_EQ(counters_.mismatch_observed, 1u);

    // Verify state is InGrace (timer scheduled for EXISTENCE mismatch)
    OrderKey key = make_order_key(dropcopy);
    OrderState* os = store.find(key);
    ASSERT_NE(os, nullptr);
    EXPECT_EQ(os->recon_state, ReconState::InGrace);

    // Primary arrives at t=50ms (within grace) with matching data
    auto primary = make_event(Source::Primary, OrdStatus::Filled, 100, 5000,
                              ns_to_tsc(50'000'000), "ORD1", 1);
    reconciler.process_event_for_test(primary);

    // No divergence - mismatch resolved, false positive avoided
    EXPECT_TRUE(drain_divergences(*divergence_ring).empty());
    EXPECT_EQ(counters_.orders_matched, 1u);
    EXPECT_EQ(counters_.false_positive_avoided, 1u);
    EXPECT_EQ(os->recon_state, ReconState::Matched);
}

// ============================================================================
// Test 3: Mismatch detected, resolves within grace → false positive avoided
// ============================================================================
TEST_F(ReconcilerWindowedTest, MismatchResolves_WithinGrace_FalsePositiveAvoided) {
    ReconConfig config;
    config.grace_period_ns = 200'000'000;  // 200ms
    config.enable_windowed_recon = true;

    util::Arena arena{util::Arena::default_capacity_bytes};
    OrderStateStore store{arena, 1024};
    auto primary_ring = std::make_unique<ingest::SpscRing<ExecEvent, 1u << 16>>();
    auto dropcopy_ring = std::make_unique<ingest::SpscRing<ExecEvent, 1u << 16>>();
    auto divergence_ring = std::make_unique<DivergenceRing>();
    auto seq_gap_ring = std::make_unique<SequenceGapRing>();
    auto timer_wheel = std::make_unique<util::WheelTimer>(0);

    Reconciler reconciler(stop_flag_, *primary_ring, *dropcopy_ring, store,
                          counters_, *divergence_ring, *seq_gap_ring,
                          timer_wheel.get(), config);

    // Primary at t=0 with qty=100
    auto primary = make_event(Source::Primary, OrdStatus::Working, 100, 5000,
                              ns_to_tsc(0), "ORD1", 1, "EX1");
    reconciler.process_event_for_test(primary);

    // Dropcopy at t=10ms with DIFFERENT qty=50 (mismatch!)
    auto dropcopy1 = make_event(Source::DropCopy, OrdStatus::Working, 50, 5000,
                                ns_to_tsc(10'000'000), "ORD1", 1, "EX1");
    reconciler.process_event_for_test(dropcopy1);

    // Should be in grace period, no divergence yet
    EXPECT_TRUE(drain_divergences(*divergence_ring).empty());
    EXPECT_EQ(counters_.mismatch_observed, 1u);

    OrderKey key = make_order_key(primary);
    OrderState* os = store.find(key);
    ASSERT_NE(os, nullptr);
    EXPECT_EQ(os->recon_state, ReconState::InGrace);

    // Corrected dropcopy at t=50ms with matching qty=100
    auto dropcopy2 = make_event(Source::DropCopy, OrdStatus::Working, 100, 5000,
                                ns_to_tsc(50'000'000), "ORD1", 2, "EX1");
    reconciler.process_event_for_test(dropcopy2);

    // Mismatch resolved - false positive avoided
    EXPECT_TRUE(drain_divergences(*divergence_ring).empty());
    EXPECT_EQ(counters_.false_positive_avoided, 1u);
    EXPECT_EQ(counters_.orders_matched, 1u);
    EXPECT_EQ(os->recon_state, ReconState::Matched);
}

// ============================================================================
// Test 4: Mismatch persists through grace → confirmed divergence
// ============================================================================
TEST_F(ReconcilerWindowedTest, MismatchPersists_ConfirmedDivergence) {
    ReconConfig config;
    config.grace_period_ns = 100'000'000;  // 100ms
    config.enable_windowed_recon = true;

    util::Arena arena{util::Arena::default_capacity_bytes};
    OrderStateStore store{arena, 1024};
    auto primary_ring = std::make_unique<ingest::SpscRing<ExecEvent, 1u << 16>>();
    auto dropcopy_ring = std::make_unique<ingest::SpscRing<ExecEvent, 1u << 16>>();
    auto divergence_ring = std::make_unique<DivergenceRing>();
    auto seq_gap_ring = std::make_unique<SequenceGapRing>();
    auto timer_wheel = std::make_unique<util::WheelTimer>(0);

    Reconciler reconciler(stop_flag_, *primary_ring, *dropcopy_ring, store,
                          counters_, *divergence_ring, *seq_gap_ring,
                          timer_wheel.get(), config);

    // Primary at t=0 with qty=100
    auto primary = make_event(Source::Primary, OrdStatus::Working, 100, 5000,
                              ns_to_tsc(0), "ORD1", 1, "EX1");
    reconciler.process_event_for_test(primary);

    // Dropcopy at t=10ms with qty=200 (mismatch)
    auto dropcopy = make_event(Source::DropCopy, OrdStatus::Working, 200, 5000,
                               ns_to_tsc(10'000'000), "ORD1", 1, "EX1");
    reconciler.process_event_for_test(dropcopy);

    // In grace period
    EXPECT_EQ(counters_.mismatch_observed, 1u);
    EXPECT_TRUE(drain_divergences(*divergence_ring).empty());

    OrderKey key = make_order_key(primary);
    OrderState* os = store.find(key);
    ASSERT_NE(os, nullptr);
    EXPECT_EQ(os->recon_state, ReconState::InGrace);

    // Advance time past grace (t=200ms, no correction arrives)
    timer_wheel->poll_expired(ns_to_tsc(200'000'000), [&](OrderKey k, std::uint32_t g) {
        reconciler.on_grace_deadline_expired(k, g);
    });

    // Divergence confirmed
    auto divs = drain_divergences(*divergence_ring);
    ASSERT_EQ(divs.size(), 1u);
    EXPECT_EQ(divs[0].type, DivergenceType::QuantityMismatch);
    EXPECT_EQ(counters_.mismatch_confirmed, 1u);
    EXPECT_EQ(os->recon_state, ReconState::DivergedConfirmed);
}

// ============================================================================
// Test 5: Primary never arrives (phantom order) → divergence after grace
// ============================================================================
TEST_F(ReconcilerWindowedTest, PrimaryNeverArrives_PhantomOrder) {
    ReconConfig config;
    config.grace_period_ns = 100'000'000;  // 100ms
    config.enable_windowed_recon = true;

    util::Arena arena{util::Arena::default_capacity_bytes};
    OrderStateStore store{arena, 1024};
    auto primary_ring = std::make_unique<ingest::SpscRing<ExecEvent, 1u << 16>>();
    auto dropcopy_ring = std::make_unique<ingest::SpscRing<ExecEvent, 1u << 16>>();
    auto divergence_ring = std::make_unique<DivergenceRing>();
    auto seq_gap_ring = std::make_unique<SequenceGapRing>();
    auto timer_wheel = std::make_unique<util::WheelTimer>(0);

    Reconciler reconciler(stop_flag_, *primary_ring, *dropcopy_ring, store,
                          counters_, *divergence_ring, *seq_gap_ring,
                          timer_wheel.get(), config);

    // Only DropCopy arrives at t=0
    auto dropcopy = make_event(Source::DropCopy, OrdStatus::Filled, 100, 5000,
                               ns_to_tsc(0), "ORD1", 1);
    reconciler.process_event_for_test(dropcopy);

    // No divergence yet - timer scheduled, state should be InGrace
    EXPECT_TRUE(drain_divergences(*divergence_ring).empty());

    // Verify order state is InGrace (timer scheduled for EXISTENCE mismatch)
    OrderKey key = make_order_key(dropcopy);
    OrderState* os = store.find(key);
    ASSERT_NE(os, nullptr);
    EXPECT_EQ(os->recon_state, ReconState::InGrace);
    EXPECT_FALSE(os->seen_internal);
    EXPECT_TRUE(os->seen_dropcopy);
    EXPECT_EQ(counters_.mismatch_observed, 1u);

    // Advance time past grace period - primary never arrives
    timer_wheel->poll_expired(ns_to_tsc(200'000'000), [&](OrderKey k, std::uint32_t g) {
        reconciler.on_grace_deadline_expired(k, g);
    });

    // Divergence should be emitted (PhantomOrder - dropcopy seen, primary not)
    auto divs = drain_divergences(*divergence_ring);
    ASSERT_EQ(divs.size(), 1u);
    EXPECT_EQ(divs[0].type, DivergenceType::PhantomOrder);
    EXPECT_EQ(counters_.mismatch_confirmed, 1u);
    EXPECT_EQ(os->recon_state, ReconState::DivergedConfirmed);
}

// ============================================================================
// Test 6: Dropcopy never arrives (missing dropcopy)
// ============================================================================
TEST_F(ReconcilerWindowedTest, DropcopyNeverArrives_MissingDropCopy) {
    ReconConfig config;
    config.grace_period_ns = 100'000'000;  // 100ms
    config.enable_windowed_recon = true;

    util::Arena arena{util::Arena::default_capacity_bytes};
    OrderStateStore store{arena, 1024};
    auto primary_ring = std::make_unique<ingest::SpscRing<ExecEvent, 1u << 16>>();
    auto dropcopy_ring = std::make_unique<ingest::SpscRing<ExecEvent, 1u << 16>>();
    auto divergence_ring = std::make_unique<DivergenceRing>();
    auto seq_gap_ring = std::make_unique<SequenceGapRing>();
    auto timer_wheel = std::make_unique<util::WheelTimer>(0);

    Reconciler reconciler(stop_flag_, *primary_ring, *dropcopy_ring, store,
                          counters_, *divergence_ring, *seq_gap_ring,
                          timer_wheel.get(), config);

    // Only Primary arrives at t=0
    auto primary = make_event(Source::Primary, OrdStatus::Filled, 100, 5000,
                              ns_to_tsc(0), "ORD1", 1);
    reconciler.process_event_for_test(primary);

    // Verify order state is InGrace (timer scheduled for EXISTENCE mismatch)
    OrderKey key = make_order_key(primary);
    OrderState* os = store.find(key);
    ASSERT_NE(os, nullptr);
    EXPECT_EQ(os->recon_state, ReconState::InGrace);
    EXPECT_TRUE(os->seen_internal);
    EXPECT_FALSE(os->seen_dropcopy);
    EXPECT_EQ(counters_.mismatch_observed, 1u);

    // No divergence yet (timer scheduled, waiting for dropcopy or expiration)
    EXPECT_TRUE(drain_divergences(*divergence_ring).empty());

    // Advance time past grace period - dropcopy never arrives
    timer_wheel->poll_expired(ns_to_tsc(200'000'000), [&](OrderKey k, std::uint32_t g) {
        reconciler.on_grace_deadline_expired(k, g);
    });

    // Divergence should be emitted (MissingDropCopy - primary seen, dropcopy not)
    auto divs = drain_divergences(*divergence_ring);
    ASSERT_EQ(divs.size(), 1u);
    EXPECT_EQ(divs[0].type, DivergenceType::MissingDropCopy);
    EXPECT_EQ(counters_.mismatch_confirmed, 1u);
    EXPECT_EQ(os->recon_state, ReconState::DivergedConfirmed);
}

// ============================================================================
// Test 7: Deduplication prevents flooding
// ============================================================================
TEST_F(ReconcilerWindowedTest, Deduplication_PreventsFlooding) {
    ReconConfig config;
    config.divergence_dedup_window_ns = 1'000'000'000;  // 1s
    config.grace_period_ns = 10'000'000;  // 10ms (short for test)
    config.enable_windowed_recon = true;

    util::Arena arena{util::Arena::default_capacity_bytes};
    OrderStateStore store{arena, 1024};
    auto primary_ring = std::make_unique<ingest::SpscRing<ExecEvent, 1u << 16>>();
    auto dropcopy_ring = std::make_unique<ingest::SpscRing<ExecEvent, 1u << 16>>();
    auto divergence_ring = std::make_unique<DivergenceRing>();
    auto seq_gap_ring = std::make_unique<SequenceGapRing>();
    auto timer_wheel = std::make_unique<util::WheelTimer>(0);

    Reconciler reconciler(stop_flag_, *primary_ring, *dropcopy_ring, store,
                          counters_, *divergence_ring, *seq_gap_ring,
                          timer_wheel.get(), config);

    // Create initial mismatch
    auto primary = make_event(Source::Primary, OrdStatus::Working, 100, 5000,
                              ns_to_tsc(0), "ORD1", 1, "EX1");
    reconciler.process_event_for_test(primary);

    auto dropcopy = make_event(Source::DropCopy, OrdStatus::Working, 200, 5000,
                               ns_to_tsc(1'000'000), "ORD1", 1, "EX1");
    reconciler.process_event_for_test(dropcopy);

    OrderKey key = make_order_key(primary);
    OrderState* os = store.find(key);
    ASSERT_NE(os, nullptr);
    EXPECT_EQ(os->recon_state, ReconState::InGrace);

    // Directly call emit_confirmed_divergence twice with proper timestamps to test deduplication
    // First emit at t=50ms
    MismatchMask mismatch{};
    mismatch.set(MismatchMask::CUM_QTY);
    const std::uint64_t first_emit_tsc = ns_to_tsc(50'000'000);
    reconciler.emit_confirmed_divergence(*os, mismatch, first_emit_tsc);

    auto divs = drain_divergences(*divergence_ring);
    ASSERT_EQ(divs.size(), 1u);

    // Second emit at t=100ms (50ms later, within 1s dedup window)
    // Same mismatch should be deduped
    const std::uint64_t second_emit_tsc = ns_to_tsc(100'000'000);
    reconciler.emit_confirmed_divergence(*os, mismatch, second_emit_tsc);

    // Verify dedup counter increased and no new divergence was emitted
    auto divs2 = drain_divergences(*divergence_ring);
    EXPECT_EQ(divs2.size(), 0u);
    EXPECT_GE(counters_.divergence_deduped, 1u);

    // Third emit at t=2s (1.95s later, outside 1s dedup window)
    // Same mismatch should NOT be deduped (enough time has passed)
    const std::uint64_t third_emit_tsc = ns_to_tsc(2'000'000'000);
    reconciler.emit_confirmed_divergence(*os, mismatch, third_emit_tsc);

    auto divs3 = drain_divergences(*divergence_ring);
    EXPECT_EQ(divs3.size(), 1u);  // Should emit since outside window
}

// ============================================================================
// Test 8: Stale timer skipped (generation mismatch)
// ============================================================================
TEST_F(ReconcilerWindowedTest, StaleTimer_Skipped) {
    ReconConfig config;
    config.grace_period_ns = 100'000'000;  // 100ms
    config.enable_windowed_recon = true;

    util::Arena arena{util::Arena::default_capacity_bytes};
    OrderStateStore store{arena, 1024};
    auto primary_ring = std::make_unique<ingest::SpscRing<ExecEvent, 1u << 16>>();
    auto dropcopy_ring = std::make_unique<ingest::SpscRing<ExecEvent, 1u << 16>>();
    auto divergence_ring = std::make_unique<DivergenceRing>();
    auto seq_gap_ring = std::make_unique<SequenceGapRing>();
    auto timer_wheel = std::make_unique<util::WheelTimer>(0);

    Reconciler reconciler(stop_flag_, *primary_ring, *dropcopy_ring, store,
                          counters_, *divergence_ring, *seq_gap_ring,
                          timer_wheel.get(), config);

    // Create mismatch, enter grace
    auto primary = make_event(Source::Primary, OrdStatus::Working, 100, 5000,
                              ns_to_tsc(0), "ORD1", 1, "EX1");
    reconciler.process_event_for_test(primary);

    auto dropcopy1 = make_event(Source::DropCopy, OrdStatus::Working, 200, 5000,
                                ns_to_tsc(10'000'000), "ORD1", 1, "EX1");
    reconciler.process_event_for_test(dropcopy1);

    EXPECT_EQ(counters_.mismatch_observed, 1u);

    OrderKey key = make_order_key(primary);
    OrderState* os = store.find(key);
    ASSERT_NE(os, nullptr);
    EXPECT_EQ(os->recon_state, ReconState::InGrace);

    // Save the timer generation before resolution
    std::uint32_t old_gen = os->timer_generation;

    // Resolve mismatch before timer fires (this increments generation via cancel)
    auto dropcopy2 = make_event(Source::DropCopy, OrdStatus::Working, 100, 5000,
                                ns_to_tsc(50'000'000), "ORD1", 2, "EX1");
    reconciler.process_event_for_test(dropcopy2);

    EXPECT_EQ(counters_.false_positive_avoided, 1u);
    EXPECT_EQ(os->recon_state, ReconState::Matched);

    // Generation should have incremented
    EXPECT_GT(os->timer_generation, old_gen);

    // Now simulate timer firing with old generation
    reconciler.on_grace_deadline_expired(key, old_gen);

    // Timer should be skipped (stale generation)
    EXPECT_TRUE(drain_divergences(*divergence_ring).empty());
    EXPECT_EQ(counters_.stale_timers_skipped, 1u);
}

// ============================================================================
// Test 9: Legacy mode (windowed recon disabled)
// ============================================================================
TEST_F(ReconcilerWindowedTest, LegacyMode_ImmediateEmission) {
    ReconConfig config;
    config.enable_windowed_recon = false;  // Disable windowed recon

    util::Arena arena{util::Arena::default_capacity_bytes};
    OrderStateStore store{arena, 1024};
    auto primary_ring = std::make_unique<ingest::SpscRing<ExecEvent, 1u << 16>>();
    auto dropcopy_ring = std::make_unique<ingest::SpscRing<ExecEvent, 1u << 16>>();
    auto divergence_ring = std::make_unique<DivergenceRing>();
    auto seq_gap_ring = std::make_unique<SequenceGapRing>();

    // Create reconciler with nullptr timer wheel (legacy mode)
    Reconciler reconciler(stop_flag_, *primary_ring, *dropcopy_ring, store,
                          counters_, *divergence_ring, *seq_gap_ring,
                          nullptr, config);  // nullptr timer wheel

    auto primary = make_event(Source::Primary, OrdStatus::Filled, 100, 5000,
                              ns_to_tsc(0), "ORD1", 1);
    reconciler.process_event_for_test(primary);

    auto dropcopy = make_event(Source::DropCopy, OrdStatus::Working, 200, 5000,
                               ns_to_tsc(10'000'000), "ORD1", 1);
    reconciler.process_event_for_test(dropcopy);

    // Immediate emission (no grace period)
    auto divs = drain_divergences(*divergence_ring);
    EXPECT_EQ(divs.size(), 1u);
    EXPECT_EQ(counters_.mismatch_observed, 0u);  // Not tracked in legacy mode
}

// ============================================================================
// Test 10: Replay determinism - same inputs produce same outputs
// ============================================================================
TEST_F(ReconcilerWindowedTest, ReplayDeterminism) {
    ReconConfig config;
    config.grace_period_ns = 100'000'000;  // 100ms
    config.enable_windowed_recon = true;

    // Generate deterministic scenario
    std::vector<ExecEvent> events;
    events.push_back(make_event(Source::Primary, OrdStatus::Working, 100, 5000,
                                ns_to_tsc(0), "ORD1", 1, "EX1"));
    events.push_back(make_event(Source::DropCopy, OrdStatus::Working, 200, 5000,
                                ns_to_tsc(10'000'000), "ORD1", 1, "EX1"));

    auto run_scenario = [&]() -> std::vector<Divergence> {
        // Fresh state
        counters_ = ReconCounters{};
        util::Arena arena{util::Arena::default_capacity_bytes};
        OrderStateStore store{arena, 1024};
        auto primary_ring = std::make_unique<ingest::SpscRing<ExecEvent, 1u << 16>>();
        auto dropcopy_ring = std::make_unique<ingest::SpscRing<ExecEvent, 1u << 16>>();
        auto divergence_ring = std::make_unique<DivergenceRing>();
        auto seq_gap_ring = std::make_unique<SequenceGapRing>();
        auto timer_wheel = std::make_unique<util::WheelTimer>(0);

        Reconciler reconciler(stop_flag_, *primary_ring, *dropcopy_ring, store,
                              counters_, *divergence_ring, *seq_gap_ring,
                              timer_wheel.get(), config);

        for (const auto& ev : events) {
            reconciler.process_event_for_test(ev);
        }

        // Fire timers at deterministic time
        timer_wheel->poll_expired(ns_to_tsc(200'000'000), [&](OrderKey k, std::uint32_t g) {
            reconciler.on_grace_deadline_expired(k, g);
        });

        std::vector<Divergence> result;
        Divergence div;
        while (divergence_ring->try_pop(div)) {
            result.push_back(div);
        }
        return result;
    };

    // Run twice
    auto divs1 = run_scenario();
    auto divs2 = run_scenario();

    // Must produce identical output
    ASSERT_EQ(divs1.size(), divs2.size());
    for (std::size_t i = 0; i < divs1.size(); i++) {
        EXPECT_EQ(divs1[i].key, divs2[i].key);
        EXPECT_EQ(divs1[i].type, divs2[i].type);
        EXPECT_EQ(divs1[i].mismatch_mask, divs2[i].mismatch_mask);
    }
}

// ============================================================================
// Test 11: Gap suppression - divergence suppressed during open gap
// ============================================================================
TEST_F(ReconcilerWindowedTest, GapSuppression_DivergenceSuppressed) {
    ReconConfig config;
    config.grace_period_ns = 100'000'000;  // 100ms
    config.enable_gap_suppression = true;
    config.enable_windowed_recon = true;
    config.gap_recheck_period_ns = 50'000'000;  // 50ms recheck

    util::Arena arena{util::Arena::default_capacity_bytes};
    OrderStateStore store{arena, 1024};
    auto primary_ring = std::make_unique<ingest::SpscRing<ExecEvent, 1u << 16>>();
    auto dropcopy_ring = std::make_unique<ingest::SpscRing<ExecEvent, 1u << 16>>();
    auto divergence_ring = std::make_unique<DivergenceRing>();
    auto seq_gap_ring = std::make_unique<SequenceGapRing>();
    auto timer_wheel = std::make_unique<util::WheelTimer>(0);

    Reconciler reconciler(stop_flag_, *primary_ring, *dropcopy_ring, store,
                          counters_, *divergence_ring, *seq_gap_ring,
                          timer_wheel.get(), config);

    // First, initialize the dropcopy sequence tracker with seq=1
    // Note: This also triggers EXISTENCE mismatch for ORD_INIT (mismatch_observed=1)
    auto dropcopy_init = make_event(Source::DropCopy, OrdStatus::Working, 0, 5000,
                                    ns_to_tsc(0), "ORD_INIT", 1, "EX0");
    reconciler.process_event_for_test(dropcopy_init);

    // Now send primary event for a different order
    // This also triggers EXISTENCE mismatch (mismatch_observed=2)
    auto primary = make_event(Source::Primary, OrdStatus::Working, 100, 5000,
                              ns_to_tsc(5'000'000), "ORD1", 1, "EX1");
    reconciler.process_event_for_test(primary);

    // Send dropcopy with a sequence gap (skip seq=2, send seq=3)
    // This should trigger gap detection: expected=2, got=3
    auto dropcopy = make_event(Source::DropCopy, OrdStatus::Working, 50, 5000,
                               ns_to_tsc(10'000'000), "ORD1", 3, "EX1");  // Note seq=3
    reconciler.process_event_for_test(dropcopy);

    // Verify sequence gap was detected
    EXPECT_EQ(counters_.dropcopy_seq_gaps, 1u);

    // Verify mismatch was observed for BOTH orders (ORD_INIT and ORD1)
    // - ORD_INIT: EXISTENCE mismatch when only dropcopy seen
    // - ORD1: EXISTENCE mismatch when only primary seen (dropcopy arrives later)
    EXPECT_EQ(counters_.mismatch_observed, 2u);

    OrderKey key = make_order_key(primary);
    OrderState* os = store.find(key);
    ASSERT_NE(os, nullptr);
    EXPECT_EQ(os->recon_state, ReconState::InGrace);
    
    // The gap suppression epoch should be set
    EXPECT_GT(os->gap_suppression_epoch, 0u);
}

// ============================================================================
// Test 12: Timer overflow fallback - verify counter exists
// ============================================================================
TEST_F(ReconcilerWindowedTest, TimerOverflow_CounterExists) {
    // This test validates that the timer_overflow counter exists and is initialized to 0
    // Triggering actual overflow is difficult without filling the wheel buckets
    EXPECT_EQ(counters_.timer_overflow, 0u);

    // Create a reconciler and verify it doesn't immediately cause overflow
    ReconConfig config;
    config.grace_period_ns = 100'000'000;
    config.enable_windowed_recon = true;

    util::Arena arena{util::Arena::default_capacity_bytes};
    OrderStateStore store{arena, 1024};
    auto primary_ring = std::make_unique<ingest::SpscRing<ExecEvent, 1u << 16>>();
    auto dropcopy_ring = std::make_unique<ingest::SpscRing<ExecEvent, 1u << 16>>();
    auto divergence_ring = std::make_unique<DivergenceRing>();
    auto seq_gap_ring = std::make_unique<SequenceGapRing>();
    auto timer_wheel = std::make_unique<util::WheelTimer>(0);

    Reconciler reconciler(stop_flag_, *primary_ring, *dropcopy_ring, store,
                          counters_, *divergence_ring, *seq_gap_ring,
                          timer_wheel.get(), config);

    // Create a simple mismatch scenario
    auto primary = make_event(Source::Primary, OrdStatus::Working, 100, 5000,
                              ns_to_tsc(0), "ORD1", 1, "EX1");
    reconciler.process_event_for_test(primary);

    auto dropcopy = make_event(Source::DropCopy, OrdStatus::Working, 200, 5000,
                               ns_to_tsc(10'000'000), "ORD1", 1, "EX1");
    reconciler.process_event_for_test(dropcopy);

    // No overflow should occur with a single order
    EXPECT_EQ(counters_.timer_overflow, 0u);
}

// ============================================================================
// Additional Integration Tests
// ============================================================================

// Test: State transitions from DivergedConfirmed back to Matched when resolved
TEST_F(ReconcilerWindowedTest, DivergedConfirmed_ResolvesToMatched) {
    ReconConfig config;
    config.grace_period_ns = 10'000'000;  // 10ms
    config.enable_windowed_recon = true;

    util::Arena arena{util::Arena::default_capacity_bytes};
    OrderStateStore store{arena, 1024};
    auto primary_ring = std::make_unique<ingest::SpscRing<ExecEvent, 1u << 16>>();
    auto dropcopy_ring = std::make_unique<ingest::SpscRing<ExecEvent, 1u << 16>>();
    auto divergence_ring = std::make_unique<DivergenceRing>();
    auto seq_gap_ring = std::make_unique<SequenceGapRing>();
    auto timer_wheel = std::make_unique<util::WheelTimer>(0);

    Reconciler reconciler(stop_flag_, *primary_ring, *dropcopy_ring, store,
                          counters_, *divergence_ring, *seq_gap_ring,
                          timer_wheel.get(), config);

    // Create mismatch
    auto primary = make_event(Source::Primary, OrdStatus::Working, 100, 5000,
                              ns_to_tsc(0), "ORD1", 1, "EX1");
    reconciler.process_event_for_test(primary);

    auto dropcopy1 = make_event(Source::DropCopy, OrdStatus::Working, 200, 5000,
                                ns_to_tsc(5'000'000), "ORD1", 1, "EX1");
    reconciler.process_event_for_test(dropcopy1);

    OrderKey key = make_order_key(primary);
    OrderState* os = store.find(key);
    ASSERT_NE(os, nullptr);

    // Confirm divergence by expiring timer
    timer_wheel->poll_expired(ns_to_tsc(50'000'000), [&](OrderKey k, std::uint32_t g) {
        reconciler.on_grace_deadline_expired(k, g);
    });

    EXPECT_EQ(os->recon_state, ReconState::DivergedConfirmed);
    EXPECT_EQ(counters_.mismatch_confirmed, 1u);

    // Now dropcopy catches up
    auto dropcopy2 = make_event(Source::DropCopy, OrdStatus::Working, 100, 5000,
                                ns_to_tsc(100'000'000), "ORD1", 2, "EX1");
    reconciler.process_event_for_test(dropcopy2);

    // Should return to Matched
    EXPECT_EQ(os->recon_state, ReconState::Matched);
    EXPECT_EQ(counters_.divergence_resolved, 1u);
}

// Test: Matched state can re-enter grace on new mismatch
TEST_F(ReconcilerWindowedTest, Matched_ReentersGrace_OnNewMismatch) {
    ReconConfig config;
    config.grace_period_ns = 100'000'000;  // 100ms
    config.enable_windowed_recon = true;

    util::Arena arena{util::Arena::default_capacity_bytes};
    OrderStateStore store{arena, 1024};
    auto primary_ring = std::make_unique<ingest::SpscRing<ExecEvent, 1u << 16>>();
    auto dropcopy_ring = std::make_unique<ingest::SpscRing<ExecEvent, 1u << 16>>();
    auto divergence_ring = std::make_unique<DivergenceRing>();
    auto seq_gap_ring = std::make_unique<SequenceGapRing>();
    auto timer_wheel = std::make_unique<util::WheelTimer>(0);

    Reconciler reconciler(stop_flag_, *primary_ring, *dropcopy_ring, store,
                          counters_, *divergence_ring, *seq_gap_ring,
                          timer_wheel.get(), config);

    // Create matching scenario first
    auto primary1 = make_event(Source::Primary, OrdStatus::Working, 100, 5000,
                               ns_to_tsc(0), "ORD1", 1, "EX1");
    reconciler.process_event_for_test(primary1);

    auto dropcopy1 = make_event(Source::DropCopy, OrdStatus::Working, 100, 5000,
                                ns_to_tsc(5'000'000), "ORD1", 1, "EX1");
    reconciler.process_event_for_test(dropcopy1);

    OrderKey key = make_order_key(primary1);
    OrderState* os = store.find(key);
    ASSERT_NE(os, nullptr);
    EXPECT_EQ(os->recon_state, ReconState::Matched);
    EXPECT_EQ(counters_.orders_matched, 1u);

    // Now primary updates (partial fill) creating new mismatch
    auto primary2 = make_event(Source::Primary, OrdStatus::PartiallyFilled, 150, 5000,
                               ns_to_tsc(50'000'000), "ORD1", 2, "EX2");
    reconciler.process_event_for_test(primary2);

    // Should re-enter grace
    EXPECT_EQ(os->recon_state, ReconState::InGrace);
    // mismatch_observed=2: first for EXISTENCE (primary only), second for CUM_QTY mismatch
    EXPECT_EQ(counters_.mismatch_observed, 2u);
}

// Test: Timer wheel stats are tracked
TEST_F(ReconcilerWindowedTest, TimerWheelStats_Tracked) {
    ReconConfig config;
    config.grace_period_ns = 10'000'000;  // 10ms
    config.enable_windowed_recon = true;

    util::Arena arena{util::Arena::default_capacity_bytes};
    OrderStateStore store{arena, 1024};
    auto primary_ring = std::make_unique<ingest::SpscRing<ExecEvent, 1u << 16>>();
    auto dropcopy_ring = std::make_unique<ingest::SpscRing<ExecEvent, 1u << 16>>();
    auto divergence_ring = std::make_unique<DivergenceRing>();
    auto seq_gap_ring = std::make_unique<SequenceGapRing>();
    auto timer_wheel = std::make_unique<util::WheelTimer>(0);

    Reconciler reconciler(stop_flag_, *primary_ring, *dropcopy_ring, store,
                          counters_, *divergence_ring, *seq_gap_ring,
                          timer_wheel.get(), config);

    // Create a mismatch to schedule a timer
    auto primary = make_event(Source::Primary, OrdStatus::Working, 100, 5000,
                              ns_to_tsc(0), "ORD1", 1, "EX1");
    reconciler.process_event_for_test(primary);

    auto dropcopy = make_event(Source::DropCopy, OrdStatus::Working, 200, 5000,
                               ns_to_tsc(1'000'000), "ORD1", 1, "EX1");
    reconciler.process_event_for_test(dropcopy);

    // Check timer was scheduled
    const auto& stats = timer_wheel->stats();
    EXPECT_GE(stats.scheduled, 1u);

    // Fire timer
    timer_wheel->poll_expired(ns_to_tsc(50'000'000), [&](OrderKey k, std::uint32_t g) {
        reconciler.on_grace_deadline_expired(k, g);
    });

    // Check timer expired
    EXPECT_GE(stats.expired, 1u);
}

// ============================================================================
// Test: One side arrives first, other arrives within grace → NO divergence
// ============================================================================
TEST_F(ReconcilerWindowedTest, OneSideFirst_OtherArrivesWithinGrace_NoDivergence) {
    ReconConfig config;
    config.grace_period_ns = 200'000'000;  // 200ms
    config.enable_windowed_recon = true;

    util::Arena arena{util::Arena::default_capacity_bytes};
    OrderStateStore store{arena, 1024};
    auto primary_ring = std::make_unique<ingest::SpscRing<ExecEvent, 1u << 16>>();
    auto dropcopy_ring = std::make_unique<ingest::SpscRing<ExecEvent, 1u << 16>>();
    auto divergence_ring = std::make_unique<DivergenceRing>();
    auto seq_gap_ring = std::make_unique<SequenceGapRing>();
    auto timer_wheel = std::make_unique<util::WheelTimer>(0);

    Reconciler reconciler(stop_flag_, *primary_ring, *dropcopy_ring, store,
                          counters_, *divergence_ring, *seq_gap_ring,
                          timer_wheel.get(), config);

    // Primary arrives first at t=0
    auto primary = make_event(Source::Primary, OrdStatus::Filled, 100, 5000,
                              ns_to_tsc(0), "ORD1", 1);
    reconciler.process_event_for_test(primary);

    // Should be in grace (EXISTENCE mismatch)
    OrderKey key = make_order_key(primary);
    OrderState* os = store.find(key);
    ASSERT_NE(os, nullptr);
    EXPECT_EQ(os->recon_state, ReconState::InGrace);
    EXPECT_EQ(counters_.mismatch_observed, 1u);

    // Dropcopy arrives at t=50ms (within 200ms grace) with matching data
    auto dropcopy = make_event(Source::DropCopy, OrdStatus::Filled, 100, 5000,
                               ns_to_tsc(50'000'000), "ORD1", 1);
    reconciler.process_event_for_test(dropcopy);

    // Mismatch resolved - false positive avoided!
    EXPECT_TRUE(drain_divergences(*divergence_ring).empty());
    EXPECT_EQ(os->recon_state, ReconState::Matched);
    EXPECT_EQ(counters_.false_positive_avoided, 1u);
    EXPECT_EQ(counters_.orders_matched, 1u);
}

// ============================================================================
// Test: Gap closes after timeout
// ============================================================================
TEST_F(ReconcilerWindowedTest, GapClosesAfterTimeout) {
    ReconConfig config;
    config.grace_period_ns = 50'000'000;   // 50ms
    config.gap_close_timeout_ns = 100'000'000;  // 100ms gap timeout
    config.enable_windowed_recon = true;
    config.enable_gap_suppression = true;

    util::Arena arena{util::Arena::default_capacity_bytes};
    OrderStateStore store{arena, 1024};
    auto primary_ring = std::make_unique<ingest::SpscRing<ExecEvent, 1u << 16>>();
    auto dropcopy_ring = std::make_unique<ingest::SpscRing<ExecEvent, 1u << 16>>();
    auto divergence_ring = std::make_unique<DivergenceRing>();
    auto seq_gap_ring = std::make_unique<SequenceGapRing>();
    auto timer_wheel = std::make_unique<util::WheelTimer>(0);

    Reconciler reconciler(stop_flag_, *primary_ring, *dropcopy_ring, store,
                          counters_, *divergence_ring, *seq_gap_ring,
                          timer_wheel.get(), config);

    // Initialize dropcopy sequence tracker with seq=1
    // Note: This triggers EXISTENCE mismatch for ORD_INIT (enters grace)
    auto dropcopy_init = make_event(Source::DropCopy, OrdStatus::Working, 0, 5000,
                                    ns_to_tsc(0), "ORD_INIT", 1, "EX0");
    reconciler.process_event_for_test(dropcopy_init);

    // Create primary event (triggers EXISTENCE mismatch for ORD1)
    auto primary = make_event(Source::Primary, OrdStatus::Working, 100, 5000,
                              ns_to_tsc(5'000'000), "ORD1", 1, "EX1");
    reconciler.process_event_for_test(primary);

    // Create dropcopy with gap (skip seq=2, send seq=3)
    // ORD1 now has both sides with qty mismatch (100 vs 50)
    auto dropcopy = make_event(Source::DropCopy, OrdStatus::Working, 50, 5000,
                               ns_to_tsc(10'000'000), "ORD1", 3, "EX1");
    reconciler.process_event_for_test(dropcopy);

    // Gap should be detected
    EXPECT_EQ(counters_.dropcopy_seq_gaps, 1u);

    // Now advance time past gap_close_timeout (100ms) and poll timer
    // This will process timers for both ORD_INIT and ORD1:
    // - ORD_INIT timer fires, checks is_gap_suppressed, gap times out
    // - ORD1 timer fires, checks is_gap_suppressed, gap already closed
    timer_wheel->poll_expired(ns_to_tsc(200'000'000), [&](OrderKey k, std::uint32_t g) {
        reconciler.on_grace_deadline_expired(k, g);
    });

    // Gap should be closed by timeout
    EXPECT_GE(counters_.gaps_closed_by_timeout, 1u);
}

// ============================================================================
// Test: Gap closes when out-of-order message arrives
// ============================================================================
TEST_F(ReconcilerWindowedTest, GapClosesOnOutOfOrderMessage) {
    ReconConfig config;
    config.grace_period_ns = 500'000'000;  // 500ms
    config.gap_close_timeout_ns = 1'000'000'000;  // 1s (long, so timeout doesn't trigger)
    config.enable_windowed_recon = true;
    config.enable_gap_suppression = true;

    util::Arena arena{util::Arena::default_capacity_bytes};
    OrderStateStore store{arena, 1024};
    auto primary_ring = std::make_unique<ingest::SpscRing<ExecEvent, 1u << 16>>();
    auto dropcopy_ring = std::make_unique<ingest::SpscRing<ExecEvent, 1u << 16>>();
    auto divergence_ring = std::make_unique<DivergenceRing>();
    auto seq_gap_ring = std::make_unique<SequenceGapRing>();
    auto timer_wheel = std::make_unique<util::WheelTimer>(0);

    Reconciler reconciler(stop_flag_, *primary_ring, *dropcopy_ring, store,
                          counters_, *divergence_ring, *seq_gap_ring,
                          timer_wheel.get(), config);

    // Initialize dropcopy sequence tracker with seq=1
    // Note: ORD_INIT enters grace with EXISTENCE mismatch (only dropcopy seen)
    auto dropcopy_init = make_event(Source::DropCopy, OrdStatus::Working, 0, 5000,
                                    ns_to_tsc(0), "ORD_INIT", 1, "EX0");
    reconciler.process_event_for_test(dropcopy_init);

    // Create gap: receive seq=3 (missing seq=2)
    // ORD2 enters grace with EXISTENCE mismatch (only dropcopy seen)
    auto dropcopy_gap = make_event(Source::DropCopy, OrdStatus::Working, 100, 5000,
                                   ns_to_tsc(10'000'000), "ORD2", 3, "EX2");
    reconciler.process_event_for_test(dropcopy_gap);

    // Gap detected
    EXPECT_EQ(counters_.dropcopy_seq_gaps, 1u);

    // Now the missing message arrives (seq=2, out of order)
    // ORD3 enters grace with EXISTENCE mismatch
    // This also closes the gap in the sequence tracker
    auto dropcopy_fill = make_event(Source::DropCopy, OrdStatus::Working, 50, 5000,
                                    ns_to_tsc(20'000'000), "ORD3", 2, "EX3");
    reconciler.process_event_for_test(dropcopy_fill);

    // Gap should be closed by the out-of-order message
    EXPECT_EQ(counters_.dropcopy_seq_out_of_order, 1u);
    EXPECT_EQ(counters_.gaps_closed_by_fill, 1u);
    // The sequence tracker's gap_open should now be false (gap closed)
}

} // namespace test
} // namespace core
