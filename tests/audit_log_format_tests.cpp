#include <gtest/gtest.h>

#include <array>
#include <vector>

#include "persist/audit_log_format.hpp"

namespace {

core::Divergence sample_divergence() {
    core::Divergence d{};
    d.key = 0xAABBCCDDEEFF0011ull;
    d.type = core::DivergenceType::QuantityMismatch;
    d.internal_status = core::OrdStatus::Working;
    d.dropcopy_status = core::OrdStatus::Filled;
    d.internal_cum_qty = -10;
    d.dropcopy_cum_qty = 50;
    d.internal_avg_px = 1000001;
    d.dropcopy_avg_px = 999999;
    d.internal_ts = 111;
    d.dropcopy_ts = 222;
    return d;
}

core::SequenceGapEvent sample_gap() {
    core::SequenceGapEvent g{};
    g.source = core::Source::DropCopy;
    g.session_id = 12;
    g.expected_seq = 42;
    g.seen_seq = 45;
    g.kind = core::GapKind::Gap;
    g.detect_ts = 777;
    return g;
}

TEST(AuditLogFormat, DivergenceRoundTrip) {
    auto div = sample_divergence();
    std::array<std::byte, persist::record_size_from_payload(persist::divergence_payload_v1_size)> buf{};
    std::size_t written = 0;
    ASSERT_TRUE(persist::encode_divergence_record_v1(div, buf, written));
    persist::DecodedRecord decoded{};
    persist::AuditLogCounters ctrs{};
    auto err = persist::decode_record(std::span<const std::byte>(buf.data(), written), decoded, &ctrs);
    EXPECT_EQ(err, persist::DecodeError::Ok);
    EXPECT_EQ(decoded.type, persist::AuditRecordType::Divergence);
    EXPECT_EQ(decoded.schema_version, persist::audit_schema_version_v1);
    EXPECT_EQ(decoded.divergence.key, div.key);
    EXPECT_EQ(decoded.divergence.type, div.type);
    EXPECT_EQ(decoded.divergence.internal_status, div.internal_status);
    EXPECT_EQ(decoded.divergence.dropcopy_status, div.dropcopy_status);
    EXPECT_EQ(decoded.divergence.internal_cum_qty, div.internal_cum_qty);
    EXPECT_EQ(decoded.divergence.dropcopy_cum_qty, div.dropcopy_cum_qty);
    EXPECT_EQ(decoded.divergence.internal_avg_px, div.internal_avg_px);
    EXPECT_EQ(decoded.divergence.dropcopy_avg_px, div.dropcopy_avg_px);
    EXPECT_EQ(decoded.divergence.internal_ts, div.internal_ts);
    EXPECT_EQ(decoded.divergence.dropcopy_ts, div.dropcopy_ts);
    EXPECT_EQ(ctrs.audit_parse_errors_version_mismatch.load(), 0);
}

TEST(AuditLogFormat, GapRoundTrip) {
    auto gap = sample_gap();
    std::array<std::byte, persist::record_size_from_payload(persist::gap_payload_v1_size)> buf{};
    std::size_t written = 0;
    ASSERT_TRUE(persist::encode_gap_record_v1(gap, buf, written));
    persist::DecodedRecord decoded{};
    persist::AuditLogCounters ctrs{};
    auto err = persist::decode_record(std::span<const std::byte>(buf.data(), written), decoded, &ctrs);
    EXPECT_EQ(err, persist::DecodeError::Ok);
    EXPECT_EQ(decoded.type, persist::AuditRecordType::SequenceGap);
    EXPECT_EQ(decoded.gap.source, gap.source);
    EXPECT_EQ(decoded.gap.kind, gap.kind);
    EXPECT_EQ(decoded.gap.session_id, gap.session_id);
    EXPECT_EQ(decoded.gap.expected_seq, gap.expected_seq);
    EXPECT_EQ(decoded.gap.seen_seq, gap.seen_seq);
    EXPECT_EQ(decoded.gap.detect_ts, gap.detect_ts);
}

TEST(AuditLogFormat, SchemaMismatchRejected) {
    auto div = sample_divergence();
    std::array<std::byte, persist::record_size_from_payload(persist::divergence_payload_v1_size)> buf{};
    std::size_t written = 0;
    ASSERT_TRUE(persist::encode_divergence_record_v1(div, buf, written));
    // Overwrite schema version to 2 ( > current).
    persist::store_le16(2, buf.data() + persist::header_size);
    const auto new_crc = persist::compute_record_crc(buf.data(),
                                                     persist::header_size,
                                                     buf.data() + persist::header_size,
                                                     persist::divergence_payload_v1_size);
    persist::store_le32(new_crc, buf.data() + persist::header_size + persist::divergence_payload_v1_size);
    persist::AuditLogCounters ctrs{};
    persist::DecodedRecord decoded{};
    auto err = persist::decode_record(std::span<const std::byte>(buf.data(), written), decoded, &ctrs);
    EXPECT_EQ(err, persist::DecodeError::VersionMismatch);
    EXPECT_EQ(ctrs.audit_parse_errors_version_mismatch.load(), 1);
}

TEST(AuditLogFormat, InvalidLengthRejected) {
    auto gap = sample_gap();
    std::array<std::byte, persist::record_size_from_payload(persist::gap_payload_v1_size)> buf{};
    std::size_t written = 0;
    ASSERT_TRUE(persist::encode_gap_record_v1(gap, buf, written));
    // Tamper payload_len to mismatch expected size.
    persist::store_le32(static_cast<std::uint32_t>(persist::gap_payload_v1_size - 1), buf.data() + 4);
    const auto new_crc = persist::compute_record_crc(buf.data(),
                                                     persist::header_size,
                                                     buf.data() + persist::header_size,
                                                     persist::gap_payload_v1_size);
    persist::store_le32(new_crc, buf.data() + persist::header_size + persist::gap_payload_v1_size);
    persist::AuditLogCounters ctrs{};
    persist::DecodedRecord decoded{};
    auto err = persist::decode_record(std::span<const std::byte>(buf.data(), written), decoded, &ctrs);
    EXPECT_EQ(err, persist::DecodeError::InvalidLength);
    EXPECT_EQ(ctrs.audit_parse_errors_invalid_length.load(), 1);
}

TEST(AuditLogFormat, TruncatedGracefulEof) {
    std::array<std::byte, 6> tiny{};
    persist::AuditLogCounters ctrs{};
    persist::DecodedRecord decoded{};
    auto err = persist::decode_record(tiny, decoded, &ctrs);
    EXPECT_EQ(err, persist::DecodeError::TruncatedAtEnd);
    EXPECT_EQ(ctrs.audit_parse_errors_truncated.load(), 1);
}

TEST(AuditLogFormat, CrcValidation) {
    auto div = sample_divergence();
    std::array<std::byte, persist::record_size_from_payload(persist::divergence_payload_v1_size)> buf{};
    std::size_t written = 0;
    ASSERT_TRUE(persist::encode_divergence_record_v1(div, buf, written));
    const auto stored_crc = persist::load_le32(buf.data() + persist::header_size + persist::divergence_payload_v1_size);
    const auto recomputed_crc = persist::compute_record_crc(buf.data(), persist::header_size,
                                                            buf.data() + persist::header_size, persist::divergence_payload_v1_size);
    EXPECT_EQ(stored_crc, recomputed_crc);

    // Corrupt one byte and expect CRC failure.
    buf[persist::header_size + 5] = std::byte{0xFF};
    persist::AuditLogCounters ctrs{};
    persist::DecodedRecord decoded{};
    auto err = persist::decode_record(std::span<const std::byte>(buf.data(), written), decoded, &ctrs);
    EXPECT_EQ(err, persist::DecodeError::InvalidCrc);
    EXPECT_EQ(ctrs.audit_parse_errors_crc.load(), 1);
}

} // namespace
