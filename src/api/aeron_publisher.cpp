#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <Aeron.h>

#include "core/wire_exec_event.hpp"

namespace {

void fill_id(char (&dest)[core::WireExecEvent::id_capacity], const std::string& src, std::uint8_t& len_out) {
    const auto len = src.size() > core::WireExecEvent::id_capacity ? core::WireExecEvent::id_capacity : src.size();
    std::memcpy(dest, src.data(), len);
    len_out = static_cast<std::uint8_t>(len);
}

core::WireExecEvent make_wire_exec(std::size_t seq) {
    core::WireExecEvent evt{};
    evt.exec_type = 2; // Fill
    evt.ord_status = 2; // Filled
    evt.price_micro = 1000000 + static_cast<std::int64_t>(seq);
    evt.qty = 100 + static_cast<std::int64_t>(seq);
    evt.cum_qty = evt.qty;
    evt.sending_time = 20240101000000000ULL + seq;
    evt.transact_time = evt.sending_time;

    fill_id(evt.exec_id, "EXEC" + std::to_string(seq), evt.exec_id_len);
    fill_id(evt.order_id, "OID" + std::to_string(seq), evt.order_id_len);
    fill_id(evt.clord_id, "CID" + std::to_string(seq), evt.clord_id_len);
    return evt;
}

bool publish(aeron::Publication& pub, const core::WireExecEvent& evt) {
    return pub.offer(reinterpret_cast<const std::uint8_t*>(&evt), sizeof(core::WireExecEvent)) > 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::cerr << "Usage: " << argv[0] << " <channel> <stream_id> <count> <sleep_ms>" << std::endl;
        return 1;
    }

    const std::string channel = argv[1];
    const std::int32_t stream_id = static_cast<std::int32_t>(std::stoi(argv[2]));
    const std::size_t count = static_cast<std::size_t>(std::stoul(argv[3]));
    const auto sleep_ms = std::chrono::milliseconds{std::stoul(argv[4])};

    aeron::Context ctx;
    auto client = aeron::Aeron::connect(ctx);
    auto pub = client->addPublication(channel, stream_id);

    std::size_t sent = 0;
    while (sent < count) {
        const auto evt = make_wire_exec(sent + 1);
        if (publish(*pub, evt)) {
            ++sent;
            std::this_thread::sleep_for(sleep_ms);
        } else {
            std::this_thread::yield();
        }
    }

    std::cout << "Published " << sent << " events to " << channel << " stream " << stream_id << std::endl;
    return 0;
}
