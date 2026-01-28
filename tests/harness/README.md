# FX-7055: Scenario-Based Test Infrastructure for Replay Determinism

## Overview

This test infrastructure provides a deterministic harness for validating the reconciliation engine's behavior. It ensures that the reconciliation engine produces identical outputs for identical inputs (replay determinism).

## Components

### 1. Scenario Builder (`tests/harness/scenario_builder.hpp`)

A fluent API for constructing test scenarios:

```cpp
auto scenario = ReconScenarioBuilder()
    .with_grace_period(std::chrono::milliseconds(500))
    .starting_at(0)
    .dropcopy_fill("ORDER1", 100, to_micro(1.2345), 0)
    .advance_time(std::chrono::milliseconds(50))
    .primary_fill("ORDER1", 100, to_micro(1.2345), 50'000'000)
    .advance_time(std::chrono::milliseconds(500));
```

**Features:**
- Event creation (primary/dropcopy orders, fills, partials)
- Time control (advance_time for simulating delays)
- Sequence gap injection
- Automatic sequence number and timestamp management

### 2. Scenario Runner (`tests/harness/scenario_runner.hpp`)

Executes scenarios against the reconciler and collects results:

```cpp
auto config = core::default_recon_config();
config.grace_period_ns = 500'000'000;  // 500ms

auto result = run_scenario(scenario, config);

EXPECT_EQ(result.confirmed_divergences.size(), 0);
EXPECT_EQ(result.counters.false_positive_avoided, 1);
```

**Features:**
- `ReconTestHarness`: Owns all reconciler dependencies
- `ScenarioRunner`: Processes events and polls timers
- `run_scenario()`: Convenience function for running scenarios
- `check_determinism()`: Validates replay determinism

### 3. Standard Test Scenarios (`tests/recon_determinism_tests.cpp`)

Four core scenarios that validate the reconciliation engine:

#### Scenario A: DropCopy Leads, Primary Within Grace
- **Expected**: No divergence (primary arrives within grace period)
- **Validates**: False positive avoidance

#### Scenario B: Primary Missing Beyond Grace
- **Expected**: Confirmed divergence (phantom order)
- **Validates**: Divergence detection after grace period expires

#### Scenario C: Gap Suppresses Confirmation
- **Expected**: No confirmed divergence (suppressed due to sequence gap)
- **Validates**: Gap-aware suppression

#### Scenario D: Replay Produces Identical Output
- **Expected**: Running the same scenario twice produces identical results
- **Validates**: Deterministic behavior

## Usage

### Creating a New Test Scenario

```cpp
TEST(ReconDeterminism, MyCustomScenario) {
    auto config = core::default_recon_config();
    config.grace_period_ns = 200'000'000;  // 200ms
    
    auto scenario = ReconScenarioBuilder()
        .with_grace_period(std::chrono::milliseconds(200))
        .starting_at(0)
        .primary_fill("ORDER1", 100, to_micro(1.2345), 0)
        .dropcopy_fill("ORDER1", 100, to_micro(1.2345), 5'000'000)
        .advance_time(std::chrono::milliseconds(10));
    
    auto result = run_scenario(scenario, config);
    
    EXPECT_EQ(result.confirmed_divergences.size(), 0);
}
```

### Price Conversion

Use the `to_micro()` helper to convert prices to micro-units:

```cpp
to_micro(1.2345)  // Returns 1234500 (price in micro-units)
```

### Time Management

Time is tracked in nanoseconds. Use helper methods for convenience:

```cpp
.advance_time(std::chrono::milliseconds(50))   // Advance by 50ms
.advance_time_ms(50)                           // Same as above
```

### Sequence Gap Simulation

Create sequence gaps to test gap-aware suppression:

```cpp
.primary_working("ORDER1", 0)                    // seq 1
.sequence_gap(core::Source::Primary, 2, 3)      // Skip seq 2-3
.primary_working("ORDER2", 10'000'000)           // seq 4 (gap detected)
```

## Building and Running Tests

### Build

```bash
cmake --preset release
cmake --build build-release --target recon_determinism_tests
```

### Run

```bash
./build-release/recon_determinism_tests
```

Or via Docker:

```bash
docker compose run --rm dev-shell cmake --preset release
docker compose run --rm dev-shell cmake --build build-release --target recon_determinism_tests
docker compose run --rm dev-shell ./build-release/recon_determinism_tests
```

## Integration with CI

Tests run automatically in CI via the existing `docker-compose` setup:

```yaml
- name: Run unit tests
  run: docker compose run --rm unit-tests
```

The `recon_determinism_tests` target is discovered and executed by `ctest` along with other test suites.

## Design Principles

1. **Determinism**: All timestamps are explicit (TSC-based), ensuring reproducible behavior
2. **Isolation**: Each scenario runs with fresh state (no cross-contamination)
3. **Fluent API**: Readable, self-documenting test scenarios
4. **Zero Heap Allocations**: Arena-based allocation for performance
5. **Gap-Aware**: Tests validate gap suppression behavior (FX-7054)

## Validating the Entire Stack

This test infrastructure validates:
- ✅ FX-7051: ReconState enum and MismatchMask work correctly
- ✅ FX-7052: WheelTimer fires at correct times
- ✅ FX-7053: Grace period prevents false positives
- ✅ FX-7054: Gap suppression works correctly
- ✅ **System is deterministic** (same input → same output)
