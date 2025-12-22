#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#include "core/wire_exec_event.hpp"
#include "persist/endianness.hpp"
#include "util/crc32c.hpp"

namespace persist {

// Wire log on-disk format (v1)
// File header (24 bytes, little-endian):
//   0x00: u32 magic          = 'W' 'I' 'R' 'E' (0x57495245)
//   0x04: u16 format_version = 1
//   0x06: u16 header_size    = 24
//   0x08: u32 endian_marker  = 0x01020304 (used to detect mismatched endian)
//   0x0C: u32 payload_size   = 151 (WireExecEvent serialized size)
//   0x10: u32 reserved       = 0
//   0x14: u32 header_crc32c  = CRC32C over bytes [0x00..0x13]
//
// Record framing (per record, little-endian):
//   0x00: u32 payload_len    (must equal payload_size, 151)
//   0x04: u64 capture_ts_ns  (system clock timestamp when captured)
//   0x0C: payload bytes      (WireExecEvent serialized)
//   0x0C + payload_len: u32 crc32c over [payload_len_le][capture_ts_le][payload]
//   Total record size (WireExecEvent payload): 4 + 8 + 151 + 4 = 167 bytes.
//
// WireExecEvent serialization (151 bytes):
//   Offset Size Field
//   0x00   1    exec_type
//   0x01   1    ord_status
//   0x02   8    seq_num (u64 LE)
//   0x0A   2    session_id (u16 LE)
//   0x0C   8    price_micro (i64 LE)
//   0x14   8    qty (i64 LE)
//   0x1C   8    cum_qty (i64 LE)
//   0x24   8    sending_time (u64 LE)
//   0x2C   8    transact_time (u64 LE)
//   0x34   32   exec_id bytes
//   0x54   1    exec_id_len
//   0x55   32   order_id bytes
//   0x75   1    order_id_len
//   0x76   32   clord_id bytes
//   0x96   1    clord_id_len

inline constexpr std::uint32_t wire_log_magic = 0x57495245u; // "WIRE"
inline constexpr std::uint16_t wire_log_format_version = 1;
inline constexpr std::uint32_t wire_log_endian_marker = 0x01020304u;
inline constexpr std::size_t wire_log_header_size = 24;
inline constexpr std::size_t wire_log_max_payload_size = 16 * 1024;
inline constexpr std::size_t wire_exec_event_wire_size = 151;
inline constexpr std::string_view default_filename_prefix() noexcept { return "wire_capture_"; }
static_assert(wire_exec_event_wire_size == 151, "WireExecEvent on-disk size must remain fixed at 151 bytes");

inline std::uint32_t crc32c_bytes(std::span<const std::byte> payload) noexcept {
    return util::Crc32c::compute(payload.data(), payload.size());
}

inline std::uint16_t read_u16_le(const std::uint8_t* p) noexcept {
    return from_le16(static_cast<std::uint16_t>(p[0]) |
                     static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[1]) << 8));
}

inline std::uint32_t read_u32_le(const std::uint8_t* p) noexcept {
    return from_le32(static_cast<std::uint32_t>(p[0]) |
                     (static_cast<std::uint32_t>(p[1]) << 8) |
                     (static_cast<std::uint32_t>(p[2]) << 16) |
                     (static_cast<std::uint32_t>(p[3]) << 24));
}

inline std::uint64_t read_u64_le(const std::uint8_t* p) noexcept {
    return from_le64(static_cast<std::uint64_t>(p[0]) |
                     (static_cast<std::uint64_t>(p[1]) << 8) |
                     (static_cast<std::uint64_t>(p[2]) << 16) |
                     (static_cast<std::uint64_t>(p[3]) << 24) |
                     (static_cast<std::uint64_t>(p[4]) << 32) |
                     (static_cast<std::uint64_t>(p[5]) << 40) |
                     (static_cast<std::uint64_t>(p[6]) << 48) |
                     (static_cast<std::uint64_t>(p[7]) << 56));
}

inline std::int64_t read_i64_le(const std::uint8_t* p) noexcept {
    return std::bit_cast<std::int64_t>(read_u64_le(p));
}

inline void write_u16_le(std::uint8_t* p, std::uint16_t v) noexcept {
    const auto le = to_le16(v);
    p[0] = static_cast<std::uint8_t>(le & 0xFFu);
    p[1] = static_cast<std::uint8_t>((le >> 8) & 0xFFu);
}

inline void write_u32_le(std::uint8_t* p, std::uint32_t v) noexcept {
    const auto le = to_le32(v);
    p[0] = static_cast<std::uint8_t>(le & 0xFFu);
    p[1] = static_cast<std::uint8_t>((le >> 8) & 0xFFu);
    p[2] = static_cast<std::uint8_t>((le >> 16) & 0xFFu);
    p[3] = static_cast<std::uint8_t>((le >> 24) & 0xFFu);
}

