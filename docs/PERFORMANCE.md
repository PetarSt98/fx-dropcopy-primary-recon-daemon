# Performance Profiling

## CPU Profiling

### Generating Flame Graphs

The `generate_flamegraph.sh` script profiles the FX reconciliation daemon under load using Linux `perf` and generates interactive SVG flame graphs for analysis.

**Prerequisites:**
- Built executables in `build/release/` (`fx_exec_recond`, `fx_aeron_publisher`)
- `aeronmd` available in PATH
- `perf` installed (`sudo apt-get install linux-tools-common linux-tools-$(uname -r)`)
- `sudo` access for `perf record`
- FlameGraph toolkit cloned to `tools/FlameGraph/`

**Setup:**
```bash
# Clone FlameGraph toolkit pinned to v1.0 (one-time setup)
git clone --branch v1.0 --depth 1 https://github.com/brendangregg/FlameGraph.git tools/FlameGraph

# Build project in release mode
cmake --preset release
cmake --build build/release
```

**Usage:**
```bash
# 60-second profile at 10K events/sec (default)
./scripts/generate_flamegraph.sh

# 60-second profile at 10K events/sec (explicit)
./scripts/generate_flamegraph.sh 60 10000

# Quick 30-second profile at 5K events/sec
./scripts/generate_flamegraph.sh 30 5000

# View results
firefox docs/performance/flamegraph.svg
```

**Output Files:**

| File | Description |
|------|-------------|
| `docs/performance/flamegraph.svg` | Interactive SVG flame graph (open in browser) |
| `docs/performance/perf_report.txt` | Full `perf report` output |
| `docs/performance/top_functions.txt` | Top CPU hot spots |

### Expected Hot Spots

- `OrderStateStore::upsert()` — Hash table operations
- `compute_mismatch()` — Field comparisons
- `WheelTimer::schedule()` — Timer management
- `apply_internal_exec()` / `apply_dropcopy_exec()` — State updates

### Interpreting Results

- **Flame graph width** = CPU time (wider = more CPU)
- **Flame graph height** = call stack depth
- **Hot colors** = high CPU usage
- Click on any frame to zoom into that call stack
- The SVG is interactive and viewable in any modern browser

### How It Works

1. Launches Aeron MediaDriver, reconciler, and two publishers (primary + dropcopy)
2. Allows a 5-second warmup period for CPU caches and branch prediction to stabilize
3. Records CPU profile using `perf record -F 999 -g` on the reconciler process
4. Generates SVG flame graph using the FlameGraph toolkit
5. Produces text reports for quick analysis
6. Cleans up all processes and temporary files

### Rate Configuration

The rate parameter is the **total** system ingress rate, split evenly between the two publishers:

- Total rate 10,000 events/sec → 5,000 per publisher
- Per-publisher rates ≥ 1,000/sec run in unthrottled mode (`sleep_ms=0`)
- Per-publisher rates < 1,000/sec use `sleep_ms = 1000 / rate`

### Troubleshooting

**`perf` not found:**
```bash
sudo apt-get install linux-tools-common linux-tools-$(uname -r)
```

**FlameGraph toolkit not found:**
```bash
git clone --branch v1.0 --depth 1 https://github.com/brendangregg/FlameGraph.git tools/FlameGraph
```

**Permission denied for `perf record`:**
```bash
# perf requires sudo for hardware performance counter access
# The script uses sudo only for perf commands
```

**Executables not found:**
```bash
# Ensure the project is built in release mode
cmake --preset release
cmake --build build/release

# Or specify a custom build directory
export BUILD_DIR=/path/to/build/release
./scripts/generate_flamegraph.sh
```

## References

- [Brendan Gregg's Flame Graphs](https://www.brendangregg.com/flamegraphs.html)
- [Linux perf wiki](https://perf.wiki.kernel.org/)
- Soak Test Guide: `docs/soak_test.md`
