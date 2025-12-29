# Logging Audit

## Search Summary
No existing calls to `util::log` were found in the repository. The only logger definition before this change lived in `src/util/log.hpp` and was included by a few translation units but not invoked.

## Hot-path identification
- `src/core/reconciler.cpp` — `Reconciler::run` loop and `process_event` dispatching per `ExecEvent` (100k+ msg/s potential).
- `src/ingest/aeron_subscriber.cpp` — Aeron fragment handler inside `AeronSubscriber::run` producing `ExecEvent` instances.
- `src/api/demo_main.cpp` — synthetic ingest loop `ingest_thread` producing `ExecEvent` instances in tight loop (test/demo hot path).
- `src/core/sequence_tracker.hpp` — sequence tracking helpers invoked per event.
- `src/ingest/spsc_ring.hpp` — ring buffer push/pop used in ingest/reconciler loops.

## Slow-path identification
- `src/api/recond_main.cpp` — startup, shutdown, configuration parsing, thread lifecycle reporting.
- `src/api/demo_main.cpp` — demo startup/shutdown reporting, statistics summarisation.
- Error handling paths across modules (e.g., store overflow or parse failures when surfaced outside the hot loop).

## Logger usage classification
| File | Function/Area | Frequency Risk | Action |
| ---- | ------------- | -------------- | ------ |
| _No existing call sites_ | — | — | Introduced routing macros to keep synchronous logger for slow paths and new async logger for hot paths; new call sites added with this patch are routed appropriately. |

## Strategy
- Hot-path logging uses the new `util::AsyncLogger` (lock-free MPSC ring, drop-on-full) via `LOG_HOT_*` macros that accept literals plus numeric arg slots (no per-call formatting).
- `LOG_WARM_FMT`/`LOG_HOT_FMT` remain available for rare diagnostics but are not intended for tight loops.
- Slow-path logging continues to use `util::SyncLogger` (mutex-protected `stderr`) via `LOG_SLOW_*` macros for startup/configuration and summary output.
- Selected hot-path counters (`Reconciler` divergence/gap drops) now emit async logs; slow-path binaries log lifecycle events via sync logger.
