#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "core/divergence.hpp"
#include "core/sequence_tracker.hpp"
#include "util/crc32c.hpp"
#include "persist/endianness.hpp"

namespace persist {

enum class AuditRecordType : std::uint32_t {
    Reserved = 0,
    Divergence = 1,
    SequenceGap = 2,
};

inline constexpr std::uint16_t audit_schema_version_v1 = 1;

// Payload sizes for schema v1 (bytes).
inline constexpr std::size_t divergence_payload_v1_size = 62;
inline constexpr std::size_t gap_payload_v1_size = 30;

inline constexpr std::size_t header_size = sizeof(std::uint32_t) * 2; // type + payload_len
inline constexpr std::size_t trailer_size = sizeof(std::uint32_t);    // crc32c

inline constexpr std::size_t record_size_from_payload(std::size_t payload_len) noexcept {
    return header_size + payload_len + trailer_size;
}

struct AuditLogCounters {
    // Recon producer drops (ring backpressure).
    std::atomic<std::uint64_t> audit_drop_divergence{0};
    std::atomic<std::uint64_t> audit_drop_gaps{0};

    // Writer drops while in degraded mode / drain-and-drop.
    std::atomic<std::uint64_t> writer_drop_divergence{0};
    std::atomic<std::uint64_t> writer_drop_gaps{0};

    // Writer / parsing errors.
    std::atomic<std::uint64_t> audit_io_errors{0};
    std::atomic<std::uint64_t> audit_recovery_attempts{0};
    std::atomic<std::uint64_t> audit_degraded_mode_time_ns{0};

    // Parse errors.
    std::atomic<std::uint64_t> audit_parse_errors_version_mismatch{0};
    std::atomic<std::uint64_t> audit_parse_errors_invalid_length{0};
    std::atomic<std::uint64_t> audit_parse_errors_truncated{0};
    std::atomic<std::uint64_t> audit_parse_errors_crc{0};
    std::atomic<std::uint64_t> audit_parse_errors_invalid_type{0};
};

enum class DecodeError {
    Ok = 0,
    TruncatedAtEnd,
    InvalidType,
    VersionMismatch,
    InvalidLength,
    InvalidCrc,
};

struct DecodedRecord {
    AuditRecordType type{AuditRecordType::Reserved};
    core::Divergence divergence{};
    core::SequenceGapEvent gap{};
    std::uint16_t schema_version{0};
    std::uint32_t payload_len{0};
};

inline void store_le16(std::uint16_t v, std::byte* out) noexcept {
    const auto le = to_le16(v);
    out[0] = static_cast<std::byte>(le & 0xFFu);
    out[1] = static_cast<std::byte>((le >> 8) & 0xFFu);
}

inline void store_le32(std::uint32_t v, std::byte* out) noexcept {
    const auto le = to_le32(v);
    out[0] = static_cast<std::byte>(le & 0xFFu);
    out[1] = static_cast<std::byte>((le >> 8) & 0xFFu);
    out[2] = static_cast<std::byte>((le >> 16) & 0xFFu);
    out[3] = static_cast<std::byte>((le >> 24) & 0xFFu);
}

inline void store_le64(std::uint64_t v, std::byte* out) noexcept {
    const auto le = to_le64(v);
    out[0] = static_cast<std::byte>(le & 0xFFu);
    out[1] = static_cast<std::byte>((le >> 8) & 0xFFu);
    out[2] = static_cast<std::byte>((le >> 16) & 0xFFu);
    out[3] = static_cast<std::byte>((le >> 24) & 0xFFu);
    out[4] = static_cast<std::byte>((le >> 32) & 0xFFu);
    out[5] = static_cast<std::byte>((le >> 40) & 0xFFu);
    out[6] = static_cast<std::byte>((le >> 48) & 0xFFu);
    out[7] = static_cast<std::byte>((le >> 56) & 0xFFu);
}

inline std::uint16_t load_le16(const std::byte* p) noexcept {
    return from_le16(static_cast<std::uint16_t>(static_cast<std::uint8_t>(p[0])) |
                     static_cast<std::uint16_t>(static_cast<std::uint8_t>(p[1]) << 8));
}

inline std::uint32_t load_le32(const std::byte* p) noexcept {
    return from_le32(static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[0])) |
                     (static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[1])) << 8) |
                     (static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[2])) << 16) |
                     (static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[3])) << 24));
}

inline std::uint64_t load_le64(const std::byte* p) noexcept {
    return from_le64(static_cast<std::uint64_t>(static_cast<std::uint8_t>(p[0])) |
                     (static_cast<std::uint64_t>(static_cast<std::uint8_t>(p[1])) << 8) |
                     (static_cast<std::uint64_t>(static_cast<std::uint8_t>(p[2])) << 16) |
                     (static_cast<std::uint64_t>(static_cast<std::uint8_t>(p[3])) << 24) |
                     (static_cast<std::uint64_t>(static_cast<std::uint8_t>(p[4])) << 32) |
                     (static_cast<std::uint64_t>(static_cast<std::uint8_t>(p[5])) << 40) |
                     (static_cast<std::uint64_t>(static_cast<std::uint8_t>(p[6])) << 48) |
                     (static_cast<std::uint64_t>(static_cast<std::uint8_t>(p[7])) << 56));
}

