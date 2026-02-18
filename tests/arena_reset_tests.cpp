#include <gtest/gtest.h>

#include <string>

#include "core/order_state_store.hpp"
#include "core/order_state.hpp"
#include "core/recon_state.hpp"

namespace {

// OrderState is approximately 256 bytes (as documented in FX-7064 problem statement)
constexpr std::size_t EXPECTED_ORDER_SIZE_BYTES = 256;

core::ExecEvent make_test_event(const std::string& clord_id) {
    core::ExecEvent evt{};
    evt.source = core::Source::Primary;
    evt.set_clord_id(clord_id.data(), clord_id.size());
    evt.status = core::OrdStatus::PartiallyFilled;
    evt.cum_qty = 100;
    evt.avg_px = 12345000;
    evt.ingest_tsc = 1000;
    return evt;
}

class ArenaResetTest : public ::testing::Test {
protected:
    util::Arena arena_{1 << 20};  // 1 MB arena
};

// Test: Safe reset after all orders are recycled
TEST_F(ArenaResetTest, SafeResetAfterAllOrdersRecycled) {
    core::OrderStateStore store(arena_, 1024);
    
    // Allocate 100 orders
    std::vector<core::OrderKey> keys;
    for (int i = 0; i < 100; ++i) {
        auto ev = make_test_event("ORDER" + std::to_string(i));
        auto* os = store.upsert(ev);
        ASSERT_NE(os, nullptr);
        keys.push_back(core::make_order_key(ev));
    }
    
    EXPECT_GT(store.arena_bytes_used(), 100 * EXPECTED_ORDER_SIZE_BYTES);  // At least 256 bytes × 100
    EXPECT_EQ(store.size(), 100);
    
    // Recycle all orders
    for (const auto& key : keys) {
        store.recycle(key);
    }
    
    EXPECT_EQ(store.size(), 0);
    EXPECT_EQ(store.recycled_count(), 100);
    
    // Should be safe to reset now (no active orders)
    EXPECT_TRUE(store.can_reset_arena());
    store.reset_arena_if_safe();
    
    // Arena should be reset to zero offset
    EXPECT_EQ(store.arena_bytes_used(), 0);
}

// Test: Defer reset when orders are in grace period
TEST_F(ArenaResetTest, DeferResetWhenOrdersInGrace) {
    core::OrderStateStore store(arena_, 1024);
    
    // Create order in grace period
    auto ev = make_test_event("ACTIVE_ORDER");
    auto* os = store.upsert(ev);
    ASSERT_NE(os, nullptr);
    os->recon_state = core::ReconState::InGrace;
    os->seen_internal = true;
    os->seen_dropcopy = true;
    
    // Should NOT be safe to reset (order in grace period)
    EXPECT_FALSE(store.can_reset_arena());
}

// Test: Defer reset when orders have gap uncertainty flags
TEST_F(ArenaResetTest, DeferResetWhenOrdersHaveGapFlags) {
    core::OrderStateStore store(arena_, 1024);
    
    // Create order with gap uncertainty
    auto ev = make_test_event("GAP_ORDER");
    auto* os = store.upsert(ev);
    ASSERT_NE(os, nullptr);
    os->recon_state = core::ReconState::Matched;  // Terminal state
    os->gap_uncertainty_flags = 1;  // Active gap uncertainty
    
    // Should NOT be safe to reset (order has gap uncertainty)
    EXPECT_FALSE(store.can_reset_arena());
}

// Test: Defer reset when orders are not in terminal state
TEST_F(ArenaResetTest, DeferResetWhenOrdersNotTerminal) {
    core::OrderStateStore store(arena_, 1024);
    
    // Create order awaiting dropcopy
    auto ev = make_test_event("AWAITING_ORDER");
    auto* os = store.upsert(ev);
    ASSERT_NE(os, nullptr);
    os->recon_state = core::ReconState::AwaitingDropCopy;
    os->seen_internal = true;
    os->seen_dropcopy = false;
    
    // Should NOT be safe to reset (order not in terminal state)
    EXPECT_FALSE(store.can_reset_arena());
}

// Test: Safe reset when all orders are recycled (even if they were in terminal state)
TEST_F(ArenaResetTest, SafeResetWhenOrdersRecycledAfterTerminal) {
    core::OrderStateStore store(arena_, 1024);
    
    // Create orders in terminal states
    auto ev1 = make_test_event("MATCHED_ORDER");
    auto* os1 = store.upsert(ev1);
    ASSERT_NE(os1, nullptr);
    os1->recon_state = core::ReconState::Matched;  // Terminal
    os1->gap_uncertainty_flags = 0;
    auto key1 = core::make_order_key(ev1);
    
    auto ev2 = make_test_event("DIVERGED_ORDER");
    auto* os2 = store.upsert(ev2);
    ASSERT_NE(os2, nullptr);
    os2->recon_state = core::ReconState::DivergedConfirmed;  // Terminal
    os2->gap_uncertainty_flags = 0;
    auto key2 = core::make_order_key(ev2);
    
    EXPECT_GT(store.arena_bytes_used(), 0);
    
    // Should NOT be safe to reset yet (orders still in hash table)
    EXPECT_FALSE(store.can_reset_arena());
    
    // Recycle the terminal orders
    store.recycle(key1);
    store.recycle(key2);
    
    // NOW it should be safe to reset (all orders recycled)
    EXPECT_TRUE(store.can_reset_arena());
    store.reset_arena_if_safe();
    
    // Arena should be reset
    EXPECT_EQ(store.arena_bytes_used(), 0);
}

// Test: Arena reset deferred when any orders remain in hash table
TEST_F(ArenaResetTest, DeferResetWhenOrdersNotRecycled) {
    core::OrderStateStore store(arena_, 1024);
    
    // Create order
    auto ev1 = make_test_event("ORDER1");
    auto* os1 = store.upsert(ev1);
    ASSERT_NE(os1, nullptr);
    os1->recon_state = core::ReconState::Matched;
    os1->gap_uncertainty_flags = 0;
    
    // Should NOT be safe to reset (order still in hash table, not recycled)
    EXPECT_FALSE(store.can_reset_arena());
    
    // Even if we add more orders
    auto ev2 = make_test_event("ORDER2");
    auto* os2 = store.upsert(ev2);
    ASSERT_NE(os2, nullptr);
    os2->recon_state = core::ReconState::InGrace;
    
    // Still not safe
    EXPECT_FALSE(store.can_reset_arena());
}

// Test: Arena memory reuse after reset
TEST_F(ArenaResetTest, ArenaMemoryReuseAfterReset) {
    core::OrderStateStore store(arena_, 1024);
    
    // Allocate and recycle 50 orders
    std::vector<core::OrderKey> keys;
    for (int i = 0; i < 50; ++i) {
        auto ev = make_test_event("ORDER" + std::to_string(i));
        auto* os = store.upsert(ev);
        ASSERT_NE(os, nullptr);
        keys.push_back(core::make_order_key(ev));
    }
    
    const std::size_t bytes_before = store.arena_bytes_used();
    EXPECT_GT(bytes_before, 50 * EXPECTED_ORDER_SIZE_BYTES);  // At least 256 bytes × 50
    
    // Recycle all orders
    for (const auto& key : keys) {
        store.recycle(key);
    }
    
    // Reset arena (all orders recycled)
    EXPECT_TRUE(store.can_reset_arena());
    store.reset_arena_if_safe();
    EXPECT_EQ(store.arena_bytes_used(), 0);
    
    // Allocate 50 more orders - should reuse arena memory
    for (int i = 50; i < 100; ++i) {
        auto ev = make_test_event("ORDER" + std::to_string(i));
        auto* os = store.upsert(ev);
        ASSERT_NE(os, nullptr);
    }
    
    const std::size_t bytes_after = store.arena_bytes_used();
    // Should use similar amount of memory as before
    EXPECT_GT(bytes_after, 12800);
    EXPECT_LE(bytes_after, bytes_before + 1000);  // Allow small overhead
}

} // namespace
