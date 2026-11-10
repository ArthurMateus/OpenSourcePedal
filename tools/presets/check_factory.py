#!/usr/bin/env python3
"""CI lint for the factory bank (REQ-PRE-001, #200): every presets/factory/**/*.osp.json validates against
preset.v1.schema.json, ids and names are unique, and the file is canonical (sorted keys, LF, trailing newline).

  python tools/presets/check_factory.py
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

import jsonschema

ROOT = Path(__file__).resolve().parents[2]


def main() -> int:
    schema = json.loads((ROOT / "presets" / "schema" / "preset.v1.schema.json").read_text(encoding="utf-8"))
    validator = jsonschema.Draft202012Validator(schema)
    files = sorted((ROOT / "presets" / "factory").rglob("*.osp.json"))
    if not files:
        print("no factory presets found", file=sys.stderr)
        return 1
    ids: set[str] = set()
    names: set[str] = set()
    for f in files:
        raw = f.read_bytes()
        doc = json.loads(raw.decode("utf-8"))
        validator.validate(doc)
        if b"\r" in raw or not raw.endswith(b"\n"):
            sys.exit(f"{f}: must use LF line endings and end with a newline")
        if doc["id"] in ids or doc["name"] in names:
            sys.exit(f"{f}: duplicate id or name")
        ids.add(doc["id"])
        names.add(doc["name"])
        print(f"{f.relative_to(ROOT)}: valid ({doc['name']})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
