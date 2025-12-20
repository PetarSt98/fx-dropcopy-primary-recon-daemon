#include "core/order_state_store.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

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
    const OrderKey key = make_order_key(ev);
    if (key == empty_key_) {
        ++overflow_count_;
        return nullptr;
    }

    const std::size_t start = hash(key) & mask();
    std::size_t idx = start;

    for (std::size_t probe = 0; probe < max_probe_; ++probe) {
        const OrderKey bucket_key = keys_[idx];
        if (bucket_key == empty_key_) {
            OrderState* st = create_order_state(arena_, key);
            if (!st) {
                ++overflow_count_;
                return nullptr;
            }
            keys_[idx] = key;
            values_[idx] = st;
            ++size_;
            return st;
        }
        if (bucket_key == key) {
            return values_[idx];
        }
        idx = (idx + 1) & mask();
    }

    ++overflow_count_;
    return nullptr;
}

OrderState* OrderStateStore::find(OrderKey key) noexcept {
    if (key == empty_key_) {
        return nullptr;
    }

    const std::size_t start = hash(key) & mask();
    std::size_t idx = start;

    for (std::size_t probe = 0; probe < max_probe_; ++probe) {
        const OrderKey bucket_key = keys_[idx];
        if (bucket_key == empty_key_) {
            return nullptr;
        }
        if (bucket_key == key) {
            return values_[idx];
        }
        idx = (idx + 1) & mask();
    }
    return nullptr;
}

void OrderStateStore::reset_epoch() noexcept {
    arena_.reset();
    std::fill_n(keys_.get(), bucket_count_, empty_key_);
    std::fill_n(values_.get(), bucket_count_, nullptr);
    size_ = 0;
    overflow_count_ = 0;
}

} // namespace core
