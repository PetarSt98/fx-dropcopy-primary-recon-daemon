#pragma once

#include <cstdint>
#include <cstddef>
#include <type_traits>

namespace core {

// WireExecEvent represents the normalized, fixed-layout execution event published on the bus.
// In production this would correspond to an SBE/flat schema; here it is a simple packed POD
// with no heap-owned fields.
#pragma pack(push, 1)
struct WireExecEvent {
    static constexpr std::size_t id_capacity = 32;

    std::uint8_t exec_type{0};
    std::uint8_t ord_status{0};
    std::int64_t price_micro{0};
    std::int64_t qty{0};
    std::int64_t cum_qty{0};
    std::uint64_t sending_time{0};
    std::uint64_t transact_time{0};

    char exec_id[id_capacity]{};
    std::uint8_t exec_id_len{0};

    char order_id[id_capacity]{};
    std::uint8_t order_id_len{0};

    char clord_id[id_capacity]{};
    std::uint8_t clord_id_len{0};
};
#pragma pack(pop)

static_assert(std::is_trivially_copyable_v<WireExecEvent>, "WireExecEvent must be trivial");
static_assert(sizeof(WireExecEvent) == 141, "WireExecEvent layout is expected to be packed and fixed-size");

} // namespace core
