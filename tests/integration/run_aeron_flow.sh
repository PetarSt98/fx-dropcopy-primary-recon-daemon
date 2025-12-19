#!/usr/bin/env bash
set -euo pipefail

AERON_DIR=${AERON_DIR:-/dev/shm/aeron-root}
RECOND_RUN_MS=${RECOND_RUN_MS:-3000}
RECON_TIMEOUT_SECONDS=${RECON_TIMEOUT_SECONDS:-30}
PUB_TIMEOUT_SECONDS=${PUB_TIMEOUT_SECONDS:-10}
PRIMARY_CHANNEL="aeron:udp?endpoint=localhost:20121"
DROPCOPY_CHANNEL="aeron:udp?endpoint=localhost:20122"
PRIMARY_STREAM=1001
DROPCOPY_STREAM=1002

mkdir -p "${AERON_DIR}"
rm -rf "${AERON_DIR}"/*
export AERON_DIR

cleanup() {
  local code=$?
  if [[ -n "${RECON_PID:-}" ]]; then
    kill "${RECON_PID}" 2>/dev/null || true
  fi
  if [[ -n "${MEDIA_DRIVER_PID:-}" ]]; then
    kill "${MEDIA_DRIVER_PID}" 2>/dev/null || true
  fi
  rm -f /tmp/recon.log /tmp/aeronmd.log
  exit $code
}
trap cleanup EXIT

# Start embedded Aeron media driver
AERON_DIR=${AERON_DIR} aeronmd -Daeron.dir=${AERON_DIR} -Daeron.socket.soReusePort=true \
  >/tmp/aeronmd.log 2>&1 &
MEDIA_DRIVER_PID=$!

# Wait for the CnC file to appear so clients don't race the driver startup
for _ in {1..50}; do
  if [[ -f "${AERON_DIR}/cnc.dat" ]]; then
    break
  fi
  sleep 0.1
done

if [[ ! -f "${AERON_DIR}/cnc.dat" ]]; then
  echo "Aeron media driver did not produce cnc.dat in ${AERON_DIR}" >&2
  cat /tmp/aeronmd.log || true
  exit 1
fi

# Launch recon daemon with a bounded run window and hard timeout so the test exits deterministically
timeout ${RECON_TIMEOUT_SECONDS}s env AERON_DIR=${AERON_DIR} RECOND_RUN_MS=${RECOND_RUN_MS} \
  fx_exec_recond "${PRIMARY_CHANNEL}" ${PRIMARY_STREAM} "${DROPCOPY_CHANNEL}" ${DROPCOPY_STREAM} \
  >/tmp/recon.log 2>&1 &
RECON_PID=$!

# Publish a handful of fragments on both channels with timeouts to avoid hangs
timeout ${PUB_TIMEOUT_SECONDS}s env AERON_DIR=${AERON_DIR} fx_aeron_publisher "${PRIMARY_CHANNEL}" ${PRIMARY_STREAM} 8 10
timeout ${PUB_TIMEOUT_SECONDS}s env AERON_DIR=${AERON_DIR} fx_aeron_publisher "${DROPCOPY_CHANNEL}" ${DROPCOPY_STREAM} 8 10

# Wait for the recon daemon to finish and inspect its logs
if ! wait ${RECON_PID}; then
  echo "Recon daemon exited with an error" >&2
  cat /tmp/recon.log || true
  exit 1
fi

primary_consumed=$(sed -n 's/.*Reconciler consumed primary: \([0-9]\+\).*/\1/p' /tmp/recon.log)
dropcopy_consumed=$(sed -n 's/.*dropcopy: \([0-9]\+\).*/\1/p' /tmp/recon.log)

if [[ -z "$primary_consumed" || -z "$dropcopy_consumed" ]]; then
  echo "Missing consumption counters in recon-daemon logs" >&2
  cat /tmp/recon.log
  exit 1
fi

if [[ $primary_consumed -eq 0 || $dropcopy_consumed -eq 0 ]]; then
  echo "Expected recon-daemon to consume fragments from both streams" >&2
  cat /tmp/recon.log
  exit 1
fi

echo "Integration succeeded: primary=${primary_consumed} dropcopy=${dropcopy_consumed}"
