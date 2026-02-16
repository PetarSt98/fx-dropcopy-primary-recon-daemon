#!/bin/bash
#
# generate_flamegraph.sh - CPU Profiling & Flame Graph Generation
#
# Profiles the FX reconciliation daemon under load using Linux perf
# and generates interactive SVG flame graphs for analysis.
#
# Usage:
#   # Full-system mode (Aeron + reconciler under load):
#   ./scripts/generate_flamegraph.sh [duration_seconds] [total_events_per_sec]
#
#   # Benchmark-only mode (pure business logic, NO Aeron I/O):
#   ./scripts/generate_flamegraph.sh --bench [duration_seconds]
#
# Arguments:
#   --bench               - Profile the reconciler_bench binary instead of the
#                           full system. Eliminates all Aeron/I/O noise from
#                           the flame graph so only business logic is visible.
#   duration_seconds      - Profiling duration in seconds (default: 60 for
#                           full-system, 30 for --bench)
#   total_events_per_sec  - TOTAL system event rate across BOTH publishers
#                           (default: 10000). Each publisher gets 50% of this rate.
#                           Only used in full-system mode.
#
# Example:
#   ./scripts/generate_flamegraph.sh 60 10000   # 60s full-system at 10k events/sec
#   ./scripts/generate_flamegraph.sh 30 5000    # 30s full-system at 5k events/sec
#   ./scripts/generate_flamegraph.sh --bench 30 # 30s benchmark-only profile
#
# Output:
#   docs/performance/flamegraph.svg     - Interactive flame graph (open in browser)
#   docs/performance/perf_report.txt    - Full perf report
#   docs/performance/top_functions.txt  - Top 20 CPU hot spots
#

set -euo pipefail

# ============================================================================
# Mode Detection
# ============================================================================

BENCH_MODE=false
if [[ "${1:-}" == "--bench" ]]; then
    BENCH_MODE=true
    shift
fi

# ============================================================================
# Configuration & Defaults
# ============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly SCRIPT_DIR
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
readonly REPO_ROOT
readonly BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build/release}"

if [[ "${BENCH_MODE}" == true ]]; then
    # Benchmark-only mode defaults
    readonly DURATION_SECS="${1:-30}"
    readonly ORDERS_PER_SEC=0
else
    # Full-system mode defaults
    readonly DURATION_SECS="${1:-60}"
    # ORDERS_PER_SEC is the TOTAL system ingress rate (combined across both publishers)
    # Each publisher will send at ORDERS_PER_SEC / 2
    readonly ORDERS_PER_SEC="${2:-10000}"
fi

# Aeron configuration
readonly AERON_DIR="${AERON_DIR:-/dev/shm/aeron-perf-$$}"
readonly PRIMARY_CHANNEL="aeron:udp?endpoint=localhost:20121"
readonly DROPCOPY_CHANNEL="aeron:udp?endpoint=localhost:20122"
readonly PRIMARY_STREAM=1001
readonly DROPCOPY_STREAM=1002

# FlameGraph toolkit
readonly FLAMEGRAPH_DIR="${REPO_ROOT}/tools/FlameGraph"

# Output directory
readonly OUTPUT_DIR="${REPO_ROOT}/docs/performance"

# Executables
readonly AERONMD="${AERONMD:-aeronmd}"
readonly FX_EXEC_RECOND="${BUILD_DIR}/fx_exec_recond"
readonly FX_AERON_PUBLISHER="${BUILD_DIR}/fx_aeron_publisher"
readonly RECONCILER_BENCH="${BUILD_DIR}/reconciler_bench"

# Warmup before profiling (seconds)
readonly WARMUP_SECS=5

# Extra buffer so publishers outlast the profiling window (seconds)
readonly PUBLISHER_BUFFER_SECS=10

# Determine if we need sudo for perf (not needed when running as root in Docker)
if [[ "$(id -u)" -eq 0 ]]; then
    SUDO=""
else
    SUDO="sudo"
fi

