#!/usr/bin/env python3
"""Repository policy guard, run in CI on every PR.

Checks (see AGENTS.md section 1):
  1. No third-party trademark / artist names in product paths (everything except docs/research, docs/adr,
     docs/state, docs/MASTER_PROMPT.md, AGENTS.md/CLAUDE.md/GEMINI.md and this guard's own word list).
  2. No ASIO SDK files committed (ADR-0001).
  3. No file > 1 MB that is not tracked by Git LFS.
  4. Every file under assets/ has a row in assets/LICENSES.md.

Exit code 0 = clean, 1 = violations (printed one per line as `path: reason`).
"""
from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GUARD_DIR = Path(__file__).resolve().parent
MAX_PLAIN_BYTES = 1_000_000

# Paths where naming the inspirations is allowed (internal notes only).
NAME_EXEMPT_PREFIXES = (
    "docs/research/",
    "docs/adr/",
    "docs/state/",
    "docs/MASTER_PROMPT.md",
    "AGENTS.md",
    "CLAUDE.md",
    "GEMINI.md",
    ".claude/",
    "tools/guard/",
)

ASIO_SDK_MARKERS = ("asio.h", "asiodrvr.h", "asiosys.h", "asiolist.h", "iasiodrv.h", "asio.cpp")

TEXT_SUFFIXES = {
    ".h", ".hpp", ".c", ".cpp", ".cc", ".cxx", ".inl", ".cmake", ".txt", ".md", ".json", ".yml", ".yaml",
    ".py", ".ps1", ".iss", ".xml", ".svg", ".html", ".css", ".js", ".ts", ".toml", ".ini", ".cfg",
}


def tracked_files() -> list[str]:
    out = subprocess.run(["git", "ls-files", "-z"], cwd=ROOT, capture_output=True, check=True).stdout
    return [p for p in out.decode("utf-8").split("\0") if p]


def lfs_files() -> set[str]:
    try:
        out = subprocess.run(["git", "lfs", "ls-files", "-n"], cwd=ROOT, capture_output=True, check=True).stdout
    except (FileNotFoundError, subprocess.CalledProcessError):
        return set()
    return {line.strip() for line in out.decode("utf-8").splitlines() if line.strip()}


def load_names() -> list[re.Pattern[str]]:
    words = []
    for line in (GUARD_DIR / "forbidden_names.txt").read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            words.append(re.compile(r"\b" + re.escape(line) + r"\b", re.IGNORECASE))
    return words


def main() -> int:
    files = tracked_files()
    lfs = lfs_files()
    names = load_names()
    problems: list[str] = []

    licenses_md = ROOT / "assets" / "LICENSES.md"
    licenses_text = licenses_md.read_text(encoding="utf-8") if licenses_md.exists() else ""

    for rel in files:
        path = ROOT / rel
        if not path.is_file():
            continue
        lower = rel.lower()

        if Path(lower).name in ASIO_SDK_MARKERS:
            problems.append(f"{rel}: looks like an ASIO SDK file; fetch it at configure time instead (ADR-0001)")

        if rel not in lfs and path.stat().st_size > MAX_PLAIN_BYTES:
            problems.append(f"{rel}: larger than 1 MB and not in Git LFS")

        if rel.startswith("assets/") and rel != "assets/LICENSES.md" and rel not in licenses_text:
            problems.append(f"{rel}: bundled asset without a row in assets/LICENSES.md")

        if not rel.startswith(NAME_EXEMPT_PREFIXES) and path.suffix.lower() in TEXT_SUFFIXES and rel not in lfs:
            text = path.read_text(encoding="utf-8", errors="ignore")
            for pattern in names:
                match = pattern.search(text)
                if match:
                    line_no = text.count("\n", 0, match.start()) + 1
                    problems.append(f"{rel}:{line_no}: forbidden third-party name '{match.group(0)}'")

    for problem in problems:
        print(problem)
    print(f"guard: {len(files)} files checked, {len(problems)} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
