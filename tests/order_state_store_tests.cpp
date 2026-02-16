#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include "core/order_state_store.hpp"

namespace {

core::ExecEvent make_event(const std::string& cid) {
    core::ExecEvent evt{};
    evt.set_clord_id(cid.data(), cid.size());
    return evt;
}

class OrderStateStoreTest : public ::testing::Test {
protected:
    util::Arena arena_{1 << 20};
};

TEST_F(OrderStateStoreTest, BasicInsertFind) {
    core::OrderStateStore store(arena_, 128);

    std::vector<core::ExecEvent> events;
    for (int i = 0; i < 10; ++i) {
        events.push_back(make_event("CID" + std::to_string(i)));
    }

    for (const auto& ev : events) {
        core::OrderState* first = store.upsert(ev);
        ASSERT_NE(first, nullptr) << "Failed to insert event with cid " << std::string_view(ev.clord_id, ev.clord_id_len);

        core::OrderState* second = store.upsert(ev);
        EXPECT_EQ(first, second) << "Expected upsert to return existing state for cid "
                                 << std::string_view(ev.clord_id, ev.clord_id_len);

        EXPECT_EQ(store.find(core::make_order_key(ev)), first);
    }

    EXPECT_EQ(store.size(), events.size());
}

TEST_F(OrderStateStoreTest, CollisionHandling) {
    core::OrderStateStore store(arena_, 4);
    const std::size_t mask = store.bucket_count() - 1;

    std::vector<bool> bucket_seen(store.bucket_count(), false);
    std::vector<core::ExecEvent> bucket_sample(store.bucket_count());
    core::ExecEvent first{};
    core::ExecEvent second{};
    bool found = false;

    for (int i = 0; i < 2000 && !found; ++i) {
        const auto ev = make_event("COLL" + std::to_string(i));
        const auto key = core::make_order_key(ev);
        const auto bucket = key & mask;
        if (!bucket_seen[bucket]) {
            bucket_seen[bucket] = true;
            bucket_sample[bucket] = ev;
        } else if (core::make_order_key(bucket_sample[bucket]) != key) {
            first = bucket_sample[bucket];
            second = ev;
            found = true;
        }
    }

    ASSERT_TRUE(found) << "Unable to synthesize two distinct keys mapping to same bucket";

    auto* s1 = store.upsert(first);
    auto* s2 = store.upsert(second);
    ASSERT_NE(s1, nullptr);
    ASSERT_NE(s2, nullptr);
    EXPECT_NE(s1, s2);

    EXPECT_EQ(store.find(core::make_order_key(first)), s1);
    EXPECT_EQ(store.find(core::make_order_key(second)), s2);
}

TEST_F(OrderStateStoreTest, EpochReset) {
    core::OrderStateStore store(arena_, 8);
    const auto ev = make_event("RESET1");
    const auto key = core::make_order_key(ev);

    core::OrderState* s1 = store.upsert(ev);
    ASSERT_NE(s1, nullptr);
    store.reset_epoch();

    EXPECT_EQ(store.find(key), nullptr);

    core::OrderState* s2 = store.upsert(ev);
    ASSERT_NE(s2, nullptr);
    EXPECT_EQ(s2->key, key);
}

TEST_F(OrderStateStoreTest, OverflowPath) {
    util::Arena small_arena(1 << 12);
    core::OrderStateStore store(small_arena, 2);

    std::size_t failed_inserts = 0;
    for (int i = 0; i < 16; ++i) {
        const auto ev = make_event("OF" + std::to_string(i));
        if (!store.upsert(ev)) {
            ++failed_inserts;
        }
    }

    EXPECT_GT(failed_inserts, 0u);
    EXPECT_GE(store.overflow_count(), failed_inserts);
}

TEST_F(OrderStateStoreTest, RecycleRemovesOrder) {
    core::OrderStateStore store(arena_, 128);

    const auto ev = make_event("RECYCLE1");
    const auto key = core::make_order_key(ev);

    core::OrderState* st = store.upsert(ev);
    ASSERT_NE(st, nullptr);
    EXPECT_EQ(store.size(), 1u);
    EXPECT_EQ(store.recycled_count(), 0u);

    store.recycle(key);

    EXPECT_EQ(store.size(), 0u);
    EXPECT_EQ(store.recycled_count(), 1u);
    EXPECT_EQ(store.find(key), nullptr);
}

TEST_F(OrderStateStoreTest, RecycleNonExistentKeyIsNoOp) {
    core::OrderStateStore store(arena_, 128);

    const auto ev = make_event("EXISTS1");
    store.upsert(ev);
    EXPECT_EQ(store.size(), 1u);

    // Recycle a key that doesn't exist
    const auto other_ev = make_event("NOTEXIST");
    const auto other_key = core::make_order_key(other_ev);
    store.recycle(other_key);

    EXPECT_EQ(store.size(), 1u);
    EXPECT_EQ(store.recycled_count(), 0u);
}

TEST_F(OrderStateStoreTest, RecycleEmptyKeyIsNoOp) {
    core::OrderStateStore store(arena_, 128);

    const auto ev = make_event("EMPTYKEY");
    store.upsert(ev);
    EXPECT_EQ(store.size(), 1u);

    // empty_key_ is numeric_limits<OrderKey>::max()
    store.recycle(std::numeric_limits<core::OrderKey>::max());

    EXPECT_EQ(store.size(), 1u);
    EXPECT_EQ(store.recycled_count(), 0u);
}

TEST_F(OrderStateStoreTest, RecycleAllowsReinsert) {
    core::OrderStateStore store(arena_, 128);

    const auto ev = make_event("REINSERT");
    const auto key = core::make_order_key(ev);

    core::OrderState* st1 = store.upsert(ev);
    ASSERT_NE(st1, nullptr);

    // Modify state before recycling to verify reinsertion is clean
    st1->internal_status = core::OrdStatus::Filled;
    st1->dropcopy_status = core::OrdStatus::Canceled;
    st1->recon_state = core::ReconState::DivergedConfirmed;
    st1->internal_cum_qty = 999;

    store.recycle(key);
    EXPECT_EQ(store.find(key), nullptr);

    // Re-insert same key - should get a properly initialized OrderState
    core::OrderState* st2 = store.upsert(ev);
    ASSERT_NE(st2, nullptr);
    EXPECT_EQ(store.size(), 1u);
    EXPECT_EQ(store.find(key), st2);

    // Verify the reinserted state is properly initialized (not stale data)
    EXPECT_EQ(st2->key, key);
    EXPECT_EQ(st2->internal_status, core::OrdStatus::Unknown);
    EXPECT_EQ(st2->dropcopy_status, core::OrdStatus::Unknown);
    EXPECT_EQ(st2->recon_state, core::ReconState::Unknown);
    EXPECT_EQ(st2->internal_cum_qty, 0);
}

TEST_F(OrderStateStoreTest, RecycleMultipleOrders) {
    core::OrderStateStore store(arena_, 128);

    std::vector<core::ExecEvent> events;
    std::vector<core::OrderKey> keys;
    for (int i = 0; i < 5; ++i) {
        auto ev = make_event("MULTI" + std::to_string(i));
        events.push_back(ev);
        keys.push_back(core::make_order_key(ev));
        ASSERT_NE(store.upsert(ev), nullptr);
    }
    EXPECT_EQ(store.size(), 5u);

    // Recycle orders 0, 2, 4
    store.recycle(keys[0]);
    store.recycle(keys[2]);
    store.recycle(keys[4]);

    EXPECT_EQ(store.size(), 2u);
    EXPECT_EQ(store.recycled_count(), 3u);

    EXPECT_EQ(store.find(keys[0]), nullptr);
    EXPECT_NE(store.find(keys[1]), nullptr);
    EXPECT_EQ(store.find(keys[2]), nullptr);
    EXPECT_NE(store.find(keys[3]), nullptr);
    EXPECT_EQ(store.find(keys[4]), nullptr);
}

TEST_F(OrderStateStoreTest, RecycledCountAccumulates) {
    core::OrderStateStore store(arena_, 128);

    for (int i = 0; i < 3; ++i) {
        const auto ev = make_event("ACC" + std::to_string(i));
        store.upsert(ev);
    }
    EXPECT_EQ(store.recycled_count(), 0u);

    for (int i = 0; i < 3; ++i) {
        const auto ev = make_event("ACC" + std::to_string(i));
        store.recycle(core::make_order_key(ev));
    }
    EXPECT_EQ(store.recycled_count(), 3u);
}

// Ensure that recycling an entry that may lie earlier in a probe chain
// does not make other colliding entries unreachable.
TEST_F(OrderStateStoreTest, RecycleDoesNotBreakProbeChain) {
    // Use a small capacity to increase the likelihood of collisions.
    core::OrderStateStore store(arena_, 4);
    const std::size_t mask = store.bucket_count() - 1;

    // Find two distinct keys that map to the same initial bucket
    std::vector<bool> bucket_seen(store.bucket_count(), false);
    std::vector<core::ExecEvent> bucket_sample(store.bucket_count());
    core::ExecEvent first{};
    core::ExecEvent second{};
    bool found = false;

    for (int i = 0; i < 2000 && !found; ++i) {
        const auto ev = make_event("CHAIN" + std::to_string(i));
        const auto key = core::make_order_key(ev);
        const auto bucket = key & mask;
        if (!bucket_seen[bucket]) {
            bucket_seen[bucket] = true;
            bucket_sample[bucket] = ev;
        } else if (core::make_order_key(bucket_sample[bucket]) != key) {
            first = bucket_sample[bucket];
            second = ev;
            found = true;
        }
    }

    ASSERT_TRUE(found) << "Unable to synthesize two distinct keys mapping to same bucket";

    const auto key1 = core::make_order_key(first);
    const auto key2 = core::make_order_key(second);

    core::OrderState* st1 = store.upsert(first);
    core::OrderState* st2 = store.upsert(second);

    ASSERT_NE(st1, nullptr);
    ASSERT_NE(st2, nullptr);
    EXPECT_NE(st1, st2);

    // Sanity check: both keys should be findable before recycling.
    EXPECT_EQ(store.find(key1), st1);
    EXPECT_EQ(store.find(key2), st2);

    // Recycle the first key; this must not prevent finding the second.
    store.recycle(key1);

    EXPECT_EQ(store.find(key1), nullptr);
    EXPECT_EQ(store.size(), 1u);
    EXPECT_EQ(store.find(key2), st2);
}

// Verify that recycling preserves the arena memory pointer for reuse,
// and re-inserting at the same tombstone slot reuses that memory.
TEST_F(OrderStateStoreTest, RecycleAndReinsertReusesArenaMemory) {
    core::OrderStateStore store(arena_, 128);

    const auto ev1 = make_event("ARENA_REUSE1");
    const auto key1 = core::make_order_key(ev1);

    core::OrderState* st1 = store.upsert(ev1);
    ASSERT_NE(st1, nullptr);
    core::OrderState* original_ptr = st1;

    // Mutate to verify it gets cleaned on reinsert
    st1->internal_cum_qty = 12345;
    st1->internal_status = core::OrdStatus::Filled;

    // Recycle - tombstone preserves the values_ pointer
    store.recycle(key1);
    EXPECT_EQ(store.find(key1), nullptr);

    // Re-insert same key - should reuse the arena memory (same pointer)
    core::OrderState* st2 = store.upsert(ev1);
    ASSERT_NE(st2, nullptr);

    // The pointer should be the same (arena memory reuse)
    EXPECT_EQ(st2, original_ptr);

    // But the state should be freshly initialized
    EXPECT_EQ(st2->key, key1);
    EXPECT_EQ(st2->internal_status, core::OrdStatus::Unknown);
    EXPECT_EQ(st2->dropcopy_status, core::OrdStatus::Unknown);
    EXPECT_EQ(st2->recon_state, core::ReconState::Unknown);
    EXPECT_EQ(st2->internal_cum_qty, 0);
    EXPECT_EQ(st2->seen_internal, false);
    EXPECT_EQ(st2->seen_dropcopy, false);
}

// Verify that repeated recycle+reinsert cycles don't grow the arena.
TEST_F(OrderStateStoreTest, ArenaDoesNotGrowAfterRecycleReinsert) {
    // Use a tiny arena - just enough for a few OrderState allocations
    const std::size_t arena_size = sizeof(core::OrderState) * 4 + 256;
    util::Arena small_arena(arena_size);
    core::OrderStateStore store(small_arena, 4);

    const auto ev = make_event("CYCLE1");
    const auto key = core::make_order_key(ev);

    // First insert uses arena
    core::OrderState* st = store.upsert(ev);
    ASSERT_NE(st, nullptr);

    // Do many recycle+reinsert cycles - should never exhaust the arena
    // because arena memory is reused via tombstone pointer preservation
    for (int i = 0; i < 100; ++i) {
        store.recycle(key);
        EXPECT_EQ(store.find(key), nullptr);

        core::OrderState* reinserted = store.upsert(ev);
        ASSERT_NE(reinserted, nullptr)
            << "Arena exhausted on cycle " << i
            << "; arena reuse is not working";
        EXPECT_EQ(reinserted->key, key);
        EXPECT_EQ(reinserted->internal_status, core::OrdStatus::Unknown);
    }

    EXPECT_EQ(store.size(), 1u);
}

// Verify that recycling a different key and inserting a new key into
// the tombstone slot still properly initializes the state.
TEST_F(OrderStateStoreTest, RecycleTombstoneReusedByDifferentKey) {
    core::OrderStateStore store(arena_, 4);
    const std::size_t mask = store.bucket_count() - 1;

    // Find two distinct keys that map to the same bucket (collision)
    std::vector<bool> bucket_seen(store.bucket_count(), false);
    std::vector<core::ExecEvent> bucket_sample(store.bucket_count());
    core::ExecEvent first{};
    core::ExecEvent second{};
    bool found = false;

    for (int i = 0; i < 2000 && !found; ++i) {
        const auto ev = make_event("TOMB" + std::to_string(i));
        const auto key_val = core::make_order_key(ev);
        const auto bucket = key_val & mask;
        if (!bucket_seen[bucket]) {
            bucket_seen[bucket] = true;
            bucket_sample[bucket] = ev;
        } else if (core::make_order_key(bucket_sample[bucket]) != key_val) {
            first = bucket_sample[bucket];
            second = ev;
            found = true;
        }
    }

    ASSERT_TRUE(found) << "Unable to synthesize two distinct keys mapping to same bucket";

    const auto key1 = core::make_order_key(first);
    const auto key2 = core::make_order_key(second);

    // Insert first key, mutate it, then recycle
    core::OrderState* st1 = store.upsert(first);
    ASSERT_NE(st1, nullptr);
    st1->internal_cum_qty = 99999;
    st1->internal_status = core::OrdStatus::Rejected;

    store.recycle(key1);

    // Insert second key - it should go into the tombstone slot
    // and be properly initialized (not inherit stale data from key1)
    core::OrderState* st2 = store.upsert(second);
    ASSERT_NE(st2, nullptr);
    EXPECT_EQ(st2->key, key2);
    EXPECT_EQ(st2->internal_status, core::OrdStatus::Unknown);
    EXPECT_EQ(st2->dropcopy_status, core::OrdStatus::Unknown);
    EXPECT_EQ(st2->recon_state, core::ReconState::Unknown);
    EXPECT_EQ(st2->internal_cum_qty, 0);
}

} // namespace
