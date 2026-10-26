#!/usr/bin/env python3
"""Compare osp-bench results with tests/bench/budget.json (ADR-0007 section 5, REQ-TEST-007).

Fails (exit 1) when a benchmark exceeds its maxCpuPercent, a full chain exceeds ceilings.fullChainCpuPercent, a NAM
benchmark exceeds ceilings.namCpuPercent, or the normalised ratio R = T / T_ref regresses by more than
regressionTolerance against r0. --no-regression checks only the CPU ceilings, for runners without a baseline yet.

usage: check_budget.py <results.json> [--budget tests/bench/budget.json] [--no-regression]
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("results")
    ap.add_argument("--budget", default=str(ROOT / "tests" / "bench" / "budget.json"))
    ap.add_argument("--no-regression", action="store_true")
    args = ap.parse_args()

    budget = json.loads(Path(args.budget).read_text(encoding="utf-8"))
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
