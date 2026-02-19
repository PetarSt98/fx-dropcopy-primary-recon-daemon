# v2 Roadmap — FX Execution State Reconciliation Daemon

This document describes the planned features and architectural changes for **v2**
of the FX drop-copy / primary reconciliation daemon. Each section explains
**what** will be built, **why** it is needed, and how it fits into the existing
architecture.

> **v1 recap** — v1 delivers lock-free SPSC ingestion, arena-backed state store,
> single-threaded reconciliation with grace-period divergence detection, Aeron
> transport, and sub-microsecond P99 end-to-end latency. See
> [RELEASE_NOTES_v1.0.0.md](RELEASE_NOTES_v1.0.0.md) for measured numbers.

---

## 1. Persistence Layer

### What

Introduce an append-only binary event journal and periodic state snapshots so
the daemon can survive process restarts and reconstruct its full canonical state
without re-ingesting the entire day's traffic.

| Component | Description |
|-----------|-------------|
| **Event Journal** | Every normalised `ExecEvent` and every `DivergenceEvent` is written sequentially to a memory-mapped, pre-allocated binary log file. Records use a fixed-size header (type tag, length, CRC-32C) followed by the serialised payload. |
| **State Snapshots** | At configurable intervals (e.g. every *N* events or every *T* seconds) the daemon serialises the full `OrderStateStore` (arena + hash table) to a snapshot file. Snapshots use the same binary framing as the journal. |
| **Recovery on Startup** | On restart the daemon locates the latest snapshot, loads it into the arena, then replays journal entries written after the snapshot sequence number to reach the exact pre-crash state. |

### Why

- **Operational safety** — process crashes, rolling upgrades, and host reboots
  must not lose reconciliation context. Without persistence the daemon would
  start with an empty state store and miss divergences that depend on prior
  order history (e.g. a partially-filled order whose fill arrived before the
  crash).
- **Auditability** — regulators and compliance teams need an immutable record
  of every execution event the system observed and every divergence it raised.
  The event journal provides exactly this.
- **Foundation for replay** — the deterministic replay engine (§3) requires
  persisted events to function.

### Design Constraints

- Journal writes must stay off the critical reconciliation path; the hot loop
  appends to an in-memory ring that a background I/O thread drains to disk.
- Snapshot serialisation must be copy-on-write–friendly so the reconciler is not
  blocked while the snapshot is flushed.
- File format must be forward-compatible: a version byte in the header allows
  older readers to skip unknown record types.

---

## 2. Instrument Support — Spot, Forward, Swap

### What

Extend the data model and reconciliation logic to handle FX **spot**, **forward**,
and **swap** instruments natively, rather than treating every event as a generic
order.

| Change | Detail |
|--------|--------|
| **`InstrumentType` enum** | Add `Spot`, `Forward`, `Swap` (and `Unknown`) to `ExecEvent` and `OrderState`. |
| **Leg-aware state** | An FX swap is two linked legs (near + far). The state store will support multi-leg order groups so divergence detection can compare both legs as a unit. |
| **Tenor & value-date fields** | `ExecEvent` gains `value_date` (int32, YYYYMMDD) and `tenor` (fixed string, e.g. "1M", "TN") to distinguish forward tenors and spot T+2 settlement. |
| **Instrument-specific matching rules** | Spot orders match on `ClOrdID` alone; swap legs match on `ClOrdID` + `LegNo`. The reconciler dispatches to the right comparator based on instrument type. |

### Why

- **Completeness** — real FX desks trade spot, outrights, and swaps. A
  reconciliation engine that only handles spot misses the most structurally
  complex (and risk-prone) product: the swap, where a fill on one leg without
  the other creates delta and funding risk.
- **Accuracy** — forward and swap fills carry value dates and tenors that
  affect settlement and cash-flow expectations. Without these fields, the
  reconciler cannot detect value-date mismatches or mis-booked tenors.
- **Production readiness** — any serious FX venue adapter produces `SecurityType`
  (FX spot, FX forward, FX swap) in its FIX messages. The daemon should parse
  and utilise this field rather than ignore it.

### Design Constraints

- The `ExecEvent` struct must stay cache-line friendly; new fields are packed
  into existing padding where possible.
- The hot-path reconciliation function must branch on instrument type at most
  once per event (virtual dispatch is acceptable if devirtualised at compile
  time via CRTP or `std::variant`).

---

## 3. Deterministic Replay Engine

### What

Build a replay binary (`recon_replay`) that reads a persisted event journal
(and optionally a snapshot) and re-runs the reconciliation pipeline from
scratch, producing an identical sequence of divergence events.

| Capability | Detail |
|------------|--------|
| **Full replay** | Read journal from the start (or from a snapshot), feed events into the reconciler in original ingestion order, and emit divergences. |
| **Windowed replay** | Replay only events within a time or sequence range, useful for incident investigation. |
| **Diff mode** | Compare the divergences produced by the replay against a stored divergence log and report any discrepancies — verifying that the engine is truly deterministic. |
| **Dry-run mode** | Replay without persisting new output; useful for debugging a code change against real production logs. |

### Why

- **Incident investigation** — when a trader or risk manager reports a P&L
  break, the team needs to replay the exact event stream to understand why the
  reconciler did (or did not) flag a divergence.
- **Regression testing** — real production traces (anonymised) become golden
  test inputs. A code change that alters divergence output for a known trace is
  immediately caught.
