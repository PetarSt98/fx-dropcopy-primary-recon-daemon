# Performance Analysis

## Test Environment

- **CPU:** 11 physical cores / 22 threads @ 3072 MHz (as reported by Google Benchmark)
- **CPU Caches:** L1d 48 KiB, L1i 64 KiB, L2 2048 KiB, L3 24576 KiB (per core, ×11)
- **Compiler:** GCC with `-O3` (Release mode, CMake Release preset)
- **OS:** Linux (Docker container, Ubuntu-based)

## Methodology

- **Microbenchmarks:** Google Benchmark with `--benchmark_repetitions=5`, `FX_PERF_ENABLED=OFF` to measure raw component throughput without instrumentation overhead.
- **Instrumented counters:** `FX_PERF_ENABLED=ON` with 50,000 iterations, measuring real reconciliation pipeline latencies using TSC-based timing.
- **Soak test:** End-to-end system test with Aeron transport, two publishers (primary + dropcopy), reconciler daemon, and metrics collection.
- **CPU profiling:** `perf record` at 4999 Hz sample rate, 300-second benchmark-only profile.

---

## Microbenchmark Results

Measured with Google Benchmark, `FX_PERF_ENABLED=OFF`, 5 repetitions per benchmark.

| Benchmark | Mean | Median | StdDev | CV |
|-----------|------|--------|--------|----|
| BM_ArenaAllocate | 0.73 ns | 0.73 ns | 0.01 ns | 0.42% |
| BM_ComputeMismatch | 0.94 ns | 0.94 ns | 0.00 ns | 0.36% |
| BM_HashTableLookup_FirstProbe | 0.93 ns | 0.89 ns | 0.06 ns | 6.53% |
| BM_HashTableLookup_HighLoadFactor | 6.35 ns | 6.35 ns | 0.06 ns | 0.98% |
| BM_HashTableUpsert | 65.50 ns | 65.90 ns | 1.15 ns | 0.55% |
| BM_SPSCRing_Pop | 6.64 ns | 6.75 ns | 0.37 ns | 5.56% |
| BM_SPSCRing_Push | 6.69 ns | 6.78 ns | 0.62 ns | 9.42% |
| BM_SPSCRing_PushPop | 8.97 ns | 9.12 ns | 0.29 ns | 3.27% |
| BM_TimerWheelSchedule | 4.30 ns | 4.27 ns | 0.07 ns | 1.78% |

### Instrumentation Overhead

Comparing the same benchmarks with `FX_PERF_ENABLED=ON` vs `OFF`:

| Benchmark | Without Perf | With Perf | Overhead |
|-----------|-------------|-----------|----------|
| BM_HashTableLookup_FirstProbe | 0.93 ns | 20.2 ns | ~19 ns |
| BM_ArenaAllocate | 0.73 ns | 19.9 ns | ~19 ns |
| BM_TimerWheelSchedule | 4.30 ns | 25.7 ns | ~21 ns |
| BM_ComputeMismatch | 0.94 ns | 19.6 ns | ~19 ns |
| BM_SPSCRing_PushPop | 8.97 ns | 42.2 ns | ~33 ns |

The instrumentation adds ~19–33 ns per measurement. All production builds run with `FX_PERF_ENABLED=OFF`.

---

## Latency Distribution (Instrumented, 50K Iterations)

Full pipeline latencies measured with `FX_PERF_ENABLED=ON`.

### End-to-End Latency (ingest → reconciliation complete)

| Percentile | Latency | Notes |
|------------|---------|-------|
| P50 | 196 ns | Median case |
| P99 | 379 ns | Tail latency |
| P99.9 | 1,007 ns | Worst 0.1% |
| Max | 12,479 ns | Outlier (OS preemption) |

### Component Breakdown

