FX Execution State Reconciliation Service
================================================
Repository: fx-exec-state-recon-daemon

FX Execution State Reconciliation Service (drop-copy vs primary, lock-free daemon) is a C++ infrastructure component for FX trading systems.

Docker quickstart
-----------------

Everything needed to build, run, and test the daemon lives inside Docker containers. The `Dockerfile` produces two images:

* `fx-recon:dev` (build/test/debug): includes the toolchain, Aeron, and the compiled build tree.
* `fx-recon:runtime` (minimal run image): contains only the runtime bits to launch the daemon or media driver.

Key commands using the simplified `docker-compose.yml`:

```bash
# Build both images (dev + runtime)
docker compose build

# Run the daemon wired to an Aeron media driver
docker compose up recon-daemon

# Run unit + integration tests (ctest preset)
docker compose run --rm --profile test unit-tests

# Run the Aeron flow integration test directly
docker compose run --rm --profile test integration-tests

# Get an interactive shell with the compiled tree for debugging
docker compose run --rm --profile dev dev-shell
```

CI/CD
-----

GitHub Actions runs the same Docker flows used locally. The workflow at `.github/workflows/ci.yml` builds the images, runs the unit suite, runs the Aeron integration flow, and then tears everything down:

```yaml
name: CI
on: [push, pull_request]
jobs:
  build-and-test:
    runs-on: ubuntu-latest
    env: { DOCKER_BUILDKIT: 1 }
    steps:
      - uses: actions/checkout@v4
      - uses: docker/setup-buildx-action@v3
      - run: docker compose build
      - run: docker compose run --rm --profile test unit-tests
      - run: docker compose run --rm --profile test integration-tests
      - run: docker compose down --volumes --remove-orphans
        if: always()
```

You do **not** need Jenkins if GitHub Actions is available. If you prefer Jenkins, point a Docker-enabled agent at this repo and use a simple pipeline:

```groovy
pipeline {
  agent any
  environment { DOCKER_BUILDKIT = '1' }
  stages {
    stage('Checkout') { steps { checkout scm } }
    stage('Build images') { steps { sh 'docker compose build' } }
    stage('Unit tests') { steps { sh 'docker compose run --rm --profile test unit-tests' } }
    stage('Integration tests') { steps { sh 'docker compose run --rm --profile test integration-tests' } }
  }
  post { always { sh 'docker compose down --volumes --remove-orphans || true' } }
}
```

Both GitHub Actions and Jenkins rely on the same `docker compose` commands shown in the quickstart, so the pipeline mirrors the developer workflow.

It ingests high-throughput execution streams from:
  - Primary execution sessions (OMS / gateway)
  - Drop-copy / exchange execution reports

and maintains a canonical, reconciled view of order and execution state in real time, with a zero-allocation, lock-free hot path suitable for HFT and low-latency FX environments.

The project is designed as if it were a production epic in an FX core team at a top-tier HFT / market-making firm.


1. Problem Statement
--------------------

In real FX trading systems you can see scenarios like:

  - Primary FIX session drops or delays a message.
  - Drop-copy shows a fill that the primary stream never reported.
  - The internal state believes an order is PendingNew, while the venue has partially filled it.
  - Message sequences on primary vs drop-copy drift, causing uncertainty about true positions.

This creates:
  - Position risk (you are longer/shorter than you think),
  - P&L misstatement,
  - Regulatory and audit problems.

This daemon solves the problem by becoming the authoritative “execution truth engine” for FX orders and fills.


2. High-Level Goals
-------------------

The service is built around these goals:

  - Reconcile primary vs drop-copy execution state in real time.
  - Detect divergences (missing fills, phantom orders, state/quantity mismatches, timing anomalies).
  - Maintain an in-memory canonical execution state per order / account / instrument.
  - Provide deterministic rebuild via event logs and snapshots.
  - Run with a lock-free, zero-allocation event-processing hot path.

It is intentionally infrastructure-focused. Monitoring, dashboards, and “pretty” tooling are out of scope for the first phase and can be added later.


3. Non-Functional Requirements
------------------------------

Target constraints (aspirational but realistic for HFT-style infra):

  - Throughput:
      - ≥ 100k execution messages per second combined (primary + drop-copy) on commodity hardware.
  - Latency:
      - Divergence detection: p99 < 5 ms, p99.9 < 20 ms from arrival of the last relevant event.
  - Hot Path:
      - No heap allocations after warm-up.
      - No locks in the event-processing path (lock-free SPSC queues + single-writer state).
  - Determinism:
      - Crash + restart from persisted logs and snapshots reconstructs the same canonical state.
  - Scope:
      - FX spot/forward/swaps; designed to be extended to additional instruments/venues.


