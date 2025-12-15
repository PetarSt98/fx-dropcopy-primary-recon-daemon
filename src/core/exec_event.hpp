#pragma once

#include <cstdint>
#include <cstddef>
#include <array>
#include <cstring>

namespace core {

enum class Source : uint8_t { Primary = 0, DropCopy = 1 };
enum class ExecType : uint8_t { New, PartialFill, Fill, Cancel, Replace, Rejected, Unknown };
enum class OrdStatus : uint8_t { New, PartiallyFilled, Filled, Canceled, Replaced, Rejected, Unknown };

struct ExecEvent {
    Source source{};
    ExecType exec_type{ExecType::Unknown};
    OrdStatus ord_status{OrdStatus::Unknown};
    int64_t price_micro{0}; // price in micro-units
    int64_t qty{0};
    int64_t cum_qty{0};
    uint64_t sending_time{0};
    uint64_t transact_time{0};
    uint64_t ingest_tsc{0};

    static constexpr std::size_t id_capacity = 32;
    char exec_id[id_capacity]{};
    std::size_t exec_id_len{0};
    char order_id[id_capacity]{};
    std::size_t order_id_len{0};
    char clord_id[id_capacity]{};
    std::size_t clord_id_len{0};

    void set_order_id(const char* data, std::size_t len) noexcept {
        const auto l = len > id_capacity ? id_capacity : len;
        std::memcpy(order_id, data, l);
        order_id_len = l;
    }
    void set_clord_id(const char* data, std::size_t len) noexcept {
        const auto l = len > id_capacity ? id_capacity : len;
        std::memcpy(clord_id, data, l);
        clord_id_len = l;
    }
    void set_exec_id(const char* data, std::size_t len) noexcept {
        const auto l = len > id_capacity ? id_capacity : len;
        std::memcpy(exec_id, data, l);
        exec_id_len = l;
    }
};

} // namespace core
