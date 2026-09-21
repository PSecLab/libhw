#!/usr/bin/env python3
"""
Seed or re-validate the table-decision rules for a pinned document.

Every table caption in a manual gets an explicit keep/skip decision with a
reason, recorded in spec/rules/<target>.tables.yaml. The importer refuses to run
if the document contains a caption the rules file has not decided on, so a new
table appearing in a revised manual becomes a CI failure rather than a silent
omission.

  python3 tools/state_tables.py --target armv7m --seed      # write a seed file
  python3 tools/state_tables.py --target armv7m --check     # verify it is current
"""

from __future__ import annotations

import argparse
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import yaml

from state.armpdf import extract_tables
from state.sources import REPO_ROOT, SourceError, get_source

RULES_DIR = REPO_ROOT / "spec" / "rules"

TARGETS = {
    "armv7m": {"source": "armv7m_arm", "architecture": "ARMv7-M"},
    "cortex_m7_r0p2": {"source": "cortex_m7_trm", "architecture": "ARMv7-M"},
    "cortex_m4_r0p0": {"source": "cortex_m4_trm", "architecture": "ARMv7-M"},
}


def rules_path(target: str) -> pathlib.Path:
    return RULES_DIR / f"{target}.tables.yaml"


def discover(target: str) -> tuple[dict, list]:
    cfg = TARGETS[target]
    src = get_source(cfg["source"])
    path = src.verify()
    _, decisions = extract_tables(str(path))
    return cfg, decisions


def seed(target: str) -> None:
    cfg, decisions = discover(target)
    existing = {}
    p = rules_path(target)
    if p.is_file():
        existing = (yaml.safe_load(p.read_text()) or {}).get("tables", {}) or {}

    tables = {}
    for d in sorted(decisions, key=lambda d: (d.table_id[0], len(d.table_id), d.table_id)):
        prev = existing.get(d.table_id)
        if prev:                      # never clobber a reviewed decision
            prev.setdefault("caption", d.caption)
            tables[d.table_id] = prev
            continue
        tables[d.table_id] = {
            "caption": d.caption,
            "keep": False,
            "reason": "UNREVIEWED - a human must decide whether this is a register enumeration",
        }

    doc = {
        "schema_version": 1,
        "source": cfg["source"],
        "architecture": cfg["architecture"],
        "tables": tables,
    }
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(yaml.safe_dump(doc, sort_keys=False, width=110))
    unreviewed = sum(1 for t in tables.values() if "UNREVIEWED" in str(t.get("reason", "")))
    print(f"{p}: {len(tables)} captions, {unreviewed} unreviewed")


def check(target: str) -> int:
    cfg, decisions = discover(target)
    p = rules_path(target)
    if not p.is_file():
        print(f"ERROR: no rules file at {p}. Run --seed and review it.", file=sys.stderr)
        return 1
    rules = (yaml.safe_load(p.read_text()) or {}).get("tables", {}) or {}

    found = {d.table_id for d in decisions}
    decided = set(rules)
    rc = 0

    for tid in sorted(found - decided):
        cap = next(d.caption for d in decisions if d.table_id == tid)
        print(f"ERROR: Table {tid} ({cap!r}) is in the document but has no decision.", file=sys.stderr)
        rc = 1
    for tid in sorted(decided - found):
        print(f"ERROR: rules decide Table {tid} but it is absent from the document.", file=sys.stderr)
        rc = 1
    for tid, r in sorted(rules.items()):
        if "UNREVIEWED" in str(r.get("reason", "")):
            print(f"ERROR: Table {tid} ({r.get('caption','')!r}) is still UNREVIEWED.", file=sys.stderr)
            rc = 1
    if rc == 0:
        print(f"{target}: all {len(found)} table captions have reviewed decisions")
    return rc


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", required=True, choices=sorted(TARGETS))
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--seed", action="store_true")
    g.add_argument("--check", action="store_true")
    a = ap.parse_args()
    try:
        if a.seed:
            seed(a.target)
            return 0
        return check(a.target)
    except SourceError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