4. Architecture Overview
------------------------

The daemon is a long-running process composed of a small set of explicit components:

  - Ingress Adapters
      - PrimaryExecIngestor:
          - Reads messages from the internal execution bus (e.g. FIX, protobuf, or a mock feed).
          - Parses and normalises into a compact ExecEvent.
      - DropCopyIngestor:
          - Reads messages from drop-copy / venue execution streams.
          - Parses FIX and normalises into ExecEvent.

  - Lock-Free Ingestion Queues
      - Two single-producer / single-consumer (SPSC) ring buffers:
          - primary_ring: primary → reconciler
          - dropcopy_ring: drop-copy → reconciler
      - Fixed-size, power-of-two capacity, cache-line aligned indices.

  - Sequencer & Session Layer
      - Per-session sequence tracking (per venue / connection).
      - Detects gaps, duplicates, and resets.
      - Attaches sequence metadata to ExecEvent.

  - Canonical State Store
      - Arena allocator:
          - Pre-allocated memory region for OrderState objects.
          - Bump-pointer allocation; no free on hot path.
      - Open-addressed hash table:
          - Key: compact OrderKey (e.g. hashed ClOrdID / internal order id).
          - Value: handle/pointer to OrderState in the arena.
      - OrderState:
          - Internal view and drop-copy view of:
              - OrdStatus
              - CumQty / LeavesQty
              - Price / AvgPx
              - Last timestamps (internal vs drop-copy)
          - Flags for anomalies and “uncertain” intervals.

  - Reconciliation Engine
      - Single-threaded core loop (or sharded by instrument).
      - Drains primary_ring and dropcopy_ring, merges streams deterministically.
      - Applies a formal order lifecycle state machine separately to:
          - internal (primary) state
          - venue (drop-copy) state
      - Compares both views after each event and classifies divergences:
          - MissingFill
          - PhantomOrder
          - StateMismatch
          - QuantityMismatch
          - TimingAnomaly

  - Divergence & Gap Streams
      - DivergenceEvent queue:
          - Structured output with type, severity, affected order, brief context.
      - SequenceGapEvent queue:
          - Flags missing or out-of-order messages on either stream.

  - Persistence & Replay (Phase 2)
      - Binary event log of normalised ExecEvent and key state changes.
      - Periodic snapshots of the canonical state.
      - Replay engine to reconstruct state and re-run reconciliation for incidents.


5. Data Model
-------------

Core types are designed for performance and clarity:

  - ExecEvent
      - Source: Primary | DropCopy
      - OrderKey
      - ExecType / OrdStatus
      - Qty / CumQty / LeavesQty
      - Price / AvgPx
      - ExecId
      - SessionId, MsgSeqNum
      - Timestamps: SendingTime, TransactTime, IngestTsc

  - OrderState
      - InternalState: lifecycle + quantities + timestamps
      - DropCopyState: lifecycle + quantities + timestamps
      - LastInternalExecId, LastDropCopyExecId
      - Flags: hasDivergence, hasGapExposure
      - Per-order statistics (optional): last divergence type, last detection time

  - DivergenceEvent
      - OrderKey
      - DivergenceType
      - InternalSnapshot (optional)
      - DropCopySnapshot (optional)
      - Severity / Confidence
      - DetectionTimestamp

  - SequenceGapEvent
      - Source (Primary | DropCopy)
      - SessionId
      - ExpectedSeqNo, SeenSeqNo
      - FirstAffectedOrderKey (if known)
      - Timestamp


6. Project Roadmap
------------------

Phase 1 – Core infra
  - Lock-free SPSC ring buffers for primary and drop-copy ingestion.
  - Basic FIX normaliser into ExecEvent.
  - Arena allocator and flat hash table for OrderState.
  - Single-threaded reconciliation engine processing both streams.
  - Minimal divergence classification and in-memory DivergenceEvent queue.
  - Per-stream sequence tracking and SequenceGapEvent queue.

Phase 2 – Persistence & Replay
  - Append-only binary event log for ExecEvent.
  - Periodic state snapshots.
  - Replayer that reconstructs identical state and replays divergences.

Phase 3 – Integration & Tooling
  - Simple CLI / RPC interface:
      - query order state by OrderKey
      - dump divergences for a given order or time range
  - EOD reconciliation summary generator (per venue, per divergence type).
  - Optional: Export divergence metrics to a monitoring system.


7. Planned Implementation Details
---------------------------------

Language & toolchain:
  - C++20 or later
  - CMake-based build
  - Linux target (x86_64)

