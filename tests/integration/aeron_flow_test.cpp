#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include <sys/wait.h>
#include <unistd.h>

#include <Aeron.h>
#include <concurrent/AtomicBuffer.h>
#include <gtest/gtest.h>

#include "core/order_state_store.hpp"
#include "core/reconciler.hpp"
#include "core/wire_exec_event.hpp"
#include "ingest/aeron_subscriber.hpp"
#include "util/arena.hpp"

namespace {

using Clock = std::chrono::steady_clock;

std::filesystem::path make_unique_aeron_dir() {
    const auto now = Clock::now().time_since_epoch().count();
    const auto dir = std::filesystem::temp_directory_path()
                     / ("aeron-int-test-" + std::to_string(::getpid()) + "-" + std::to_string(now));
    std::filesystem::create_directories(dir);
    return dir;
}

class ProcessGuard {
public:
    explicit ProcessGuard(pid_t pid) : pid_(pid) {}

    ProcessGuard(const ProcessGuard&) = delete;
    ProcessGuard& operator=(const ProcessGuard&) = delete;

    ProcessGuard(ProcessGuard&& other) noexcept : pid_(other.pid_) { other.pid_ = -1; }
    ProcessGuard& operator=(ProcessGuard&& other) noexcept {
        if (this != &other) {
            stop();
            pid_ = other.pid_;
            other.pid_ = -1;
        }
        return *this;
    }

    ~ProcessGuard() { stop(); }

    bool valid() const noexcept { return pid_ > 0; }

