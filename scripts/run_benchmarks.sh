#!/bin/bash
set -euo pipefail

echo "=========================================="
echo "FX Reconciliation Microbenchmarks"
echo "=========================================="
echo ""
echo "Hardware: $(lscpu | grep 'Model name' | cut -d':' -f2 | xargs)"
echo "Compiler: $(g++ --version | head -1)"
echo "Build: Release (-O3 -march=native)"
echo ""
echo "=========================================="
echo ""

./build-release/reconciler_bench \
    --benchmark_repetitions=5 \
    --benchmark_report_aggregates_only=true \
    --benchmark_display_aggregates_only=true \
    --benchmark_counters_tabular=true \
    --benchmark_out=benchmark_results.json \
    --benchmark_out_format=json
