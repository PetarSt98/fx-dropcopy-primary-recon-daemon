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
#include "core/recon_config.hpp"
#include "core/wire_exec_event.hpp"
#include "ingest/aeron_subscriber.hpp"
#include "util/arena.hpp"
#include "util/wheel_timer.hpp"
#include "util/rdtsc.hpp"

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

// ===== Helper Functions for E2E Reconciliation Tests =====

// Wait for divergence with timeout
bool wait_for_divergence(core::DivergenceRing& ring, 
                        core::Divergence& out,
                        std::chrono::milliseconds timeout) {
    auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        if (ring.try_pop(out)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return false;
}

// Wait for gap event with timeout
bool wait_for_gap_event(core::SequenceGapRing& ring,
                       core::SequenceGapEvent& out,
                       std::chrono::milliseconds timeout) {
    auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        if (ring.try_pop(out)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return false;
}

// Create wire event with custom parameters
core::WireExecEvent make_wire_exec_custom(
    std::uint64_t seq,
    const std::string& clord_id,
    std::int64_t qty,
    std::int64_t price_micro) {
    core::WireExecEvent evt{};
    evt.exec_type = 2; // Fill
    evt.ord_status = 2; // Filled
    evt.seq_num = seq;
    evt.session_id = 0;
    evt.price_micro = price_micro;
    evt.qty = qty;
    evt.cum_qty = qty;
    evt.sending_time = 20240101000000000ULL + seq;
    evt.transact_time = evt.sending_time;

    fill_id(evt.exec_id, "EXEC" + clord_id, evt.exec_id_len);
    fill_id(evt.order_id, "OID" + clord_id, evt.order_id_len);
    fill_id(evt.clord_id, clord_id, evt.clord_id_len);
    return evt;
}

/// Wait for Aeron publication to be connected and ready to accept messages.
/// 
/// Aeron publications must establish a connection with subscribers before they can
/// successfully publish messages. This function polls the connection status at 10ms
/// intervals, which balances responsiveness with CPU usage.
///
/// @param pub The Aeron publication to check
/// @param deadline Maximum time to wait for connection
/// @return true if publication becomes connected before deadline, false otherwise
bool wait_for_publication_ready(aeron::Publication& pub, Clock::time_point deadline) {
    while (Clock::now() < deadline) {
        if (pub.isConnected()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return false;
}

/// Publish a wire event with retry logic until successful or timeout.
///
/// Wraps the publish() call with retry logic to handle cases where the publication
/// is connected but not yet ready to accept messages (e.g., buffer full, flow control).
/// Uses a 1ms sleep between retries to reduce CPU consumption while maintaining
/// responsiveness. This matches the pattern used in publish_fragments().
///
/// @param pub The Aeron publication to use
/// @param evt The wire event to publish
/// @param deadline Maximum time to keep retrying
/// @return true if message was successfully published, false if timeout reached
bool publish_with_retry(aeron::Publication& pub, const core::WireExecEvent& evt, Clock::time_point deadline) {
    while (Clock::now() < deadline) {
        if (publish(pub, evt)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return false;
}


// RAII wrapper for test environment setup and cleanup
class AeronTestEnvironment {
public:
    // Configuration for test environment
    struct Config {
        std::string primary_channel;
        std::string dropcopy_channel;
        std::int32_t primary_stream;
        std::int32_t dropcopy_stream;
        std::uint64_t grace_period_ns{200'000'000};  // 200ms default
        bool enable_gap_suppression{true};
        bool enable_timer_wheel{true};
    };

    explicit AeronTestEnvironment(const Config& cfg)
        : config_(cfg)
        , aeron_dir_(make_unique_aeron_dir())
        , media_driver_(launch_media_driver(aeron_dir_))
        , primary_ring_(std::make_unique<ingest::Ring>())
        , dropcopy_ring_(std::make_unique<ingest::Ring>())
        , arena_(util::Arena::default_capacity_bytes)
        , store_(arena_, 1u << 12)
        , timer_wheel_(0)
    {
        setenv("AERON_DIR", aeron_dir_.string().c_str(), 1);
        
        if (!media_driver_.valid()) {
            throw std::runtime_error("Failed to start Aeron media driver");
        }

        const auto cnc_path = aeron_dir_ / "cnc.dat";
        if (!wait_for_file(cnc_path, std::chrono::seconds{5})) {
            throw std::runtime_error("Aeron media driver did not create cnc.dat");
        }

        // Setup reconciler config
        recon_config_ = core::default_recon_config();
        recon_config_.grace_period_ns = config_.grace_period_ns;
        recon_config_.enable_gap_suppression = config_.enable_gap_suppression;

        // Setup Aeron context and client
        aeron::Context context;
        context.aeronDir(aeron_dir_.string());
        client_ = aeron::Aeron::connect(context);

        // Create reconciler
        if (config_.enable_timer_wheel) {
            reconciler_ = std::make_unique<core::Reconciler>(
                stop_flag_, *primary_ring_, *dropcopy_ring_, store_, counters_,
                divergence_ring_, seq_gap_ring_, &timer_wheel_, recon_config_);
        } else {
            reconciler_ = std::make_unique<core::Reconciler>(
                stop_flag_, *primary_ring_, *dropcopy_ring_, store_, counters_,
                divergence_ring_, seq_gap_ring_);
        }

        // Create subscribers
        primary_sub_ = std::make_unique<ingest::AeronSubscriber>(
            config_.primary_channel, config_.primary_stream,
            *primary_ring_, primary_stats_, core::Source::Primary,
            client_, stop_flag_);
        
        dropcopy_sub_ = std::make_unique<ingest::AeronSubscriber>(
            config_.dropcopy_channel, config_.dropcopy_stream,
            *dropcopy_ring_, dropcopy_stats_, core::Source::DropCopy,
            client_, stop_flag_);

        // Start threads
        primary_thread_ = std::thread([this] { primary_sub_->run(); });
        dropcopy_thread_ = std::thread([this] { dropcopy_sub_->run(); });
        recon_thread_ = std::thread([this] { reconciler_->run(); });

        if (config_.enable_timer_wheel) {
            timer_thread_ = std::thread([this] {
                while (!stop_flag_.load(std::memory_order_acquire)) {
                    auto now = util::rdtsc();
                    timer_wheel_.poll_expired(now, [this](core::OrderKey key, std::uint32_t gen) {
                        reconciler_->on_grace_deadline_expired(key, gen);
                    });
                    std::this_thread::sleep_for(std::chrono::milliseconds{10});
                }
            });
        }

        // Setup publisher client
        aeron::Context pub_context;
        pub_context.aeronDir(aeron_dir_.string());
        pub_client_ = aeron::Aeron::connect(pub_context);
        
        // Give subscribers time to connect and start polling
        // This ensures the Aeron subscribers discover publishers when we create publications
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }

    ~AeronTestEnvironment() {
        cleanup();
    }

    // Delete copy/move to ensure RAII semantics
    AeronTestEnvironment(const AeronTestEnvironment&) = delete;
    AeronTestEnvironment& operator=(const AeronTestEnvironment&) = delete;
    AeronTestEnvironment(AeronTestEnvironment&&) = delete;
    AeronTestEnvironment& operator=(AeronTestEnvironment&&) = delete;

    // Accessors
    core::ReconCounters& counters() { return counters_; }
    core::DivergenceRing& divergence_ring() { return divergence_ring_; }
    core::SequenceGapRing& seq_gap_ring() { return seq_gap_ring_; }
    std::shared_ptr<aeron::Aeron> pub_client() { return pub_client_; }
    const std::string& primary_channel() const { return config_.primary_channel; }
    const std::string& dropcopy_channel() const { return config_.dropcopy_channel; }
    std::int32_t primary_stream() const { return config_.primary_stream; }
    std::int32_t dropcopy_stream() const { return config_.dropcopy_stream; }

private:
    void cleanup() {
        if (cleaned_) return;
        stop_flag_.store(true, std::memory_order_release);
        if (primary_thread_.joinable()) primary_thread_.join();
        if (dropcopy_thread_.joinable()) dropcopy_thread_.join();
        if (recon_thread_.joinable()) recon_thread_.join();
        if (timer_thread_.joinable()) timer_thread_.join();
        media_driver_.stop();
        std::filesystem::remove_all(aeron_dir_);
        cleaned_ = true;
    }

    Config config_;
    std::filesystem::path aeron_dir_;
    ProcessGuard media_driver_;
    
    std::unique_ptr<ingest::Ring> primary_ring_;
    std::unique_ptr<ingest::Ring> dropcopy_ring_;
    ingest::ThreadStats primary_stats_{};
    ingest::ThreadStats dropcopy_stats_{};
    core::ReconCounters counters_{};
    core::DivergenceRing divergence_ring_;
    core::SequenceGapRing seq_gap_ring_;
    std::atomic<bool> stop_flag_{false};
    util::Arena arena_;
    core::OrderStateStore store_;
    util::WheelTimer timer_wheel_;
    core::ReconConfig recon_config_;

    std::shared_ptr<aeron::Aeron> client_;
    std::shared_ptr<aeron::Aeron> pub_client_;
    std::unique_ptr<core::Reconciler> reconciler_;
    std::unique_ptr<ingest::AeronSubscriber> primary_sub_;
    std::unique_ptr<ingest::AeronSubscriber> dropcopy_sub_;

    std::thread primary_thread_;
    std::thread dropcopy_thread_;
    std::thread recon_thread_;
    std::thread timer_thread_;
    
    bool cleaned_{false};
};

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

// ===== Test 6: Matching Orders (No Divergence) - Baseline Test =====
TEST(AeronFlowIntegrationTest, MatchingOrdersProduceNoDivergence) {
    // Scenario: Primary and dropcopy report identical fills
    // Expected: No divergence emitted, orders_matched counter incremented

    AeronTestEnvironment::Config config{
        .primary_channel = "aeron:udp?endpoint=localhost:20123",
        .dropcopy_channel = "aeron:udp?endpoint=localhost:20124",
        .primary_stream = 1003,
        .dropcopy_stream = 1004,
        .grace_period_ns = 200'000'000,
        .enable_gap_suppression = true,
        .enable_timer_wheel = true
    };
    AeronTestEnvironment env(config);

    const auto publish_deadline = Clock::now() + std::chrono::seconds{10};
    auto primary_pub = make_publication(*env.pub_client(), env.primary_channel(), 
                                       env.primary_stream(), publish_deadline);
    auto dropcopy_pub = make_publication(*env.pub_client(), env.dropcopy_channel(), 
                                        env.dropcopy_stream(), publish_deadline);
    ASSERT_TRUE(primary_pub && dropcopy_pub) << "Failed to create publications";

    // Wait for publications to be ready
    ASSERT_TRUE(wait_for_publication_ready(*primary_pub, publish_deadline)) 
        << "Primary publication not ready";
    ASSERT_TRUE(wait_for_publication_ready(*dropcopy_pub, publish_deadline)) 
        << "Dropcopy publication not ready";

    // Publish matching fills: ORDER1, qty=100, price=1.2345
    auto primary_fill = make_wire_exec_custom(1, "ORDER1", 100, 1234500);
    auto dropcopy_fill = make_wire_exec_custom(1, "ORDER1", 100, 1234500);

    ASSERT_TRUE(publish_with_retry(*primary_pub, primary_fill, publish_deadline)) 
        << "Failed to publish primary fill";
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
    ASSERT_TRUE(publish_with_retry(*dropcopy_pub, dropcopy_fill, publish_deadline)) 
        << "Failed to publish dropcopy fill";

    // Wait for messages to be consumed
    const auto consumption_deadline = Clock::now() + std::chrono::seconds{2};
    while (Clock::now() < consumption_deadline) {
        if (env.counters().internal_events > 0 && env.counters().dropcopy_events > 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    ASSERT_GT(env.counters().internal_events, 0) << "Primary message not consumed";
    ASSERT_GT(env.counters().dropcopy_events, 0) << "Dropcopy message not consumed";

    // Wait for processing
    const auto processing_deadline = Clock::now() + std::chrono::seconds{5};
    while (Clock::now() < processing_deadline) {
        if (env.counters().orders_matched > 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }

    // Wait a bit longer to ensure no divergence is emitted
    std::this_thread::sleep_for(std::chrono::milliseconds{500});

    // Verify: no divergence emitted
    core::Divergence div;
    EXPECT_FALSE(env.divergence_ring().try_pop(div)) << "Unexpected divergence for matching orders";

    // Verify: orders_matched counter incremented
    EXPECT_EQ(env.counters().orders_matched, 1) << "Expected orders_matched=1";
}

// ===== Test 1: Phantom Order Detection (DropCopy-only order) =====
TEST(AeronFlowIntegrationTest, PhantomOrderDetectedEndToEnd) {
    // Scenario: DropCopy fill arrives, primary never sends matching event
    // Expected: After grace period expires, PhantomOrder divergence emitted

    AeronTestEnvironment::Config config{
        .primary_channel = "aeron:udp?endpoint=localhost:20125",
        .dropcopy_channel = "aeron:udp?endpoint=localhost:20126",
        .primary_stream = 1005,
        .dropcopy_stream = 1006,
        .grace_period_ns = 200'000'000,
        .enable_gap_suppression = true,
        .enable_timer_wheel = true
    };
    AeronTestEnvironment env(config);

    const auto publish_deadline = Clock::now() + std::chrono::seconds{10};
    auto dropcopy_pub = make_publication(*env.pub_client(), env.dropcopy_channel(), 
                                        env.dropcopy_stream(), publish_deadline);
    ASSERT_TRUE(dropcopy_pub) << "Failed to create dropcopy publication";

    // Wait for publication to be ready
    ASSERT_TRUE(wait_for_publication_ready(*dropcopy_pub, publish_deadline)) 
        << "Dropcopy publication not ready";

    // Publish fill event ONLY to dropcopy channel
    auto dropcopy_fill = make_wire_exec_custom(1, "PHANTOM1", 100, 1234500);
    ASSERT_TRUE(publish_with_retry(*dropcopy_pub, dropcopy_fill, publish_deadline)) 
        << "Failed to publish dropcopy fill";

    // Wait for message to be consumed
    const auto consumption_deadline = Clock::now() + std::chrono::seconds{2};
    while (Clock::now() < consumption_deadline) {
        if (env.counters().dropcopy_events > 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    ASSERT_GT(env.counters().dropcopy_events, 0) << "Dropcopy message not consumed";

    // Wait for grace period + buffer time
    core::Divergence div;
    bool found_divergence = wait_for_divergence(env.divergence_ring(), div, std::chrono::milliseconds{800});

    // Verify: PhantomOrder divergence emitted
    ASSERT_TRUE(found_divergence) << "Expected PhantomOrder divergence";
    EXPECT_EQ(div.type, core::DivergenceType::PhantomOrder);
    EXPECT_GT(env.counters().divergence_phantom, 0) << "Expected divergence_phantom counter > 0";
}

// ===== Test 2: Quantity Mismatch Detection =====
TEST(AeronFlowIntegrationTest, QuantityMismatchDetectedEndToEnd) {
    // Scenario: Primary and dropcopy both report same order, different quantities
    // Expected: QuantityMismatch divergence emitted

    AeronTestEnvironment::Config config{
        .primary_channel = "aeron:udp?endpoint=localhost:20127",
        .dropcopy_channel = "aeron:udp?endpoint=localhost:20128",
        .primary_stream = 1007,
        .dropcopy_stream = 1008,
        .grace_period_ns = 200'000'000,
        .enable_gap_suppression = true,
        .enable_timer_wheel = true
    };
    AeronTestEnvironment env(config);

    const auto publish_deadline = Clock::now() + std::chrono::seconds{10};
    auto primary_pub = make_publication(*env.pub_client(), env.primary_channel(), 
                                       env.primary_stream(), publish_deadline);
    auto dropcopy_pub = make_publication(*env.pub_client(), env.dropcopy_channel(), 
                                        env.dropcopy_stream(), publish_deadline);
    ASSERT_TRUE(primary_pub && dropcopy_pub) << "Failed to create publications";

    // Wait for publications to be ready
    ASSERT_TRUE(wait_for_publication_ready(*primary_pub, publish_deadline)) 
        << "Primary publication not ready";
    ASSERT_TRUE(wait_for_publication_ready(*dropcopy_pub, publish_deadline)) 
        << "Dropcopy publication not ready";

    // Publish primary fill: ORDER1, qty=100
    auto primary_fill = make_wire_exec_custom(1, "ORDER1", 100, 1234500);
    ASSERT_TRUE(publish_with_retry(*primary_pub, primary_fill, publish_deadline)) 
        << "Failed to publish primary fill";
    std::this_thread::sleep_for(std::chrono::milliseconds{10});

    // Publish dropcopy fill: ORDER1, qty=150 (MISMATCH)
    auto dropcopy_fill = make_wire_exec_custom(1, "ORDER1", 150, 1234500);
    ASSERT_TRUE(publish_with_retry(*dropcopy_pub, dropcopy_fill, publish_deadline)) 
        << "Failed to publish dropcopy fill";

    // Wait for messages to be consumed
    const auto consumption_deadline = Clock::now() + std::chrono::seconds{2};
    while (Clock::now() < consumption_deadline) {
        if (env.counters().internal_events > 0 && env.counters().dropcopy_events > 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    ASSERT_GT(env.counters().internal_events, 0) << "Primary message not consumed";
    ASSERT_GT(env.counters().dropcopy_events, 0) << "Dropcopy message not consumed";

    // Wait for divergence
    core::Divergence div;
    bool found_divergence = wait_for_divergence(env.divergence_ring(), div, std::chrono::milliseconds{800});

    // Verify: QuantityMismatch divergence emitted
    ASSERT_TRUE(found_divergence) << "Expected QuantityMismatch divergence";
    EXPECT_EQ(div.type, core::DivergenceType::QuantityMismatch);
    EXPECT_GT(env.counters().divergence_quantity_mismatch, 0) << "Expected divergence_quantity_mismatch counter > 0";
}

// ===== Test 3: Price Mismatch Detection =====
TEST(AeronFlowIntegrationTest, PriceMismatchDetectedEndToEnd) {
    // Scenario: Primary and dropcopy report same order, different prices
    // Expected: StateMismatch divergence emitted

    AeronTestEnvironment::Config config{
        .primary_channel = "aeron:udp?endpoint=localhost:20129",
        .dropcopy_channel = "aeron:udp?endpoint=localhost:20130",
        .primary_stream = 1009,
        .dropcopy_stream = 1010,
        .grace_period_ns = 200'000'000,
        .enable_gap_suppression = true,
        .enable_timer_wheel = true
    };
    AeronTestEnvironment env(config);

    const auto publish_deadline = Clock::now() + std::chrono::seconds{10};
    auto primary_pub = make_publication(*env.pub_client(), env.primary_channel(),
                                       env.primary_stream(), publish_deadline);
    auto dropcopy_pub = make_publication(*env.pub_client(), env.dropcopy_channel(),
                                        env.dropcopy_stream(), publish_deadline);
    ASSERT_TRUE(primary_pub && dropcopy_pub) << "Failed to create publications";

    // Wait for publications to be ready
    ASSERT_TRUE(wait_for_publication_ready(*primary_pub, publish_deadline)) 
        << "Primary publication not ready";
    ASSERT_TRUE(wait_for_publication_ready(*dropcopy_pub, publish_deadline)) 
        << "Dropcopy publication not ready";

    // Publish primary fill: ORDER1, price=1.2345
    auto primary_fill = make_wire_exec_custom(1, "ORDER1", 100, 1234500);
    ASSERT_TRUE(publish_with_retry(*primary_pub, primary_fill, publish_deadline)) 
        << "Failed to publish primary fill";
    std::this_thread::sleep_for(std::chrono::milliseconds{10});

    // Publish dropcopy fill: ORDER1, price=1.5000 (MISMATCH)
    auto dropcopy_fill = make_wire_exec_custom(1, "ORDER1", 100, 1500000);
    ASSERT_TRUE(publish_with_retry(*dropcopy_pub, dropcopy_fill, publish_deadline)) 
        << "Failed to publish dropcopy fill";

    // Wait for messages to be consumed
    const auto consumption_deadline = Clock::now() + std::chrono::seconds{2};
    while (Clock::now() < consumption_deadline) {
        if (env.counters().internal_events > 0 && env.counters().dropcopy_events > 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    ASSERT_GT(env.counters().internal_events, 0) << "Primary message not consumed";
    ASSERT_GT(env.counters().dropcopy_events, 0) << "Dropcopy message not consumed";

    // Wait for divergence
    core::Divergence div;
    bool found_divergence = wait_for_divergence(env.divergence_ring(), div, std::chrono::milliseconds{800});

    // Verify: StateMismatch divergence emitted (price mismatch classified as state mismatch)
    ASSERT_TRUE(found_divergence) << "Expected StateMismatch divergence";
    EXPECT_EQ(div.type, core::DivergenceType::StateMismatch);
    EXPECT_GT(env.counters().divergence_state_mismatch, 0) << "Expected divergence_state_mismatch counter > 0";
}

// ===== Test 4: Sequence Gap Detection =====
TEST(AeronFlowIntegrationTest, SequenceGapDetectedEndToEnd) {
    // Scenario: Primary stream has sequence gap (seq 1, 2, skip 3-4, then 5)
    // Expected: Gap event emitted to seq_gap_ring

    AeronTestEnvironment::Config config{
        .primary_channel = "aeron:udp?endpoint=localhost:20131",
        .dropcopy_channel = "aeron:udp?endpoint=localhost:20132",
        .primary_stream = 1011,
        .dropcopy_stream = 1012,
        .grace_period_ns = 200'000'000,
        .enable_gap_suppression = true,
        .enable_timer_wheel = true
    };
    AeronTestEnvironment env(config);

    const auto publish_deadline = Clock::now() + std::chrono::seconds{10};
    auto primary_pub = make_publication(*env.pub_client(), env.primary_channel(),
                                       env.primary_stream(), publish_deadline);
    ASSERT_TRUE(primary_pub) << "Failed to create primary publication";

    // Wait for publication to be ready
    ASSERT_TRUE(wait_for_publication_ready(*primary_pub, publish_deadline)) 
        << "Primary publication not ready";

    // Publish primary events: seq 1, seq 2
    auto evt1 = make_wire_exec_custom(1, "ORDER1", 100, 1234500);
    auto evt2 = make_wire_exec_custom(2, "ORDER2", 100, 1234500);
    ASSERT_TRUE(publish_with_retry(*primary_pub, evt1, publish_deadline)) 
        << "Failed to publish seq 1";
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
    ASSERT_TRUE(publish_with_retry(*primary_pub, evt2, publish_deadline)) 
        << "Failed to publish seq 2";
    std::this_thread::sleep_for(std::chrono::milliseconds{10});

    // Publish primary event: seq 5 (gap detected: 3-4 missing)
    auto evt5 = make_wire_exec_custom(5, "ORDER5", 100, 1234500);
    ASSERT_TRUE(publish_with_retry(*primary_pub, evt5, publish_deadline)) 
        << "Failed to publish seq 5";

    // Wait for gap event
    core::SequenceGapEvent gap;
    bool found_gap = wait_for_gap_event(env.seq_gap_ring(), gap, std::chrono::milliseconds{500});

    // Verify: gap event emitted
    ASSERT_TRUE(found_gap) << "Expected sequence gap event";
    EXPECT_EQ(gap.source, core::Source::Primary);
    EXPECT_EQ(gap.expected_seq, 3) << "Expected gap at sequence 3";
    EXPECT_EQ(gap.seen_seq, 5) << "Expected gap detected when seq 5 arrived";
    EXPECT_GT(env.counters().primary_seq_gaps, 0) << "Expected primary_seq_gaps counter > 0";
}


// ===== Test 5: Gap Suppression of Divergences =====
TEST(AeronFlowIntegrationTest, GapSuppressesDivergenceEndToEnd) {
    // Scenario: Sequence gap present when divergence would normally confirm
    // Expected: Divergence is suppressed (not emitted)

    AeronTestEnvironment::Config config{
        .primary_channel = "aeron:udp?endpoint=localhost:20133",
        .dropcopy_channel = "aeron:udp?endpoint=localhost:20134",
        .primary_stream = 1013,
        .dropcopy_stream = 1014,
        .grace_period_ns = 200'000'000,
        .enable_gap_suppression = true,
        .enable_timer_wheel = true
    };
    AeronTestEnvironment env(config);

    const auto publish_deadline = Clock::now() + std::chrono::seconds{10};
    auto primary_pub = make_publication(*env.pub_client(), env.primary_channel(),
                                       env.primary_stream(), publish_deadline);
    auto dropcopy_pub = make_publication(*env.pub_client(), env.dropcopy_channel(),
                                        env.dropcopy_stream(), publish_deadline);
    ASSERT_TRUE(primary_pub && dropcopy_pub) << "Failed to create publications";

    // Wait for publications to be ready
    ASSERT_TRUE(wait_for_publication_ready(*primary_pub, publish_deadline)) 
        << "Primary publication not ready";
    ASSERT_TRUE(wait_for_publication_ready(*dropcopy_pub, publish_deadline)) 
        << "Dropcopy publication not ready";

    // Create sequence gap on primary stream (seq 1, then seq 5)
    auto primary_evt1 = make_wire_exec_custom(1, "GAP_ORDER1", 100, 1234500);
    ASSERT_TRUE(publish_with_retry(*primary_pub, primary_evt1, publish_deadline)) 
        << "Failed to publish primary seq 1";
    std::this_thread::sleep_for(std::chrono::milliseconds{10});

    auto primary_evt5 = make_wire_exec_custom(5, "GAP_ORDER5", 100, 1234500);
    ASSERT_TRUE(publish_with_retry(*primary_pub, primary_evt5, publish_deadline)) 
        << "Failed to publish primary seq 5 (gap)";
    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    // Publish dropcopy-only fill (would normally become PhantomOrder)
    auto dropcopy_fill = make_wire_exec_custom(1, "PHANTOM_GAP", 100, 1234500);
    ASSERT_TRUE(publish_with_retry(*dropcopy_pub, dropcopy_fill, publish_deadline)) 
        << "Failed to publish dropcopy fill";

    // Wait for messages to be consumed
    const auto consumption_deadline = Clock::now() + std::chrono::seconds{2};
    while (Clock::now() < consumption_deadline) {
        if (env.counters().internal_events >= 2 && env.counters().dropcopy_events > 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    ASSERT_GE(env.counters().internal_events, 2) << "Primary messages not consumed";
    ASSERT_GT(env.counters().dropcopy_events, 0) << "Dropcopy message not consumed";

    // Wait for grace period + buffer
    std::this_thread::sleep_for(std::chrono::milliseconds{600});

    // Verify: divergence_ring is EMPTY (suppressed due to gap)
    core::Divergence div;
    bool found_divergence = env.divergence_ring().try_pop(div);
    EXPECT_FALSE(found_divergence) << "Divergence should be suppressed due to gap";

    // Verify: gap_suppressions counter incremented
    EXPECT_GT(env.counters().gap_suppressions, 0) << "Expected gap_suppressions counter > 0";
}






