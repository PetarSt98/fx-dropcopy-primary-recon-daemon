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

# Create output directory for benchmark results
mkdir -p bench/out

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

# Run benchmarks and save JSON output to bench/out/
./build-release/reconciler_bench \
    --benchmark_repetitions=5 \
    --benchmark_report_aggregates_only=true \
    --benchmark_display_aggregates_only=true \
    --benchmark_counters_tabular=true \
    --benchmark_out=bench/out/benchmark_results.json \
    --benchmark_out_format=json

echo ""
echo "=========================================="
echo "Generating Markdown Table"
echo "=========================================="
echo ""

# Generate markdown table from JSON results
if command -v python3 &> /dev/null; then
    python3 scripts/format_benchmark_results.py bench/out/benchmark_results.json bench/out/benchmark_results.md
    echo ""
    echo "Results saved to:"
    echo "  - JSON: bench/out/benchmark_results.json"
    echo "  - Markdown: bench/out/benchmark_results.md"
    echo ""
    echo "Markdown table preview:"
    echo ""
    cat bench/out/benchmark_results.md
else
    echo "Warning: python3 not found, skipping markdown generation"
    echo "JSON results saved to: bench/out/benchmark_results.json"
fi

