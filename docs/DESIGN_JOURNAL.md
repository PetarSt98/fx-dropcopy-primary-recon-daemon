# Design Journal

This document captures the key design decisions, trade-offs, and rationale behind the FX Execution State Reconciliation Daemon.

---

## 1. Why C++17 with Zero-Allocation Hot Path?

**Decision:** Single-threaded reconciliation core with no heap allocations after warm-up.

**Rationale:**
- FX reconciliation is latency-sensitive — divergence detection must complete before positions drift further.
- Heap allocations invoke `malloc`, which can trigger `mmap`/`brk` syscalls and introduce unpredictable latency spikes.
- By pre-allocating all memory (arena + hash table + ring buffers) during startup, the hot path avoids syscall jitter entirely.

**Trade-off:** Higher initial memory footprint (~580 MB RSS at startup) in exchange for deterministic latency.

---

## 2. Lock-Free SPSC Ring Buffers

**Decision:** Use single-producer/single-consumer (SPSC) ring buffers instead of mutexes or MPSC queues.

**Rationale:**
- The architecture has exactly one producer per ring (primary ingestor → `primary_ring`, dropcopy ingestor → `dropcopy_ring`) and one consumer (the reconciler).
- SPSC rings are the simplest lock-free structure: only `std::atomic` load/store on head/tail indices, no CAS loops.
- Measured push/pop latency: **~7 ns median**, **~15 ns P99** — effectively free compared to mutex contention.

**Alternative considered:** `folly::ProducerConsumerQueue` — rejected because it adds a third-party dependency for marginal benefit over a hand-rolled SPSC ring.

---

## 3. Open-Addressed Hash Table with Linear Probing

**Decision:** Custom open-addressed hash table with linear probing for `OrderState` lookup by `OrderKey`.

**Rationale:**
- `std::unordered_map` allocates nodes on the heap (one `new` per insert) — incompatible with zero-allocation hot path.
- Open addressing stores entries inline in a contiguous array, which is cache-friendly and arena-compatible.
- Linear probing gives good cache locality for sequential probes (prefetch-friendly).

**Measured performance:**
- First-probe lookup: **0.93 ns** (median)
- At 90% load factor: **6.35 ns** (median)
- Upsert: **65.5 ns** (median) — includes allocation of new `OrderState` on first insert.

**Trade-off:** Linear probing degrades at high load factors due to clustering. We size the table to stay below 70% load in production, giving ~3 average probes.

---

## 4. Arena Allocator (Bump Pointer)

**Decision:** Pre-allocate a single contiguous memory region and allocate `OrderState` objects via bump pointer.

**Rationale:**
- Bump-pointer allocation is `O(1)` — just increment a pointer. No free-list management, no fragmentation.
- Measured: **0.73 ns** per allocation (median microbenchmark), **10 ns P50** under instrumented load.
- `OrderState` objects are never individually freed on the hot path. The arena is recycled in bulk during maintenance windows.

**Trade-off:** Memory is not reclaimed until the arena is reset. This is acceptable because peak active orders are far below arena capacity.

---

## 5. Hierarchical Timing Wheel for Grace Periods

**Decision:** Use a hierarchical timing wheel for scheduling grace-period expirations instead of a priority queue.

**Rationale:**
- After receiving a primary execution event, the reconciler waits a configurable grace period before flagging a missing dropcopy.
- A binary heap (`std::priority_queue`) gives `O(log n)` insert — fine for small n, but constant factors matter at nanosecond scale.
- A timing wheel gives `O(1)` insert by hashing the expiration time into a bucket.

**Measured:** **4.30 ns** median schedule, **13 ns P50** under instrumented load.

**Trade-off:** Timing wheels have limited resolution (bucket granularity). For this use case, millisecond-level granularity is sufficient.

---

## 6. Gap-Aware Reconciliation

**Decision:** Suppress divergence alerts during detected sequence gaps.

**Rationale:**
- When the primary or dropcopy stream has a sequence gap, the reconciler cannot know if missing events exist in the gap.
- Firing divergence alerts during a gap would produce false positives, eroding trust in the system.
- Instead, the reconciler marks affected orders as "gap-exposed" and defers divergence classification until the gap is resolved (via replay or gap-fill).

**Trade-off:** Delayed divergence detection during gaps. Acceptable because gap-fill typically resolves within seconds.

---

## 7. Deterministic Replay

**Decision:** All reconciliation logic is deterministic given the same input event sequence.

**Rationale:**
- For audit compliance and incident investigation, replaying the same events must produce the same state and the same divergences.
- No use of wall-clock time in reconciliation decisions — all timing is based on event timestamps and TSC counters (assumes invariant TSC / `constant_tsc` CPU feature, standard on modern x86_64).
- This enables "replay from log" for post-mortem analysis without approximation.

---

## 8. Aeron for Transport

**Decision:** Use Aeron (UDP-based messaging) for ingestion transport instead of raw sockets or TCP-based messaging.

**Rationale:**
- Aeron provides reliable, ordered UDP delivery with built-in flow control and backpressure.
- Single-writer/single-reader semantics map cleanly to our SPSC architecture.
- Aeron's shared-memory (`/dev/shm`) media driver avoids kernel-space network stack overhead for co-located processes.
- Well-tested in production at multiple HFT firms.

**Trade-off:** Aeron adds operational complexity (media driver process, shared memory management). Mitigated by Docker Compose orchestration.

---

## 9. Why NOT Optimize the Hash Table Further

**Decision:** Keep linear probing despite profiling showing `OrderStateStore::find` at 15.80% CPU.

**Rationale:**
- Current throughput headroom is sufficient for target load.
- Robin-hood hashing or SIMD bucket scanning would reduce probe count but add implementation complexity.
- The hash table is not the bottleneck — the system sustains the target event rate with CPU headroom to spare.
- Premature optimization adds debugging complexity without business value.

**Revisit if:** Probe count monitoring shows degradation at higher load factors or throughput targets increase.

---

## 10. Why NOT Implement Arena Recycling

**Decision:** No per-object deallocation or free-list in the arena allocator.

**Rationale:**
- Arena capacity vastly exceeds peak active orders.
- Recycling adds ~200 lines of code, a free-list data structure, and subtle use-after-free risks.
- Bulk reset during maintenance windows is simpler and safer.

**Revisit if:** Arena utilization approaches capacity or the system needs to run indefinitely without maintenance windows.

---

## References

- [Performance Analysis](PERFORMANCE.md) — Detailed benchmark results and profiling data
- [Soak Test Guide](soak_test.md) — Stability validation methodology
- [Main README](../README.md) — Project overview and quickstart
