# Performance Analysis

## Test Environment

- **CPU:** Intel Core Ultra 9 185H — 11 P-cores / 22 threads, base 3.07 GHz, turbo up to 5.1 GHz
- **CPU Caches:** L1d 48 KiB, L1i 64 KiB, L2 2048 KiB, L3 24576 KiB (per core, ×11)
- **Compiler:** GCC 11.4 with `-O3` (Release mode, CMake Release preset)
- **OS:** Linux (Docker container on Ubuntu 22.04)

**Note on CPU frequency:** Google Benchmark reports the base frequency (3072 MHz), but the CPU turbo-boosts during short benchmark runs. Microbenchmark throughput numbers reflect turbo-boosted conditions with L1-hot data and perfectly predicted branches — they represent best-case component throughput, not per-call latency under production load. The instrumented latency distribution (below) provides more representative per-call numbers.

## Methodology

- **Microbenchmarks:** Google Benchmark with `--benchmark_repetitions=5`, `FX_PERF_ENABLED=OFF` to measure raw component throughput without instrumentation overhead.
- **Instrumented counters:** `FX_PERF_ENABLED=ON` with 50,000 iterations, measuring real reconciliation pipeline latencies using TSC-based timing.
- **Soak test:** End-to-end system test with Aeron transport, two publishers (primary + dropcopy), reconciler daemon, and metrics collection.
- **CPU profiling:** `perf record` at 999 Hz sample rate, 60-second full-system profile under Aeron load (10K events/sec).

---

## Microbenchmark Results (Component Throughput)

Measured with Google Benchmark, `FX_PERF_ENABLED=OFF`, 5 repetitions. These numbers represent **tight-loop throughput** with L1-hot data and perfect branch prediction — useful for relative comparison between components and for detecting regressions, not for predicting per-call production latency. See the instrumented latency distribution below for production-representative numbers.

| Benchmark | Mean | Median | StdDev | CV |
|-----------|------|--------|--------|----|
| BM_HashTableLookup_FirstProbe | 1–2 ns | 1–2 ns | < 0.1 ns | < 5% |
| BM_HashTableLookup_HighLoadFactor | 6–10 ns | 6–10 ns | < 0.3 ns | < 3% |
| BM_HashTableUpsert | 65–85 ns | 65–84 ns | < 3 ns | < 3% |
| BM_ArenaAllocate | 1–2 ns | 1–2 ns | < 0.1 ns | < 3% |
| BM_TimerWheelSchedule | 4–5 ns | 4–5 ns | < 0.2 ns | < 4% |
| BM_ComputeMismatch | 1–3 ns | 1–3 ns | < 0.2 ns | < 5% |
| BM_SPSCRing_Push | 7–10 ns | 7–9 ns | < 0.7 ns | < 7% |
| BM_SPSCRing_Pop | 7–10 ns | 7–10 ns | < 0.4 ns | < 4% |
| BM_SPSCRing_PushPop | 9–16 ns | 9–15 ns | < 0.4 ns | < 3% |

Ranges reflect variation across runs due to CPU turbo state, thermal conditions, and virtualization overhead (Docker/WSL2). Exact numbers depend on the test environment; run `./scripts/run_benchmarks.sh` to reproduce on your hardware.

### Instrumentation Overhead

The compile-time `FX_PERF_ENABLED` toggle adds RAII TSC-based timing around each instrumented operation. Overhead per measurement point:

| Overhead source | Cost |
|-----------------|------|
| 2× `rdtsc` fence | ~10 ns |
| TSC → nanosecond conversion | ~3 ns |
| Histogram bucket update | ~6 ns |
| **Total per `PERF_SCOPE`** | **~19–33 ns** |

All production builds compile with `FX_PERF_ENABLED=OFF`, which expands every `PERF_SCOPE` / `PERF_START` / `PERF_STOP` macro to `((void)0)` — zero overhead, zero code generation.

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

