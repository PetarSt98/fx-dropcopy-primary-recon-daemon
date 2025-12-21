#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <optional>
#include <limits>
#include <cerrno>
#include <cstddef>
#include <algorithm>
#include <array>

#include "persist/wire_capture_writer.hpp"

namespace {

class FakeSteadyClock : public util::SteadyClock {
public:
    using time_point = util::SteadyClock::time_point;
    time_point now() const noexcept override { return now_; }
    void advance(std::chrono::milliseconds d) noexcept { now_ += d; }
private:
    time_point now_{std::chrono::steady_clock::now()};
};

class FakeSystemClock : public util::SystemClock {
public:
    using time_point = util::SystemClock::time_point;
    time_point now() const noexcept override { return now_; }
    void set(time_point tp) noexcept { now_ = tp; }
private:
    time_point now_{std::chrono::system_clock::now()};
};

class FakeFileSink : public persist::IFileSink {
public:
    explicit FakeFileSink(std::size_t limit = std::numeric_limits<std::size_t>::max(),
                          bool inject_eintr = false)
        : limit_per_call_(limit), inject_eintr_(inject_eintr) {}

    persist::IoResult open(const std::string&) noexcept override {
        open_ = true;
        data_.clear();
        return {true, 0};
    }

    void close() noexcept override { open_ = false; }

    persist::IoResult writev(const struct iovec* iov, int iovcnt, std::size_t& bytes_written) noexcept override {
        bytes_written = 0;
        if (!open_) {
            return {false, EBADF};
        }
        if (inject_eintr_) {
            inject_eintr_ = false;
            return {false, EINTR};
        }
        std::size_t remaining = limit_per_call_;
        for (int i = 0; i < iovcnt && remaining > 0; ++i) {
            const std::size_t take = std::min<std::size_t>(remaining, iov[i].iov_len);
            const auto* bytes = static_cast<const std::byte*>(iov[i].iov_base);
            data_.insert(data_.end(), bytes, bytes + take);
            bytes_written += take;
            remaining -= take;
            if (take < iov[i].iov_len) {
                break;
            }
        }
        size_bytes_ += bytes_written;
        return {true, 0};
    }

    std::uint64_t current_size() const noexcept override { return size_bytes_; }
    bool is_open() const noexcept override { return open_; }

    std::vector<std::byte> data_;
    std::size_t limit_per_call_;
    bool inject_eintr_{false};
    bool open_{false};
    std::uint64_t size_bytes_{0};
};

std::vector<persist::WireRecordView> parse_records(const std::vector<std::byte>& bytes) {
    std::vector<persist::WireRecordView> out;
    std::size_t offset = 0;
    while (offset + persist::framed_size(0) <= bytes.size()) {
        persist::WireRecordView view;
        if (!persist::parse_record(bytes.data() + offset, bytes.size() - offset, view)) {
            break;
        }
        out.push_back(view);
        offset += persist::framed_size(view.payload_length);
    }
    return out;
}

} // namespace

TEST(WireCaptureWriter, HandlesPartialWritesAndEintr) {
    auto steady = std::make_unique<FakeSteadyClock>();
    auto sys = std::make_unique<FakeSystemClock>();
    FakeFileSink* sink_ptr = nullptr;
    persist::WireCaptureConfig cfg;
    cfg.rotate_max_bytes = 1024;
    cfg.steady_clock = std::move(steady);
    cfg.system_clock = std::move(sys);
    cfg.sink_factory = [&sink_ptr]() {
        auto ptr = std::make_unique<FakeFileSink>(4, true); // force partial and EINTR on first call
        sink_ptr = ptr.get();
        return ptr;
    };
    persist::WireCaptureWriter writer(std::move(cfg));
    writer.start();
    const std::string payload_str = "ABCDE";
    writer.try_submit(std::span<const std::byte>(reinterpret_cast<const std::byte*>(payload_str.data()), payload_str.size()));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    writer.stop();
    ASSERT_NE(sink_ptr, nullptr);
    ASSERT_FALSE(sink_ptr->data_.empty());
    auto recs = parse_records(sink_ptr->data_);
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_TRUE(persist::validate_record(recs[0]));
}

TEST(WireCaptureWriter, RotationBySize) {
    auto steady = std::make_unique<FakeSteadyClock>();
    auto sys = std::make_unique<FakeSystemClock>();
    std::vector<FakeFileSink*> sinks;
    persist::WireCaptureConfig cfg;
    cfg.rotate_max_bytes = persist::framed_size(4) + 1; // rotate after each record
    cfg.steady_clock = std::move(steady);
    cfg.system_clock = std::move(sys);
    cfg.sink_factory = [&sinks]() {
        auto ptr = std::make_unique<FakeFileSink>();
        sinks.push_back(ptr.get());
        return ptr;
    };
    persist::WireCaptureWriter writer(std::move(cfg));
    writer.start();
    std::array<std::byte, 4> payload{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    writer.try_submit(payload);
    writer.try_submit(payload);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    writer.stop();
    ASSERT_GE(sinks.size(), 2u);
    int records = 0;
    for (auto* s : sinks) {
        auto recs = parse_records(s->data_);
        records += static_cast<int>(recs.size());
    }
    EXPECT_EQ(records, 2);
}

TEST(WireCaptureWriter, DropsOnQueueFull) {
    persist::WireCaptureConfig cfg;
    cfg.rotate_max_bytes = 1024;
    cfg.sink_factory = []() { return std::make_unique<FakeFileSink>(); };
    persist::WireCaptureWriter writer(std::move(cfg));
    // Do not start writer to keep queue full.
    std::array<std::byte, 16> payload{};
    int pushes = 0;
    for (int i = 0; i < 1200; ++i) {
        if (writer.try_submit(payload)) {
            ++pushes;
        }
    }
    auto metrics = writer.snapshot_metrics();
    EXPECT_GT(metrics.drops_queue_full, 0u);
}