- **Regulatory compliance** — several jurisdictions require the ability to
  reconstruct execution state at any point during the trading day. Deterministic
  replay satisfies this requirement without needing a full database.

### Design Constraints

- Replay must produce **bit-identical** divergence output for the same input
  journal, regardless of wall-clock time, host, or CPU. This means the
  reconciler must never use `std::chrono::system_clock` or `rdtsc` in its
  decision logic (only in instrumentation counters).
- The replay binary shares the same reconciler and state-store code as the live
  daemon — no separate "offline" implementation.

---

## 4. Multi-Session Support

### What

Allow the daemon to ingest and reconcile events from **multiple concurrent FIX
sessions** (e.g. several prime brokers, multiple venue connections, or
primary + backup gateways).

| Change | Detail |
|--------|--------|
| **Session registry** | A runtime-configurable table mapping `(SessionId, Source)` to an Aeron stream ID, sequence-number space, and session-specific parameters (heartbeat interval, expected latency). |
| **Per-session SPSC rings** | Instead of two global rings (primary + drop-copy), the daemon creates a ring pair per session. The reconciler round-robins or priority-drains across all rings. |
| **Cross-session matching** | Orders may appear on different sessions (e.g. primary on session A, drop-copy on session B). The reconciler matches by `ClOrdID` / `OrderID` across sessions, using the session registry to know which pairs are logically linked. |
| **Session lifecycle events** | Handle session logon, logout, and sequence reset cleanly. On a drop-copy reconnect the daemon marks all open orders from that session as "uncertain" until the gap is resolved. |

### Why

- **Production topology** — real FX firms connect to multiple venues (EBS,
  Reuters, Cboe FX, single-bank platforms) and receive drop-copy from each. A
  single-session daemon cannot reconcile across all of them without manual
  multiplexing.
- **Resilience** — primary/backup gateway failover changes the session ID for
  the same logical order flow. The daemon must handle session switches without
  losing reconciliation continuity.
- **Scalability** — per-session rings isolate back-pressure. A slow venue
  connection cannot stall reconciliation of a fast one.

### Design Constraints

- The number of sessions is fixed at startup (configured, not dynamic) to avoid
  runtime memory allocation.
- The reconciler's core loop must remain single-threaded. Multi-session support
  is achieved by draining multiple rings in the same loop iteration, not by
  adding threads.
- Session configuration is loaded from a simple TOML or JSON file at startup.

---

## 5. Divergence Consumer Framework

### What

Provide a structured output path for divergence events so downstream systems
(risk, compliance, monitoring) can consume them in real time.

| Component | Description |
|-----------|-------------|
| **Aeron divergence publisher** | Serialise `DivergenceEvent` to a dedicated Aeron channel/stream so downstream consumers receive divergences with the same low-latency transport used for ingestion. |
| **Callback interface** | A `DivergenceConsumer` abstract class with a single `on_divergence(const DivergenceEvent&)` method. The daemon ships with a logging consumer and an Aeron-publish consumer; users can implement their own (e.g. write to Kafka, send an alert, update a dashboard). |
| **Batched EOD summary** | At end-of-day (or on demand via a CLI signal) the daemon produces a summary report: total events per session, divergences by type and severity, sequence gaps, and per-instrument statistics. |
| **Severity filtering** | Consumers can subscribe to a minimum severity level (e.g. only `Critical` and `High`), reducing noise for operational dashboards while keeping full fidelity in the event journal. |

### Why

- **Actionable output** — detecting a divergence is only valuable if someone or
  something can act on it. The consumer framework bridges the gap between
  detection and response.
- **Separation of concerns** — the reconciliation engine should not know or care
  about how divergences are delivered. A pluggable consumer interface keeps the
  core clean and testable.
- **Operational visibility** — the EOD summary gives the desk a single-page
  view of reconciliation health, replacing manual spreadsheet-based checks that
  are error-prone and delayed.

### Design Constraints

- The `on_divergence` callback is invoked **synchronously** on the reconciler
  thread. Consumers that perform I/O must copy the event and hand off to their
  own background thread to avoid stalling the hot path.
- The Aeron publisher consumer reuses the daemon's existing Aeron publication
  infrastructure; no new media-driver connections are opened.

---

## Prioritisation & Dependencies

The features above have the following dependency order:

```
Persistence ──► Deterministic Replay
     │
     └──► (independent) Instrument Support
                          Multi-Session Support
                          Divergence Consumer
```

**Suggested implementation order:**

| Priority | Feature | Rationale |
|----------|---------|-----------|
| 1 | Persistence layer | Foundation for replay; also the most operationally urgent (crash recovery). |
| 2 | Deterministic replay | Immediately useful once persistence exists; unlocks regression testing against production traces. |
| 3 | Multi-session support | Required before the daemon can be deployed against real multi-venue topologies. |
| 4 | Instrument support (spot/forward/swap) | Adds business-domain correctness; can be developed in parallel with multi-session. |
| 5 | Divergence consumer framework | Highest value when all other features are in place and the daemon is producing real divergence output. |

---

## Non-Goals for v2

The following are explicitly **out of scope** for v2 and deferred to later
releases:

- GUI / web dashboard for divergence visualisation.
- Automatic gap-fill recovery (requesting retransmission from the venue).
- Multi-threaded / sharded reconciliation (the core loop stays single-threaded).
- Support for non-FX asset classes (equities, futures, options).
- Prometheus / OpenTelemetry metrics export (may be added as a divergence
  consumer plugin, but is not a v2 deliverable).
