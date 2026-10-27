#!/usr/bin/env python3
"""Combine several osp-bench result files into one, taking the per-benchmark median (REQ-TEST-007, #83).

The output has the osp-bench schema, so tools/bench/check_budget.py reads it unchanged. It adds "runs" and, per
benchmark, "rSpread" = max(R) / min(R) - 1 over the runs, so the run-to-run noise of a runner is on record.

usage: median.py <out.json> <run1.json> <run2.json> [...]
"""
from __future__ import annotations

import json
import statistics
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) < 4:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    out, paths = Path(sys.argv[1]), [Path(p) for p in sys.argv[2:]]
    runs = [json.loads(p.read_text(encoding="utf-8")) for p in paths]
    names = list(runs[0]["benchmarks"])
    for p, r in zip(paths, runs):
        if list(r["benchmarks"]) != names:
            print(f"{p}: benchmark list differs from {paths[0]}", file=sys.stderr)
            return 1

    combined = {k: v for k, v in runs[0].items() if k != "benchmarks"}
    combined["runs"] = len(runs)
    combined["tRefSeconds"] = statistics.median(r["tRefSeconds"] for r in runs)
    combined["benchmarks"] = {}
    for name in names:
        rows = [r["benchmarks"][name] for r in runs]
        ratios = [row["r"] for row in rows]
        combined["benchmarks"][name] = {
            "tMedianSeconds": statistics.median(row["tMedianSeconds"] for row in rows),
            "cpuPercent": statistics.median(row["cpuPercent"] for row in rows),
            "r": statistics.median(ratios),
            "rSpread": max(ratios) / min(ratios) - 1.0,
        }
        b = combined["benchmarks"][name]
        print(f"{name:20s} R median {b['r']:.6f}  spread {b['rSpread']:+.1%} over {len(runs)} runs")

    out.write_text(json.dumps(combined, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
