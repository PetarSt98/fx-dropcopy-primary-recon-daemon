#!/bin/bash
#
# soak_test.sh - 24-Hour Soak Test & Stability Validation
#
# Production-grade end-to-end load test for FX reconciliation daemon.
# Launches Aeron MediaDriver, reconciler, and high-rate publishers for
# sustained periods to validate memory stability, CPU usage, and throughput.
#
# Usage:
#   ./soak_test.sh [duration_hours] [orders_per_sec]
#
# Example:
#   ./soak_test.sh 24 10000    # 24-hour run at 10k orders/sec
#   ./soak_test.sh 0.1 100     # 6-minute test at 100 orders/sec
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

# Test parameters
readonly DURATION_HOURS="${1:-24}"
readonly ORDERS_PER_SEC="${2:-10000}"

# Aeron configuration
readonly AERON_DIR="${AERON_DIR:-/var/tmp/aeron-soak-$$}"
readonly PRIMARY_CHANNEL="aeron:udp?endpoint=localhost:20121"
readonly DROPCOPY_CHANNEL="aeron:udp?endpoint=localhost:20122"
readonly PRIMARY_STREAM=1001
readonly DROPCOPY_STREAM=1002

# Output files
readonly LOG_DIR="${REPO_ROOT}/soak_logs"
readonly METRICS_CSV="${LOG_DIR}/soak_metrics.csv"
readonly MEDIA_DRIVER_LOG="${LOG_DIR}/aeronmd.log"
readonly RECOND_LOG="${LOG_DIR}/recond.log"
readonly PRIMARY_PUB_LOG="${LOG_DIR}/primary_pub.log"
readonly DROPCOPY_PUB_LOG="${LOG_DIR}/dropcopy_pub.log"

# Monitoring interval (seconds)
readonly MONITOR_INTERVAL=10

# PID tracking
MEDIA_DRIVER_PID=""
RECOND_PID=""
PRIMARY_PUB_PID=""
DROPCOPY_PUB_PID=""

# Executables
readonly AERONMD="${AERONMD:-aeronmd}"
readonly FX_EXEC_RECOND="${BUILD_DIR}/fx_exec_recond"
readonly FX_AERON_PUBLISHER="${BUILD_DIR}/fx_aeron_publisher"

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
    
    # Stop monitoring loop if running
    if [[ -n "${MONITOR_PID:-}" ]] && kill -0 "${MONITOR_PID}" 2>/dev/null; then
        log "Stopping monitoring loop (PID ${MONITOR_PID})"
        kill "${MONITOR_PID}" 2>/dev/null || true
        wait "${MONITOR_PID}" 2>/dev/null || true
    fi
    
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
    
    # Clean up Aeron directory
    if [[ -z "${AERON_DIR}" ]]; then
        log "Refusing to remove Aeron directory: AERON_DIR is empty"
    elif [[ "${AERON_DIR}" != /var/tmp/aeron-soak-* ]]; then
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
# Process Monitoring
# ============================================================================

get_process_stats() {
    local pid="$1"
    if [[ ! -d "/proc/${pid}" ]]; then
        echo "0 0.0"
        return 1
    fi
    
    # Get RSS in KB and convert to MB
    local rss_kb
    rss_kb=$(awk '/^VmRSS:/ {print $2}' "/proc/${pid}/status" 2>/dev/null || echo "0")
    local rss_mb=$((rss_kb / 1024))
    
    # Get CPU percentage from ps
    local cpu_pct
    cpu_pct=$(ps -p "${pid}" -o %cpu= 2>/dev/null | tr -d ' ' || echo "0.0")
    
    echo "${rss_mb} ${cpu_pct}"
}

