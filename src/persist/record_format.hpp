#pragma once
// FX-8001: Binary journal record format (v1)
//
// On-disk layout (little-endian):
//   [0..4)    magic      0x5245434E ("RECN")
//   [4..5)    version    1
//   [5..6)    type       RecordType enum
//   [6..8)    flags      application-defined
//   [8..12)   length     payload byte count
//   [12..20)  seqno      monotonic sequence number
//   [20..24)  crc32c     CRC-32C over bytes [0..20) ++ payload
//   [24 .. 24+length)    payload bytes
//   Padding to 8-byte alignment with zeros.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "persist/crc32c.hpp"

namespace persist {

static constexpr std::uint32_t kMagic            = 0x5245434Eu; // "RECN"
static constexpr std::uint8_t  kVersion           = 1;
static constexpr std::size_t   kHeaderSize        = 24;
static constexpr std::uint32_t kMaxPayloadBytes   = 1u << 20; // 1 MiB
static constexpr std::size_t   kAlignment         = 8;

enum class RecordType : std::uint8_t {
    ExecEvent        = 1,
    DivergenceEvent  = 2,
    SnapshotChunk    = 3,
    SnapshotMeta     = 4,
};

// ── encode ──────────────────────────────────────────────────────────────────

struct EncodeResult {
    std::size_t total_bytes;
    bool ok;
};

inline constexpr std::size_t align_up(std::size_t n) noexcept {
    return (n + kAlignment - 1) & ~(kAlignment - 1);
}

inline constexpr std::size_t record_size(std::uint32_t payload_len) noexcept {
    return align_up(kHeaderSize + payload_len);
}

/// Encode a v1 record into @p buf (caller must provide record_size(payload_len) bytes).
inline EncodeResult encode_record_v1(std::byte* buf,
                                     RecordType type,
                                     std::uint16_t flags,
                                     std::uint64_t seqno,
                                     const std::byte* payload,
                                     std::uint32_t payload_len) noexcept {
    if (payload_len > kMaxPayloadBytes) return {0, false};

    const std::size_t total = record_size(payload_len);

    // header
    std::uint32_t m = kMagic;
    std::memcpy(buf,      &m, 4);
    buf[4] = static_cast<std::byte>(kVersion);
    buf[5] = static_cast<std::byte>(type);
    std::memcpy(buf + 6,  &flags, 2);
    std::memcpy(buf + 8,  &payload_len, 4);
    std::memcpy(buf + 12, &seqno, 8);

    // payload
    if (payload_len > 0 && payload)
        std::memcpy(buf + kHeaderSize, payload, payload_len);

    // zero padding
    const std::size_t pad_start = kHeaderSize + payload_len;
    if (total > pad_start)
        std::memset(buf + pad_start, 0, total - pad_start);

    // CRC-32C over header[0..20) + payload
    std::uint32_t crc = crc32c(buf, 20);
    if (payload_len > 0)
        crc = crc32c(buf + kHeaderSize, payload_len, crc);
    std::memcpy(buf + 20, &crc, 4);

    return {total, true};
}

// ── decode ──────────────────────────────────────────────────────────────────

enum class DecodeStatus { Ok, NeedMoreData, Corrupt };

struct DecodeResult {
    DecodeStatus   status        = DecodeStatus::NeedMoreData;
    std::size_t    bytes_consumed = 0;
    RecordType     type          = {};
    std::uint16_t  flags         = 0;
    std::uint64_t  seqno         = 0;
    const std::byte* payload     = nullptr;
    std::uint32_t  payload_len   = 0;
};

/// Decode one v1 record starting at @p data[0..data_len).
inline DecodeResult decode_record_v1(const std::byte* data,
                                     std::size_t data_len) noexcept {
    DecodeResult r{};
    if (data_len < kHeaderSize) return r; // NeedMoreData

    std::uint32_t magic{};
    std::memcpy(&magic, data, 4);
    if (magic != kMagic) { r.status = DecodeStatus::Corrupt; return r; }

    if (static_cast<std::uint8_t>(data[4]) != kVersion) {
        r.status = DecodeStatus::Corrupt; return r;
    }

    std::uint32_t plen{};
    std::memcpy(&plen, data + 8, 4);
    if (plen > kMaxPayloadBytes) { r.status = DecodeStatus::Corrupt; return r; }

    const std::size_t total = record_size(plen);
    if (data_len < total) return r; // NeedMoreData

    // verify CRC
    std::uint32_t stored_crc{};
    std::memcpy(&stored_crc, data + 20, 4);

    std::uint32_t crc = crc32c(data, 20);
    if (plen > 0)
        crc = crc32c(data + kHeaderSize, plen, crc);

    if (crc != stored_crc) { r.status = DecodeStatus::Corrupt; return r; }

    // success
    r.status         = DecodeStatus::Ok;
    r.bytes_consumed = total;
    r.type           = static_cast<RecordType>(static_cast<std::uint8_t>(data[5]));
    std::memcpy(&r.flags, data + 6, 2);
    std::memcpy(&r.seqno, data + 12, 8);
    r.payload        = (plen > 0) ? data + kHeaderSize : nullptr;
    r.payload_len    = plen;
    return r;
}

} // namespace persist