# Find the real perf binary, bypassing Ubuntu's kernel-version-checking wrapper.
# On WSL2, the wrapper fails because Microsoft's custom kernel version doesn't
# match any Ubuntu linux-tools package. We locate the actual ELF binary directly.
find_perf_binary() {
    local candidate
    # Search for real perf binary (may be a symlink, so don't use -type f)
    candidate=$(find /usr/lib/linux-tools /usr/lib/linux-tools-* -name perf 2>/dev/null | head -1)
    if [[ -n "${candidate}" && -x "${candidate}" ]]; then
        echo "${candidate}"
        return 0
    fi
    # Fallback: try the standard perf command (works on native Linux)
    if perf --version &>/dev/null; then
        command -v perf
        return 0
    fi
    return 1
}

PERF_CMD=$(find_perf_binary) || {
    echo "FATAL: Could not find a working perf binary." >&2
    echo "  Searched: /usr/lib/linux-tools/*/perf and PATH" >&2
    echo "  Install: apt-get install linux-tools-common linux-tools-generic" >&2
    exit 1
}
readonly PERF_CMD
echo "[perf] Using: ${PERF_CMD}" >&2

# PID tracking
MEDIA_DRIVER_PID=""
RECOND_PID=""
PRIMARY_PUB_PID=""
DROPCOPY_PUB_PID=""

# ============================================================================
# Utility Functions
# ============================================================================

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" >&2
}

error() {
    log "ERROR: $*"
    exit 1
}

check_executable() {
    local exe="$1"
    local name="$2"
    if [[ ! -x "${exe}" ]]; then
        error "${name} not found or not executable: ${exe}"
    fi
}

# ============================================================================
# Cleanup & Signal Handling
# ============================================================================

