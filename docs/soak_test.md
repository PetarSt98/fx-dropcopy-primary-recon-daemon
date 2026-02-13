# Soak Test & Stability Validation

## Overview

The soak test suite provides automated, production-grade 24-hour stability testing for the FX reconciliation daemon. It validates memory stability, CPU usage, and throughput under sustained high-load conditions.

## Components

### 1. `scripts/soak_test.sh`

Production-level Bash script that orchestrates a complete end-to-end load test.

**Features:**
- Configurable test duration (default: 24 hours)
- Configurable total event rate (default: 10,000 events/sec split across both publishers)
- Launches and manages all required components:
  - Aeron MediaDriver (runs in /dev/shm for optimal performance)
  - Reconciler daemon (`fx_exec_recond`)
  - Two FIX publishers (primary and dropcopy streams, each at 50% of total rate)
- Comprehensive logging for all components
- Real-time metrics collection every 10 seconds
- Event drop verification (compares expected vs processed events)
- Publisher liveness monitoring
- Automatic cleanup on exit/interrupt
- Defensive coding with proper error handling

**Usage:**
```bash
# Default: 24-hour test at 10k events/sec total (5k per publisher)
./scripts/soak_test.sh

# Custom duration and rate
./scripts/soak_test.sh 48 5000    # 48 hours at 5k events/sec total (2.5k per publisher)
./scripts/soak_test.sh 0.1 100    # 6 minutes at 100 events/sec total (50 per publisher)
```

**Important:** The rate parameter is the TOTAL system ingress rate. Each publisher sends at 50% of this rate.

**Requirements:**
- Built executables in `build/release/`:
  - `fx_exec_recond`
  - `fx_aeron_publisher`
- `aeronmd` available in PATH
- `/proc` filesystem (Linux) for metrics collection
- `/dev/shm` available (for Aeron performance)

**Output Files:**
- `soak_logs/soak_metrics.csv` - Timestamped metrics (RSS, CPU, internal/dropcopy/total events)
- `soak_logs/recond.log` - Reconciler daemon logs
- `soak_logs/aeronmd.log` - MediaDriver logs
- `soak_logs/primary_pub.log` - Primary publisher logs
- `soak_logs/dropcopy_pub.log` - Dropcopy publisher logs

### 2. `scripts/analyze_soak_test.py`

Production-grade Python analysis tool for processing soak test results.

**Features:**
- Pandas-based CSV processing
- Memory leak detection (configurable threshold, default 50MB)
- CPU usage statistics (mean, max)
- Throughput analysis (total events, duration, effective rate)
- Clear pass/fail status with formatted output
- Type-safe with proper error handling

**Usage:**
```bash
# Analyze default metrics file
python3 scripts/analyze_soak_test.py soak_logs/soak_metrics.csv

# Custom leak threshold (100MB)
python3 scripts/analyze_soak_test.py --leak-threshold 100 soak_logs/soak_metrics.csv

# Show help
python3 scripts/analyze_soak_test.py --help
```

**Requirements:**
- Python 3.6+
- pandas: `pip install pandas`

**Output Example:**
```
============================================================
Soak Test Analysis (duration: 24.0 hours)
============================================================

Memory Usage:
  Initial: 580 MB
  Final:   595 MB
  Max:     612 MB
  Leak:    +15 MB

CPU Usage:
  Mean: 12.3%
  Max:  18.7%

Throughput:
  Total:    864,000,000 events
  Duration: 24.0 hours
  Rate:     10,000 events/sec

✅ PASS: No significant memory leak (threshold: 50 MB)

============================================================
```

## Workflow

### Quick Test (Development)
For rapid verification during development:
```bash
# Build the project
cmake --preset release
cmake --build build/release

# Run 5-minute test
./scripts/soak_test.sh 0.083 1000

# Analyze results
python3 scripts/analyze_soak_test.py soak_logs/soak_metrics.csv
```

### Production Validation (24-Hour)
For full production-grade validation:
```bash
# Build in release mode
cmake --preset release
cmake --build build/release

# Run 24-hour soak test
./scripts/soak_test.sh 24 10000

# Analyze results
python3 scripts/analyze_soak_test.py soak_logs/soak_metrics.csv
```

### CI Integration
Example for GitHub Actions or Jenkins:
```yaml
- name: Soak Test (1 hour)
  run: |
    ./scripts/soak_test.sh 1 5000
    python3 scripts/analyze_soak_test.py soak_logs/soak_metrics.csv
```

