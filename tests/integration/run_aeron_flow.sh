#!/usr/bin/env bash
set -euo pipefail

COMPOSE="docker compose -f docker-compose.yml -f tests/integration/docker-compose.it.yml"

$COMPOSE down -v --remove-orphans >/dev/null 2>&1 || true
$COMPOSE build
$COMPOSE up --no-color --attach-dependencies

logs=$($COMPOSE logs recon-daemon)
primary_consumed=$(echo "$logs" | sed -n 's/.*Reconciler consumed primary: \([0-9]\+\).*/\1/p')
dropcopy_consumed=$(echo "$logs" | sed -n 's/.*dropcopy: \([0-9]\+\).*/\1/p')

$COMPOSE down -v --remove-orphans

if [[ -z "$primary_consumed" || -z "$dropcopy_consumed" ]]; then
  echo "Missing consumption counters in recon-daemon logs" >&2
  exit 1
fi

if [[ $primary_consumed -eq 0 || $dropcopy_consumed -eq 0 ]]; then
  echo "Expected recon-daemon to consume fragments from both streams" >&2
  echo "$logs"
  exit 1
fi

echo "Integration succeeded: primary=$primary_consumed dropcopy=$dropcopy_consumed"
