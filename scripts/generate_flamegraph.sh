#!/bin/bash
#
# generate_flamegraph.sh - CPU Profiling & Flame Graph Generation
#
# Profiles the FX reconciliation daemon under load using Linux perf
# and generates interactive SVG flame graphs for analysis.
#
# Usage:
#   ./scripts/generate_flamegraph.sh [duration_seconds] [total_events_per_sec]
#
# Arguments:
#   duration_seconds      - Profiling duration in seconds (default: 60)
#   total_events_per_sec  - TOTAL system event rate across BOTH publishers
#                           (default: 10000). Each publisher gets 50% of this rate.
#
# Example:
#   ./scripts/generate_flamegraph.sh 60 10000   # 60s profile at 10k events/sec
#   ./scripts/generate_flamegraph.sh 30 5000    # 30s profile at 5k events/sec
#
# Output:
#   docs/performance/flamegraph.svg     - Interactive flame graph (open in browser)
#   docs/performance/perf_report.txt    - Full perf report
#   docs/performance/top_functions.txt  - Top 20 CPU hot spots
#

set -euo pipefail

# ============================================================================
# Configuration & Defaults
# ============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly SCRIPT_DIR
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
readonly REPO_ROOT
readonly BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build/release}"

# Profiling parameters
readonly DURATION_SECS="${1:-60}"
# ORDERS_PER_SEC is the TOTAL system ingress rate (combined across both publishers)
# Each publisher will send at ORDERS_PER_SEC / 2
readonly ORDERS_PER_SEC="${2:-10000}"

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

# Warmup before profiling (seconds)
readonly WARMUP_SECS=5

# Extra buffer so publishers outlast the profiling window (seconds)
readonly PUBLISHER_BUFFER_SECS=10

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
    elif [[ "${AERON_DIR}" != /dev/shm/aeron-perf-* ]]; then
        log "Refusing to remove suspicious Aeron directory (unexpected path): ${AERON_DIR}"
    elif [[ ! -f "${AERON_DIR}/cnc.dat" ]]; then
        log "Refusing to remove Aeron directory without cnc.dat: ${AERON_DIR}"
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

    # Check perf availability
    if ! command -v perf &> /dev/null; then
        error "perf not found. Install with: sudo apt-get install linux-tools-common linux-tools-\$(uname -r)"
    fi

    # Check sudo availability
    if ! command -v sudo &> /dev/null; then
        error "sudo not found. sudo is required for perf record"
    fi

    # Check FlameGraph toolkit
    if [[ ! -x "${FLAMEGRAPH_DIR}/stackcollapse-perf.pl" ]]; then
        error "FlameGraph toolkit not found at ${FLAMEGRAPH_DIR}. Install with: git clone https://github.com/brendangregg/FlameGraph.git tools/FlameGraph"
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

    # Calculate sleep_ms to achieve desired rate
    # For rates < 1000/sec per publisher: sleep_ms = 1000 / rate
    # For rates >= 1000/sec per publisher: sleep_ms = 0 (unthrottled)
    local sleep_ms=0
    if [[ "${per_publisher_rate}" -lt 1000 ]]; then
        sleep_ms=$((1000 / per_publisher_rate))
    fi

    # Calculate total events for this publisher over the profiling + warmup duration
    local total_duration=$((DURATION_SECS + WARMUP_SECS + PUBLISHER_BUFFER_SECS))
    local total_events=$((per_publisher_rate * total_duration))

    export AERON_DIR

    "${FX_AERON_PUBLISHER}" \
        "${channel}" \
        "${stream}" \
        "${total_events}" \
        "${sleep_ms}" \
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
    sudo perf record -F 999 -g -p "${RECOND_PID}" -o "${REPO_ROOT}/perf.data" -- sleep "${DURATION_SECS}"

    log "Perf recording complete"
}

generate_reports() {
    log "Generating reports..."

    mkdir -p "${OUTPUT_DIR}"

    # Generate full perf report
    log "Generating perf report..."
    sudo perf report -i "${REPO_ROOT}/perf.data" --stdio --no-children \
        | tee "${OUTPUT_DIR}/perf_report.txt" > /dev/null

    # Generate top functions summary
    log "Generating top functions..."
    sudo perf report -i "${REPO_ROOT}/perf.data" --stdio --no-children --percent-limit 0.1 \
        | head -80 | tee "${OUTPUT_DIR}/top_functions.txt" > /dev/null

    # Generate flame graph
    log "Generating flame graph SVG..."
    sudo perf script -i "${REPO_ROOT}/perf.data" \
        | "${FLAMEGRAPH_DIR}/stackcollapse-perf.pl" \
        | "${FLAMEGRAPH_DIR}/flamegraph.pl" \
            --title "FX Reconciler CPU Profile (${DURATION_SECS}s @ ${ORDERS_PER_SEC} events/sec)" \
            --subtitle "$(date '+%Y-%m-%d %H:%M:%S')" \
        | tee "${OUTPUT_DIR}/flamegraph.svg" > /dev/null

    # Clean up perf.data
    log "Cleaning up perf.data"
    rm -f "${REPO_ROOT}/perf.data"

    log "Reports generated successfully"
}

# ============================================================================
# Main
# ============================================================================

main() {
    log "Starting CPU profiling"
    log "Duration: ${DURATION_SECS}s | Total system rate: ${ORDERS_PER_SEC} events/sec (split across 2 publishers)"

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

    # Run profiling
    run_profiling

    # Generate all reports
    generate_reports

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

main "$@"
