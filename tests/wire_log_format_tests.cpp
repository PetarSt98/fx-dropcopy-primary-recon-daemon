#include <gtest/gtest.h>
#include <array>
#include <cstring>

#include "persist/wire_log_format.hpp"

TEST(WireLogFormat, CrcVector) {
    const char* msg = "123456789";
    std::span<const std::byte> payload(reinterpret_cast<const std::byte*>(msg), 9);
    const std::uint32_t crc = util::Crc32c::compute(payload.data(), payload.size());
    EXPECT_EQ(crc, 0xE3069283u);
}

TEST(WireLogFormat, EncodeParseValidate) {
    const std::array<std::byte, 4> payload{std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}};
    persist::RecordFields fields{};
    persist::encode_record(payload, 1234, fields);
    std::array<std::byte, persist::framed_size(4)> framed{};
    std::memcpy(framed.data(), fields.length_le.data(), fields.length_le.size());
    std::memcpy(framed.data() + fields.length_le.size(), fields.capture_ts_le.data(), fields.capture_ts_le.size());
    std::memcpy(framed.data() + fields.length_le.size() + fields.capture_ts_le.size(), payload.data(), payload.size());
    std::memcpy(framed.data() + fields.length_le.size() + fields.capture_ts_le.size() + payload.size(),
                fields.checksum_le.data(),
                fields.checksum_le.size());

    persist::WireRecordView view;
    ASSERT_TRUE(persist::parse_record(framed.data(), framed.size(), view));
    EXPECT_EQ(view.payload_length, payload.size());
    EXPECT_EQ(view.capture_ts, 1234u);
    EXPECT_TRUE(persist::validate_record(view));
}

TEST(WireLogFormat, HeaderRoundTrip) {
    persist::WireLogHeaderFields header{};
    std::array<std::byte, persist::wire_log_header_size> bytes{};
    persist::encode_header(header, bytes);
    persist::WireLogHeaderFields parsed{};
    ASSERT_TRUE(persist::parse_header(bytes, parsed));
    EXPECT_EQ(parsed.magic, persist::wire_log_magic);
    EXPECT_EQ(parsed.format_version, persist::wire_log_format_version);
    EXPECT_EQ(parsed.payload_size, persist::wire_exec_event_wire_size);
}

TEST(WireLogFormat, WireExecEventSerialization) {
    core::WireExecEvent evt{};
    evt.exec_type = 3;
    evt.ord_status = 4;
    evt.seq_num = 0x1122334455667788ULL;
    evt.session_id = 0xA1B2;
    evt.price_micro = -123456789;
    evt.qty = 999;
    evt.cum_qty = 500;
    evt.sending_time = 0x1020304050607080ULL;
    evt.transact_time = 0x8877665544332211ULL;
    const char exec_id[] = "EXECID";
    const char order_id[] = "ORDERID";
    const char clord_id[] = "CLORDID";
    evt.exec_id_len = sizeof(exec_id) - 1;
    std::memcpy(evt.exec_id, exec_id, evt.exec_id_len);
    evt.order_id_len = sizeof(order_id) - 1;
    std::memcpy(evt.order_id, order_id, evt.order_id_len);
    evt.clord_id_len = sizeof(clord_id) - 1;
    std::memcpy(evt.clord_id, clord_id, evt.clord_id_len);

    std::array<std::uint8_t, persist::wire_exec_event_wire_size> buf{};
    const auto written = persist::serialize_wire_exec_event(evt, buf.data());
    EXPECT_EQ(written, persist::wire_exec_event_wire_size);

    core::WireExecEvent decoded{};
    persist::deserialize_wire_exec_event(decoded, buf.data());
    EXPECT_EQ(decoded.exec_type, evt.exec_type);
    EXPECT_EQ(decoded.ord_status, evt.ord_status);
    EXPECT_EQ(decoded.seq_num, evt.seq_num);
    EXPECT_EQ(decoded.session_id, evt.session_id);
    EXPECT_EQ(decoded.price_micro, evt.price_micro);
    EXPECT_EQ(decoded.qty, evt.qty);
    EXPECT_EQ(decoded.cum_qty, evt.cum_qty);
    EXPECT_EQ(decoded.sending_time, evt.sending_time);
    EXPECT_EQ(decoded.transact_time, evt.transact_time);
    EXPECT_EQ(decoded.exec_id_len, evt.exec_id_len);
    EXPECT_EQ(std::string_view(decoded.exec_id, decoded.exec_id_len), std::string_view(exec_id));
    EXPECT_EQ(std::string_view(decoded.order_id, decoded.order_id_len), std::string_view(order_id));
    EXPECT_EQ(std::string_view(decoded.clord_id, decoded.clord_id_len), std::string_view(clord_id));
}
