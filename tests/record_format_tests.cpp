#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "persist/record_format.hpp"

namespace {

using namespace persist;

// ── helpers ──────────────────────────────────────────────────────────────────

// Small payload for most tests.
static const std::byte kHello[] = {
    std::byte{'H'}, std::byte{'e'}, std::byte{'l'},
    std::byte{'l'}, std::byte{'o'}};

// Convenience: encode into a vector (aborts test on failure).
static void encode_into(std::vector<std::byte>& buf, RecordType type,
                        std::uint16_t flags, std::uint64_t seqno,
                        const std::byte* payload, std::uint32_t payload_len) {
    buf.resize(kHeaderSize + payload_len + 7); // worst-case
    std::size_t written = 0;
    bool ok = encode_record_v1(type, flags, seqno, payload, payload_len,
                               buf.data(), buf.size(), written);
    ASSERT_TRUE(ok);
    buf.resize(written);
}

static std::vector<std::byte> encode(RecordType type, std::uint16_t flags,
                                     std::uint64_t seqno,
                                     const std::byte* payload,
                                     std::uint32_t payload_len) {
    std::vector<std::byte> buf;
    encode_into(buf, type, flags, seqno, payload, payload_len);
    return buf;
}

// ── Compile-time checks ─────────────────────────────────────────────────────

TEST(RecordFormatConstants, HeaderSize) {
    EXPECT_EQ(kHeaderSize, 24u);
    EXPECT_EQ(sizeof(RecordHeaderV1), kHeaderSize);
}

TEST(RecordFormatConstants, Magic) {
    EXPECT_EQ(kMagic, 0x5245434Eu);
}

TEST(RecordFormatConstants, Align8) {
    EXPECT_EQ(align8(0), 0u);
    EXPECT_EQ(align8(1), 8u);
    EXPECT_EQ(align8(7), 8u);
    EXPECT_EQ(align8(8), 8u);
    EXPECT_EQ(align8(9), 16u);
    EXPECT_EQ(align8(24), 24u);
}

TEST(RecordFormatConstants, PaddingFor) {
    EXPECT_EQ(padding_for(0), 0u);
    EXPECT_EQ(padding_for(1), 7u);
    EXPECT_EQ(padding_for(5), 3u);
    EXPECT_EQ(padding_for(8), 0u);
    EXPECT_EQ(padding_for(9), 7u);
}

// ── CRC-32C basic sanity ────────────────────────────────────────────────────

TEST(CRC32C, EmptyInput) {
    EXPECT_EQ(crc32c(nullptr, 0), 0x00000000u);
}

TEST(CRC32C, KnownVector) {
    // CRC-32C of "123456789" is 0xE3069283
    const char* input = "123456789";
    auto crc = crc32c(reinterpret_cast<const std::byte*>(input), 9);
    EXPECT_EQ(crc, 0xE3069283u);
}

// ── Encode basic ────────────────────────────────────────────────────────────

TEST(EncodeRecord, BasicRoundTrip) {
    auto buf = encode(RecordType::ExecEvent, 0, 42, kHello, 5);
    // Header 24 + payload 5 + padding 3 = 32
    EXPECT_EQ(buf.size(), 32u);
}

TEST(EncodeRecord, EmptyPayload) {
    auto buf = encode(RecordType::SnapshotMeta, 0, 1, nullptr, 0);
    EXPECT_EQ(buf.size(), kHeaderSize); // no payload, no padding needed
}

TEST(EncodeRecord, PayloadTooLarge) {
    std::byte dummy{};
    std::array<std::byte, 64> out{};
    std::size_t written = 0;
    bool ok = encode_record_v1(RecordType::ExecEvent, 0, 1, &dummy,
                               kMaxPayloadBytes + 1, out.data(), out.size(),
                               written);
    EXPECT_FALSE(ok);
    EXPECT_EQ(written, 0u);
}

TEST(EncodeRecord, BufferTooSmall) {
    std::array<std::byte, 8> tiny{};
    std::size_t written = 0;
    bool ok = encode_record_v1(RecordType::ExecEvent, 0, 1, kHello, 5,
                               tiny.data(), tiny.size(), written);
    EXPECT_FALSE(ok);
    EXPECT_EQ(written, 0u);
}

// ── Decode basic ────────────────────────────────────────────────────────────

TEST(DecodeRecord, BasicRoundTrip) {
    auto buf = encode(RecordType::DivergenceEvent, 0, 99, kHello, 5);
    DecodedRecordView view{};
    auto st = decode_record_v1(buf.data(), buf.size(), view);
    EXPECT_EQ(st, DecodeStatus::Ok);
    EXPECT_EQ(view.type, RecordType::DivergenceEvent);
    EXPECT_EQ(view.flags, 0u);
    EXPECT_EQ(view.length, 5u);
    EXPECT_EQ(view.seqno, 99u);
    EXPECT_EQ(view.total_bytes, 32u);
    EXPECT_EQ(std::memcmp(view.payload, kHello, 5), 0);
}

TEST(DecodeRecord, NeedMoreDataShortHeader) {
    std::array<std::byte, 10> buf{};
    DecodedRecordView view{};
    EXPECT_EQ(decode_record_v1(buf.data(), buf.size(), view),
              DecodeStatus::NeedMoreData);
}

TEST(DecodeRecord, NeedMoreDataShortPayload) {
    auto buf = encode(RecordType::ExecEvent, 0, 1, kHello, 5);
    DecodedRecordView view{};
    // Provide only header + 2 bytes of payload (need 3 payload + 3 padding = 6 more)
    EXPECT_EQ(decode_record_v1(buf.data(), kHeaderSize + 2, view),
              DecodeStatus::NeedMoreData);
}

TEST(DecodeRecord, BadMagic) {
    auto buf = encode(RecordType::ExecEvent, 0, 1, kHello, 5);
    buf[0] = std::byte{0xFF}; // corrupt magic
    DecodedRecordView view{};
    EXPECT_EQ(decode_record_v1(buf.data(), buf.size(), view),
              DecodeStatus::BadMagic);
}

TEST(DecodeRecord, BadVersion) {
    auto buf = encode(RecordType::ExecEvent, 0, 1, kHello, 5);
    buf[4] = std::byte{99}; // bad version
    DecodedRecordView view{};
    EXPECT_EQ(decode_record_v1(buf.data(), buf.size(), view),
              DecodeStatus::BadVersion);
}

TEST(DecodeRecord, BadCrc) {
    auto buf = encode(RecordType::ExecEvent, 0, 1, kHello, 5);
    // Corrupt a payload byte
    buf[kHeaderSize] = std::byte{0xFF};
    DecodedRecordView view{};
    EXPECT_EQ(decode_record_v1(buf.data(), buf.size(), view),
              DecodeStatus::BadCrc);
}

TEST(DecodeRecord, BadLength) {
    auto buf = encode(RecordType::ExecEvent, 0, 1, kHello, 5);
    // Overwrite length field with value exceeding cap
    std::uint32_t huge = kMaxPayloadBytes + 1;
    detail::write_le32(buf.data() + 8, huge);
    DecodedRecordView view{};
    EXPECT_EQ(decode_record_v1(buf.data(), buf.size(), view),
              DecodeStatus::BadLength);
}

// ── Decode with exact-size buffer ───────────────────────────────────────────

TEST(DecodeRecord, ExactSizeBuffer) {
    auto buf = encode(RecordType::SnapshotChunk, 0, 7, kHello, 5);
    DecodedRecordView view{};
    // Pass exactly total_bytes
    auto st = decode_record_v1(buf.data(), buf.size(), view);
    EXPECT_EQ(st, DecodeStatus::Ok);
    EXPECT_EQ(view.total_bytes, buf.size());
}

// ── Unknown RecordType is skippable ─────────────────────────────────────────

TEST(DecodeRecord, UnknownTypeIsSkippable) {
    // Encode with an unknown type value (255)
    std::array<std::byte, 64> buf{};
    std::size_t written = 0;
    bool ok = encode_record_v1(static_cast<RecordType>(255), 0, 1, kHello, 5,
                               buf.data(), buf.size(), written);
    EXPECT_TRUE(ok);

    DecodedRecordView view{};
    auto st = decode_record_v1(buf.data(), written, view);
    EXPECT_EQ(st, DecodeStatus::Ok);
    EXPECT_EQ(static_cast<std::uint8_t>(view.type), 255);
    EXPECT_EQ(view.length, 5u);
}

// ── Padding bytes are zero ──────────────────────────────────────────────────

TEST(EncodeRecord, PaddingBytesAreZero) {
    // payload_len = 5 ⇒ 3 bytes padding
    auto buf = encode(RecordType::ExecEvent, 0, 1, kHello, 5);
    ASSERT_EQ(buf.size(), 32u);
    for (std::size_t i = kHeaderSize + 5; i < 32; ++i) {
        EXPECT_EQ(buf[i], std::byte{0}) << "non-zero padding at offset " << i;
    }
}

// ── Little-endian on disk ───────────────────────────────────────────────────

TEST(EncodeRecord, LittleEndianMagic) {
    auto buf = encode(RecordType::ExecEvent, 0, 1, kHello, 5);
    // kMagic = 0x5245434E → LE bytes: 4E 43 45 52
    EXPECT_EQ(buf[0], std::byte{0x4E});
    EXPECT_EQ(buf[1], std::byte{0x43});
    EXPECT_EQ(buf[2], std::byte{0x45});
    EXPECT_EQ(buf[3], std::byte{0x52});
}

TEST(EncodeRecord, LittleEndianSeqno) {
    auto buf = encode(RecordType::ExecEvent, 0, 0x0102030405060708ULL, kHello, 5);
    // seqno at offset 12..19 in LE
    EXPECT_EQ(buf[12], std::byte{0x08});
    EXPECT_EQ(buf[13], std::byte{0x07});
    EXPECT_EQ(buf[14], std::byte{0x06});
    EXPECT_EQ(buf[15], std::byte{0x05});
    EXPECT_EQ(buf[16], std::byte{0x04});
    EXPECT_EQ(buf[17], std::byte{0x03});
    EXPECT_EQ(buf[18], std::byte{0x02});
    EXPECT_EQ(buf[19], std::byte{0x01});
}

// ── Scanner ─────────────────────────────────────────────────────────────────

TEST(ScanRecords, EmptyBuffer) {
    auto stats = scan_records(nullptr, 0);
    EXPECT_EQ(stats.records_ok, 0u);
    EXPECT_EQ(stats.records_bad, 0u);
    EXPECT_EQ(stats.bytes_consumed, 0u);
}

TEST(ScanRecords, SingleRecord) {
    auto buf = encode(RecordType::ExecEvent, 0, 1, kHello, 5);
    auto stats = scan_records(buf.data(), buf.size());
    EXPECT_EQ(stats.records_ok, 1u);
    EXPECT_EQ(stats.records_bad, 0u);
    EXPECT_EQ(stats.bytes_consumed, buf.size());
}

TEST(ScanRecords, MultipleRecords) {
    auto r1 = encode(RecordType::ExecEvent, 0, 1, kHello, 5);
    auto r2 = encode(RecordType::DivergenceEvent, 0, 2, kHello, 5);
    auto r3 = encode(RecordType::SnapshotChunk, 0, 3, kHello, 5);

    std::vector<std::byte> buf;
    buf.insert(buf.end(), r1.begin(), r1.end());
    buf.insert(buf.end(), r2.begin(), r2.end());
    buf.insert(buf.end(), r3.begin(), r3.end());

    auto stats = scan_records(buf.data(), buf.size());
    EXPECT_EQ(stats.records_ok, 3u);
    EXPECT_EQ(stats.records_bad, 0u);
    EXPECT_EQ(stats.bytes_consumed, buf.size());
}

TEST(ScanRecords, TrailingPartialRecord) {
    auto r1 = encode(RecordType::ExecEvent, 0, 1, kHello, 5);
    // Append a partial second record (only a few header bytes)
    std::vector<std::byte> buf(r1.begin(), r1.end());
    buf.resize(buf.size() + 4, std::byte{0x4E}); // not enough for header

    auto stats = scan_records(buf.data(), buf.size());
    EXPECT_EQ(stats.records_ok, 1u);
    EXPECT_EQ(stats.records_bad, 0u);
    EXPECT_EQ(stats.bytes_consumed, r1.size());
}

TEST(ScanRecords, CorruptSecondRecord) {
    auto r1 = encode(RecordType::ExecEvent, 0, 1, kHello, 5);
    auto r2 = encode(RecordType::DivergenceEvent, 0, 2, kHello, 5);
    // Corrupt r2 magic
    r2[0] = std::byte{0xFF};

    std::vector<std::byte> buf;
    buf.insert(buf.end(), r1.begin(), r1.end());
    buf.insert(buf.end(), r2.begin(), r2.end());

    auto stats = scan_records(buf.data(), buf.size());
    EXPECT_EQ(stats.records_ok, 1u);
    EXPECT_EQ(stats.records_bad, 1u);
    EXPECT_EQ(stats.first_bad_offset, r1.size());
    EXPECT_EQ(stats.bytes_consumed, r1.size());
}

// ── Encode/decode with various payload sizes (alignment coverage) ───────────

class RecordRoundTripParam : public ::testing::TestWithParam<std::uint32_t> {};

TEST_P(RecordRoundTripParam, RoundTrip) {
    const std::uint32_t plen = GetParam();
    std::vector<std::byte> payload(plen, std::byte{0xAB});
    auto buf = encode(RecordType::ExecEvent, 0, plen, payload.data(), plen);

    const std::size_t expected_total = kHeaderSize + plen + padding_for(plen);
    EXPECT_EQ(buf.size(), expected_total);

    DecodedRecordView view{};
    auto st = decode_record_v1(buf.data(), buf.size(), view);
    EXPECT_EQ(st, DecodeStatus::Ok);
    EXPECT_EQ(view.length, plen);
    EXPECT_EQ(view.total_bytes, expected_total);
    if (plen > 0) {
        EXPECT_EQ(std::memcmp(view.payload, payload.data(), plen), 0);
    }
}

INSTANTIATE_TEST_SUITE_P(PayloadSizes, RecordRoundTripParam,
                         ::testing::Values(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 15,
                                           16, 31, 32, 63, 64, 100, 255, 256,
                                           1000, 4096));

} // namespace