cleanup() {
    local exit_code=$?
    log "Cleanup initiated (exit code: ${exit_code})"

    # Terminate all child processes
    local pids=(
        "${DROPCOPY_PUB_PID}"
        "${PRIMARY_PUB_PID}"
        "${RECOND_PID}"
        "${MEDIA_DRIVER_PID}"
    )

    for pid in "${pids[@]}"; do
        if [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null; then
            log "Terminating process ${pid}"
            kill "${pid}" 2>/dev/null || true
            sleep 0.5
            if kill -0 "${pid}" 2>/dev/null; then
                log "Force killing process ${pid}"
                kill -9 "${pid}" 2>/dev/null || true
            fi
        fi
    done

    # Clean up perf.data if it exists
    if [[ -f "${REPO_ROOT}/perf.data" ]]; then
        log "Removing perf.data"
        rm -f "${REPO_ROOT}/perf.data"
    fi

    # Clean up Aeron directory
    if [[ -z "${AERON_DIR}" ]]; then
        log "Refusing to remove Aeron directory: AERON_DIR is empty"
    elif [[ "${AERON_DIR}" != /dev/shm/aeron-perf* ]]; then
        log "Refusing to remove suspicious Aeron directory (unexpected path): ${AERON_DIR}"
    elif [[ ! -f "${AERON_DIR}/cnc.dat" ]]; then
        log "Refusing to remove Aeron directory: cnc.dat not found in ${AERON_DIR}"
    else
        log "Removing Aeron directory: ${AERON_DIR}"
        rm -rf "${AERON_DIR}"
    fi

    log "Cleanup complete"
}

trap cleanup EXIT INT TERM

# ============================================================================
# Dependency Validation
# ============================================================================

validate_dependencies() {
    log "Validating dependencies..."

    # Check executables
    check_executable "${FX_EXEC_RECOND}" "fx_exec_recond"
    check_executable "${FX_AERON_PUBLISHER}" "fx_aeron_publisher"

    if ! command -v "${AERONMD}" &> /dev/null; then
        error "aeronmd not found in PATH"
    fi

    # Perf was already resolved at startup via find_perf_binary()
    check_executable "${PERF_CMD}" "perf"

    # Check sudo availability (not needed when running as root)
    if [[ -n "${SUDO}" ]] && ! command -v sudo &> /dev/null; then
        error "sudo not found. sudo is required for perf record (or run as root)"
    fi

    # Check FlameGraph toolkit
    if [[ ! -x "${FLAMEGRAPH_DIR}/stackcollapse-perf.pl" ]]; then
        error "FlameGraph toolkit not found at ${FLAMEGRAPH_DIR}. Install with: git clone --branch v1.0 --depth 1 https://github.com/brendangregg/FlameGraph.git tools/FlameGraph"
    fi

    if [[ ! -x "${FLAMEGRAPH_DIR}/flamegraph.pl" ]]; then
        error "flamegraph.pl not found in ${FLAMEGRAPH_DIR}"
    fi

    log "All dependencies validated"
}

# ============================================================================
# Component Launch
# ============================================================================

launch_media_driver() {
    log "Launching Aeron MediaDriver"

    mkdir -p "${AERON_DIR}"

    # Export AERON_DIR for child processes
    export AERON_DIR

    # Launch aeronmd in background
    "${AERONMD}" \
        "-Daeron.dir=${AERON_DIR}" \
        "-Daeron.socket.soReusePort=true" \
        > /dev/null 2>&1 &

    MEDIA_DRIVER_PID=$!
    log "MediaDriver started (PID ${MEDIA_DRIVER_PID})"

    # Wait for cnc.dat to appear (timeout in seconds)
    local timeout=10
    local start_time
    start_time=$(date +%s)
    while :; do
        if [[ -f "${AERON_DIR}/cnc.dat" ]]; then
            log "MediaDriver ready (cnc.dat found)"
            return 0
        fi

        local now
        now=$(date +%s)
        if (( now - start_time >= timeout )); then
            break
        fi

        sleep 0.5
    done

    error "MediaDriver failed to start (cnc.dat not found)"
}

launch_reconciler() {
    log "Launching reconciler daemon"

    export AERON_DIR
    # Keep reconciler alive for the full profiling window (backgrounded stdin is EOF)
    local run_ms=$(( (DURATION_SECS + WARMUP_SECS + PUBLISHER_BUFFER_SECS + 30) * 1000 ))
    export RECOND_RUN_MS="${run_ms}"

    "${FX_EXEC_RECOND}" \
        "${PRIMARY_CHANNEL}" \
        "${PRIMARY_STREAM}" \
        "${DROPCOPY_CHANNEL}" \
        "${DROPCOPY_STREAM}" \
        > /dev/null 2>&1 &

    RECOND_PID=$!
    log "Reconciler started (PID ${RECOND_PID})"

    # Give it time to initialize
    sleep 2

    if ! kill -0 "${RECOND_PID}" 2>/dev/null; then
        error "Reconciler failed to start"
    fi
}

launch_publisher() {
    local name="$1"
    local channel="$2"
    local stream="$3"

    log "Launching ${name} publisher"

    # ORDERS_PER_SEC is the TOTAL system rate, split evenly between both publishers
    # Each publisher gets 50% of the total target rate
    local per_publisher_rate=$((ORDERS_PER_SEC / 2))

    # Calculate sleep_us (microseconds) to achieve desired rate
    # sleep_us = 1,000,000 / rate. For rates >= 500k/sec: unthrottled (0).
    local sleep_us=0
    if [[ "${per_publisher_rate}" -lt 500000 ]]; then
        sleep_us=$((1000000 / per_publisher_rate))
    fi

    # Calculate total events for this publisher over the profiling + warmup duration
    local total_duration=$((DURATION_SECS + WARMUP_SECS + PUBLISHER_BUFFER_SECS))
    local total_events=$((per_publisher_rate * total_duration))

    export AERON_DIR

    "${FX_AERON_PUBLISHER}" \
        "${channel}" \
        "${stream}" \
        "${total_events}" \
        "${sleep_us}" \
        > /dev/null 2>&1 &

    local pid=$!
    log "${name} publisher started (PID ${pid}, target: ${total_events} events over ${total_duration}s)"

    if [[ "${name}" == "Primary" ]]; then
        PRIMARY_PUB_PID="${pid}"
    else
        DROPCOPY_PUB_PID="${pid}"
    fi
}

# ============================================================================
# Profiling
# ============================================================================

run_profiling() {
    log "Starting perf profiling of reconciler (PID ${RECOND_PID}) for ${DURATION_SECS} seconds..."

    # Record CPU profile with call graphs at 999Hz
    ${SUDO} "${PERF_CMD}" record -F 999 -g -p "${RECOND_PID}" -o "${REPO_ROOT}/perf.data" -- sleep "${DURATION_SECS}"

    log "Perf recording complete"
}

generate_reports() {
    log "Generating reports..."

    mkdir -p "${OUTPUT_DIR}"

    # Temporarily disable pipefail for report pipelines (head causes SIGPIPE)
    set +o pipefail

    # Generate full perf report
    log "Generating perf report..."
    ${SUDO} "${PERF_CMD}" report -i "${REPO_ROOT}/perf.data" --stdio --no-children \
        > "${OUTPUT_DIR}/perf_report.txt" 2>/dev/null || true

    # Generate top functions summary
    log "Generating top functions..."
    ${SUDO} "${PERF_CMD}" report -i "${REPO_ROOT}/perf.data" --stdio --no-children --percent-limit 0.1 \
        | head -80 > "${OUTPUT_DIR}/top_functions.txt" 2>/dev/null || true

    # Generate flame graph
    log "Generating flame graph SVG..."
    local fg_title
    if [[ "${BENCH_MODE}" == true ]]; then
        fg_title="FX Reconciler BENCHMARK Profile (${DURATION_SECS}s, pure business logic)"
    else
        fg_title="FX Reconciler CPU Profile (${DURATION_SECS}s @ ${ORDERS_PER_SEC} events/sec)"
    fi
    ${SUDO} "${PERF_CMD}" script -i "${REPO_ROOT}/perf.data" \
        | "${FLAMEGRAPH_DIR}/stackcollapse-perf.pl" \
        | "${FLAMEGRAPH_DIR}/flamegraph.pl" \
            --title "${fg_title}" \
            --subtitle "$(date '+%Y-%m-%d %H:%M:%S')" \
        > "${OUTPUT_DIR}/flamegraph.svg" 2>/dev/null || true

    set -o pipefail

    # Clean up perf.data
    log "Cleaning up perf.data"
    rm -f "${REPO_ROOT}/perf.data"

    log "Reports generated successfully"
}

# ============================================================================
# Benchmark-Only Mode
# ============================================================================

main_bench() {
    log "Starting BENCHMARK-ONLY CPU profiling (no Aeron I/O)"
    log "Duration: ~${DURATION_SECS}s (Google Benchmark will run for this long)"

    # Validate benchmark binary
    check_executable "${RECONCILER_BENCH}" "reconciler_bench"
    check_executable "${PERF_CMD}" "perf"

    # Check FlameGraph toolkit
    if [[ ! -x "${FLAMEGRAPH_DIR}/stackcollapse-perf.pl" ]]; then
        error "FlameGraph toolkit not found at ${FLAMEGRAPH_DIR}"
    fi

    mkdir -p "${OUTPUT_DIR}"

    # Google Benchmark runs each benchmark for --benchmark_min_time.
    # With ~10 benchmarks, per-bench time = total / 10 to keep wall clock close to DURATION_SECS.
    local num_benchmarks=10
    local per_bench_secs=$(( DURATION_SECS / num_benchmarks ))
    if [[ "${per_bench_secs}" -lt 2 ]]; then
        per_bench_secs=2
    fi

    log "Launching reconciler_bench under perf (~${per_bench_secs}s per benchmark, ~${DURATION_SECS}s total)..."

    # Profile the benchmark binary directly - pure business logic
    # stderr shows benchmark progress; stdout shows results
    ${SUDO} "${PERF_CMD}" record -F 4999 -g \
        -o "${REPO_ROOT}/perf.data" \
        -- "${RECONCILER_BENCH}" \
            --benchmark_min_time="${per_bench_secs}s" \
            --benchmark_repetitions=1 \
            --benchmark_enable_random_interleaving=false \
        || true

    if [[ ! -f "${REPO_ROOT}/perf.data" ]]; then
        error "perf.data not found - profiling failed"
    fi

    # Generate reports (same pipeline as full-system mode)
    generate_reports

    # Print summary
    echo ""
    echo "============================================================"
    echo "Benchmark-Only CPU Profiling Complete"
    echo "============================================================"
    echo ""
    echo "Mode:           --bench (pure business logic, NO Aeron I/O)"
    echo "Duration:       ~${DURATION_SECS} seconds"
    echo "Binary:         ${RECONCILER_BENCH}"
    echo "Sample rate:    4999 Hz"
    echo ""
    echo "Output files:"
    echo "  Flame graph:    ${OUTPUT_DIR}/flamegraph.svg"
    echo "  Perf report:    ${OUTPUT_DIR}/perf_report.txt"
    echo "  Top functions:  ${OUTPUT_DIR}/top_functions.txt"
    echo ""
    echo "View flame graph:"
    echo "  firefox ${OUTPUT_DIR}/flamegraph.svg"
    echo ""
    echo "============================================================"
    echo ""

    log "Benchmark-only profiling completed successfully"
}

# ============================================================================
# Main (Full-System Mode)
# ============================================================================

main() {
    log "Starting CPU profiling"
    log "Duration: ${DURATION_SECS}s | Total system rate: ${ORDERS_PER_SEC} events/sec (split across 2 publishers)"

    # Clean stale Aeron state from previous runs
    if [[ -d "${AERON_DIR}" ]]; then
        log "Cleaning stale Aeron directory: ${AERON_DIR}"
        rm -rf "${AERON_DIR}"
    fi

    # Validate all dependencies
    validate_dependencies

    # Create output directory
    mkdir -p "${OUTPUT_DIR}"

    # Launch components in order
    launch_media_driver
    sleep 1

    launch_reconciler
    sleep 2

    launch_publisher "Primary" "${PRIMARY_CHANNEL}" "${PRIMARY_STREAM}"
    sleep 1

    launch_publisher "Dropcopy" "${DROPCOPY_CHANNEL}" "${DROPCOPY_STREAM}"

    log "All components launched successfully"

    # Warmup period
    log "Warming up for ${WARMUP_SECS} seconds..."
    sleep "${WARMUP_SECS}"

    # Verify reconciler is still running after warmup
    if ! kill -0 "${RECOND_PID}" 2>/dev/null; then
        error "Reconciler died during warmup"
    fi

    # Run profiling (target process may exit before duration ends; that's OK)
    run_profiling || true

    # Generate reports if perf.data was captured
    if [[ -f "${REPO_ROOT}/perf.data" ]]; then
        generate_reports
    else
        log "WARNING: perf.data not found, skipping report generation"
    fi

    # Print summary
    echo ""
    echo "============================================================"
    echo "CPU Profiling Complete"
    echo "============================================================"
    echo ""
    echo "Configuration:"
    echo "  Duration:         ${DURATION_SECS} seconds"
    echo "  Total event rate: ${ORDERS_PER_SEC} events/sec (split across 2 publishers)"
    echo "  Per-publisher:    $((ORDERS_PER_SEC / 2)) events/sec"
    echo ""
    echo "Output files:"
    echo "  Flame graph:    ${OUTPUT_DIR}/flamegraph.svg"
    echo "  Perf report:    ${OUTPUT_DIR}/perf_report.txt"
    echo "  Top functions:  ${OUTPUT_DIR}/top_functions.txt"
    echo ""
    echo "View flame graph:"
    echo "  firefox ${OUTPUT_DIR}/flamegraph.svg"
    echo ""
    echo "============================================================"
    echo ""

    log "CPU profiling completed successfully"
}

if [[ "${BENCH_MODE}" == true ]]; then
    main_bench "$@"
else
    main "$@"
fi
