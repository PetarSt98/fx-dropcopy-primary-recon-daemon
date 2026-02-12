#!/usr/bin/env python3
"""
Parse Google Benchmark JSON output and generate a markdown table.
"""

import json
import sys
from pathlib import Path


def format_time(ns):
    """Format nanoseconds into human-readable string."""
    if ns < 1000:
        return f"{ns:.2f} ns"
    elif ns < 1000000:
        return f"{ns / 1000:.2f} μs"
    elif ns < 1000000000:
        return f"{ns / 1000000:.2f} ms"
    else:
        return f"{ns / 1000000000:.2f} s"


def _to_ns(value: float, unit: str) -> float:
    mult = {"ns": 1.0, "us": 1_000.0, "ms": 1_000_000.0, "s": 1_000_000_000.0}
    return float(value) * mult.get(unit, 1.0)


def parse_benchmark_json(json_path):
    """Parse benchmark JSON and extract aggregate results."""
    with open(json_path, "r") as f:
        data = json.load(f)

    by_name = {}  # base_name -> dict

    for bench in data.get("benchmarks", []):
        # Prefer explicit aggregate rows (more reliable than name suffix matching)
        if bench.get("run_type") != "aggregate":
            continue

        agg = bench.get("aggregate_name", "")
        if agg not in ("mean", "median", "stddev"):  # skip cv by default
            continue

        name = bench.get("name", "")
        base_name = (
            name.replace("_mean", "")
                .replace("_median", "")
                .replace("_stddev", "")
                .replace("_cv", "")
        )

        unit = bench.get("time_unit", "ns")
        time_val = bench.get("real_time", bench.get("cpu_time", 0))
        time_ns = _to_ns(time_val, unit)

        entry = by_name.get(base_name)
        if entry is None:
            entry = {"name": base_name, "label": bench.get("label", "")}
            by_name[base_name] = entry

        entry[agg] = time_ns

    # Preserve deterministic ordering (alphabetical). If you prefer insertion order, remove sorted().
    return [by_name[k] for k in sorted(by_name.keys())]


def generate_markdown_table(benchmarks):
    """Generate markdown table from benchmark results."""
    lines = []
    lines.append("## FX Reconciliation Microbenchmark Results")
    lines.append("")
    lines.append("| Benchmark | Mean | Median | StdDev | Label |")
    lines.append("|-----------|------|--------|--------|-------|")
    
    for bench in benchmarks:
        name = bench['name']
        mean = format_time(bench.get('mean', 0))
        median = format_time(bench.get('median', 0))
        stddev = format_time(bench.get('stddev', 0))
        label = bench.get('label', '-')
        
        lines.append(f"| {name} | {mean} | {median} | {stddev} | {label} |")
    
    lines.append("")
    lines.append(f"*Results from {len(benchmarks)} benchmarks*")
    lines.append("")
    
    return '\n'.join(lines)


def main():
    if len(sys.argv) < 2:
        print("Usage: format_benchmark_results.py <benchmark_results.json> [output.md]")
        sys.exit(1)
    
    json_path = Path(sys.argv[1])
    if not json_path.exists():
        print(f"Error: {json_path} not found")
        sys.exit(1)
    
    benchmarks = parse_benchmark_json(json_path)
    markdown = generate_markdown_table(benchmarks)
    
    if len(sys.argv) >= 3:
        output_path = Path(sys.argv[2])
        output_path.parent.mkdir(parents=True, exist_ok=True)
        with open(output_path, 'w') as f:
            f.write(markdown)
        print(f"Markdown table written to {output_path}")
    else:
        print(markdown)


if __name__ == '__main__':
    main()
