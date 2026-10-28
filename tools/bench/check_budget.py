#!/usr/bin/env python3
"""Compare osp-bench results with tests/bench/budget.json (ADR-0007 section 5, REQ-TEST-007).

Fails (exit 1) when a benchmark exceeds its maxCpuPercent, a full chain exceeds ceilings.fullChainCpuPercent, a NAM
benchmark exceeds ceilings.namCpuPercent, or the normalised ratio R = T / T_ref regresses by more than
regressionTolerance against r0. --no-regression checks only the CPU ceilings, for runners without a baseline yet.

Per-benchmark entries live in tests/bench/budget.d/<block>.json ({"benchmarks": {...}}), next to the global
reference/ceilings/tolerance in budget.json, so a block PR adds a file instead of editing a shared one (#115). The files
are merged in name order; a benchmark named twice is an error.

usage: check_budget.py <results.json> [--budget tests/bench/budget.json] [--no-regression]
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def load_budget(path: Path) -> dict:
    """budget.json plus every budget.d/*.json beside it (benchmarks merged, duplicates rejected)."""
    budget = json.loads(path.read_text(encoding="utf-8"))
    merged = dict(budget.get("benchmarks", {}))
    for part in sorted((path.parent / "budget.d").glob("*.json")):
        for name, entry in json.loads(part.read_text(encoding="utf-8")).get("benchmarks", {}).items():
            if name in merged:
                raise SystemExit(f"budget: benchmark '{name}' defined twice (again in {part.name})")
            merged[name] = entry
    budget["benchmarks"] = merged
    return budget


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("results")
    ap.add_argument("--budget", default=str(ROOT / "tests" / "bench" / "budget.json"))
    ap.add_argument("--no-regression", action="store_true")
    args = ap.parse_args()

    budget = load_budget(Path(args.budget))
    results = json.loads(Path(args.results).read_text(encoding="utf-8"))["benchmarks"]
    tol = budget["regressionTolerance"]
    ceilings = budget["ceilings"]
    failures = []

    for name, b in budget["benchmarks"].items():
        r = results.get(name)
        if r is None:
            failures.append(f"{name}: missing from results")
            continue
        cpu, ratio = r["cpuPercent"], r["r"]
        limit = b["maxCpuPercent"]
        if b["kind"] == "full_chain":
            limit = min(limit, ceilings["fullChainCpuPercent"])
        elif b["kind"] == "nam":
            limit = min(limit, ceilings["namCpuPercent"])
        delta = ratio / b["r0"] - 1.0
        print(f"{name:20s} cpu {cpu:9.4f} % (max {limit:g})  R {ratio:.6f} (r0 {b['r0']:.6f}, {delta:+.1%})")
        if cpu >= limit:
            failures.append(f"{name}: {cpu:.4f} % CPU >= {limit:g} %")
        if not args.no_regression and delta > tol:
            failures.append(f"{name}: R regressed {delta:+.1%} (> {tol:.0%})")

    for f in failures:
        print(f"FAIL {f}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
