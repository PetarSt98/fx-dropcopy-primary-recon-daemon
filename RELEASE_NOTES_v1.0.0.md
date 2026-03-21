# Release v1.0.0 — Production-Ready FX Reconciliation Engine

---

## Highlights

- **Sub-microsecond P99 end-to-end reconciliation latency** (379 ns measured), 13,000x under the 5 ms NFR target
- **5.98 million events ingested with zero drops** under sustained 20K events/sec soak test
- **Zero heap allocations on the hot path** — all state pre-allocated at startup via arena + SPSC rings
- **Lock-free, single-writer architecture** — no mutexes, no CAS loops, no contention in the reconciliation core
- **Deterministic reconciliation** — identical state reconstruction from the same input sequence, for audit and replay

---

## System-Level Performance

Measured on an 11-core x86_64 (Intel Core Ultra 9 185H), GCC 11.4 `-O3`, Linux Docker container.

### End-to-End Latency (ingest → divergence detection complete)

| Percentile | Latency |
|------------|---------|
| P50 | 196 ns |
| P99 | 379 ns |
| P99.9 | 1,007 ns |

Instrumented with TSC-based timing across the full reconciliation pipeline (50K events). Max outliers (~12 µs) are OS preemption on non-isolated cores.

### Soak Test (20K events/sec sustained)

| Metric | Result |
|--------|--------|
| Events processed | 5,980,000 (primary: 2.99M + dropcopy: 2.99M) |
| Event drops | **0** |
| Crashes | **0** |
| Memory (RSS) | 557 MB → 647 MB (arena warm-up, stabilizes at steady state) |

### Component Latency (instrumented pipeline, TSC-based)

| Operation | P50 | P99 | P99.9 |
|-----------|-----|-----|-------|
| Reconciler process event | 245 ns | 535 ns | 858 ns |
| Hash table upsert | 54 ns | 289 ns | 330 ns |
| Hash table lookup | 18 ns | 20 ns | 25 ns |
| SPSC ring push | 13 ns | 15 ns | 15 ns |
| SPSC ring pop | 10 ns | 13 ns | 13 ns |
| Arena allocate | 10 ns | 15 ns | 28 ns |
| Timer wheel schedule | 13 ns | 15 ns | 15 ns |
| Mismatch compute | 10 ns | 15 ns | 25 ns |

Instrumentation adds ~19 ns per measurement point. Production builds compile with `FX_PERF_ENABLED=OFF` (zero-overhead no-op macros).

### CPU Profile

Full-system `perf record` (60s, 10K events/sec sustained Aeron load, 182K samples):

| Function | % CPU | Analysis |
|----------|-------|----------|
| `AeronSubscriber::run` | 48.2% | Busy-polling shared memory for inbound messages |
| `Reconciler::run` | 32.4% | Main event loop — spin-polling SPSC ring |
| `Subscription::poll` | 17.3% | Aeron message delivery |
| Business logic total | < 0.2% | `upsert` + `find` + `process_event` + `alloc_state` |

**97.8% of CPU is message transport and polling.** The reconciliation engine is invisible in the profile — business logic completes faster than the profiler can sample it.

Full interactive flame graph: [`docs/performance/flamegraph.svg`](docs/performance/flamegraph.svg)

---

## Architecture

```
Primary FIX ──► Aeron ──► SPSC Ring ──┐
                                      ├──► Reconciler ──► Divergence Stream
Dropcopy FIX ──► Aeron ──► SPSC Ring ──┘        │
                                          OrderStateStore
                                          (Arena + Hash Table)
```

| Component | Design |
|-----------|--------|
| **Transport** | Aeron reliable UDP with `/dev/shm` media driver for co-located IPC |
| **Ingestion** | Per-stream SPSC ring buffers — `std::atomic` load/store only, no CAS |
| **State store** | Open-addressed hash table (linear probing, FNV-1a keys) + bump-pointer arena with intrusive freelist for terminal-order recycling |
| **Reconciliation** | Single-threaded core: two-stage mismatch detection with grace-period timing wheel, gap-aware divergence suppression, FIX order lifecycle state machine |
| **Determinism** | No wall-clock in decision logic — all timing via event TSC + `constant_tsc` invariant |

---

## Key Features

- **Two-stage reconciliation** — grace-period timing wheel suppresses transient mismatches, confirmed divergences emit only after deadline expiry
- **Gap-aware suppression** — per-session sequence tracking with per-order gap-uncertainty flags prevents false positives during stream gaps
- **Order recycling** — terminal orders (Filled/Canceled/Rejected on both sides) are tombstoned in the hash table and returned to an intrusive freelist for arena memory reuse
- **Divergence deduplication** — configurable time-windowed suppression prevents repeated alerts for the same mismatch
- **Compile-time perf toggle** — `FX_PERF_ENABLED` controls TSC instrumentation; `OFF` compiles to zero-cost no-ops via preprocessor macros

---

## Documentation

- **[Performance Report](docs/PERFORMANCE.md)** — Latency distributions, CPU profile, soak test results, bottleneck analysis, production tuning recommendations
- **[Design Journal](docs/DESIGN_JOURNAL.md)** — Rationale for every architectural decision: why SPSC over MPSC, linear probing over robin-hood, arena over malloc, and what was intentionally NOT optimized
- **[Soak Test Guide](docs/soak_test.md)** — Methodology for stability validation under sustained load

---

## What's Next (v2.0 Roadmap)

- Append-only binary event journal + state snapshots (crash recovery)
- Deterministic replay engine for incident investigation and regression testing
- Multi-session support (multiple venues / prime brokers)
- FX instrument extensions (spot, forward, swap with leg-aware matching)
- Divergence consumer framework (Aeron publisher, callback interface, EOD summary)

---

## How to Run

```bash
# Docker quickstart
docker compose build
docker compose up recon-daemon

# Unit + integration tests
docker compose run --rm --profile test unit-tests
docker compose run --rm --profile test integration-tests

# Microbenchmarks (Google Benchmark, zero instrumentation)
docker compose run --rm --profile dev dev-shell bash -c \
  "cmake -B build/release -DCMAKE_BUILD_TYPE=Release \
   -DCMAKE_PREFIX_PATH=/opt/aeron \
   -DFX_BUILD_BENCHMARKS=ON -DFX_PERF_ENABLED=OFF && \
   cmake --build build/release -j\$(nproc) && \
   ./build/release/reconciler_bench"

# Soak test (duration hours, total events/sec)
./scripts/soak_test.sh 0.083 20000
```
