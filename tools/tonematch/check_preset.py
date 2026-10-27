#!/usr/bin/env python3
"""CI check for osp-tonematch (ADR-0008, #120 review): run it on the bundled CC0 assets, once with the factory IRs
and once with --ir-dir (file:/// uris that need a sha256), and validate both presets against preset.v1.schema.json.

  python tools/tonematch/check_preset.py build/release/tools/osp-tonematch.exe
"""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path

import jsonschema

ROOT = Path(__file__).resolve().parents[2]


def run(exe: Path, out: Path, extra: list[str]) -> dict:
    di = ROOT / "assets" / "di" / "strum-arp-01.wav"
    # Name with non-ASCII and an over-long tail: the writer must keep it valid UTF-8 and <= 64 characters.
    name = "Check préset " + "x" * 80
    cmd = [str(exe), "--ref", str(di), "--di", str(di), "--end", "6", "--out", str(out), "--name", name, *extra]
    res = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if res.returncode != 0:
        sys.exit(f"osp-tonematch failed ({res.returncode}):\n{res.stdout}\n{res.stderr}")
    return json.loads(out.read_text(encoding="utf-8"))


def main() -> int:
    exe = Path(sys.argv[1])
    schema = json.loads((ROOT / "presets" / "schema" / "preset.v1.schema.json").read_text(encoding="utf-8"))
    validator = jsonschema.Draft202012Validator(schema)
    with tempfile.TemporaryDirectory() as tmp:
        for label, extra in (("factory IRs", []), ("--ir-dir", ["--ir-dir", str(ROOT / "assets" / "cab")])):
            doc = run(exe, Path(tmp) / "p.osp.json", extra)
            validator.validate(doc)
            ir = doc["blocks"][2]["channels"][0]["resources"]["ir1"]
            print(f"{label}: valid preset, name {len(doc['name'])} chars, ir1 {ir['uri']}"
                  + (f" sha256 {ir['sha256'][:12]}..." if "sha256" in ir else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
