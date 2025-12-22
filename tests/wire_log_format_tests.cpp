#include <gtest/gtest.h>
#include <array>
#include <cstring>

#include "persist/wire_log_format.hpp"

TEST(WireLogFormat, CrcVector) {
    const char* msg = "123456789";
    std::span<const std::byte> payload(reinterpret_cast<const std::byte*>(msg), 9);
    const std::uint32_t crc = persist::crc32c(payload);
    EXPECT_EQ(crc, 0xE3069283u);
}

TEST(WireLogFormat, EncodeParseValidate) {
    const std::array<std::byte, 4> payload{std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}};
    std::uint32_t len_le{0};
    std::uint32_t crc_le{0};
    persist::encode_record(payload, len_le, crc_le);
    std::array<std::byte, persist::framed_size(4)> framed{};
    std::memcpy(framed.data(), &len_le, sizeof(len_le));
    std::memcpy(framed.data() + sizeof(len_le), payload.data(), payload.size());
    std::memcpy(framed.data() + sizeof(len_le) + payload.size(), &crc_le, sizeof(crc_le));

    persist::WireRecordView view;
    ASSERT_TRUE(persist::parse_record(framed.data(), framed.size(), view));
    EXPECT_EQ(view.payload_length, payload.size());
    EXPECT_TRUE(persist::validate_record(view));
}
