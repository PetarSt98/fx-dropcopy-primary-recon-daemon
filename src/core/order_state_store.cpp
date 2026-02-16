#include "core/order_state_store.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include "util/perf_macros.hpp"

namespace core {

namespace {
constexpr std::size_t default_probe_limit = 64;
}

std::size_t OrderStateStore::next_power_of_two(std::size_t v) {
    if (v == 0) {
        return 1;
    }
    if ((v & (v - 1)) == 0) {
        return v;
    }
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

OrderState* OrderStateStore::upsert(const ExecEvent& ev) noexcept {
    PERF_SCOPE(::util::PerfCounterId::HashTableUpsert);
    
    const OrderKey key = make_order_key(ev);
    if (key == empty_key_ || key == tombstone_key_) {
        ++overflow_count_;
        return nullptr;
    }

    const std::size_t start = hash(key) & mask();
    std::size_t idx = start;
    std::size_t first_tombstone = std::numeric_limits<std::size_t>::max();

    for (std::size_t probe = 0; probe < max_probe_; ++probe) {
        const OrderKey bucket_key = keys_[idx];
        if (bucket_key == empty_key_) {
            const std::size_t insert_idx = (first_tombstone != std::numeric_limits<std::size_t>::max())
                                           ? first_tombstone : idx;
            
            // If tombstone has memory from a recycled order, reuse it
            if (insert_idx != idx && values_[insert_idx] != nullptr) {
                OrderState* st = values_[insert_idx];
                std::memset(static_cast<void*>(st), 0, sizeof(OrderState));
                st->key = key;
                st->internal_status = OrdStatus::Unknown;
                st->dropcopy_status = OrdStatus::Unknown;
                st->recon_state = ReconState::Unknown;
                keys_[insert_idx] = key;
                ++size_;
                return st;
            }
            
            // Original path: allocate new from arena
            OrderState* st = create_order_state(arena_, key);
            if (!st) {
                ++overflow_count_;
                return nullptr;
            }
            keys_[insert_idx] = key;
            values_[insert_idx] = st;
            ++size_;
            return st;
        }
        if (bucket_key == tombstone_key_) {
            // Remember first tombstone slot for potential reuse
            if (first_tombstone == std::numeric_limits<std::size_t>::max()) {
                first_tombstone = idx;
            }
        } else if (bucket_key == key) {
            return values_[idx];
        }
        idx = (idx + 1) & mask();
    }

    ++overflow_count_;
    return nullptr;
}

OrderState* OrderStateStore::find(OrderKey key) noexcept {
    PERF_SCOPE(::util::PerfCounterId::HashTableLookup);
    
    if (key == empty_key_ || key == tombstone_key_) {
        return nullptr;
    }

    const std::size_t start = hash(key) & mask();
    std::size_t idx = start;

    for (std::size_t probe = 0; probe < max_probe_; ++probe) {
        const OrderKey bucket_key = keys_[idx];
        if (bucket_key == empty_key_) {
            return nullptr;
        }
        // Skip tombstones - they don't terminate probe chain
        if (bucket_key == key) {
            return values_[idx];
        }
        idx = (idx + 1) & mask();
    }
    return nullptr;
}

void OrderStateStore::recycle(OrderKey key) noexcept {
    if (key == empty_key_ || key == tombstone_key_) {
        return;
    }

    const std::size_t start = hash(key) & mask();
    std::size_t idx = start;

    for (std::size_t probe = 0; probe < max_probe_; ++probe) {
        const OrderKey bucket_key = keys_[idx];

        if (bucket_key == key) {
            keys_[idx] = tombstone_key_;
            --size_;
            ++recycled_count_;
            return;
        }

        if (bucket_key == empty_key_) {
            return;
        }

        idx = (idx + 1) & mask();
    }
}

void OrderStateStore::reset_epoch() noexcept {
    arena_.reset();
    std::fill_n(keys_.get(), bucket_count_, empty_key_);
    std::fill_n(values_.get(), bucket_count_, nullptr);
    size_ = 0;
    overflow_count_ = 0;
}

} // namespace core