get_recond_counters() {
    # Extract event counters from recond log
    # Looking for: "Reconciler processed internal=XXX dropcopy=YYY"
    if [[ ! -f "${RECOND_LOG}" ]]; then
        echo "0"
        return
    fi
    
    local total=0
    local internal
    local dropcopy
    
    # Get the last reported counters from the tail of the log to avoid
    # repeatedly scanning the entire file and to remain portable (no grep -P).
    internal=$(
        tail -n 2000 "${RECOND_LOG}" 2>/dev/null \
            | sed -n 's/.*internal=\([0-9][0-9]*\).*/\1/p' \
            | tail -n 1
    )
    dropcopy=$(
        tail -n 2000 "${RECOND_LOG}" 2>/dev/null \
            | sed -n 's/.*dropcopy=\([0-9][0-9]*\).*/\1/p' \
            | tail -n 1
    )
    
    # Default to 0 if no counters have been seen yet
    if [[ -z "${internal}" ]]; then
        internal=0
    fi
    if [[ -z "${dropcopy}" ]]; then
        dropcopy=0
    fi
    
    total=$((internal + dropcopy))
    echo "${total}"
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
        > "${MEDIA_DRIVER_LOG}" 2>&1 &
    
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
        > "${RECOND_LOG}" 2>&1 &
    
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
    local log_file="$4"
    local pid_var="$5"
    
    log "Launching ${name} publisher"
    
    # Calculate sleep_ms to achieve desired rate for moderate/low loads.
    # The publisher sleeps sleep_ms after each event.
    # For rates < 1000/sec, calculate sleep_ms = 1000ms / rate
    #   Example: 100 orders/sec -> 10ms sleep per event
    # For rates >= 1000/sec, we deliberately set sleep_ms=0, which instructs
    # fx_aeron_publisher to run unthrottled (aside from backpressure). In this
    # regime, ORDERS_PER_SEC is used to size total_events, but the *actual*
    # send rate is "max throughput" and may exceed the requested rate.
    local sleep_ms=0
    if [[ "${ORDERS_PER_SEC}" -lt 1000 ]]; then
        sleep_ms=$((1000 / ORDERS_PER_SEC))
    else
        sleep_ms=0
        log "ORDERS_PER_SEC=${ORDERS_PER_SEC} >= 1000: running publisher in unthrottled max-throughput mode (actual send rate may exceed requested rate)."
    fi
    
    # Calculate total events for this publisher
    # Note: Each publisher runs independently, so with two publishers
    # the reconciler will see approximately 2x the requested ORDERS_PER_SEC
    local duration_sec
    duration_sec=$(bc <<< "${DURATION_HOURS} * 3600" | cut -d. -f1)
    local total_events=$((ORDERS_PER_SEC * duration_sec))
    
    export AERON_DIR
    
    "${FX_AERON_PUBLISHER}" \
        "${channel}" \
        "${stream}" \
        "${total_events}" \
        "${sleep_ms}" \
        > "${log_file}" 2>&1 &
    
    local pid=$!
    eval "${pid_var}=${pid}"
    log "${name} publisher started (PID ${pid})"
}

# ============================================================================
# Monitoring Loop
# ============================================================================

monitor_processes() {
    local start_time
    start_time=$(date +%s)
    local duration_sec
    duration_sec=$(bc <<< "${DURATION_HOURS} * 3600" | cut -d. -f1)
    local end_time=$((start_time + duration_sec))
    
    log "Monitoring for ${DURATION_HOURS} hours (${duration_sec} seconds)"
    
    # Initialize CSV with header
    echo "timestamp,elapsed_sec,recond_rss_mb,recond_cpu_pct,events_total" > "${METRICS_CSV}"
    
    while true; do
        local now
        now=$(date +%s)
        local elapsed=$((now - start_time))
        
        # Check if critical processes are still running
        if ! kill -0 "${RECOND_PID}" 2>/dev/null; then
            error "Reconciler process died unexpectedly"
        fi
        
        if ! kill -0 "${MEDIA_DRIVER_PID}" 2>/dev/null; then
            error "MediaDriver process died unexpectedly"
        fi
        
        # Collect metrics
        local timestamp
        timestamp=$(date '+%Y-%m-%d %H:%M:%S')
        
        local recond_stats
        recond_stats=$(get_process_stats "${RECOND_PID}")
        read -r recond_rss recond_cpu <<< "${recond_stats}"
        
        local events_total
        events_total=$(get_recond_counters)
        
        # Write to CSV
        echo "${timestamp},${elapsed},${recond_rss},${recond_cpu},${events_total}" >> "${METRICS_CSV}"
        
        # Log progress every 10 minutes (600 seconds)
        if ((elapsed % 600 == 0)) && ((elapsed > 0)); then
            local hours_elapsed
            hours_elapsed=$(bc <<< "scale=2; ${elapsed} / 3600")
            log "Progress: ${hours_elapsed}h elapsed | RSS: ${recond_rss} MB | CPU: ${recond_cpu}% | Events: ${events_total}"
        fi
        
        # Check if test duration elapsed (after collecting metrics)
        if [[ $now -ge $end_time ]]; then
            log "Test duration reached (${elapsed} seconds)"
            break
        fi
        
        sleep "${MONITOR_INTERVAL}"
    done
}

