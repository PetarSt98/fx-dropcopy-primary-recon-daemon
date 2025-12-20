#pragma once

#include <cstdint>
#include <cstddef>
#include <array>
#include <cstring>

#include "core/wire_exec_event.hpp"

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

inline ExecEvent from_wire(const WireExecEvent& w, Source src, uint64_t ingest_tsc) noexcept {
    static_assert(ExecEvent::id_capacity == WireExecEvent::id_capacity,
                  "ExecEvent and WireExecEvent must agree on id capacity");
    ExecEvent evt{};
    evt.source = src;
    evt.exec_type = static_cast<ExecType>(w.exec_type);
    evt.ord_status = static_cast<OrdStatus>(w.ord_status);
    evt.price_micro = w.price_micro;
    evt.qty = w.qty;
    evt.cum_qty = w.cum_qty;
    evt.sending_time = w.sending_time;
    evt.transact_time = w.transact_time;
    evt.ingest_tsc = ingest_tsc;

    const auto exec_len = static_cast<std::size_t>(
        w.exec_id_len > WireExecEvent::id_capacity ? WireExecEvent::id_capacity : w.exec_id_len);
    std::memcpy(evt.exec_id, w.exec_id, exec_len);
    evt.exec_id_len = exec_len;

    const auto order_len = static_cast<std::size_t>(
        w.order_id_len > WireExecEvent::id_capacity ? WireExecEvent::id_capacity : w.order_id_len);
    std::memcpy(evt.order_id, w.order_id, order_len);
    evt.order_id_len = order_len;

    const auto clord_len = static_cast<std::size_t>(
        w.clord_id_len > WireExecEvent::id_capacity ? WireExecEvent::id_capacity : w.clord_id_len);
    std::memcpy(evt.clord_id, w.clord_id, clord_len);
    evt.clord_id_len = clord_len;

    return evt;
}

} // namespace core
