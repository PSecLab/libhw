#!/usr/bin/env python3
"""
Report what changed between the committed manifest and the pinned source.

Updating to a newer manual should be an explicit, reviewable diff rather than a
silent regeneration, so this compares the manifest in spec/arm against a fresh
import and prints the differences.

    python3 tools/state_source_diff.py --target armv7m
    python3 tools/state_source_diff.py --target armv7m --against spec/arm/armv7m.yaml
"""

from __future__ import annotations

import argparse
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import yaml

from state.generate import load_manifest
from state.importer import import_target
from state.sources import SourceError

FIELDS = ("encoding", "width", "namespace", "classification",
          "feature_requirement", "readable", "writable", "snapshot")


def index(entries: list[dict]) -> dict[str, dict]:
    return {e["state_id"]: e for e in entries}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", required=True)
    ap.add_argument("--arch-manifest")
    ap.add_argument("--against", help="compare two manifest files instead of re-importing")
    a = ap.parse_args()

    try:
        old = index(load_manifest(a.target)["entries"])
        if a.against:
            new = index(yaml.safe_load(pathlib.Path(a.against).read_text())["entries"])
        else:
            names = None
            if a.arch_manifest:
                names = {e["canonical_name"] for e in load_manifest(a.arch_manifest)["entries"]}
            entries, _ = import_target(a.target, arch_names=names)
            new = index([e.to_dict() for e in entries])
    except (SourceError, FileNotFoundError) as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 2

    added = sorted(set(new) - set(old))
    removed = sorted(set(old) - set(new))
    common = set(old) & set(new)

    changed_encoding: list[str] = []
    changed_feature: list[str] = []
    changed_other: list[str] = []
    unchanged = 0

    for k in sorted(common):
        diffs = [f for f in FIELDS if old[k].get(f) != new[k].get(f)]
        if not diffs:
            unchanged += 1
        elif "encoding" in diffs:
            changed_encoding.append(f"{k}: {old[k].get('encoding')} -> {new[k].get('encoding')}")
        elif "feature_requirement" in diffs:
            changed_feature.append(
                f"{k}: {old[k].get('feature_requirement')} -> {new[k].get('feature_requirement')}")
        else:
            changed_other.append(f"{k}: {', '.join(diffs)}")

    w = 26
    print(f"{'New source entries:':<{w}} {len(added)}")
    for k in added[:20]:
        print(f"    + {k}")
    print(f"{'Removed source entries:':<{w}} {len(removed)}")
    for k in removed[:20]:
        print(f"    - {k}")
    print(f"{'Changed encodings:':<{w}} {len(changed_encoding)}")
    for k in changed_encoding[:20]:
        print(f"    ~ {k}")
    print(f"{'Changed feature rules:':<{w}} {len(changed_feature)}")
    for k in changed_feature[:20]:
        print(f"    ~ {k}")
    print(f"{'Other changes:':<{w}} {len(changed_other)}")
    for k in changed_other[:20]:
        print(f"    ~ {k}")
    print(f"{'Unchanged:':<{w}} {unchanged}")

    return 1 if (added or removed or changed_encoding or changed_feature or changed_other) else 0


if __name__ == "__main__":
    raise SystemExit(main())
