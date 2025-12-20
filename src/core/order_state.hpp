#pragma once

#include <cstdint>
#include <cstring>
#include <type_traits>

#include "core/exec_event.hpp"
#include "util/arena.hpp"

namespace core {

using OrderKey = std::uint64_t;

inline OrderKey make_order_key(const ExecEvent& evt) noexcept {
    // FNV-1a 64-bit hash over ClOrdID bytes; deterministic and stable.
    static constexpr OrderKey fnv_offset_basis = 14695981039346656037ULL;
    static constexpr OrderKey fnv_prime = 1099511628211ULL;

    OrderKey hash = fnv_offset_basis;
    for (std::size_t i = 0; i < evt.clord_id_len; ++i) {
        hash ^= static_cast<std::uint8_t>(evt.clord_id[i]);
        hash *= fnv_prime;
    }
    return hash;
}

struct OrderState {
    OrderKey key{0};

    // Internal (primary) view.
    OrdStatus internal_status{OrdStatus::Unknown};
    std::int64_t internal_cum_qty{0};
    std::int64_t internal_avg_px{0};
    std::uint64_t last_internal_ts{0};
    char last_internal_exec_id[ExecEvent::id_capacity]{};
    std::uint8_t last_internal_exec_id_len{0};

    // Drop-copy view.
    std::int64_t dropcopy_cum_qty{0};
    std::int64_t dropcopy_avg_px{0};
    std::uint64_t last_dropcopy_ts{0};
    char last_dropcopy_exec_id[ExecEvent::id_capacity]{};
    std::uint8_t last_dropcopy_exec_id_len{0};

    // Bookkeeping.
    bool seen_internal{false};
    bool seen_dropcopy{false};
    bool has_divergence{false};
    bool has_gap{false};
    std::uint32_t divergence_count{0};
};

inline OrderState* create_order_state(util::Arena& arena, OrderKey key) noexcept {
    void* mem = arena.allocate(sizeof(OrderState), alignof(OrderState));
    if (!mem) {
        return nullptr;
    }
    std::memset(mem, 0, sizeof(OrderState));
    auto* state = static_cast<OrderState*>(mem);
    state->key = key;
    return state;
}

static_assert(std::is_trivially_copyable_v<OrderState>, "OrderState must remain trivially copyable");

} // namespace core
