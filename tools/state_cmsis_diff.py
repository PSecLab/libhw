#!/usr/bin/env python3
"""Cross-check a manifest against a CMSIS-Core header (an independent check, not an authority)."""

from __future__ import annotations

import argparse
import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from state.cmsis import diff_against, parse_header
from state.generate import load_manifest


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", required=True, action="append",
                    help="repeatable; CMSIS models a whole implementation, so compare\n"
                         "the architecture manifest and its CPU overlay together")
    ap.add_argument("--header", required=True)
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()

    hp = pathlib.Path(a.header).expanduser()
    if not hp.is_file():
        print(f"ERROR: no CMSIS header at {hp}", file=sys.stderr)
        return 2

    cmsis = parse_header(hp)
    entries = []
    for t in a.target:
        entries.extend(load_manifest(t)["entries"])
    d = diff_against(entries, cmsis)

    if a.json:
        print(json.dumps(d, indent=2))
        return 0

    print(f"CMSIS header:       {hp}")
    print(f"Manifests:          {', '.join(a.target)}")
    print(f"CMSIS registers:    {d['cmsis_registers']}")
    print(f"Manifest registers: {d['manifest_registers']}")
    print(f"Agree on address:   {d['in_both']}")
    print(f"Only in CMSIS:      {len(d['only_in_cmsis'])}")
    for r in d["only_in_cmsis"][:25]:
        print(f"    {r['address']}  {r['cmsis']}")
    print(f"Only in manifest:   {len(d['only_in_manifest'])}")
    for r in d["only_in_manifest"][:25]:
        print(f"    {r['address']}  {r['name']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
