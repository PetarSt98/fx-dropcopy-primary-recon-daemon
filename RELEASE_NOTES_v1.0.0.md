# Release v1.0.0 — Production-Ready FX Reconciliation Engine

🎉 **First production release!**

---

## What's New

### Performance Validation

- Comprehensive latency instrumentation with TSC-based timing
- Microbenchmark suite using Google Benchmark (9 benchmarks, 5 repetitions each)
- Soak test at 20K events/sec with full event integrity verification
- CPU flame graph analysis (300-second `perf record` profile at 4999 Hz)

### Measured Performance

| Metric | Result |
|--------|--------|
| End-to-end P50 latency | 196 ns |
| End-to-end P99 latency | 379 ns |
| End-to-end P99.9 latency | 1,007 ns |
| Soak test events processed | 5,980,000 (zero drops) |
| Hash lookup (first probe) | 0.93 ns median |
| Arena allocation | 0.73 ns median |
| SPSC ring push+pop | 8.97 ns median |

### Key Features

- **Lock-free SPSC ring buffers** — ~7 ns push/pop, no contention
- **Zero-allocation hot path** — arena allocator at sub-nanosecond allocation
- **Hierarchical timing wheel** — O(1) grace period scheduling at 4.3 ns
- **Gap-aware reconciliation** — prevents false divergences during sequence gaps
- **Deterministic replay** — identical state reconstruction for audit compliance
- **Aeron transport** — reliable UDP with shared-memory media driver

### Benchmarks

| Component | Latency (Mean) |
|-----------|----------------|
| Hash lookup (first probe) | 0.93 ns |
| Arena allocation | 0.73 ns |
| Timer wheel schedule | 4.30 ns |
| Compute mismatch | 0.94 ns |
| SPSC ring push | 6.69 ns |
| SPSC ring pop | 6.64 ns |
| Hash upsert | 65.50 ns |
| Full pipeline (P99) | 379 ns |

### Documentation

- **[Performance Report](docs/PERFORMANCE.md)** — Detailed benchmark results, latency distributions, CPU profile analysis, bottleneck discussion, and reproduction instructions.
- **[Design Journal](docs/DESIGN_JOURNAL.md)** — Rationale for every major architectural decision: why SPSC over MPSC, why linear probing over robin-hood, why arena over free-list, and why NOT to optimize prematurely.
- **[Soak Test Guide](docs/soak_test.md)** — Methodology for stability validation under sustained load.

---

## Architecture

```
Primary FIX ──► Aeron ──► SPSC Ring ──┐
                                      ├──► Reconciler ──► Divergence Stream
Dropcopy FIX ──► Aeron ──► SPSC Ring ──┘        │
                                          OrderStateStore
                                          (Arena + Hash Table)
```

- **Ingestion:** Aeron UDP transport with per-stream SPSC ring buffers
- **State Store:** Open-addressed hash table backed by arena allocator
- **Reconciliation:** Single-threaded core with deterministic event ordering
- **Grace Periods:** Hierarchical timing wheel for deferred divergence detection

---

## What's Next (v2.0.0 Roadmap)

- Multi-threaded reconciliation (sharded by instrument)
- Prometheus metrics export
- Gap-fill recovery protocol
- Persistence and replay engine (Phase 2)

---

## How to Run

```bash
# Docker quickstart
docker compose build
docker compose up recon-daemon

# Run tests
docker compose run --rm --profile test unit-tests
docker compose run --rm --profile test integration-tests

# Run benchmarks
docker compose run --rm dev-shell bash -c \
  "cmake -B build/release -DCMAKE_BUILD_TYPE=Release \
   -DCMAKE_PREFIX_PATH=/opt/aeron \
   -DFX_BUILD_BENCHMARKS=ON -DFX_PERF_ENABLED=OFF && \
   cmake --build build/release -j\$(nproc) && \
   ./build/release/reconciler_bench"
```

---

*This release demonstrates production-grade low-latency systems design with comprehensive performance validation.*