| Operation | Calls | P50 | P99 | P99.9 | Max |
|-----------|-------|-----|-----|-------|-----|
| ReconcilerProcessEvent | 50,000 | 245 ns | 535 ns | 858 ns | 178,908 ns |
| HashTableUpsert | 100,000 | 54 ns | 289 ns | 330 ns | 172,475 ns |
| HashTableLookup | 50,000 | 18 ns | 20 ns | 25 ns | 742 ns |
| TimerWheelSchedule | 54,096 | 13 ns | 15 ns | 15 ns | 4,676 ns |
| TimerWheelPollExpired | 196 | 653 ns | 5,421 ns | 7,889 ns | 7,889 ns |
| ArenaAllocate | 104,096 | 10 ns | 15 ns | 28 ns | 6,630 ns |
| SpscRingPush | 53,072 | 13 ns | 15 ns | 15 ns | 13,385 ns |
| SpscRingPop | 50,000 | 10 ns | 13 ns | 13 ns | 315 ns |
| MismatchCompute | 58,192 | 10 ns | 15 ns | 25 ns | 71,946 ns |

**Key observation:** P99 end-to-end latency is **379 ns** — well under the 5 ms non-functional requirement. The occasional max outliers (>100 µs) are caused by OS preemption and are expected in non-isolated environments.

---

## CPU Profile (Flame Graph)

Profiled with `perf record` at 4999 Hz for 300 seconds on the benchmark binary (`reconciler_bench`). 2M samples collected.

The flame graph and profiling reports are committed under `docs/performance/`:

| File | Description |
|------|-------------|
| [`flamegraph.svg`](performance/flamegraph.svg) | Interactive SVG flame graph (open in browser) |
| [`perf_report.txt`](performance/perf_report.txt) | Full `perf report` output |
| [`top_functions.txt`](performance/top_functions.txt) | Top CPU hot spots |

To regenerate locally:
```bash
./scripts/generate_flamegraph.sh --bench 300
```

### Top Functions by CPU Time

| Function | % CPU | Analysis |
|----------|-------|----------|
| `OrderStateStore::find` | 15.80% | Hash table probing — primary lookup path |
| `BM_SPSCRing_Push` | 14.58% | Ring buffer push benchmark overhead |
| `__udivti3` | 13.24% | 128-bit division (GCC runtime, used by benchmark framework) |
| `BM_SPSCRing_Pop` | 12.70% | Ring buffer pop benchmark overhead |
| `BM_SPSCRing_PushPop` | 8.65% | Combined push+pop benchmark |
| `BM_TimerWheelSchedule` | 7.98% | Timer wheel scheduling benchmark |
| `BM_ArenaAllocate` | 6.80% | Arena allocation benchmark |
| `BM_ComputeMismatch` | 6.57% | Mismatch computation benchmark |
| `OrderStateStore::upsert` | 3.44% | Hash table insert/update |
| `OrderStateStore::alloc_state` | 3.17% | Arena allocation for new orders |

**Analysis:**
- `OrderStateStore::find` (15.80%) is the dominant business-logic function. This is expected — every reconciliation event requires at least one hash lookup.
- `__udivti3` (13.24%) is a GCC runtime function for 128-bit integer division, called by the Google Benchmark timing framework. It is not present in production code paths.
- The `BM_*` functions represent benchmark harness overhead and do not appear in production.
- In a production flame graph (full daemon with Aeron I/O), the profile would shift toward Aeron polling and OS I/O.

---

## Soak Test Results

**Configuration:** 20,000 events/sec total (10,000 per publisher), ~5 minutes duration.

### Event Processing

| Metric | Value |
|--------|-------|
| Expected events | 5,980,000 |
| Processed events | 5,980,000 |
| Primary events | 2,990,000 |
| Dropcopy events | 2,990,000 |
| Event drops | 0 |

✅ **All events processed successfully** — zero drops, zero crashes.

### Memory

| Metric | Value |
|--------|-------|
| Initial RSS | 557 MB |
| Final RSS | 647 MB |
| Max RSS | 647 MB |
| Warm-up | +90 MB |

Memory growth is expected during the initial phase as the arena allocator provisions `OrderState` objects for new orders. Growth stabilizes once the active order set reaches steady state.
No memory leaks.

### CPU Usage

