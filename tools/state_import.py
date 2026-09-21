#!/usr/bin/env python3
"""Import a pinned manual into a normalized state manifest."""

from __future__ import annotations

import argparse
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import yaml

from state.importer import SPEC_DIR, import_target, write_manifest
from state.sources import SourceError


def arch_names(target: str) -> set[str]:
    """Names already accounted for by the architecture manifest a CPU implements."""
    p = SPEC_DIR / f"{target}.yaml"
    if not p.is_file():
        return set()
    doc = yaml.safe_load(p.read_text())
    return {e["canonical_name"] for e in doc["entries"]}


def arch_addrs(target: str) -> dict[str, str]:
    """
    Absolute address -> architectural name.

    A TRM may spell an architectural register differently from the architecture
    manual, so the overlay also resolves by address.
    """
    p = SPEC_DIR / f"{target}.yaml"
    if not p.is_file():
        return {}
    doc = yaml.safe_load(p.read_text())
    out: dict[str, str] = {}
    for e in doc["entries"]:
        enc = (e.get("encoding") or "").strip().upper()
        if enc.startswith("0X") and e.get("encoding_kind") == "absolute":
            out.setdefault(enc, e["canonical_name"])
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", required=True)
    ap.add_argument("--arch-manifest", help="architecture target a CPU overlay builds on")
    a = ap.parse_args()

    try:
        names = arch_names(a.arch_manifest) if a.arch_manifest else None
        addrs = arch_addrs(a.arch_manifest) if a.arch_manifest else None
        entries, report = import_target(a.target, arch_names=names, arch_addrs=addrs)
        out = write_manifest(a.target, entries, report)
    except SourceError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 2

    print(f"{a.target}: {report.source_records} source records -> {out}")
    for k, v in report.counts.items():
        if v:
            print(f"    {k:22} {v}")
    for tid, cc in report.crosscheck.items():
        status = "OK" if not cc["missing"] else f"MISSING {len(cc['missing'])}"
        print(f"    crosscheck {tid}: {cc['present']}/{cc['index_entries']} {status}")
        if cc["missing"]:
            print(f"      {cc['missing'][:12]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