Performance techniques:
  - SPSC ring buffers (single producer / single consumer):
      - power-of-two size, index masking instead of modulo
      - 64-byte cache-line alignment for head/tail indices
  - Preallocated arena allocator for OrderState objects:
      - no dynamic memory on hot path
  - Open-addressed hash table for orders:
      - single writer, lock-free reads
  - Branch minimisation and compact data structs for OrderState and ExecEvent.
  - TSC-based internal timing for micro-benchmarking latency.

Testing & validation:
  - Synthetic traffic generator producing primary + drop-copy flows with:
      - normal cases
      - missing primary fills
      - missing drop-copy fills
      - sequence gaps and resends
  - Deterministic functional tests:
      - given a fixed input trace, reconciliation results must be stable.
  - Latency and throughput benchmarks:
      - measure ingest → detection latency distribution
      - measure max sustainable throughput before queues saturate


8. Status
---------

This repository is being developed as a production-style, educational project for FX/HFT infrastructure engineering:

  - Focus: core infra, correctness, performance.
  - All data, venues, and identifiers in the project are synthetic and not related to any real firm.

Planned milestones:
  - v0.1 – Basic reconciliation of primary vs drop-copy from synthetic logs.
  - v0.2 – Lock-free ingestion + zero-allocation state store.
  - v0.3 – Divergence & gap detection with latency/throughput benchmarks.
  - v0.4 – Persistence and replay engine.
  - v1.0 – End-to-end, production-style daemon with query/CLI interface.


9. Why This Project Exists
--------------------------

The goal of this repository is to:

  - Demonstrate production-grade FX execution infrastructure design.
  - Showcase low-latency C++ techniques (lock-free queues, arena allocation, cache-aware data structures).
  - Show practical FX business understanding:
      - drop-copy vs primary execution,
      - order lifecycle,
      - position and P&L integrity,
      - operational and regulatory risk.

It is intentionally not a toy matching engine or generic simulator; it focuses on a real, essential piece of infrastructure that serious FX trading firms actually need to get right.


10. Containerized build and runtime
-----------------------------------

The repository ships with a multi-stage `Dockerfile` that builds Aeron and the
reconciliation binaries, plus a `docker-compose.yml` that co-locates an Aeron
media driver with the recon daemon for easy bring-up.

Build the image locally:

```
docker build -t fx-recon:latest .
```

The Dockerfile clones the Aeron tag (instead of using a tarball) so Gradle sees
Git metadata when producing the Java media driver artifacts; this avoids build
breaks during the Aeron Java/JNI packaging stage.

Launch the media driver and reconciler with the sample Aeron channels/stream IDs
from compose:

```
docker compose up --build
```

Tweak the Aeron channel endpoints/stream IDs in `docker-compose.yml` to match
your environment or venue connectivity.

11. Running tests locally (PowerShell or Bash)
---------------------------------------------

**Local Aeron dependency for non-Docker builds**

If `cmake --preset debug` fails with `Aeron client not found`, point CMake at an Aeron install that contains `include/` and
`lib/` (or `lib64/`) directories:

- PowerShell (Windows):
  ```powershell
  $env:AERON_ROOT = "C:/path/to/aeron"   # where include/aeron/Aeron.h and lib/libaeron_client.lib live
  cmake --preset debug
  ```

- Bash (Linux/macOS):
  ```bash
  export AERON_ROOT=/opt/aeron
  cmake --preset debug
  ```

To build Aeron yourself on Windows with minimal options:

```powershell
git clone https://github.com/real-logic/aeron.git
cmake -S aeron -B aeron-build -DAERON_TESTS=OFF -DAERON_SMOKE_TESTS=OFF -DCMAKE_INSTALL_PREFIX="C:/deps/aeron"
cmake --build aeron-build --config Release --target install
$env:AERON_ROOT = "C:/deps/aeron"  # reuse for this project
```

CMake also honors `aeron_DIR` or `CMAKE_PREFIX_PATH` if you prefer those variables over `AERON_ROOT`.

**Unit tests (mocked Aeron client)**

- On Windows PowerShell (requires CMake/Ninja and Aeron headers/libs on the
  host path via `AERON_ROOT`, `aeron_DIR`, or system install):

  ```powershell
  cmake --preset debug
  cmake --build --preset debug --target unit_tests
  .\build\debug\unit_tests.exe
  ```

- On Bash/Linux:

  ```bash
  cmake --preset debug
  cmake --build --preset debug --target unit_tests
  ./build/debug/unit_tests
  ```

**Integration (Aeron flow) tests**

Run the integration flow fully inside Docker using the single top-level
compose file (no extra compose overlays required):

```bash
docker compose build --profile test
docker compose run --rm --profile test integration-tests
```

The `integration-tests` service starts an embedded Aeron media driver and
launches the recon daemon and publishers inside the container, then verifies
the “Reconciler consumed” counters in the recon logs.
