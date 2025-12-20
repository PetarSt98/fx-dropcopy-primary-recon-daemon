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

} // namespace
