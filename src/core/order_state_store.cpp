#include "core/order_state_store.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include "util/perf_macros.hpp"

namespace core {

static constexpr std::size_t default_probe_limit = 64;

std::size_t OrderStateStore::next_power_of_two(std::size_t v) {
    if (v == 0) return 1;
    if ((v & (v - 1)) == 0) return v;
    std::size_t n = 1;
    while (n < v && n < (std::numeric_limits<std::size_t>::max() >> 1)) {
        n <<= 1;
    }
    if (n < v) {
        throw std::runtime_error("OrderStateStore capacity overflow");
    }
    return n;
}

OrderStateStore::OrderStateStore(util::Arena& arena, std::size_t capacity_hint)
    : arena_(arena) {
    if (capacity_hint == 0) {
        throw std::invalid_argument("OrderStateStore capacity_hint must be > 0");
    }

    const std::size_t desired = capacity_hint > (std::numeric_limits<std::size_t>::max() / 2)
                                    ? std::numeric_limits<std::size_t>::max()
                                    : capacity_hint * 2;
    bucket_count_ = next_power_of_two(desired);
    if (bucket_count_ < 2) {
        throw std::runtime_error("OrderStateStore bucket_count underflow");
    }

    keys_ = std::make_unique<OrderKey[]>(bucket_count_);
    values_ = std::make_unique<OrderState*[]>(bucket_count_);

    max_probe_ = std::min<std::size_t>(bucket_count_, default_probe_limit);

    reset_epoch();
}

// ---------------------------------------------------------------------------
// Freelist: reuse recycled arena memory without heap allocation.
// Recycled OrderState slots are linked via an intrusive singly-linked list
// stored in the first sizeof(pointer) bytes of the OrderState memory.
// ---------------------------------------------------------------------------

OrderState* OrderStateStore::alloc_state(OrderKey key) noexcept {
    OrderState* st = nullptr;

    if (free_head_) {
        // Reuse recycled slot -- zero allocation.
        st = free_head_;
        OrderState* next{};
        std::memcpy(&next, st, sizeof(next));
        free_head_ = next;
    } else {
        // First-time allocation from arena.
        st = create_order_state(arena_, key);
        if (!st) return nullptr;
        // create_order_state already initialises key; we still memset below
        // for a uniform code path.
    }

    std::memset(static_cast<void*>(st), 0, sizeof(OrderState));
    st->key = key;
    st->internal_status = OrdStatus::Unknown;
    st->dropcopy_status = OrdStatus::Unknown;
    st->recon_state = ReconState::Unknown;
    return st;
}

void OrderStateStore::free_state(OrderState* st) noexcept {
    // Push onto intrusive freelist (store next pointer in first 8 bytes).
    std::memcpy(st, &free_head_, sizeof(free_head_));
    free_head_ = st;
}

// ---------------------------------------------------------------------------
// Core operations
// ---------------------------------------------------------------------------

OrderState* OrderStateStore::upsert(const ExecEvent& ev) noexcept {
    PERF_SCOPE(::util::PerfCounterId::HashTableUpsert);

    const OrderKey key = make_order_key(ev);
    if (key == empty_key_ || key == tombstone_key_) {
        ++overflow_count_;
        return nullptr;
    }

    const std::size_t m = mask();
    std::size_t idx = hash(key) & m;
    std::size_t first_tombstone = bucket_count_;  // Sentinel: no tombstone seen yet

    for (std::size_t probe = 0; probe < max_probe_; ++probe) {
        const OrderKey k = keys_[idx];

        if (k == key) {
            return values_[idx];  // Existing entry.
        }

        if (k == tombstone_key_) {
            // Track first tombstone for potential reuse
            if (first_tombstone == bucket_count_) {
                first_tombstone = idx;
            }
        } else if (k == empty_key_) {
            // Empty slot or we can reuse a tombstone we saw earlier
            const std::size_t insert_idx = (first_tombstone != bucket_count_) ? first_tombstone : idx;
            
            OrderState* st = alloc_state(key);
            if (!st) {
                ++overflow_count_;
                return nullptr;
            }
            keys_[insert_idx] = key;
            values_[insert_idx] = st;
            ++size_;
            return st;
        }

        idx = (idx + 1) & m;
    }

    // Probe limit exceeded
    ++overflow_count_;
    return nullptr;
}

OrderState* OrderStateStore::find(OrderKey key) noexcept {
    PERF_SCOPE(::util::PerfCounterId::HashTableLookup);

    if (key == empty_key_ || key == tombstone_key_) return nullptr;

    const std::size_t m = mask();
    std::size_t idx = hash(key) & m;

    for (std::size_t probe = 0; probe < max_probe_; ++probe) {
        const OrderKey k = keys_[idx];
        if (k == key)            return values_[idx];
        if (k == empty_key_)     return nullptr;
        // Skip tombstones and continue probing
        idx = (idx + 1) & m;
    }
    return nullptr;
}

void OrderStateStore::recycle(OrderKey key) noexcept {
    if (key == empty_key_ || key == tombstone_key_) return;

    const std::size_t m = mask();
    std::size_t idx = hash(key) & m;

    for (std::size_t probe = 0; probe < max_probe_; ++probe) {
        const OrderKey k = keys_[idx];
        if (k == key) {
            // Return the OrderState memory to the freelist.
            free_state(values_[idx]);
            values_[idx] = nullptr;
            
            // Mark slot as tombstone
            keys_[idx] = tombstone_key_;
            
            --size_;
            ++recycled_count_;
            return;
        }
        if (k == empty_key_) {
            return;  // Not present.
        }
        idx = (idx + 1) & m;
    }
    // Not found (probe limit exceeded).
}

void OrderStateStore::reset_epoch() noexcept {
    arena_.reset();
    std::fill_n(keys_.get(), bucket_count_, empty_key_);
    std::fill_n(values_.get(), bucket_count_, nullptr);
    size_ = 0;
    overflow_count_ = 0;
    free_head_ = nullptr;
}

} // namespace core