inline bool encode_divergence_payload_v1(const core::Divergence& div,
                                         std::span<std::byte> out,
                                         std::size_t& written) noexcept {
    if (out.size() < divergence_payload_v1_size) {
        return false;
    }
    written = divergence_payload_v1_size;
    std::byte* p = out.data();
    store_le16(audit_schema_version_v1, p);            // schema
    p += 2;
    p[0] = static_cast<std::byte>(div.type);           // divergence type
    p[1] = static_cast<std::byte>(div.internal_status);// internal status
    p[2] = static_cast<std::byte>(div.dropcopy_status);// dropcopy status
    p[3] = std::byte{0};                               // reserved
    p += 4;
    store_le64(div.key, p); p += 8;
    store_le64(static_cast<std::uint64_t>(div.internal_cum_qty), p); p += 8;
    store_le64(static_cast<std::uint64_t>(div.dropcopy_cum_qty), p); p += 8;
    store_le64(static_cast<std::uint64_t>(div.internal_avg_px), p); p += 8;
    store_le64(static_cast<std::uint64_t>(div.dropcopy_avg_px), p); p += 8;
    store_le64(div.internal_ts, p); p += 8;
    store_le64(div.dropcopy_ts, p);
    return true;
}

inline bool encode_gap_payload_v1(const core::SequenceGapEvent& gap,
                                  std::span<std::byte> out,
                                  std::size_t& written) noexcept {
    if (out.size() < gap_payload_v1_size) {
        return false;
    }
    written = gap_payload_v1_size;
    std::byte* p = out.data();
    store_le16(audit_schema_version_v1, p); p += 2;
    p[0] = static_cast<std::byte>(gap.source); p += 1;
    p[0] = static_cast<std::byte>(gap.kind); p += 1;
    store_le16(gap.session_id, p); p += 2;
    store_le64(gap.expected_seq, p); p += 8;
    store_le64(gap.seen_seq, p); p += 8;
    store_le64(gap.detect_ts, p);
    return true;
}

inline std::uint32_t crc32c_bytes(std::span<const std::byte> data) noexcept {
    return util::Crc32c::compute(data.data(), data.size());
}

inline std::uint32_t compute_record_crc(const std::byte* header_le,
                                        std::size_t header_len,
                                        const std::byte* payload,
                                        std::size_t payload_len) noexcept {
    std::uint32_t crc = util::Crc32c::initial;
    crc = util::Crc32c::update(crc, header_le, header_len);
    crc = util::Crc32c::update(crc, payload, payload_len);
    return util::Crc32c::finalize(crc);
}

inline std::size_t encode_record_header(AuditRecordType type,
                                        std::uint32_t payload_len,
                                        std::byte* out_header8) noexcept {
    store_le32(static_cast<std::uint32_t>(type), out_header8);
    store_le32(payload_len, out_header8 + 4);
    return header_size;
}

inline bool encode_divergence_record_v1(const core::Divergence& div,
                                        std::span<std::byte> out,
                                        std::size_t& written) noexcept {
    const std::size_t needed = record_size_from_payload(divergence_payload_v1_size);
    if (out.size() < needed) {
        return false;
    }
    std::byte* base = out.data();
    encode_record_header(AuditRecordType::Divergence, static_cast<std::uint32_t>(divergence_payload_v1_size), base);
    std::size_t payload_written = 0;
    if (!encode_divergence_payload_v1(div, std::span<std::byte>(base + header_size, divergence_payload_v1_size), payload_written)) {
        return false;
    }
    const auto crc = compute_record_crc(base, header_size, base + header_size, payload_written);
    store_le32(crc, base + header_size + payload_written);
    written = needed;
    return true;
}

inline bool encode_gap_record_v1(const core::SequenceGapEvent& gap,
                                 std::span<std::byte> out,
                                 std::size_t& written) noexcept {
    const std::size_t needed = record_size_from_payload(gap_payload_v1_size);
    if (out.size() < needed) {
        return false;
    }
    std::byte* base = out.data();
    encode_record_header(AuditRecordType::SequenceGap, static_cast<std::uint32_t>(gap_payload_v1_size), base);
    std::size_t payload_written = 0;
    if (!encode_gap_payload_v1(gap, std::span<std::byte>(base + header_size, gap_payload_v1_size), payload_written)) {
        return false;
    }
    const auto crc = compute_record_crc(base, header_size, base + header_size, payload_written);
    store_le32(crc, base + header_size + payload_written);
    written = needed;
    return true;
}

