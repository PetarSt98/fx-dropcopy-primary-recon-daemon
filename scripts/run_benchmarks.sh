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

echo "Running ./build-release/reconciler_bench"
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

# Run perf-instrumented build if it exists
if [ -f ./build-release-perf/reconciler_bench ]; then
    echo ""
    echo "=========================================="
    echo "FX Reconciliation Microbenchmarks (Perf-Instrumented)"
    echo "=========================================="
    echo ""
    echo "NOTE: This build was compiled with FX_PERF_ENABLED=ON."
    echo "      Each PERF_SCOPE call adds two RDTSC fences, a TSC-to-ns"
    echo "      conversion, and a histogram bucket update (~15-25 ns overhead)."
    echo "      These numbers reflect instrumented overhead, NOT raw operation"
    echo "      latency.  Use the build-release results above for true cost."
    echo ""
    echo "Running ./build-release-perf/reconciler_bench"
    echo ""

    ./build-release-perf/reconciler_bench \
        --benchmark_repetitions=5 \
        --benchmark_report_aggregates_only=true \
        --benchmark_display_aggregates_only=true \
        --benchmark_counters_tabular=true \
        --benchmark_out=bench/out/benchmark_results_perf.json \
        --benchmark_out_format=json

    echo ""
    echo "=========================================="
    echo "Generating Markdown Table (Perf-Instrumented)"
    echo "=========================================="
    echo ""

    if command -v python3 &> /dev/null; then
        python3 scripts/format_benchmark_results.py bench/out/benchmark_results_perf.json bench/out/benchmark_results_perf.md
        echo ""
        echo "Results saved to:"
        echo "  - JSON: bench/out/benchmark_results_perf.json"
        echo "  - Markdown: bench/out/benchmark_results_perf.md"
        echo ""
        echo "Markdown table preview:"
        echo ""
        cat bench/out/benchmark_results_perf.md
    else
        echo "Warning: python3 not found, skipping markdown generation"
        echo "JSON results saved to: bench/out/benchmark_results_perf.json"
    fi
fi