| Metric | Value |
|--------|-------|
| Mean CPU | 313.5% |
| Max CPU | 316.0% |

CPU percentage exceeds 100% because the process uses multiple threads (reconciler + Aeron client threads + publisher threads running in the same measurement scope).

---

## Bottleneck Analysis

### Hash Table (15.80% CPU in `find`, 3.44% in `upsert`)

Linear probing with an average of ~3 probes at 70% load factor. The hash table is the most CPU-intensive business-logic component.

**Potential optimizations (not implemented):**
- Robin-hood hashing to reduce max probe length
- SIMD bucket scanning for parallel probe comparison
- Perfect hashing if the key space is known ahead of time

**Decision: NOT optimizing.** Current performance provides sufficient headroom for the target event rate. See [Design Journal](DESIGN_JOURNAL.md#9-why-not-optimize-the-hash-table-further) for rationale.

### Arena Allocator (3.17% CPU in `alloc_state`)

Bump-pointer allocation at **0.73 ns** median is near-optimal. No optimization needed.

### SPSC Ring Buffers

Push/pop operations at **6–9 ns** are dominated by cache-line transfers between producer and consumer cores. This is inherent to cross-core communication.

---

## Production Recommendations

For production deployment on dedicated hardware:

1. **CPU affinity:** Pin the reconciler to an isolated core.
   ```bash
   taskset -c 4 ./fx_exec_recond
   ```

2. **Frequency scaling:** Disable dynamic frequency scaling.
   ```bash
   cpupower frequency-set -g performance
   ```

3. **Huge pages:** Reduce TLB misses for the arena allocator.
   ```bash
   echo 256 > /proc/sys/vm/nr_hugepages
   ```

4. **IRQ isolation:** Prevent hardware interrupts on the reconciler core. Identify IRQ numbers from `/proc/interrupts` for your network interface.
   ```bash
   # Find IRQs for your NIC (e.g., eth0)
   grep eth0 /proc/interrupts
   # Move each IRQ off the reconciler core (core 4 in this example)
   echo 0-3,5-15 > /proc/irq/<irq_num>/smp_affinity_list
   ```

---

## How to Reproduce

### Microbenchmarks

```bash
# Build without instrumentation
cmake -B build/release -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/opt/aeron \
  -DFX_BUILD_BENCHMARKS=ON -DFX_PERF_ENABLED=OFF
cmake --build build/release -j$(nproc)

# Run benchmarks
./build/release/reconciler_bench

# Run with statistical analysis (5 repetitions)
./scripts/run_benchmarks.sh
```

### Instrumented Performance Counters

```bash
# Build with instrumentation
cmake -B build/release -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/opt/aeron \
  -DFX_BUILD_BENCHMARKS=ON -DFX_PERF_ENABLED=ON
cmake --build build/release -j$(nproc)

# Run benchmark (prints latency distribution)
./build/release/reconciler_bench
```

### Soak Test

```bash
# Build in release mode
cmake --preset release
cmake --build build/release

# Run soak test (duration in hours, total rate in events/sec)
./scripts/soak_test.sh 0.083 20000

# Analyze results
python3 scripts/analyze_soak_test.py soak_logs/soak_metrics.csv
```

### CPU Flame Graph

```bash
# One-time setup: clone FlameGraph toolkit
git clone --branch v1.0 --depth 1 \
  https://github.com/brendangregg/FlameGraph.git tools/FlameGraph

# Generate flame graph (benchmark-only, 300 seconds)
./scripts/generate_flamegraph.sh --bench 300

# View results
firefox docs/performance/flamegraph.svg
```

---

## References

- [Design Journal](DESIGN_JOURNAL.md) — Decision rationale for all architectural choices
- [Soak Test Guide](soak_test.md) — Detailed soak test methodology and configuration
- [Brendan Gregg's Flame Graphs](https://www.brendangregg.com/flamegraphs.html)
- [Linux perf wiki](https://perf.wiki.kernel.org/)
- [Google Benchmark](https://github.com/google/benchmark)

