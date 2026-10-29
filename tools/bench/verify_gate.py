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
    ap.add_argument("--uncalibrated", choices=("fail", "warn"), default="fail",
                    help="as check_budget.py; with warn on an uncalibrated runner, the decision-logic fixtures are "
                         "built on the canonical profile instead of the real timings")
    args = ap.parse_args()
    budget_path = Path(args.budget).resolve()
    runner_path = Path(args.runner).resolve() if args.runner else None
    budget = load_budget(budget_path, runner_path, args.uncalibrated)
    measured = json.loads(Path(args.results).read_text(encoding="utf-8"))
    uncalibrated = any("uncalibrated" in b for b in budget["benchmarks"].values())

    with tempfile.TemporaryDirectory(prefix="osp-gate-") as directory:
        probe_path = Path(directory) / "probe.json"
        current = {"runner": runner_path}  # switched to the canonical profile (None) on an uncalibrated runner

        def check(doc: dict, expected: int, message: str = "", runner: Path | None | str = "current",
                  mode: str = args.uncalibrated) -> None:
            runner = current["runner"] if runner == "current" else runner
            probe_path.write_text(json.dumps(doc), encoding="utf-8")
            command = [sys.executable, str(ROOT / "tools/bench/check_budget.py"), str(probe_path),
                       "--budget", str(budget_path), "--uncalibrated", mode]
            if runner is not None:
                command += ["--runner", str(runner)]
            result = subprocess.run(command, capture_output=True, text=True, timeout=30)
            if result.returncode != expected or message not in result.stdout + result.stderr:
                raise SystemExit(f"gate verification failed (expected exit {expected} / {message}):\n"
                                 + result.stdout + result.stderr)

        check(measured, 0)
        print("PASS measured report and CPU ceilings")
        if uncalibrated:
            # The real timings have no baseline on this processor, so test the decision logic on the canonical
            # profile: a synthetic report sitting exactly at canonical r0, checked without --runner.
            canonical = load_budget(budget_path)
            synthetic = copy.deepcopy(measured)
            for name, row in synthetic["benchmarks"].items():
                factor = canonical["benchmarks"][name]["r0"] / row["r"]
                row["r"] = canonical["benchmarks"][name]["r0"]
                for metric in ("tMedianSeconds", "cpuPercent"):
                    row[metric] *= factor
            measured, budget = synthetic, canonical
            current["runner"] = None
            check(measured, 0)
            print("PASS uncalibrated runner: R gate skipped with a warning; logic checks use the canonical profile")
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
        check(measured, 1, "uncalibrated processor", unknown_path, mode="fail")
        check(measured, 0, "R gate skipped", unknown_path, mode="warn")
        over = copy.deepcopy(measured)
        over["benchmarks"][name]["cpuPercent"] = budget["benchmarks"][name]["maxCpuPercent"] * 2.0
        check(over, 1, f"FAIL {name}:", unknown_path, mode="warn")
        print("PASS unknown processor: rejected in fail mode; in warn mode R is skipped but CPU ceilings still fail; "
              "no cross-architecture fallback")
    return 0


if __name__ == "__main__":
    sys.exit(main())
