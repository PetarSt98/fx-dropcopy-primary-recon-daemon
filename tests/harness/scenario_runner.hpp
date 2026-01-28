#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/divergence.hpp"
#include "core/exec_event.hpp"
#include "core/order_state_store.hpp"
#include "core/reconciler.hpp"
#include "core/recon_config.hpp"
#include "core/sequence_tracker.hpp"
#include "ingest/spsc_ring.hpp"
#include "util/arena.hpp"
#include "util/wheel_timer.hpp"
#include "tests/harness/scenario_builder.hpp"

namespace test {

using ExecRing = ingest::SpscRing<core::ExecEvent, 1u << 16>;

// Result of running a scenario
struct ScenarioResult {
    std::vector<core::Divergence> confirmed_divergences;
    std::vector<core::SequenceGapEvent> gap_events;
    core::ReconCounters counters;
    std::uint64_t total_events_processed;
    std::uint64_t final_timestamp;
};

// Test harness that owns all reconciler dependencies
class ReconTestHarness {
public:
    explicit ReconTestHarness(const core::ReconConfig& config = core::default_recon_config())
        : primary_ring_(std::make_unique<ExecRing>()),
          dropcopy_ring_(std::make_unique<ExecRing>()),
          divergence_ring_(std::make_unique<core::DivergenceRing>()),
          seq_gap_ring_(std::make_unique<core::SequenceGapRing>()),
          store_(arena_, 1024u),
          timer_wheel_(0),
          config_(config)
    {
        reconciler_ = std::make_unique<core::Reconciler>(
            stop_flag_,
            *primary_ring_,
            *dropcopy_ring_,
            store_,
            counters_,
            *divergence_ring_,
            *seq_gap_ring_,
            &timer_wheel_,
            config_);
    }

    // Process single event
    void process_event(const core::ExecEvent& ev) {
        // Update last_poll_tsc to match production behavior (gap timeout logic relies on this)
        reconciler_->set_last_poll_tsc_for_test(ev.ingest_tsc);
        reconciler_->process_event_for_test(ev);
    }

    // Poll timer wheel at given timestamp
    void poll_timers(std::uint64_t now_tsc) {
        // Update the last poll timestamp before polling
        reconciler_->set_last_poll_tsc_for_test(now_tsc);
        
        // Poll expired timers
        timer_wheel_.poll_expired(now_tsc, [this](core::OrderKey key, std::uint32_t gen) {
            reconciler_->on_grace_deadline_expired(key, gen);
        });
    }

    // Collect results
    std::vector<core::Divergence> collect_divergences() {
        std::vector<core::Divergence> divergences;
        core::Divergence div;
        while (divergence_ring_->try_pop(div)) {
            divergences.push_back(div);
        }
        return divergences;
    }

    std::vector<core::SequenceGapEvent> collect_gap_events() {
        std::vector<core::SequenceGapEvent> gaps;
        core::SequenceGapEvent gap;
        while (seq_gap_ring_->try_pop(gap)) {
            gaps.push_back(gap);
        }
        return gaps;
    }

    // Accessors
    core::ReconCounters& counters() { return counters_; }
    core::OrderStateStore& store() { return store_; }
    util::WheelTimer& timer_wheel() { return timer_wheel_; }
    
    core::OrderState* find_order(const std::string& clord_id) {
        // Create a dummy event to generate the key
        core::ExecEvent dummy{};
        dummy.set_clord_id(clord_id.c_str(), clord_id.size());
        core::OrderKey key = core::make_order_key(dummy);
        return store_.find(key);
    }

private:
    std::atomic<bool> stop_flag_{false};
    std::unique_ptr<ExecRing> primary_ring_;
    std::unique_ptr<ExecRing> dropcopy_ring_;
    std::unique_ptr<core::DivergenceRing> divergence_ring_;
    std::unique_ptr<core::SequenceGapRing> seq_gap_ring_;
    util::Arena arena_{util::Arena::default_capacity_bytes};
    core::OrderStateStore store_;
    core::ReconCounters counters_{};
    util::WheelTimer timer_wheel_;
    core::ReconConfig config_;
    std::unique_ptr<core::Reconciler> reconciler_;
};

// Scenario runner that executes complete scenarios
class ScenarioRunner {
public:
    explicit ScenarioRunner(const core::ReconConfig& config = core::default_recon_config())
        : config_(config) {}

    // Run complete scenario
    ScenarioResult run(const ReconScenarioBuilder& scenario) {
        ReconTestHarness harness(config_);
        
        auto events = scenario.build();
        std::uint64_t current_ts = 0;
        
        // Process each event
        for (const auto& event : events) {
            harness.process_event(event);
            current_ts = event.ingest_tsc;
            
            // Poll timers at event timestamp
            if (current_ts > 0) {
                harness.poll_timers(current_ts);
            }
        }
        
        // Final timer poll to catch any pending expirations
        // Use the scenario's current timestamp (which may be advanced beyond last event)
        std::uint64_t final_ts = scenario.current_timestamp();
        if (final_ts > current_ts) {
            current_ts = final_ts;
        }
        
        // Poll at the final timestamp to process any expired timers
        if (current_ts > 0) {
            harness.poll_timers(current_ts);
        }
        
        // Collect results
        ScenarioResult result;
        result.confirmed_divergences = harness.collect_divergences();
        result.gap_events = harness.collect_gap_events();
        result.counters = harness.counters();
        result.total_events_processed = events.size();
        result.final_timestamp = current_ts;
        
        return result;
    }

    // Convenience method
    std::size_t run_and_count_divergences(const ReconScenarioBuilder& scenario) {
        auto result = run(scenario);
        return result.confirmed_divergences.size();
    }

private:
    core::ReconConfig config_;
};

// Free functions for convenience

// Run scenario with fresh state
inline ScenarioResult run_scenario(const ReconScenarioBuilder& scenario,
                                   const core::ReconConfig& config = core::default_recon_config()) {
    ScenarioRunner runner(config);
    return runner.run(scenario);
}

// Check replay determinism (runs twice, compares results)
inline bool check_determinism(const ReconScenarioBuilder& scenario,
                             const core::ReconConfig& config = core::default_recon_config()) {
    auto result1 = run_scenario(scenario, config);
    auto result2 = run_scenario(scenario, config);
    
    // Compare divergence counts
    if (result1.confirmed_divergences.size() != result2.confirmed_divergences.size()) {
        return false;
    }
    
    // Compare key counters
    if (result1.counters.mismatch_confirmed != result2.counters.mismatch_confirmed ||
        result1.counters.false_positive_avoided != result2.counters.false_positive_avoided ||
        result1.counters.orders_matched != result2.counters.orders_matched ||
        result1.counters.gap_suppressions != result2.counters.gap_suppressions) {
        return false;
    }
    
    // Compare divergence details
    for (size_t i = 0; i < result1.confirmed_divergences.size(); ++i) {
        if (result1.confirmed_divergences[i].key != result2.confirmed_divergences[i].key ||
            result1.confirmed_divergences[i].type != result2.confirmed_divergences[i].type) {
            return false;
        }
    }
    
    return true;
}

} // namespace test