    void stop() noexcept {
        if (pid_ > 0) {
            kill(pid_, SIGTERM);
            int status = 0;
            waitpid(pid_, &status, 0);
            pid_ = -1;
        }
    }

private:
    pid_t pid_{-1};
};

pid_t launch_media_driver(const std::filesystem::path& aeron_dir) {
    pid_t pid = fork();
    if (pid == 0) {
        const std::string dir_arg = "-Daeron.dir=" + aeron_dir.string();
        execlp("aeronmd", "aeronmd", dir_arg.c_str(), "-Daeron.socket.soReusePort=true", nullptr);
        std::cerr << "Failed to exec aeronmd" << std::endl;
        std::_Exit(127);
    }
    return pid;
}

bool wait_for_file(const std::filesystem::path& file, std::chrono::milliseconds timeout) {
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        if (std::filesystem::exists(file)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
    return false;
}

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

std::shared_ptr<aeron::Publication> make_publication(aeron::Aeron& client,
                                                     const std::string& channel,
                                                     std::int32_t stream_id,
                                                     Clock::time_point deadline) {
    const auto reg_id = client.addPublication(channel, stream_id);
    while (Clock::now() < deadline) {
        auto pub = client.findPublication(reg_id);
        if (pub) {
            return pub;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    return nullptr;
}

bool publish_fragments(aeron::Aeron& client,
                       const std::string& channel,
                       std::int32_t stream_id,
                       std::size_t count,
                       Clock::time_point deadline) {
    auto pub = make_publication(client, channel, stream_id, deadline);
    if (!pub) {
        std::cerr << "Failed to acquire publication for " << channel << " stream " << stream_id << std::endl;
        return false;
    }

    std::size_t sent = 0;
    while (sent < count && Clock::now() < deadline) {
        if (publish(*pub, make_wire_exec(sent + 1))) {
            ++sent;
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        } else {
            std::this_thread::yield();
        }
    }

    if (sent != count) {
        std::cerr << "Published only " << sent << "/" << count << " fragments for " << channel << std::endl;
    }
    return sent == count;
}

} // namespace

TEST(AeronFlowIntegrationTest, EndToEndConsumesBothStreams) {
    const std::string primary_channel = "aeron:udp?endpoint=localhost:20121";
    const std::string dropcopy_channel = "aeron:udp?endpoint=localhost:20122";
    constexpr std::int32_t primary_stream = 1001;
    constexpr std::int32_t dropcopy_stream = 1002;

    const auto aeron_dir = make_unique_aeron_dir();
    setenv("AERON_DIR", aeron_dir.string().c_str(), 1);

    ProcessGuard media_driver(launch_media_driver(aeron_dir));
    ASSERT_TRUE(media_driver.valid()) << "Failed to start Aeron media driver";

    const auto cnc_path = aeron_dir / "cnc.dat";
    ASSERT_TRUE(wait_for_file(cnc_path, std::chrono::seconds{5})) << "Aeron media driver did not create " << cnc_path;

    auto primary_ring = std::make_unique<ingest::Ring>();
    auto dropcopy_ring = std::make_unique<ingest::Ring>();
    ingest::ThreadStats primary_stats;
    ingest::ThreadStats dropcopy_stats;
    core::ReconCounters counters{};
    core::DivergenceRing divergence_ring;
    core::SequenceGapRing seq_gap_ring;
    std::atomic<bool> stop_flag{false};
    util::Arena arena(util::Arena::default_capacity_bytes);
    constexpr std::size_t order_capacity_hint = 1u << 12;
    core::OrderStateStore store(arena, order_capacity_hint);

    aeron::Context context;
    context.aeronDir(aeron_dir.string());
    auto client = aeron::Aeron::connect(context);

    core::Reconciler recon(stop_flag, *primary_ring, *dropcopy_ring, store, counters, divergence_ring, seq_gap_ring);
    ingest::AeronSubscriber primary_sub(primary_channel,
                                        primary_stream,
                                        *primary_ring,
                                        primary_stats,
                                        core::Source::Primary,
                                        client,
                                        stop_flag);
    ingest::AeronSubscriber dropcopy_sub(dropcopy_channel,
                                         dropcopy_stream,
                                         *dropcopy_ring,
                                         dropcopy_stats,
                                         core::Source::DropCopy,
                                         client,
                                         stop_flag);

    std::thread primary_thread([&] { primary_sub.run(); });
    std::thread dropcopy_thread([&] { dropcopy_sub.run(); });
    std::thread recon_thread([&] { recon.run(); });

    bool cleaned = false;
    auto cleanup = [&] {
        if (cleaned) return;
        stop_flag.store(true, std::memory_order_release);
        if (primary_thread.joinable()) primary_thread.join();
        if (dropcopy_thread.joinable()) dropcopy_thread.join();
        if (recon_thread.joinable()) recon_thread.join();
        media_driver.stop();
        std::filesystem::remove_all(aeron_dir);
        cleaned = true;
    };
    const auto guard = std::unique_ptr<void, std::function<void(void*)>>(nullptr, [&](void*) { cleanup(); });

    aeron::Context pub_context;
    pub_context.aeronDir(aeron_dir.string());
    auto pub_client = aeron::Aeron::connect(pub_context);

    const auto publish_deadline = Clock::now() + std::chrono::seconds{10};
    if (!publish_fragments(*pub_client, primary_channel, primary_stream, 8, publish_deadline)
        || !publish_fragments(*pub_client, dropcopy_channel, dropcopy_stream, 8, publish_deadline)) {
        FAIL() << "Failed to publish fragments to one or both channels";
    }

    const auto consumption_deadline = Clock::now() + std::chrono::seconds{10};
    while (Clock::now() < consumption_deadline) {
        if (counters.internal_events > 0 && counters.dropcopy_events > 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }

    cleanup();

    const bool consumed_primary = counters.internal_events > 0;
    const bool consumed_dropcopy = counters.dropcopy_events > 0;

    SCOPED_TRACE("primary=" + std::to_string(counters.internal_events) + " dropcopy=" +
                 std::to_string(counters.dropcopy_events));

    EXPECT_TRUE(consumed_primary);
    EXPECT_TRUE(consumed_dropcopy);
}
