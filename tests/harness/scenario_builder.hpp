#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <chrono>
#include <cstring>

#include "core/exec_event.hpp"

namespace test {

// Type aliases for consistency
using Timestamp = std::uint64_t;  // TSC cycles or nanoseconds
using Qty = std::int64_t;
using Price = std::int64_t;       // Micro-units (1.2345 = 1234500)
using SeqNo = std::uint64_t;

// Convert price to micro-units
constexpr Price to_micro(double price) noexcept {
    return static_cast<Price>(price * 1'000'000.0);
}

// Fluent API for constructing test scenarios
class ReconScenarioBuilder {
public:
    ReconScenarioBuilder() = default;

    // Configuration
    ReconScenarioBuilder& with_grace_period(std::chrono::nanoseconds grace) {
        grace_period_ns_ = static_cast<std::uint64_t>(grace.count());
        return *this;
    }

    ReconScenarioBuilder& starting_at(Timestamp ts) {
        current_ts_ = ts;
        initial_ts_ = ts;
        return *this;
    }

    // Time control
    ReconScenarioBuilder& advance_time(std::chrono::nanoseconds duration) {
        current_ts_ += static_cast<std::uint64_t>(duration.count());
        return *this;
    }

    ReconScenarioBuilder& advance_time_ms(std::uint64_t ms) {
        current_ts_ += ms * 1'000'000;  // ms to ns
        return *this;
    }

    // Primary (internal) events
    ReconScenarioBuilder& primary_new_order(const std::string& clord_id, Timestamp ts = 0) {
        add_event(core::Source::Primary, core::OrdStatus::New, clord_id, 0, 0, ts);
        return *this;
    }

    ReconScenarioBuilder& primary_fill(const std::string& clord_id, Qty cum_qty, Price avg_px, Timestamp ts = 0) {
        add_event(core::Source::Primary, core::OrdStatus::Filled, clord_id, cum_qty, avg_px, ts);
        return *this;
    }

    ReconScenarioBuilder& primary_partial_fill(const std::string& clord_id, Qty cum_qty, Price avg_px, Timestamp ts = 0) {
        add_event(core::Source::Primary, core::OrdStatus::PartiallyFilled, clord_id, cum_qty, avg_px, ts);
        return *this;
    }

    ReconScenarioBuilder& primary_working(const std::string& clord_id, Timestamp ts = 0) {
        add_event(core::Source::Primary, core::OrdStatus::Working, clord_id, 0, 0, ts);
        return *this;
    }

    // DropCopy events
    ReconScenarioBuilder& dropcopy_new_order(const std::string& clord_id, Timestamp ts = 0) {
        add_event(core::Source::DropCopy, core::OrdStatus::New, clord_id, 0, 0, ts);
        return *this;
    }

    ReconScenarioBuilder& dropcopy_fill(const std::string& clord_id, Qty cum_qty, Price avg_px, Timestamp ts = 0) {
        add_event(core::Source::DropCopy, core::OrdStatus::Filled, clord_id, cum_qty, avg_px, ts);
        return *this;
    }

    ReconScenarioBuilder& dropcopy_partial_fill(const std::string& clord_id, Qty cum_qty, Price avg_px, Timestamp ts = 0) {
        add_event(core::Source::DropCopy, core::OrdStatus::PartiallyFilled, clord_id, cum_qty, avg_px, ts);
        return *this;
    }

    // Sequence gap simulation
    ReconScenarioBuilder& sequence_gap(core::Source source, SeqNo from, SeqNo to) {
        if (source == core::Source::Primary) {
            primary_seq_ = to;  // Next event will be to+1
        } else {
            dropcopy_seq_ = to;
        }
        return *this;
    }

    // Build the event sequence
    std::vector<core::ExecEvent> build() const {
        return events_;
    }

    std::uint64_t grace_period_ns() const {
        return grace_period_ns_;
    }

    Timestamp current_timestamp() const {
        return current_ts_;
    }

private:
    void add_event(core::Source source, core::OrdStatus status, 
                   const std::string& clord_id, Qty cum_qty, Price avg_px, Timestamp ts) {
        core::ExecEvent ev{};
        ev.source = source;
        ev.ord_status = status;
        
        // Map OrdStatus to ExecType
        switch (status) {
        case core::OrdStatus::Filled:
            ev.exec_type = core::ExecType::Fill;
            break;
        case core::OrdStatus::PartiallyFilled:
            ev.exec_type = core::ExecType::PartialFill;
            break;
        case core::OrdStatus::Canceled:
            ev.exec_type = core::ExecType::Cancel;
            break;
        case core::OrdStatus::Replaced:
            ev.exec_type = core::ExecType::Replace;
            break;
        case core::OrdStatus::Rejected:
            ev.exec_type = core::ExecType::Rejected;
            break;
        default:
            ev.exec_type = core::ExecType::New;
            break;
        }

        // Auto-increment sequence number per source
        if (source == core::Source::Primary) {
            ev.seq_num = ++primary_seq_;
        } else {
            ev.seq_num = ++dropcopy_seq_;
        }

        // Use provided timestamp or current time
        Timestamp event_ts = (ts == 0) ? current_ts_ : ts;
        ev.ingest_tsc = event_ts;
        ev.transact_time = event_ts;
        ev.sending_time = event_ts;

        // Set quantities and price
        ev.cum_qty = cum_qty;
        ev.qty = cum_qty;  // For simplicity, qty = cum_qty
        ev.price_micro = avg_px;

        // Set ClOrdID
        ev.set_clord_id(clord_id.c_str(), clord_id.size());

        // Auto-generate unique exec_id
        std::string exec_id = "EXEC" + std::to_string(++exec_id_counter_);
        ev.set_exec_id(exec_id.c_str(), exec_id.size());

        events_.push_back(ev);
    }

    std::vector<core::ExecEvent> events_;
    std::uint64_t grace_period_ns_{500'000'000};  // 500ms default
    Timestamp current_ts_{0};
    Timestamp initial_ts_{0};
    SeqNo primary_seq_{0};
    SeqNo dropcopy_seq_{0};
    std::uint64_t exec_id_counter_{0};
};

} // namespace test