inline void write_u64_le(std::uint8_t* p, std::uint64_t v) noexcept {
    const auto le = to_le64(v);
    p[0] = static_cast<std::uint8_t>(le & 0xFFu);
    p[1] = static_cast<std::uint8_t>((le >> 8) & 0xFFu);
    p[2] = static_cast<std::uint8_t>((le >> 16) & 0xFFu);
    p[3] = static_cast<std::uint8_t>((le >> 24) & 0xFFu);
    p[4] = static_cast<std::uint8_t>((le >> 32) & 0xFFu);
    p[5] = static_cast<std::uint8_t>((le >> 40) & 0xFFu);
    p[6] = static_cast<std::uint8_t>((le >> 48) & 0xFFu);
    p[7] = static_cast<std::uint8_t>((le >> 56) & 0xFFu);
}

inline void write_i64_le(std::uint8_t* p, std::int64_t v) noexcept {
    write_u64_le(p, std::bit_cast<std::uint64_t>(v));
}

inline std::size_t serialize_wire_exec_event(const core::WireExecEvent& evt, std::uint8_t* buffer) noexcept {
    std::uint8_t* p = buffer;
    p[0] = evt.exec_type;
    p[1] = evt.ord_status;
    p += 2;

    write_u64_le(p, evt.seq_num);
    p += 8;
    write_u16_le(p, evt.session_id);
    p += 2;
    write_i64_le(p, evt.price_micro);
    p += 8;
    write_i64_le(p, evt.qty);
    p += 8;
    write_i64_le(p, evt.cum_qty);
    p += 8;
    write_u64_le(p, evt.sending_time);
    p += 8;
    write_u64_le(p, evt.transact_time);
    p += 8;

    std::memcpy(p, evt.exec_id, core::WireExecEvent::id_capacity);
    p += core::WireExecEvent::id_capacity;
    p[0] = evt.exec_id_len;
    ++p;

    std::memcpy(p, evt.order_id, core::WireExecEvent::id_capacity);
    p += core::WireExecEvent::id_capacity;
    p[0] = evt.order_id_len;
    ++p;

    std::memcpy(p, evt.clord_id, core::WireExecEvent::id_capacity);
    p += core::WireExecEvent::id_capacity;
    p[0] = evt.clord_id_len;
    ++p;

    return static_cast<std::size_t>(p - buffer);
}

inline std::size_t deserialize_wire_exec_event(core::WireExecEvent& evt, const std::uint8_t* buffer) noexcept {
    const std::uint8_t* p = buffer;
    evt = {};
    evt.exec_type = p[0];
    evt.ord_status = p[1];
    p += 2;

    evt.seq_num = read_u64_le(p);
    p += 8;
    evt.session_id = read_u16_le(p);
    p += 2;
    evt.price_micro = read_i64_le(p);
    p += 8;
    evt.qty = read_i64_le(p);
    p += 8;
    evt.cum_qty = read_i64_le(p);
    p += 8;
    evt.sending_time = read_u64_le(p);
    p += 8;
    evt.transact_time = read_u64_le(p);
    p += 8;

    std::memcpy(evt.exec_id, p, core::WireExecEvent::id_capacity);
    p += core::WireExecEvent::id_capacity;
    evt.exec_id_len = *p++;

    std::memcpy(evt.order_id, p, core::WireExecEvent::id_capacity);
    p += core::WireExecEvent::id_capacity;
    evt.order_id_len = *p++;

    std::memcpy(evt.clord_id, p, core::WireExecEvent::id_capacity);
    p += core::WireExecEvent::id_capacity;
    evt.clord_id_len = *p++;

    return static_cast<std::size_t>(p - buffer);
}

struct WireLogHeaderFields {
    std::uint32_t magic{wire_log_magic};
    std::uint16_t format_version{wire_log_format_version};
    std::uint16_t header_size{static_cast<std::uint16_t>(wire_log_header_size)};
    std::uint32_t endian_marker{wire_log_endian_marker};
    std::uint32_t payload_size{static_cast<std::uint32_t>(wire_exec_event_wire_size)};
    std::uint32_t reserved{0};
};

inline std::uint32_t compute_header_crc(const WireLogHeaderFields& h) noexcept {
    std::array<std::byte, wire_log_header_size - sizeof(std::uint32_t)> buf{};
    std::uint8_t* p = reinterpret_cast<std::uint8_t*>(buf.data());
    write_u32_le(p, h.magic); p += 4;
    write_u16_le(p, h.format_version); p += 2;
    write_u16_le(p, h.header_size); p += 2;
    write_u32_le(p, h.endian_marker); p += 4;
    write_u32_le(p, h.payload_size); p += 4;
    write_u32_le(p, h.reserved);
    return util::Crc32c::compute(buf.data(), buf.size());
}

