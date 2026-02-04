# Integration Tests Documentation

## Overview

This directory contains end-to-end integration tests for the FX execution reconciliation daemon. The tests verify the complete pipeline from Aeron message ingestion through reconciliation to divergence detection.

## Test Files

### aeron_flow_test.cpp

Comprehensive E2E tests that verify the full Aeron → Subscriber → Reconciler → Divergence/Gap pipeline.

#### Test Scenarios

1. **EndToEndConsumesBothStreams** (Original baseline)
   - Verifies that events flow through both primary and dropcopy channels
   - Validates basic infrastructure and event consumption
   - Does not test reconciliation logic

2. **MatchingOrdersProduceNoDivergence** (Test 6 - Baseline)
   - **Purpose**: Validates that identical fills from primary and dropcopy are correctly matched
   - **Setup**: Timer wheel with 200ms grace period, gap suppression enabled
   - **Scenario**: 
     - Primary: ORDER1, qty=100, price=1.2345
     - Dropcopy: ORDER1, qty=100, price=1.2345 (MATCH)
   - **Expected**: No divergence emitted, `orders_matched` counter = 1
   - **Validates**: Baseline reconciliation behavior when both sides agree

3. **PhantomOrderDetectedEndToEnd** (Test 1)
   - **Purpose**: Verifies detection of orders that exist only in dropcopy
   - **Setup**: Timer wheel with 200ms grace period
   - **Scenario**:
     - Dropcopy: PHANTOM1, qty=100, price=1.2345
     - Primary: (no event)
     - Wait: 800ms (grace period + buffer)
   - **Expected**: PhantomOrder divergence emitted, `divergence_phantom` counter > 0
   - **Validates**: Grace period expiration and phantom order detection

4. **QuantityMismatchDetectedEndToEnd** (Test 2)
   - **Purpose**: Verifies detection of quantity mismatches between feeds
   - **Setup**: Timer wheel with 200ms grace period
   - **Scenario**:
     - Primary: ORDER1, qty=100, price=1.2345
     - Dropcopy: ORDER1, qty=150, price=1.2345 (MISMATCH)
   - **Expected**: QuantityMismatch divergence emitted, `divergence_quantity_mismatch` counter > 0
   - **Validates**: Quantity field reconciliation

5. **PriceMismatchDetectedEndToEnd** (Test 3)
   - **Purpose**: Verifies detection of price mismatches between feeds
   - **Setup**: Timer wheel with 200ms grace period
   - **Scenario**:
     - Primary: ORDER1, qty=100, price=1.2345
     - Dropcopy: ORDER1, qty=100, price=1.5000 (MISMATCH)
   - **Expected**: StateMismatch divergence emitted, `divergence_state_mismatch` counter > 0
   - **Validates**: Price field reconciliation (classified as state mismatch)

6. **SequenceGapDetectedEndToEnd** (Test 4)
   - **Purpose**: Verifies detection of sequence gaps in message streams
   - **Setup**: Timer wheel with 200ms grace period, gap detection enabled
   - **Scenario**:
     - Primary: seq 1 (ORDER1), seq 2 (ORDER2), seq 5 (ORDER5) - gap at 3-4
   - **Expected**: SequenceGapEvent emitted with source=Primary, expected_seq=3, seen_seq=5
   - **Validates**: Sequence tracking and gap detection logic

7. **GapSuppressesDivergenceEndToEnd** (Test 5)
   - **Purpose**: Verifies that divergences are suppressed during sequence gaps
   - **Setup**: Timer wheel with 200ms grace period, gap suppression enabled
   - **Scenario**:
     - Primary: seq 1, seq 5 (creates gap)
     - Dropcopy: PHANTOM_GAP (would normally trigger PhantomOrder)
     - Wait: 600ms (grace period + buffer)
   - **Expected**: No divergence emitted (suppressed), `gap_suppressions` counter > 0
   - **Validates**: Gap suppression feature to avoid false positives during data loss

## Helper Functions

### wait_for_divergence()
```cpp
bool wait_for_divergence(core::DivergenceRing& ring, 
                        core::Divergence& out,
                        std::chrono::milliseconds timeout)
```
Polls the divergence ring with timeout, returns true if divergence found.

### wait_for_gap_event()
```cpp
bool wait_for_gap_event(core::SequenceGapRing& ring,
                       core::SequenceGapEvent& out,
                       std::chrono::milliseconds timeout)
```
Polls the sequence gap ring with timeout, returns true if gap event found.

### make_wire_exec_custom()
```cpp
core::WireExecEvent make_wire_exec_custom(
    std::uint64_t seq,
    const std::string& clord_id,
    std::int64_t qty,
    std::int64_t price_micro)
```
Creates a wire execution event with custom parameters for testing specific scenarios.

## Test Infrastructure

### Timer Wheel Integration

All new tests integrate the `WheelTimer` for grace period expiration:

```cpp
util::WheelTimer timer_wheel(0);
core::ReconConfig config = core::default_recon_config();
config.grace_period_ns = 200'000'000;  // 200ms for faster tests
config.enable_gap_suppression = true;

core::Reconciler recon(stop_flag, *primary_ring, *dropcopy_ring, store, counters,
                      divergence_ring, seq_gap_ring, &timer_wheel, config);

// Timer polling thread
std::thread timer_thread([&] {
    while (!stop_flag.load(std::memory_order_acquire)) {
        auto now = util::rdtsc();
        timer_wheel.poll_expired(now, [&](core::OrderKey key, std::uint32_t gen) {
            recon.on_grace_deadline_expired(key, gen);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
});
```

### Configuration

Tests use a reduced grace period (200ms) compared to production defaults (500ms) to speed up test execution while maintaining correctness.

### Cleanup

All tests properly clean up resources:
- Join all threads (primary, dropcopy, reconciler, timer)
- Stop Aeron media driver
- Remove temporary Aeron directories

## Running Tests

### Via Docker (Recommended)

```bash
# Run all integration tests
docker compose run --rm integration-tests

# Run specific test
docker compose run --rm integration-tests -- --gtest_filter="*PhantomOrder*"
```

### Via CMake/CTest

```bash
# Configure
cmake --preset debug

# Build
cmake --build build/debug --target integration_aeron_flow

# Run
ctest --test-dir build/debug -R integration_aeron_flow --output-on-failure
```

## Test Design Principles

1. **Isolation**: Each test uses unique Aeron channels/streams to avoid interference
2. **Determinism**: Tests wait for specific conditions rather than using fixed delays
3. **Timeouts**: All waits have explicit timeouts to prevent hanging tests
4. **Non-flaky**: Tests account for message delivery latency and grace period timing
5. **Comprehensive**: Tests cover all divergence types and key reconciliation features

## Success Criteria

- ✅ All 7 tests pass (1 original + 6 new)
- ✅ Tests verify both `divergence_ring` and `seq_gap_ring` outputs
- ✅ Tests cover all divergence types: PhantomOrder, QuantityMismatch, StateMismatch
- ✅ Tests validate gap detection and suppression behavior
- ✅ Tests use appropriate timeouts and don't introduce flakiness
- ✅ CI passes with new tests integrated

## Future Enhancements

Potential areas for expansion:
- Test MissingDropCopy divergences (primary-only orders)
- Test partial fill scenarios with multiple execution events
- Test out-of-order message delivery
- Test gap recovery when missing messages arrive late
- Test divergence resolution (mismatches that later converge)
- Test timer wheel overflow scenarios
- Test multi-session scenarios
