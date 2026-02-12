#!/bin/bash
set -euo pipefail

# Check if benchmark executable exists
if [ ! -f ./build-release/reconciler_bench ]; then
    echo "Error: reconciler_bench not found at ./build-release/reconciler_bench"
    echo "Please build the project first with:"
    echo "  mkdir -p build-release && cd build-release"
    echo "  cmake -DCMAKE_BUILD_TYPE=Release -DFX_BUILD_BENCHMARKS=ON .."
    echo "  cmake --build . --target reconciler_bench"
    exit 1
fi

echo "=========================================="
echo "FX Reconciliation Microbenchmarks"
echo "=========================================="
echo ""
echo "Hardware: $(lscpu | grep 'Model name' | cut -d':' -f2 | xargs)"
echo "Compiler: $(${CXX:-g++} --version | head -1)"
echo "Build: Release"
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