inline bool decode_divergence_v1(std::span<const std::byte> payload,
                                 core::Divergence& out) noexcept {
    if (payload.size() != divergence_payload_v1_size) {
        return false;
    }
    const std::byte* p = payload.data();
    out = {};
    const auto schema = load_le16(p); p += 2;
    if (schema != audit_schema_version_v1) {
        return false;
    }
    out.type = static_cast<core::DivergenceType>(static_cast<std::uint8_t>(p[0])); p += 1;
    out.internal_status = static_cast<core::OrdStatus>(static_cast<std::uint8_t>(p[0])); p += 1;
    out.dropcopy_status = static_cast<core::OrdStatus>(static_cast<std::uint8_t>(p[0])); p += 1;
    p += 1; // reserved
    out.key = load_le64(p); p += 8;
    out.internal_cum_qty = static_cast<std::int64_t>(load_le64(p)); p += 8;
    out.dropcopy_cum_qty = static_cast<std::int64_t>(load_le64(p)); p += 8;
    out.internal_avg_px = static_cast<std::int64_t>(load_le64(p)); p += 8;
    out.dropcopy_avg_px = static_cast<std::int64_t>(load_le64(p)); p += 8;
    out.internal_ts = load_le64(p); p += 8;
    out.dropcopy_ts = load_le64(p);
    return true;
}

inline bool decode_gap_v1(std::span<const std::byte> payload,
                          core::SequenceGapEvent& out) noexcept {
    if (payload.size() != gap_payload_v1_size) {
        return false;
    }
    const std::byte* p = payload.data();
    out = {};
    const auto schema = load_le16(p); p += 2;
    if (schema != audit_schema_version_v1) {
        return false;
    }
    out.source = static_cast<core::Source>(static_cast<std::uint8_t>(p[0])); p += 1;
    out.kind = static_cast<core::GapKind>(static_cast<std::uint8_t>(p[0])); p += 1;
    out.session_id = load_le16(p); p += 2;
    out.expected_seq = load_le64(p); p += 8;
    out.seen_seq = load_le64(p); p += 8;
    out.detect_ts = load_le64(p);
    return true;
}

inline DecodeError decode_record(std::span<const std::byte> data,
                                 DecodedRecord& out,
                                 AuditLogCounters* counters = nullptr) noexcept {
    if (data.size() < header_size) {
        if (counters) { counters->audit_parse_errors_truncated.fetch_add(1, std::memory_order_relaxed); }
        return DecodeError::TruncatedAtEnd;
    }
    const std::byte* header = data.data();
    const std::uint32_t type_le = load_le32(header);
    const std::uint32_t payload_len = load_le32(header + 4);
    const std::size_t total = record_size_from_payload(payload_len);
    if (data.size() < total) {
        if (counters) { counters->audit_parse_errors_truncated.fetch_add(1, std::memory_order_relaxed); }
        return DecodeError::TruncatedAtEnd;
    }
    const AuditRecordType type = static_cast<AuditRecordType>(type_le);
    if (type != AuditRecordType::Divergence && type != AuditRecordType::SequenceGap) {
        if (counters) { counters->audit_parse_errors_invalid_type.fetch_add(1, std::memory_order_relaxed); }
        return DecodeError::InvalidType;
    }
    const std::byte* payload = header + header_size;
    const std::byte* crc_ptr = payload + payload_len;
    const std::uint32_t crc_expected = load_le32(crc_ptr);
    const auto crc_computed = compute_record_crc(header, header_size, payload, payload_len);
    if (crc_expected != crc_computed) {
        if (counters) { counters->audit_parse_errors_crc.fetch_add(1, std::memory_order_relaxed); }
        return DecodeError::InvalidCrc;
    }

    out = {};
    out.type = type;
    out.payload_len = payload_len;
    const auto schema_version = load_le16(payload);
    out.schema_version = schema_version;
    if (schema_version > audit_schema_version_v1) {
        if (counters) { counters->audit_parse_errors_version_mismatch.fetch_add(1, std::memory_order_relaxed); }
        return DecodeError::VersionMismatch;
    }

    if (type == AuditRecordType::Divergence) {
        if (payload_len != divergence_payload_v1_size) {
            if (counters) { counters->audit_parse_errors_invalid_length.fetch_add(1, std::memory_order_relaxed); }
            return DecodeError::InvalidLength;
        }
        if (!decode_divergence_v1(std::span<const std::byte>(payload, payload_len), out.divergence)) {
            if (counters) { counters->audit_parse_errors_invalid_length.fetch_add(1, std::memory_order_relaxed); }
            return DecodeError::InvalidLength;
        }
    } else if (type == AuditRecordType::SequenceGap) {
        if (payload_len != gap_payload_v1_size) {
            if (counters) { counters->audit_parse_errors_invalid_length.fetch_add(1, std::memory_order_relaxed); }
            return DecodeError::InvalidLength;
        }
        if (!decode_gap_v1(std::span<const std::byte>(payload, payload_len), out.gap)) {
            if (counters) { counters->audit_parse_errors_invalid_length.fetch_add(1, std::memory_order_relaxed); }
            return DecodeError::InvalidLength;
        }
    }
    return DecodeError::Ok;
}

inline bool is_graceful_eof(DecodeError err) noexcept {
    return err == DecodeError::TruncatedAtEnd;
}

inline std::string_view audit_filename_prefix() noexcept { return "audit_"; }

} // namespace persist
