#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string_view>

#include "core/exec_event.hpp"
#include "core/wire_exec_event.hpp"

namespace {

TEST(WireExecTest, FromWireRoundtrip) {
    core::WireExecEvent wire{};
    wire.exec_type = static_cast<std::uint8_t>(core::ExecType::Fill);
    wire.ord_status = static_cast<std::uint8_t>(core::OrdStatus::Filled);
    wire.seq_num = 42;
    wire.session_id = 7;
    wire.price_micro = 123456;
    wire.qty = 1000;
    wire.cum_qty = 2000;
    wire.sending_time = 111;
    wire.transact_time = 222;

    const char exec_id[] = "EXECID";
    const char order_id[] = "ORDERID";
    const char clord_id[] = "CLORDID";
    wire.exec_id_len = sizeof(exec_id) - 1;
    std::memcpy(wire.exec_id, exec_id, wire.exec_id_len);
    wire.order_id_len = sizeof(order_id) - 1;
    std::memcpy(wire.order_id, order_id, wire.order_id_len);
    wire.clord_id_len = sizeof(clord_id) - 1;
    std::memcpy(wire.clord_id, clord_id, wire.clord_id_len);

    const auto evt = core::from_wire(wire, core::Source::Primary, 999);

    EXPECT_EQ(evt.source, core::Source::Primary);
    EXPECT_EQ(evt.exec_type, core::ExecType::Fill);
    EXPECT_EQ(evt.ord_status, core::OrdStatus::Filled);
    EXPECT_EQ(evt.seq_num, wire.seq_num);
    EXPECT_EQ(evt.session_id, wire.session_id);
    EXPECT_EQ(evt.price_micro, wire.price_micro);
    EXPECT_EQ(evt.qty, wire.qty);
    EXPECT_EQ(evt.cum_qty, wire.cum_qty);
    EXPECT_EQ(evt.sending_time, wire.sending_time);
    EXPECT_EQ(evt.transact_time, wire.transact_time);
    EXPECT_EQ(evt.ingest_timestamp_ns, 999);

    ASSERT_EQ(evt.exec_id_len, wire.exec_id_len);
    EXPECT_EQ(std::string_view(evt.exec_id, evt.exec_id_len), std::string_view(exec_id));

    ASSERT_EQ(evt.order_id_len, wire.order_id_len);
    EXPECT_EQ(std::string_view(evt.order_id, evt.order_id_len), std::string_view(order_id));

    ASSERT_EQ(evt.clord_id_len, wire.clord_id_len);
    EXPECT_EQ(std::string_view(evt.clord_id, evt.clord_id_len), std::string_view(clord_id));
}

} // namespace
