#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <Aeron.h>
#include <concurrent/AtomicBuffer.h>

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
    evt.seq_num = static_cast<std::uint64_t>(seq);
    evt.session_id = 0;
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
    std::array<std::uint8_t, sizeof(core::WireExecEvent)> buffer{};
    std::memcpy(buffer.data(), &evt, sizeof(core::WireExecEvent));

    aeron::concurrent::AtomicBuffer atomic_buffer(buffer.data(), buffer.size());
    return pub.offer(atomic_buffer, 0, static_cast<aeron::util::index_t>(buffer.size())) > 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::cerr << "Usage: " << argv[0] << " <channel> <stream_id> <count> <sleep_us>" << std::endl;
        return 1;
    }

    const std::string channel = argv[1];
    const std::int32_t stream_id = static_cast<std::int32_t>(std::stoi(argv[2]));
    const std::size_t count = static_cast<std::size_t>(std::stoul(argv[3]));
    const auto sleep_us = std::chrono::microseconds{std::stoul(argv[4])};

    aeron::Context ctx;
    const char* aeron_dir = std::getenv("AERON_DIR");
    if (aeron_dir && aeron_dir[0] != '\0') {
        ctx.aeronDir(aeron_dir);
    }
    auto client = aeron::Aeron::connect(ctx);
    const auto pub_reg_id = client->addPublication(channel, stream_id);

    std::shared_ptr<aeron::Publication> pub;
    const auto wait_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!pub && std::chrono::steady_clock::now() < wait_deadline) {
        pub = client->findPublication(pub_reg_id);
        if (!pub) {
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }
    }

    if (!pub) {
        std::cerr << "Publication not available for registration id " << pub_reg_id << std::endl;
        return 1;
    }

    // Wait for at least one subscriber to connect before sending.
    // Without this, early events are lost because Aeron UDP has no
    // delivery guarantee when no subscriber is listening.
    {
        const auto conn_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
        while (!pub->isConnected() && std::chrono::steady_clock::now() < conn_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        if (!pub->isConnected()) {
            std::cerr << "No subscriber connected within 10s" << std::endl;
            return 1;
        }
        // Brief warmup after connection: Aeron's isConnected() can return
        // true before the subscriber image is fully established at the
        // media driver level. This avoids losing the first few hundred events.
        std::this_thread::sleep_for(std::chrono::milliseconds{250});
    }

    // Pre-generate events upfront to keep std::to_string allocation out of
    // the hot loop. Cap at 100k to avoid OOM on long soak tests (e.g. 18M events).
    // The OrderStateStore now handles high tombstone density via periodic
    // compaction, so the pre-generated count is not constrained by store capacity.
    constexpr std::size_t max_pregenerated = 100000;
    const std::size_t pregenerate_count = count < max_pregenerated ? count : max_pregenerated;

    std::vector<core::WireExecEvent> events;
    events.reserve(pregenerate_count);
    for (std::size_t i = 0; i < pregenerate_count; ++i) {
        events.push_back(make_wire_exec(i + 1));
    }

    std::size_t sent = 0;
    if (sleep_us.count() == 0) {
        // Truly unthrottled: tight busy loop with no yield/sleep.
        // This saturates the reconciler to stress-test business logic.
        while (sent < count) {
            if (publish(*pub, events[sent % pregenerate_count])) {
                ++sent;
            }
        }
    } else {
        while (sent < count) {
            if (publish(*pub, events[sent % pregenerate_count])) {
                ++sent;
                std::this_thread::sleep_for(sleep_us);
            } else {
                std::this_thread::yield();
            }
        }
    }

    std::cout << "Published " << sent << " events to " << channel << " stream " << stream_id << std::endl;
    return 0;
}