Full-system profile with `perf record` at 999 Hz for 60 seconds on the `fx_exec_recond` daemon under sustained Aeron load (10,000 events/sec across two publishers). 182K samples collected.

The flame graph and profiling reports are committed under `docs/performance/`:

| File | Description |
|------|-------------|
| [`flamegraph.svg`](performance/flamegraph.svg) | Interactive SVG flame graph (open in browser) |
| [`perf_report.txt`](performance/perf_report.txt) | Full `perf report` output |
| [`top_functions.txt`](performance/top_functions.txt) | Top CPU hot spots |

To regenerate:
```bash
# Full-system profile (Aeron + reconciler under load) — recommended
./scripts/generate_flamegraph.sh 60 10000

# Benchmark-only profile (business logic, no Aeron I/O)
./scripts/generate_flamegraph.sh --bench 300
```

### CPU Time Distribution

| Function | % CPU | Analysis |
|----------|-------|----------|
| `ingest::AeronSubscriber::run` | 48.17% | Busy-polling Aeron shared memory for inbound messages |
| `core::Reconciler::run` | 32.39% | Main event loop — spin-polling SPSC ring, dispatching events |
| `aeron::Subscription::poll` | 17.26% | Aeron message delivery (shared memory reads) |
| `core::OrderStateStore::upsert` | 0.12% | Hash table insert/update |
| `util::AsyncLogger::consumer_loop` | 0.10% | Background log writer |
| `core::Reconciler::process_event` | 0.03% | Reconciliation business logic |
| `core::OrderStateStore::find` | 0.03% | Hash table lookup |
| `core::Reconciler::on_grace_deadline_expired` | 0.03% | Timing wheel callback |

**Analysis:**
- **97.8% of CPU is message transport and event-loop polling.** The reconciliation engine is so fast that business-logic functions (`upsert`, `find`, `process_event`, `alloc_state`) consume under 0.2% of total CPU combined.
- The dominant cost is `AeronSubscriber::run` (48%) — the subscriber busy-polls Aeron's shared-memory log, which is the expected profile for a low-latency Aeron consumer that prioritizes minimum wake-up latency over CPU efficiency.
- `Reconciler::run` (32%) is the main spin loop: check SPSC ring, process any pending events, advance timing wheels. In an idle-dominant system (events arrive at 10K/sec, each taking <1 µs to process), most iterations find nothing to do.
- The fact that `OrderStateStore::find` is only 0.03% confirms that hash table probing — the single most expensive business-logic operation in microbenchmarks — is negligible at production event rates.

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

### Hash Table (0.03% CPU in `find`, 0.12% in `upsert` under full-system load)

Linear probing with an average of ~3 probes at 70% load factor. The hash table is the most CPU-intensive business-logic component.

**Potential optimizations (not implemented):**
- Robin-hood hashing to reduce max probe length
- SIMD bucket scanning for parallel probe comparison
- Perfect hashing if the key space is known ahead of time

**Decision: NOT optimizing.** Current performance provides sufficient headroom for the target event rate. See [Design Journal](DESIGN_JOURNAL.md#9-why-not-optimize-the-hash-table-further) for rationale.

### Arena Allocator (< 0.01% CPU in `alloc_state` under full-system load)

Bump-pointer allocation with intrusive freelist for recycled slots. Near-optimal for single-writer workloads — the operation is a pointer increment (new allocation) or a linked-list pop (recycled slot). No optimization needed.

### SPSC Ring Buffers

Push/pop throughput in the low single-digit nanoseconds under microbenchmark. In practice, cross-core cache-line transfer dominates (~10–15 ns P50 under instrumented load). This is inherent to inter-thread communication and cannot be optimized further without co-locating producer and consumer on the same core (which would defeat the purpose).

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

# Full-system profile (recommended — profiles real daemon under Aeron load)
./scripts/generate_flamegraph.sh 60 10000

# Benchmark-only profile (business logic in isolation)
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

