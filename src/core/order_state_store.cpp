#include "core/order_state_store.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include "util/perf_macros.hpp"

namespace core {

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
    if (key == empty_key_) {
        ++overflow_count_;
        return nullptr;
    }

    const std::size_t m = mask();
    std::size_t idx = hash(key) & m;

    for (std::size_t probe = 0; probe < bucket_count_; ++probe) {
        const OrderKey k = keys_[idx];

        if (k == key) {
            return values_[idx];  // Existing entry.
        }

        if (k == empty_key_) {
            // Empty slot -- insert here.
            OrderState* st = alloc_state(key);
            if (!st) {
                ++overflow_count_;
                return nullptr;
            }
            keys_[idx] = key;
            values_[idx] = st;
            ++size_;
            return st;
        }

        idx = (idx + 1) & m;
    }

    // Table completely full (size == bucket_count). Should not happen at < 50% load.
    ++overflow_count_;
    return nullptr;
}

OrderState* OrderStateStore::find(OrderKey key) noexcept {
    PERF_SCOPE(::util::PerfCounterId::HashTableLookup);

    if (key == empty_key_) return nullptr;

    const std::size_t m = mask();
    std::size_t idx = hash(key) & m;

    for (std::size_t probe = 0; probe < bucket_count_; ++probe) {
        const OrderKey k = keys_[idx];
        if (k == key)       return values_[idx];
        if (k == empty_key_) return nullptr;
        idx = (idx + 1) & m;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Backward-shift deletion (Robin Hood style).
//
// Instead of tombstoning, we shift subsequent entries backward to fill the
// gap left by the deleted entry. This preserves probe-chain invariants
// without tombstones, so the table never degrades over time.
//
// Algorithm:
//   1. Find and clear the target slot (set to empty).
//   2. Walk forward through the cluster. For each occupied slot, check if
//      it "belongs" at or before the gap (i.e., its ideal position is at
//      or before the empty slot in the circular probe order).
//   3. If yes, shift it backward into the gap and repeat from the new gap.
//   4. Stop when we hit an empty slot -- the cluster is clean.
// ---------------------------------------------------------------------------

void OrderStateStore::recycle(OrderKey key) noexcept {
    if (key == empty_key_) return;

    const std::size_t m = mask();
    std::size_t idx = hash(key) & m;

    // Phase 1: find the entry.
    for (std::size_t probe = 0; probe < bucket_count_; ++probe) {
        const OrderKey k = keys_[idx];
        if (k == key) goto found;
        if (k == empty_key_) return;  // Not present.
        idx = (idx + 1) & m;
    }
    return;  // Not found (table full scan -- should not happen).

found:
    // Return the OrderState memory to the freelist.
    free_state(values_[idx]);
    values_[idx] = nullptr;

    --size_;
    ++recycled_count_;

    // Phase 2: backward-shift to fill the gap.
    std::size_t gap = idx;
    std::size_t next = (gap + 1) & m;

    for (std::size_t probe = 0; probe < bucket_count_; ++probe) {
        const OrderKey next_key = keys_[next];

        if (next_key == empty_key_) {
            // End of cluster -- clear the gap and we're done.
            keys_[gap] = empty_key_;
            values_[gap] = nullptr;
            return;
        }

        // Where does this entry ideally belong?
        const std::size_t ideal = hash(next_key) & m;

        // Should we shift this entry into the gap?
        // Yes if the ideal position is "at or before" the gap in the circular
        // probe order. Equivalently: the entry is displaced past the gap.
        //
        // For a circular table, entry at `next` with ideal `ideal` should be
        // shifted into `gap` iff `ideal` is NOT in the range (gap, next] mod N.
        // This is the standard backward-shift condition for linear probing.
        bool shift;
        if (gap <= next) {
            // gap and next are in normal (non-wrapped) order.
            // Shift if ideal is NOT in (gap, next], i.e. ideal <= gap or ideal > next.
            shift = (ideal <= gap) || (ideal > next);
        } else {
            // gap > next means the range wraps around the table end.
            // Shift if ideal <= gap AND ideal > next.
            shift = (ideal <= gap) && (ideal > next);
        }

        if (shift) {
            keys_[gap] = next_key;
            values_[gap] = values_[next];
            gap = next;
        }

        next = (next + 1) & m;
    }

    // Fallthrough: clear final gap.
    keys_[gap] = empty_key_;
    values_[gap] = nullptr;
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
