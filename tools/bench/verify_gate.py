#!/usr/bin/env python3
"""Black-box gate checks against a measured report and its stored budgets (REQ-TEST-007).

The real report must pass. For EACH case independently, a synthetic timing fixture at r0 * 1.20 must fail the R
gate. This tests the decision logic, not a claimed hardware slowdown. Also check the exact +10% boundary and reject
missing, unbudgeted and non-finite measurements. No source, baseline or real report is modified.

usage: verify_gate.py <results.json> [--budget tests/bench/budget.json] [--runner runner.json]
"""
from __future__ import annotations

import argparse
import copy
import json
import subprocess
import sys
import tempfile
from pathlib import Path

from check_budget import ROOT, load_budget


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("results")
    ap.add_argument("--budget", default=str(ROOT / "tests/bench/budget.json"))
    ap.add_argument("--runner", help="bench-runner.json for this report's processor profile")
    args = ap.parse_args()
    budget_path = Path(args.budget).resolve()
    runner_path = Path(args.runner).resolve() if args.runner else None
    budget = load_budget(budget_path, runner_path)
    measured = json.loads(Path(args.results).read_text(encoding="utf-8"))

    with tempfile.TemporaryDirectory(prefix="osp-gate-") as directory:
        probe_path = Path(directory) / "probe.json"

        def check(doc: dict, expected: int, message: str = "", runner: Path | None = runner_path) -> None:
            probe_path.write_text(json.dumps(doc), encoding="utf-8")
            command = [sys.executable, str(ROOT / "tools/bench/check_budget.py"), str(probe_path),
                       "--budget", str(budget_path)]
            if runner is not None:
                command += ["--runner", str(runner)]
            result = subprocess.run(command, capture_output=True, text=True, timeout=30)
            if result.returncode != expected or message not in result.stdout + result.stderr:
                raise SystemExit(f"gate verification failed (expected exit {expected} / {message}):\n"
                                 + result.stdout + result.stderr)

        check(measured, 0)
        print("PASS measured report and CPU ceilings")
        for name, row in measured["benchmarks"].items():
            slow = copy.deepcopy(measured)
            target = budget["benchmarks"][name]["r0"] * 1.20
            factor = target / row["r"]
            slow["benchmarks"][name]["r"] = target
            for metric in ("tMedianSeconds", "cpuPercent"):
                slow["benchmarks"][name][metric] *= factor
            check(slow, 1, f"FAIL {name}: R regressed")
        print(f"PASS independent +20% timing fixtures rejected for all {len(measured['benchmarks'])} cases")

        name = next(iter(measured["benchmarks"]))
        boundary = copy.deepcopy(measured)
        boundary["benchmarks"][name]["r"] = budget["benchmarks"][name]["r0"] * 1.10
        check(boundary, 0)
        missing = copy.deepcopy(measured)
        del missing["benchmarks"][name]
        check(missing, 1, f"FAIL {name}: missing from results")
        unbudgeted = copy.deepcopy(measured)
        unbudgeted["benchmarks"]["__unbudgeted_probe__"] = measured["benchmarks"][name]
        check(unbudgeted, 1, "FAIL __unbudgeted_probe__: missing budget")
        nonfinite = copy.deepcopy(measured)
        nonfinite["benchmarks"][name]["r"] = float("nan")
        check(nonfinite, 1, f"FAIL {name}: invalid metrics or budget")
        print("PASS +10% boundary; missing, unbudgeted and non-finite data rejected")
        unknown_path = Path(directory) / "unknown-runner.json"
        unknown_path.write_text(json.dumps({"processor": "__uncalibrated_probe__"}), encoding="utf-8")
        check(measured, 1, "uncalibrated processor", unknown_path)
        print("PASS unknown processor rejected; no cross-architecture fallback")
    return 0


if __name__ == "__main__":
    sys.exit(main())
