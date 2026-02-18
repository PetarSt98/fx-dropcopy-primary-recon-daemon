# FX Reconciliation Microbenchmarks

Production-grade microbenchmark suite using Google Benchmark for measuring performance of FX DropCopy Reconciler critical-path components.

## Overview

This benchmark suite measures the performance of:
- **Hash table operations**: First-probe lookup, high load factor lookup, upsert
- **Arena allocation**: Bump allocator for OrderState structs
- **Timer wheel**: O(1) deadline scheduling
- **Mismatch computation**: Field comparison for divergence detection
- **SPSC ring**: Lock-free push/pop operations

## Building Benchmarks

Benchmarks are opt-in to avoid pulling the Google Benchmark dependency for regular builds.

### Quick Start

```bash
# 1. Create release build directory
mkdir -p build-release && cd build-release

# 2. Configure with benchmarks enabled
cmake -DCMAKE_BUILD_TYPE=Release -DFX_BUILD_BENCHMARKS=ON ..

# 3. Build the benchmark executable
cmake --build . --target reconciler_bench

# 4. Return to repository root
cd ..
```

### Build Options

- `-DCMAKE_BUILD_TYPE=Release`: Required for accurate performance measurements
- `-DFX_BUILD_BENCHMARKS=ON`: Enables benchmark targets
- `-DFX_PERF_ENABLED=OFF`: **Important** - Keep perf instrumentation disabled to avoid measurement distortion

**Warning**: Building with `-DFX_PERF_ENABLED=ON` will distort benchmark results due to instrumentation overhead.

## Running Benchmarks

### Automated Script (Recommended)

The `run_benchmarks.sh` script handles benchmark execution, JSON output, and markdown table generation:

```bash
./scripts/run_benchmarks.sh
```

**Output locations:**
- Console: Live results with mean/median/stddev statistics
- `bench/out/benchmark_results.json`: Machine-readable JSON format
- `bench/out/benchmark_results.md`: Human-readable markdown table

### Manual Execution

For custom benchmark runs:

```bash
# Run with default settings (5 repetitions, aggregated results)
./build-release/reconciler_bench

# Run specific benchmark
./build-release/reconciler_bench --benchmark_filter=BM_HashTable.*

# Custom repetitions and output format
./build-release/reconciler_bench \
    --benchmark_repetitions=10 \
    --benchmark_report_aggregates_only=true \
    --benchmark_out=custom_results.json \
    --benchmark_out_format=json

# Run with shorter time per benchmark (for quick validation)
./build-release/reconciler_bench --benchmark_min_time=0.1s
```

## Performance Tuning for Stable Results

For maximum measurement stability and reproducibility:

### 1. CPU Governor Settings

Set CPU to performance mode to prevent frequency scaling:

```bash
# Check current governor
cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

# Set to performance mode (requires root)
sudo cpupower frequency-set --governor performance
```

### 2. Disable Turbo Boost (Optional)

For the most stable results, disable CPU turbo/boost:

```bash
# Intel (requires root)
echo 1 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo

# AMD (requires root)
echo 0 | sudo tee /sys/devices/system/cpu/cpufreq/boost
```

### 3. Reduce System Noise

```bash
# Close unnecessary applications
# Disable background services (package managers, indexers, etc.)
# Run benchmarks when system is idle
```

### 4. Isolate CPUs (Advanced)

For production performance testing, consider isolating CPUs:

```bash
# Add to kernel boot parameters in /etc/default/grub:
# isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3

# Then run benchmarks on isolated CPUs:
taskset -c 2 ./build-release/reconciler_bench
```

## Benchmark Descriptions

### Hash Table Benchmarks

- **BM_HashTableLookup_FirstProbe**: Best-case lookup (key in first slot)
- **BM_HashTableLookup_HighLoadFactor**: Realistic late probe with 90% load factor
- **BM_HashTableUpsert**: Insert or update operation (includes allocation)

### Memory Management

- **BM_ArenaAllocate**: Bump allocator performance for OrderState structs

### Timer Operations

- **BM_TimerWheelSchedule**: O(1) deadline scheduling with proper TSC distribution

### Reconciliation Logic

- **BM_ComputeMismatch**: Field comparison for divergence detection

### Lock-Free Queue

- **BM_SPSCRing_Push**: Single-producer push operation
- **BM_SPSCRing_Pop**: Single-consumer pop operation
- **BM_SPSCRing_PushPop**: Round-trip push and pop

## Interpreting Results

### Key Metrics

- **Mean**: Average time across all iterations
- **Median**: Middle value (less affected by outliers)
- **StdDev**: Standard deviation (lower is more stable)

### Expected Performance Ranges

Typical results on modern x86-64 CPUs (3GHz+):

| Operation | Expected Range |
|-----------|---------------|
| Hash table lookup | 2-5 ns |
| Hash table upsert | 60-100 ns |
| Arena allocate | 1-3 ns |
| Timer wheel schedule | 3-5 ns |
| Compute mismatch | 1-3 ns |
| SPSC push/pop | 5-15 ns |

### Coefficient of Variation (CV)

CV < 5% indicates stable, reproducible measurements. Higher CV suggests:
- CPU frequency scaling is active
- System load/interference
- Cache effects
- Need for more repetitions

## Troubleshooting

### Benchmarks Won't Build

```
Error: benchmark::benchmark not found
```

**Solution**: Enable benchmarks with `-DFX_BUILD_BENCHMARKS=ON` during CMake configuration.

### Distorted Results

If results seem inconsistent:

1. Check CPU governor: `cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor`
2. Verify perf instrumentation is disabled: Build without `-DFX_PERF_ENABLED=ON`
3. Reduce system load: Close background applications
4. Increase repetitions: `--benchmark_repetitions=10` or higher

### Python Script Errors

If markdown generation fails:

```bash
# Check Python version (requires 3.6+)
python3 --version

# Run script manually
python3 scripts/format_benchmark_results.py bench/out/benchmark_results.json
```

## Integration with CI/CD

For automated performance regression detection:

```yaml
# Example GitHub Actions workflow
- name: Run Benchmarks
  run: |
    cmake -B build -DCMAKE_BUILD_TYPE=Release -DFX_BUILD_BENCHMARKS=ON
    cmake --build build --target reconciler_bench
    ./scripts/run_benchmarks.sh
    
- name: Upload Results
  uses: actions/upload-artifact@v3
  with:
    name: benchmark-results
    path: bench/out/
```

## Results

For actual benchmark results, CPU profiling data, and flame graph analysis, see:

- [Performance Analysis](../docs/PERFORMANCE.md) — Full microbenchmark tables, latency distributions, CPU profile, and soak test results
- [Design Journal](../docs/DESIGN_JOURNAL.md) — Rationale for architectural decisions informed by profiling

## References

- [Google Benchmark Documentation](https://github.com/google/benchmark)
- [Linux CPU Performance Guide](https://easyperf.net/blog/2019/08/02/Perf-measurement-environment-on-Linux)
- [Reducing Measurement Noise](https://llvm.org/docs/Benchmarking.html)
