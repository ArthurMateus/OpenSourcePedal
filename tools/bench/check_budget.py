#!/usr/bin/env python3
"""Compare osp-bench results with tests/bench/budget.json (ADR-0007 section 5, REQ-TEST-007).

Fails (exit 1) when a benchmark exceeds its maxCpuPercent, a full chain exceeds ceilings.fullChainCpuPercent, a NAM
benchmark exceeds ceilings.namCpuPercent, or the normalised ratio R = T / T_ref regresses by more than
regressionTolerance against r0. --no-regression checks only the CPU ceilings, for runners without a baseline yet.

Per-benchmark entries live in tests/bench/budget.d/<block>.json ({"benchmarks": {...}}), next to the global
reference/ceilings/tolerance in budget.json, so a block PR adds a file instead of editing a shared one (#115). The files
are merged in name order; a benchmark named twice is an error.

CI supplies --runner bench-runner.json to select a measured processor profile. Unknown processors fail closed unless
--uncalibrated warn (CI): then the R gate is skipped, with a warning, for cases that have no profile for that processor,
and the CPU ceilings still apply. Profiles are never borrowed across processors. Without --runner, r0 is the canonical
profile for backwards-compatible offline use. CPU-only mode needs no profile.

usage: check_budget.py <results.json> [--budget tests/bench/budget.json] [--runner runner.json] [--no-regression]
                       [--uncalibrated fail|warn]
"""
from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def load_budget(path: Path, runner: Path | None = None, uncalibrated: str = "fail") -> dict:
    """budget.json plus every budget.d/*.json beside it (benchmarks merged, duplicates rejected).

    With uncalibrated="warn", a case without a profile for the runner's processor keeps its canonical r0 for display
    only and is marked "uncalibrated": the R gate skips it, the CPU ceilings still apply (ADR-0007 REQ-TEST-007 A2)."""
    budget = json.loads(path.read_text(encoding="utf-8"))
    merged = dict(budget.get("benchmarks", {}))
    for part in sorted((path.parent / "budget.d").glob("*.json")):
        for name, entry in json.loads(part.read_text(encoding="utf-8")).get("benchmarks", {}).items():
            if name in merged:
                raise SystemExit(f"budget: benchmark '{name}' defined twice (again in {part.name})")
            merged[name] = entry
    budget["benchmarks"] = merged
    if runner is not None:
        processor = json.loads(runner.read_text(encoding="utf-8"))["processor"]
        for name, entry in merged.items():
            profile = entry.get("processorBaselines", {}).get(processor)
            if profile is None:
                if uncalibrated != "warn":
                    raise SystemExit(f"budget: uncalibrated processor '{processor}' for {name}; "
                                     "record a five-run CI median in processorBaselines")
                entry["uncalibrated"] = processor
                continue
            entry["r0"] = profile["r0"]
    return budget


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("results")
    ap.add_argument("--budget", default=str(ROOT / "tests" / "bench" / "budget.json"))
    ap.add_argument("--no-regression", action="store_true")
    ap.add_argument("--runner", help="bench-runner.json processor provenance (required by CI)")
    ap.add_argument("--uncalibrated", choices=("fail", "warn"), default="fail",
                    help="a runner processor without a stored profile: fail closed (default), or skip the R gate for "
                         "those cases with a warning while still enforcing the CPU ceilings (CI)")
    args = ap.parse_args()

    runner = Path(args.runner) if args.runner and not args.no_regression else None
    budget = load_budget(Path(args.budget), runner, args.uncalibrated)
    results = json.loads(Path(args.results).read_text(encoding="utf-8"))["benchmarks"]
    tol = budget["regressionTolerance"]
    ceilings = budget["ceilings"]
    failures = [f"{name}: missing budget" for name in sorted(results.keys() - budget["benchmarks"].keys())]
    if not math.isfinite(tol) or tol < 0:
        raise SystemExit("budget: regressionTolerance must be finite and non-negative")

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
        if (
            not all(math.isfinite(v) for v in (cpu, ratio, b["r0"], limit))
            or cpu < 0 or ratio <= 0 or b["r0"] <= 0 or limit <= 0
        ):
            failures.append(f"{name}: invalid metrics or budget")
            continue
        delta = ratio / b["r0"] - 1.0
        note = "  [uncalibrated: R not gated]" if "uncalibrated" in b else ""
        print(f"{name:20s} cpu {cpu:9.4f} % (max {limit:g})  R {ratio:.6f} (r0 {b['r0']:.6f}, {delta:+.1%}){note}")
        if cpu >= limit:
            failures.append(f"{name}: {cpu:.4f} % CPU >= {limit:g} %")
        # Compare the ratio directly so an exact +10% boundary is not rejected by subtraction rounding.
        if not args.no_regression and "uncalibrated" not in b and ratio > b["r0"] * (1.0 + tol):
            failures.append(f"{name}: R regressed {delta:+.1%} (> {tol:.0%})")

    skipped = sorted(n for n, b in budget["benchmarks"].items() if "uncalibrated" in b)
    if skipped:
        processor = budget["benchmarks"][skipped[0]]["uncalibrated"]
        # GitHub annotation: visible on the run summary without failing the job.
        print(f"::warning title=bench R gate skipped::uncalibrated processor '{processor}': R gate skipped for "
              f"{len(skipped)} case(s), CPU ceilings enforced; record its five-run median in processorBaselines")
    for f in failures:
        print(f"FAIL {f}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