inline void encode_header(const WireLogHeaderFields& h, std::array<std::byte, wire_log_header_size>& out) noexcept {
    std::uint8_t* p = reinterpret_cast<std::uint8_t*>(out.data());
    write_u32_le(p, h.magic); p += 4;
    write_u16_le(p, h.format_version); p += 2;
    write_u16_le(p, h.header_size); p += 2;
    write_u32_le(p, h.endian_marker); p += 4;
    write_u32_le(p, h.payload_size); p += 4;
    write_u32_le(p, h.reserved); p += 4;
    write_u32_le(p, compute_header_crc(h));
}

inline bool parse_header(std::span<const std::byte> bytes, WireLogHeaderFields& out) noexcept {
    if (bytes.size() < wire_log_header_size) {
        return false;
    }
    const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
    WireLogHeaderFields tmp{};
    tmp.magic = read_u32_le(data);
    tmp.format_version = read_u16_le(data + 4);
    tmp.header_size = read_u16_le(data + 6);
    tmp.endian_marker = read_u32_le(data + 8);
    tmp.payload_size = read_u32_le(data + 12);
    tmp.reserved = read_u32_le(data + 16);
    const auto header_crc = read_u32_le(data + 20);

    if (tmp.magic != wire_log_magic) {
        return false;
    }
    if (tmp.header_size != wire_log_header_size) {
        return false;
    }
    if (tmp.format_version > wire_log_format_version) {
        return false;
    }
    if (tmp.endian_marker != wire_log_endian_marker) {
        return false;
    }
    if (tmp.payload_size != wire_exec_event_wire_size) {
        return false;
    }
    if (compute_header_crc(tmp) != header_crc) {
        return false;
    }
    out = tmp;
    return true;
}

struct WireRecordView {
    std::uint32_t payload_length{0};
    std::uint64_t capture_ts{0};
    std::span<const std::byte> payload{};
    std::uint32_t checksum{0};
    std::span<const std::byte> frame_bytes{};
};

inline constexpr std::size_t framed_size(std::size_t payload_len) noexcept {
    return sizeof(std::uint32_t) + sizeof(std::uint64_t) + payload_len + sizeof(std::uint32_t);
}

inline std::uint32_t crc32c_record(const std::byte* length_field_le,
                                   const std::byte* capture_ts_le,
                                   std::span<const std::byte> payload) noexcept {
    std::uint32_t crc = util::Crc32c::initial;
    crc = util::Crc32c::update(crc, length_field_le, sizeof(std::uint32_t));
    crc = util::Crc32c::update(crc, capture_ts_le, sizeof(std::uint64_t));
    crc = util::Crc32c::update(crc, payload.data(), payload.size());
    return util::Crc32c::finalize(crc);
}

struct RecordFields {
    std::array<std::byte, sizeof(std::uint32_t)> length_le{};
    std::array<std::byte, sizeof(std::uint64_t)> capture_ts_le{};
    std::array<std::byte, sizeof(std::uint32_t)> checksum_le{};
};

inline void encode_record(std::span<const std::byte> payload,
                          std::uint64_t capture_ts,
                          RecordFields& out) noexcept {
    write_u32_le(reinterpret_cast<std::uint8_t*>(out.length_le.data()), static_cast<std::uint32_t>(payload.size()));
    write_u64_le(reinterpret_cast<std::uint8_t*>(out.capture_ts_le.data()), capture_ts);
    const auto crc = crc32c_record(out.length_le.data(), out.capture_ts_le.data(), payload);
    write_u32_le(reinterpret_cast<std::uint8_t*>(out.checksum_le.data()), crc);
}

inline bool parse_record(const std::byte* data,
                         std::size_t size,
                         WireRecordView& out) noexcept {
    if (size < framed_size(0)) {
        return false;
    }
    const std::uint32_t payload_len = read_u32_le(reinterpret_cast<const std::uint8_t*>(data));
    const std::size_t total = framed_size(payload_len);
    if (total > size) {
        return false;
    }
    const std::uint64_t capture_ts = read_u64_le(reinterpret_cast<const std::uint8_t*>(data + sizeof(std::uint32_t)));
    const std::byte* payload_ptr = data + sizeof(std::uint32_t) + sizeof(std::uint64_t);
    const std::uint32_t checksum = read_u32_le(reinterpret_cast<const std::uint8_t*>(payload_ptr + payload_len));

    out.payload_length = payload_len;
    out.capture_ts = capture_ts;
    out.payload = std::span<const std::byte>(payload_ptr, payload_len);
    out.checksum = checksum;
    out.frame_bytes = std::span<const std::byte>(data, total);
    return true;
}

inline bool validate_record(const WireRecordView& rec) noexcept {
    const auto* base = rec.frame_bytes.data();
    const auto computed = crc32c_record(base, base + sizeof(std::uint32_t), rec.payload);
    return computed == rec.checksum;
}

inline std::string_view filename_prefix() noexcept { return default_filename_prefix(); }

} // namespace persist
