#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "persist/crc32c.hpp"

namespace persist {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
constexpr std::uint32_t kMagic           = 0x5245434Eu; // 'RECN'
constexpr std::uint8_t  kVersion         = 1;
constexpr std::uint32_t kMaxPayloadBytes = 1u << 20;    // 1 MiB

// ---------------------------------------------------------------------------
// RecordType
// ---------------------------------------------------------------------------
enum class RecordType : std::uint8_t {
    ExecEvent       = 1,
    DivergenceEvent = 2,
    SnapshotChunk   = 3,
    SnapshotMeta    = 4,
};

// ---------------------------------------------------------------------------
// RecordHeaderV1 — packed struct with portable packing for exact on-disk layout
// ---------------------------------------------------------------------------
// On-disk layout (all little-endian):
//   [0..3]   magic    uint32
//   [4]      version  uint8
//   [5]      type     uint8
//   [6..7]   flags    uint16
//   [8..11]  length   uint32
//   [12..19] seqno    uint64
//   [20..23] crc32c   uint32
// Total: 24 bytes
constexpr std::size_t kHeaderSize = 24;

#if defined(_MSC_VER)
#  pragma pack(push, 1)
#endif

struct
#if defined(__GNUC__) || defined(__clang__)
__attribute__((packed))
#endif
RecordHeaderV1 {
    std::uint32_t magic;
    std::uint8_t  version;
    std::uint8_t  type;
    std::uint16_t flags;
    std::uint32_t length;
    std::uint64_t seqno;
    std::uint32_t crc32c;
};

#if defined(_MSC_VER)
#  pragma pack(pop)
#endif

static_assert(sizeof(RecordHeaderV1) == kHeaderSize,
              "RecordHeaderV1 must be exactly 24 bytes");

// ---------------------------------------------------------------------------
// Alignment helpers
// ---------------------------------------------------------------------------
constexpr std::size_t align8(std::size_t n) noexcept {
    return (n + 7u) & ~std::size_t{7};
}

constexpr std::size_t padding_for(std::size_t payload_len) noexcept {
    return align8(payload_len) - payload_len;
}

// ---------------------------------------------------------------------------
// Little-endian serialization helpers (portable, no UB)
// ---------------------------------------------------------------------------
namespace detail {

inline void write_le16(std::byte* dst, std::uint16_t v) noexcept {
    dst[0] = static_cast<std::byte>(v & 0xFF);
    dst[1] = static_cast<std::byte>((v >> 8) & 0xFF);
}
inline void write_le32(std::byte* dst, std::uint32_t v) noexcept {
    dst[0] = static_cast<std::byte>(v & 0xFF);
    dst[1] = static_cast<std::byte>((v >> 8) & 0xFF);
    dst[2] = static_cast<std::byte>((v >> 16) & 0xFF);
    dst[3] = static_cast<std::byte>((v >> 24) & 0xFF);
}
inline void write_le64(std::byte* dst, std::uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i) {
        dst[i] = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
    }
}
inline std::uint16_t read_le16(const std::byte* src) noexcept {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(src[0]) |
        (static_cast<std::uint16_t>(src[1]) << 8));
}
inline std::uint32_t read_le32(const std::byte* src) noexcept {
    return static_cast<std::uint32_t>(src[0]) |
           (static_cast<std::uint32_t>(src[1]) << 8) |
           (static_cast<std::uint32_t>(src[2]) << 16) |
           (static_cast<std::uint32_t>(src[3]) << 24);
}
inline std::uint64_t read_le64(const std::byte* src) noexcept {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(src[i]) << (8 * i);
    }
    return v;
}

} // namespace detail

// ---------------------------------------------------------------------------
// DecodeStatus
// ---------------------------------------------------------------------------
enum class DecodeStatus {
    Ok,
    NeedMoreData,
    BadMagic,
    BadVersion,
    BadLength,
    BadCrc,
};

// ---------------------------------------------------------------------------
// DecodedRecordView
// ---------------------------------------------------------------------------
struct DecodedRecordView {
    RecordType     type;
    std::uint16_t  flags;
    std::uint32_t  length;
    std::uint64_t  seqno;
    const std::byte* payload;
    std::size_t    total_bytes; // header + payload + padding
};

