#include "test_main.hpp"

#include <cstdint>
#include <cstring>

#include "core/exec_event.hpp"
#include "core/wire_exec_event.hpp"

namespace wire_exec_tests {

bool test_from_wire_roundtrip() {
    core::WireExecEvent wire{};
    wire.exec_type = static_cast<std::uint8_t>(core::ExecType::Fill);
    wire.ord_status = static_cast<std::uint8_t>(core::OrdStatus::Filled);
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
    if (evt.source != core::Source::Primary) return false;
    if (evt.exec_type != core::ExecType::Fill) return false;
    if (evt.ord_status != core::OrdStatus::Filled) return false;
    if (evt.price_micro != wire.price_micro || evt.qty != wire.qty || evt.cum_qty != wire.cum_qty) return false;
    if (evt.sending_time != wire.sending_time || evt.transact_time != wire.transact_time) return false;
    if (evt.ingest_tsc != 999) return false;
    if (evt.exec_id_len != wire.exec_id_len || std::memcmp(evt.exec_id, exec_id, evt.exec_id_len) != 0) return false;
    if (evt.order_id_len != wire.order_id_len || std::memcmp(evt.order_id, order_id, evt.order_id_len) != 0) return false;
    if (evt.clord_id_len != wire.clord_id_len || std::memcmp(evt.clord_id, clord_id, evt.clord_id_len) != 0) return false;
    return true;
}

void add_tests(std::vector<TestCase>& tests) {
    tests.push_back({"from_wire_roundtrip", test_from_wire_roundtrip});
}

} // namespace wire_exec_tests