# ============================================================================
# Test Summary
# ============================================================================

print_summary() {
    log "Generating test summary"
    
    local end_time
    end_time=$(date '+%Y-%m-%d %H:%M:%S')
    
    echo ""
    echo "============================================================"
    echo "Soak Test Summary"
    echo "============================================================"
    echo ""
    echo "Configuration:"
    echo "  Duration:    ${DURATION_HOURS} hours"
    echo "  Target rate: ${ORDERS_PER_SEC} orders/sec"
    echo "  End time:    ${end_time}"
    echo ""
    echo "Logs:"
    echo "  Metrics CSV: ${METRICS_CSV}"
    echo "  MediaDriver: ${MEDIA_DRIVER_LOG}"
    echo "  Reconciler:  ${RECOND_LOG}"
    echo "  Primary pub: ${PRIMARY_PUB_LOG}"
    echo "  Dropcopy pub: ${DROPCOPY_PUB_LOG}"
    echo ""
    echo "Analysis:"
    echo "  Run: python3 ${SCRIPT_DIR}/analyze_soak_test.py ${METRICS_CSV}"
    echo ""
    echo "============================================================"
    echo ""
}

# ============================================================================
# Main
# ============================================================================

main() {
    log "Starting soak test"
    log "Duration: ${DURATION_HOURS} hours | Rate: ${ORDERS_PER_SEC} orders/sec"
    
    # Validate executables
    check_executable "${FX_EXEC_RECOND}" "fx_exec_recond"
    check_executable "${FX_AERON_PUBLISHER}" "fx_aeron_publisher"
    
    if ! command -v "${AERONMD}" &> /dev/null; then
        error "aeronmd not found in PATH"
    fi
    
    # Validate required tools
    if ! command -v bc &> /dev/null; then
        error "bc not found in PATH (required for duration calculations)"
    fi
    
    if ! command -v ps &> /dev/null; then
        error "ps not found in PATH (required for CPU metrics)"
    fi
    
    if [[ ! -d /proc ]]; then
        error "/proc filesystem not available (required for RSS metrics)"
    fi
    
    # Create log directory
    mkdir -p "${LOG_DIR}"
    
    # Launch components in order
    launch_media_driver
    sleep 1
    
    launch_reconciler
    sleep 2
    
    launch_publisher "Primary" "${PRIMARY_CHANNEL}" "${PRIMARY_STREAM}" "${PRIMARY_PUB_LOG}" "PRIMARY_PUB_PID"
    sleep 1
    
    launch_publisher "Dropcopy" "${DROPCOPY_CHANNEL}" "${DROPCOPY_STREAM}" "${DROPCOPY_PUB_LOG}" "DROPCOPY_PUB_PID"
    sleep 2
    
    log "All components launched successfully"
    
    # Start monitoring
    monitor_processes
    
    # Print summary
    print_summary
    
    log "Soak test completed successfully"
}

main "$@"