// ---------------------------------------------------------------------------
// encode_record_v1
// ---------------------------------------------------------------------------
[[nodiscard]] inline bool encode_record_v1(
    RecordType      type,
    std::uint16_t   flags,
    std::uint64_t   seqno,
    const std::byte* payload,
    std::uint32_t   payload_len,
    std::byte*      out_buf,
    std::size_t     out_cap,
    std::size_t&    out_bytes_written
) noexcept {
    out_bytes_written = 0;

    if (payload_len > kMaxPayloadBytes) return false;
    if (!payload && payload_len > 0) return false;

    const std::size_t pad   = padding_for(payload_len);
    const std::size_t total = kHeaderSize + payload_len + pad;
    if (out_cap < total) return false;
    if (!out_buf) return false;

    // Compute CRC-32C over payload only
    const std::uint32_t crc = crc32c(payload, payload_len);

    // Write header fields in little-endian
    std::byte* p = out_buf;
    detail::write_le32(p,      kMagic);           p += 4;
    *p = static_cast<std::byte>(kVersion);         p += 1;
    *p = static_cast<std::byte>(type);             p += 1;
    detail::write_le16(p,      flags);             p += 2;
    detail::write_le32(p,      payload_len);       p += 4;
    detail::write_le64(p,      seqno);             p += 8;
    detail::write_le32(p,      crc);               p += 4;

    // Payload
    if (payload_len > 0) {
        std::memcpy(p, payload, payload_len);
        p += payload_len;
    }

    // Padding zeros
    for (std::size_t i = 0; i < pad; ++i) {
        *p++ = std::byte{0};
    }

    out_bytes_written = total;
    return true;
}

// ---------------------------------------------------------------------------
// decode_record_v1
// ---------------------------------------------------------------------------
[[nodiscard]] inline DecodeStatus decode_record_v1(
    const std::byte* data,
    std::size_t      size,
    DecodedRecordView& out
) noexcept {
    if (size < kHeaderSize) return DecodeStatus::NeedMoreData;
    if (!data) return DecodeStatus::NeedMoreData;

    const std::uint32_t magic   = detail::read_le32(data);
    if (magic != kMagic) return DecodeStatus::BadMagic;

    const std::uint8_t version  = static_cast<std::uint8_t>(data[4]);
    if (version != kVersion) return DecodeStatus::BadVersion;

    const std::uint8_t  rtype   = static_cast<std::uint8_t>(data[5]);
    const std::uint16_t flags   = detail::read_le16(data + 6);
    const std::uint32_t length  = detail::read_le32(data + 8);
    const std::uint64_t seqno   = detail::read_le64(data + 12);
    const std::uint32_t crc_val = detail::read_le32(data + 20);

    if (length > kMaxPayloadBytes) return DecodeStatus::BadLength;

    const std::size_t pad   = padding_for(length);
    const std::size_t total = kHeaderSize + length + pad;
    if (size < total) return DecodeStatus::NeedMoreData;

    // Validate CRC-32C over payload only
    const std::uint32_t computed = crc32c(data + kHeaderSize, length);
    if (computed != crc_val) return DecodeStatus::BadCrc;

    out.type        = static_cast<RecordType>(rtype);
    out.flags       = flags;
    out.length      = length;
    out.seqno       = seqno;
    out.payload     = data + kHeaderSize;
    out.total_bytes = total;

    return DecodeStatus::Ok;
}

// ---------------------------------------------------------------------------
// ScanStats + scan_records
// ---------------------------------------------------------------------------
struct ScanStats {
    std::size_t records_ok    = 0;
    std::size_t records_bad   = 0;
    std::size_t bytes_consumed = 0;
    std::size_t first_bad_offset = static_cast<std::size_t>(-1);
};

/// Iterate a buffer of concatenated records. Stops on NeedMoreData (partial
/// trailing record), on the first non-Ok decode status (for example BadMagic,
/// BadVersion, BadLength, BadCrc), or after the buffer is exhausted.
inline ScanStats scan_records(const std::byte* data, std::size_t size) noexcept {
    ScanStats stats{};
    if (!data && size > 0) return stats;
    std::size_t offset = 0;

    while (offset < size) {
        DecodedRecordView view{};
        const DecodeStatus st = decode_record_v1(data + offset, size - offset, view);

        if (st == DecodeStatus::NeedMoreData) {
            break; // partial trailing record — stop
        }
        if (st == DecodeStatus::Ok) {
            ++stats.records_ok;
            offset += view.total_bytes;
        } else {
            ++stats.records_bad;
            if (stats.first_bad_offset == static_cast<std::size_t>(-1)) {
                stats.first_bad_offset = offset;
            }
            break; // cannot skip — unknown frame boundary
        }
    }

    stats.bytes_consumed = offset;
    return stats;
}

} // namespace persist