## Metrics Collected

The soak test collects the following metrics every 10 seconds:

| Metric | Description |
|--------|-------------|
| `timestamp` | Wall-clock time of measurement (human-readable) |
| `epoch_sec` | Unix timestamp (seconds since epoch) |
| `elapsed_sec` | Seconds since test start |
| `recond_rss_mb` | Reconciler RSS memory in MB |
| `recond_cpu_pct` | Reconciler CPU usage percentage |
| `internal_events` | Events processed from internal/primary stream |
| `dropcopy_events` | Events processed from dropcopy stream |
| `events_total` | Total events processed (internal + dropcopy) |

## Acceptance Criteria

A successful soak test must meet:

1. **Stability**: No process crashes or OOM for full duration
2. **Memory**: Memory growth < 50 MB over test duration
3. **Data Integrity**: Event drops < 100 (verified by comparing expected vs processed counts)
4. **Performance**: Sustained throughput at target rate (accounting for rate splitting across publishers)
5. **Publisher Health**: Both publishers complete successfully without early termination

## Troubleshooting

### Test Fails to Start

**Issue**: "aeronmd not found"
```bash
# Add Aeron to PATH
export PATH="/opt/aeron/bin:$PATH"
```

**Issue**: Executables not found
```bash
# Specify build directory
export BUILD_DIR=/path/to/build/release
./scripts/soak_test.sh
```

### Memory Leak Detection

If the analysis shows a memory leak:

1. Check if leak is consistent or increasing
2. Run shorter tests to isolate the issue
3. Profile with valgrind or heaptrack:
   ```bash
   valgrind --leak-check=full ./build/release/fx_exec_recond ...
   ```

### High CPU Usage

If CPU usage exceeds expectations:

1. Check CPU frequency scaling (governor should be "performance")
2. Reduce order rate to isolate bottleneck
3. Profile with perf:
   ```bash
   perf record -g ./build/release/fx_exec_recond ...
   perf report
   ```

## Architecture Notes

### Component Lifecycle

1. **MediaDriver**: Launched first, provides Aeron transport layer
2. **Reconciler**: Connects to MediaDriver, starts consuming
3. **Publishers**: Start publishing events at configured rate
4. **Monitoring**: Runs in background, samples metrics every 10s
5. **Cleanup**: All processes terminated cleanly on exit

### Rate Control

**Important:** The rate parameter is the TOTAL system ingress rate, split evenly between the two publishers.

The publisher rate is configured via the `sleep_ms` delay used by `fx_aeron_publisher`:
- For per-publisher rates < 1000/sec: `sleep_ms = 1000 / rate` milliseconds per successful event (the publisher sleeps this long after each send)
- For per-publisher rates ≥ 1000/sec: `sleep_ms = 0` (no sleep between sends; the publisher runs unthrottled)

**Example:** If you specify `10000` events/sec:
- Each publisher targets 5000 events/sec
- At 5000/sec per publisher, sleep_ms = 0 (unthrottled mode)
- Actual rate depends on system capacity and backpressure

The effective rate may vary based on:
- Network latency (UDP transport)
- System scheduler
- Reconciler processing speed
- Available CPU/memory
- Aeron backpressure mechanisms

**Note:** For rates ≥ 2000/sec total (≥1000/sec per publisher), the test runs in "max throughput" mode where actual rate may exceed the target. This is intentional for high-rate soak testing.

### Signal Handling

The soak test handles:
- `SIGINT` (Ctrl+C): Graceful shutdown
- `SIGTERM`: Graceful shutdown
- `EXIT`: Cleanup trap always runs

## Future Enhancements

Potential improvements for FX-7063 and beyond:

1. **CPU Profiling**: Integrated perf/flamegraph generation
2. **Memory Profiling**: Automatic heaptrack integration
3. **Distributed Testing**: Multi-host publisher support
4. **Cloud Deployment**: Kubernetes/Docker Compose orchestration
5. **Advanced Analytics**: Latency percentiles, tail latency analysis

## References

- Main README: `../README.md`
- Logging Audit: `./logging_audit.md`
- Issue: FX-7062 (24-Hour Soak Test & Stability Validation)
- Next: FX-7063 (CPU Profiling & Flame Graph Generation)
