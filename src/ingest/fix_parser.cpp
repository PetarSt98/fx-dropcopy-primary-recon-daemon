#include "ingest/fix_parser.hpp"

#include <charconv>
#include <cstring>

#include "util/rdtsc.hpp"

namespace ingest {
namespace {

constexpr char soh = '\x01';

inline bool parse_int64(const char* begin, const char* end, int64_t& out) noexcept {
    auto res = std::from_chars(begin, end, out);
    return res.ec == std::errc{} && res.ptr == end;
}

inline bool parse_uint64(const char* begin, const char* end, uint64_t& out) noexcept {
    auto res = std::from_chars(begin, end, out);
    return res.ec == std::errc{} && res.ptr == end;
}

inline void assign_id(const char* begin, const char* end, char* dest, std::size_t& len) noexcept {
    const auto l = static_cast<std::size_t>(end - begin);
    const auto to_copy = l > core::ExecEvent::id_capacity ? core::ExecEvent::id_capacity : l;
    std::memcpy(dest, begin, to_copy);
    len = to_copy;
}

inline core::ExecType map_exec_type(char c) noexcept {
    switch (c) {
    case '0': return core::ExecType::New;
    case '1': return core::ExecType::PartialFill;
    case '2': return core::ExecType::Fill;
    case '4': return core::ExecType::Cancel;
    case '5': return core::ExecType::Replace;
    case '8': return core::ExecType::Rejected;
    default: return core::ExecType::Unknown;
    }
}

inline core::OrdStatus map_ord_status(char c) noexcept {
    switch (c) {
    case '0': return core::OrdStatus::New;
    case 'A': return core::OrdStatus::PendingNew;
    case '6': return core::OrdStatus::CancelPending;
    case '1': return core::OrdStatus::PartiallyFilled;
    case '2': return core::OrdStatus::Filled;
    case '4': return core::OrdStatus::Canceled;
    case '5': return core::OrdStatus::Replaced;
    case '8': return core::OrdStatus::Rejected;
    default: return core::OrdStatus::Unknown;
    }
}

} // namespace

ParseResult parse_exec_report(const char* data, std::size_t len, core::ExecEvent& out) noexcept {
    bool has_exec_type = false;
    bool has_ord_status = false;
    bool has_price = false;
    bool has_qty = false;
    bool has_exec_id = false;
    bool has_time = false;

    const char* ptr = data;
    const char* end = data + len;

    while (ptr < end) {
        const char* tag_start = ptr;
        while (ptr < end && *ptr != '=') ++ptr;
        if (ptr >= end) break;
        int64_t tag = 0;
        if (!parse_int64(tag_start, ptr, tag)) return ParseResult::Invalid;
        ++ptr; // skip '='
        const char* val_start = ptr;
        while (ptr < end && *ptr != soh) ++ptr;
        const char* val_end = ptr;
        if (ptr < end) ++ptr; // skip separator

        switch (tag) {
        case 35: { // MsgType
            if (val_start == val_end) return ParseResult::Invalid;
            // Only accept ExecReport (8)
            if (*val_start != '8') return ParseResult::Invalid;
            break;
        }
        case 150: { // ExecType
            if (val_start == val_end) return ParseResult::Invalid;
            out.exec_type = map_exec_type(*val_start);
            has_exec_type = true;
            break;
        }
        case 39: { // OrdStatus
            if (val_start == val_end) return ParseResult::Invalid;
            out.ord_status = map_ord_status(*val_start);
            has_ord_status = true;
            break;
        }
        case 31: { // LastPx -> using as price
            int64_t price = 0;
            if (!parse_int64(val_start, val_end, price)) return ParseResult::Invalid;
            out.price_micro = price;
            has_price = true;
            break;
        }
        case 32: { // LastQty -> qty
            int64_t qty = 0;
            if (!parse_int64(val_start, val_end, qty)) return ParseResult::Invalid;
            out.qty = qty;
            has_qty = true;
            break;
        }
        case 14: { // CumQty
            int64_t cum = 0;
            if (!parse_int64(val_start, val_end, cum)) return ParseResult::Invalid;
            out.cum_qty = cum;
            break;
        }
        case 17: { // ExecID
            assign_id(val_start, val_end, out.exec_id, out.exec_id_len);
            has_exec_id = true;
            break;
        }
        case 11: { // ClOrdID
            assign_id(val_start, val_end, out.clord_id, out.clord_id_len);
            break;
        }
        case 37: { // OrderID
            assign_id(val_start, val_end, out.order_id, out.order_id_len);
            break;
        }
        case 52: { // SendingTime
            uint64_t ts = 0;
            if (!parse_uint64(val_start, val_end, ts)) return ParseResult::Invalid;
            out.sending_time = ts;
            has_time = true;
            break;
        }
        case 60: { // TransactTime
            uint64_t ts = 0;
            if (!parse_uint64(val_start, val_end, ts)) return ParseResult::Invalid;
            out.transact_time = ts;
            has_time = true;
            break;
        }
        default:
            break;
        }
    }

    out.ingest_tsc = util::rdtsc();

    if (out.order_id_len == 0 && out.clord_id_len == 0) {
        return ParseResult::MissingField;
    }
    if (!has_exec_type || !has_ord_status || !has_price || !has_qty || !has_exec_id || !has_time) {
        return ParseResult::MissingField;
    }
    return ParseResult::Ok;
}

} // namespace ingest
