#include "test_main.hpp"

#include <string>
#include <vector>

#include "core/order_state_store.hpp"

namespace order_state_store_tests {

namespace {
core::ExecEvent make_event(const std::string& cid) {
    core::ExecEvent evt{};
    evt.set_clord_id(cid.data(), cid.size());
    return evt;
}
} // namespace

bool test_basic_insert_find() {
    util::Arena arena(1 << 20);
    core::OrderStateStore store(arena, 128);

    std::vector<core::ExecEvent> events;
    for (int i = 0; i < 10; ++i) {
        events.push_back(make_event("CID" + std::to_string(i)));
    }

    for (const auto& ev : events) {
        core::OrderState* first = store.upsert(ev);
        if (!first) {
            return false;
        }
        core::OrderState* second = store.upsert(ev);
        if (first != second) {
            return false;
        }
        if (store.find(core::make_order_key(ev)) != first) {
            return false;
        }
    }

    return store.size() == events.size();
}

bool test_collision_handling() {
    util::Arena arena(1 << 20);
    core::OrderStateStore store(arena, 4);
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

    if (!found) {
        return false;
    }

    auto* s1 = store.upsert(first);
    auto* s2 = store.upsert(second);
    if (!s1 || !s2 || s1 == s2) {
        return false;
    }
    return store.find(core::make_order_key(first)) == s1 &&
           store.find(core::make_order_key(second)) == s2;
}

bool test_epoch_reset() {
    util::Arena arena(1 << 20);
    core::OrderStateStore store(arena, 8);
    const auto ev = make_event("RESET1");
    const auto key = core::make_order_key(ev);

    core::OrderState* s1 = store.upsert(ev);
    if (!s1) {
        return false;
    }
    store.reset_epoch();
    if (store.find(key) != nullptr) {
        return false;
    }
    core::OrderState* s2 = store.upsert(ev);
    return s2 && s2 != s1;
}

bool test_overflow_path() {
    util::Arena arena(1 << 12);
    core::OrderStateStore store(arena, 2);

    std::size_t failed_inserts = 0;
    for (int i = 0; i < 16; ++i) {
        const auto ev = make_event("OF" + std::to_string(i));
        if (!store.upsert(ev)) {
            ++failed_inserts;
        }
    }

    return failed_inserts > 0 && store.overflow_count() >= failed_inserts;
}

void add_tests(std::vector<TestCase>& tests) {
    tests.push_back({"order_state_store_basic_insert_find", test_basic_insert_find});
    tests.push_back({"order_state_store_collision_handling", test_collision_handling});
    tests.push_back({"order_state_store_epoch_reset", test_epoch_reset});
    tests.push_back({"order_state_store_overflow_path", test_overflow_path});
}

} // namespace order_state_store_tests
