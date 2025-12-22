#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>
#include <string_view>
#include <bit>
#include <array>

#include "util/crc32c.hpp"
#include "persist/endianness.hpp"
#include "core/wire_exec_event.hpp"

namespace persist {

// Wire log record framing used by WireCaptureWriter and WireLogReader:
//   [u32 payload_len_le][payload bytes][u32 crc32c_le]
// The payload is the serialized WireExecEvent (little-endian, packed, fixed-size).
// There is no file header or per-record capture timestamp in the current writer.
// Checksum covers the length prefix (little-endian) and payload bytes.
//
// WireExecEvent serialization layout (little-endian):
//  offset size field
//       0   1   exec_type
//       1   1   ord_status
//       2   8   seq_num
//      10   2   session_id
//      12   8   price_micro
//      20   8   qty
//      28   8   cum_qty
//      36   8   sending_time
//      44   8   transact_time
//      52  32   exec_id[32]
//      84   1   exec_id_len
//      85  32   order_id[32]
//     117   1   order_id_len
//     118  32   clord_id[32]
//     150   1   clord_id_len
// Total serialized size: 151 bytes.

inline constexpr std::size_t wire_exec_event_serialized_size = 151;
inline constexpr std::size_t max_wire_payload_size = 16 * 1024;

struct WireRecordView {
    std::uint32_t payload_length{0};
    std::span<const std::byte> payload{};
    std::uint32_t checksum{0};
};

inline std::uint32_t read_u32_le(const std::byte* p) noexcept {
    std::uint32_t v{};
    std::memcpy(&v, p, sizeof(v));
    return from_le32(v);
}

inline std::uint64_t read_u64_le(const std::byte* p) noexcept {
    std::uint64_t v{};
    std::memcpy(&v, p, sizeof(v));
    return from_le64(v);
}

inline std::uint16_t read_u16_le(const std::byte* p) noexcept {
    std::uint16_t v{};
    std::memcpy(&v, p, sizeof(v));
    return from_le16(v);
}

inline void write_u32_le(std::uint32_t v, std::byte* p) noexcept {
    const auto le = to_le32(v);
    std::memcpy(p, &le, sizeof(le));
}

inline void write_u64_le(std::uint64_t v, std::byte* p) noexcept {
    const auto le = to_le64(v);
    std::memcpy(p, &le, sizeof(le));
}

inline void write_u16_le(std::uint16_t v, std::byte* p) noexcept {
    const auto le = to_le16(v);
    std::memcpy(p, &le, sizeof(le));
}

inline constexpr std::size_t framed_size(std::size_t payload_len) noexcept {
    return sizeof(std::uint32_t) + payload_len + sizeof(std::uint32_t);
}

inline std::uint32_t crc32c(std::span<const std::byte> payload) noexcept {
    return util::Crc32c::compute(payload.data(), payload.size());
}

inline std::uint32_t compute_record_crc(std::uint32_t payload_len_le,
                                        std::span<const std::byte> payload) noexcept {
    std::uint32_t crc = util::Crc32c::initial;
    crc = util::Crc32c::update(crc, reinterpret_cast<const std::byte*>(&payload_len_le), sizeof(payload_len_le));
    crc = util::Crc32c::update(crc, payload.data(), payload.size());
    return util::Crc32c::finalize(crc);
}

inline void encode_record(std::span<const std::byte> payload,
                          std::uint32_t& length_le,
                          std::uint32_t& checksum_le) noexcept {
    length_le = to_le32(static_cast<std::uint32_t>(payload.size()));
    const auto crc = compute_record_crc(length_le, payload);
    checksum_le = to_le32(crc);
}

inline bool parse_record(const std::byte* data,
                         std::size_t size,
                         WireRecordView& out) noexcept {
    if (size < framed_size(0)) {
        return false;
    }
    const std::uint32_t payload_len = read_u32_le(data);
    const std::size_t total = framed_size(payload_len);
    if (total > size || payload_len == 0 || payload_len > max_wire_payload_size) {
        return false;
    }
    out.payload_length = payload_len;
    out.payload = std::span<const std::byte>(data + sizeof(std::uint32_t), payload_len);
    out.checksum = read_u32_le(data + sizeof(std::uint32_t) + payload_len);
    return true;
}

inline bool validate_record(const WireRecordView& rec) noexcept {
    const auto len_le = to_le32(rec.payload_length);
    const auto crc = compute_record_crc(len_le, rec.payload);
    return crc == rec.checksum;
}

inline bool serialize_wire_exec_event(const core::WireExecEvent& evt, std::byte* out) noexcept {
    std::byte* p = out;
    p[0] = static_cast<std::byte>(evt.exec_type);
    p[1] = static_cast<std::byte>(evt.ord_status);
    p += 2;
    write_u64_le(evt.seq_num, p); p += 8;
    write_u16_le(evt.session_id, p); p += 2;
    write_u64_le(static_cast<std::uint64_t>(evt.price_micro), p); p += 8;
    write_u64_le(static_cast<std::uint64_t>(evt.qty), p); p += 8;
    write_u64_le(static_cast<std::uint64_t>(evt.cum_qty), p); p += 8;
    write_u64_le(evt.sending_time, p); p += 8;
    write_u64_le(evt.transact_time, p); p += 8;
    std::memcpy(p, evt.exec_id, core::WireExecEvent::id_capacity); p += core::WireExecEvent::id_capacity;
    p[0] = static_cast<std::byte>(evt.exec_id_len); p += 1;
    std::memcpy(p, evt.order_id, core::WireExecEvent::id_capacity); p += core::WireExecEvent::id_capacity;
    p[0] = static_cast<std::byte>(evt.order_id_len); p += 1;
    std::memcpy(p, evt.clord_id, core::WireExecEvent::id_capacity); p += core::WireExecEvent::id_capacity;
    p[0] = static_cast<std::byte>(evt.clord_id_len);
    return true;
}

inline bool deserialize_wire_exec_event(core::WireExecEvent& out, const std::byte* data, std::size_t len) noexcept {
    if (len != wire_exec_event_serialized_size) {
        return false;
    }
    const std::byte* p = data;
    out.exec_type = static_cast<std::uint8_t>(p[0]);
    out.ord_status = static_cast<std::uint8_t>(p[1]);
    p += 2;
    out.seq_num = read_u64_le(p); p += 8;
    out.session_id = read_u16_le(p); p += 2;
    out.price_micro = static_cast<std::int64_t>(read_u64_le(p)); p += 8;
    out.qty = static_cast<std::int64_t>(read_u64_le(p)); p += 8;
    out.cum_qty = static_cast<std::int64_t>(read_u64_le(p)); p += 8;
    out.sending_time = read_u64_le(p); p += 8;
    out.transact_time = read_u64_le(p); p += 8;
    std::memcpy(out.exec_id, p, core::WireExecEvent::id_capacity); p += core::WireExecEvent::id_capacity;
    out.exec_id_len = static_cast<std::uint8_t>(p[0]); p += 1;
    std::memcpy(out.order_id, p, core::WireExecEvent::id_capacity); p += core::WireExecEvent::id_capacity;
    out.order_id_len = static_cast<std::uint8_t>(p[0]); p += 1;
    std::memcpy(out.clord_id, p, core::WireExecEvent::id_capacity); p += core::WireExecEvent::id_capacity;
    out.clord_id_len = static_cast<std::uint8_t>(p[0]);
    return true;
}

inline std::string_view default_filename_prefix() noexcept {
    return "wire_capture_";
}

} // namespace persist
