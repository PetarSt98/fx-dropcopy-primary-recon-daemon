# Logging Audit

## Search Summary
No legacy `util::log` call sites existed before the async/hot split. Logging entry points are introduced by this work.

## Hot-path identification
- `src/core/reconciler.cpp` — `Reconciler::run` loop and `process_event` per `ExecEvent` (100k+ msg/s potential).
- `src/ingest/aeron_subscriber.cpp` — Aeron fragment handler producing `ExecEvent` instances.
- `src/api/demo_main.cpp` — synthetic ingest loop `ingest_thread` driving tight message production.
- `src/core/sequence_tracker.hpp` — sequence tracking helpers invoked per event.
- `src/ingest/spsc_ring.hpp` — ring push/pop used in ingest/reconciler loops.

## Slow-path identification
- `src/api/recond_main.cpp` — startup, shutdown, configuration parsing, thread lifecycle reporting.
- `src/api/demo_main.cpp` — demo startup/shutdown reporting, statistics summarisation.
- Error handling surfaced outside hot loops (configuration issues, file I/O, shutdown summaries).

## Logger routing
- **Hot channel:** `util::HotLogger` (per-thread SPSC rings, drop-new on full). Structured events with no producer-side formatting. Macros in `util/hot_log.hpp` (e.g., `HOT_SEQ_GAP`, `HOT_RING_DROP`, `HOT_STORE_OVERFLOW`) write fixed payloads; consumer formats text.
- **Warm channel:** `util::AsyncLogger` remains for occasional formatted diagnostics via `LOG_WARM_FMT`; not for tight loops.
- **Slow channel:** `util::SyncLogger` via `LOG_SLOW_*` macros for lifecycle and configuration paths.

## Call-site actions
| File | Function/Area | Frequency Risk | Action |
| ---- | ------------- | -------------- | ------ |
| `src/core/reconciler.cpp` | hot loop drop counters (seq gap ring, store overflow, divergence ring) | Hot | migrated to structured hot events (`HOT_RING_DROP`, `HOT_STORE_OVERFLOW`). |
| `src/api/demo_main.cpp` | startup/shutdown | Slow | slow logger retained; hot logger init switched to `HotLogger` config. |
| `src/api/recond_main.cpp` | startup/shutdown | Slow | slow logger retained; hot logger init switched to `HotLogger` config. |

## Event guidance
- Hot events focus on: sequence gaps, divergence detection, store/ring overflow, checksum/hash mismatches, latency samples, state anomalies, transport up/down, periodic metrics snapshots.
- Producer-side work: fill numeric fields only; no strings/formatting/heap. Consumer thread renders human-readable lines.
- Overflow policy: per-ring and global drop counters increment on full; producers never block. Monitor `HotLogger::dropped()` on a slow path to surface sustained loss.
