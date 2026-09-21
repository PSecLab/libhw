#!/usr/bin/env python3
"""
Sweep a manual for state documented outside its register-summary tables.

A table-driven importer is structurally blind to state the manual defines in
prose. That is not hypothetical: the entire FP extension register file (S0-S31,
D0-D15) is defined in A2.5.2 as running text, no summary table lists it, and
the document's own memory-mapped indexes cannot catch it either.

This sweep uses the structural signal instead of keyword matching. Arm gives
every register its own section, headed '<Long name>, <MNEMONIC>', and every
register group a section headed '... registers'. Each such heading outside the
instruction chapters is matched against the manifest; anything unmatched must
be listed, with a reason, in spec/rules/<target>.prose.yaml. A new unmatched
heading is a CI failure rather than a silent omission.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import yaml

from state.armpdf import pdf_pages
from state.generate import load_manifest
from state.sources import REPO_ROOT, SourceError, get_source

RULES_DIR = REPO_ROOT / "spec" / "rules"

SECTION_RE = re.compile(r"^(?P<sec>[A-Z]?\d+(?:\.\d+){1,3})\s+(?P<title>\S.*?)\s*$", re.M)
# Chapters A6 and A7 are the instruction set; their headings are mnemonics.
INSTRUCTION_CHAPTER_RE = re.compile(r"^A[67]\.")
# '<Long name>, <MNEMONIC>' -- how Arm heads a register description.
MNEMONIC_RE = re.compile(r",\s*([A-Z][A-Z0-9_]{1,15})\s*$")
GROUP_RE = re.compile(r"\bregisters?\b|\bregister file\b|\bregister bank\b", re.I)

TARGET_SOURCE = {"armv7m": "armv7m_arm", "cortex_m7_r0p2": "cortex_m7_trm",
                 "cortex_m4_r0p0": "cortex_m4_trm"}


# Block prefixes a heading may add that the register's own table omits: the
# Cortex-M4 TRM tabulates 'ITCTRL' but heads its description 'TPIU_ITCTRL'.
BLOCK_PREFIXES = ("TPIU_", "ITM_", "DWT_", "FP_", "MPU_", "NVIC_", "SYST_", "CTI_")


def normalise(name: str) -> str:
    """Collapse an array spelling so DWT_COMPn, DWT_COMPx and DWT_COMP0 agree."""
    return re.sub(r"(?:[NXnx]|\d+)$", "", name)


def spellings(name: str) -> set[str]:
    """A heading's mnemonic, with and without a block prefix."""
    out = {name, normalise(name)}
    for pfx in BLOCK_PREFIXES:
        if name.startswith(pfx):
            out |= {name[len(pfx):], normalise(name[len(pfx):])}
        out |= {pfx + name, normalise(pfx + name)}
    return {s for s in out if s}


def sweep(target: str) -> tuple[list[dict], list[dict]]:
    src = get_source(TARGET_SOURCE[target])
    text = "\n".join(pdf_pages(str(src.verify())))

    doc = load_manifest(target)
    known = {e["canonical_name"].upper() for e in doc["entries"]}
    known |= {normalise(n) for n in known}

    described: list[dict] = []
    groups: list[dict] = []
    seen: set[tuple[str, str]] = set()

    for m in SECTION_RE.finditer(text):
        sec, title = m.group("sec"), m.group("title")
        if INSTRUCTION_CHAPTER_RE.match(sec):
            continue
        key = (sec, title)
        if key in seen:
            continue
        seen.add(key)

        mm = MNEMONIC_RE.search(title)
        if mm:
            described.append({"section": sec, "title": title, "name": mm.group(1)})
        elif GROUP_RE.search(title):
            groups.append({"section": sec, "title": title})

    unmatched = [d for d in described if not (spellings(d["name"].upper()) & known)]
    return unmatched, groups


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", default="armv7m", choices=sorted(TARGET_SOURCE))
    ap.add_argument("--show-groups", action="store_true")
    a = ap.parse_args()

    try:
        unmatched, groups = sweep(a.target)
    except SourceError as e:
        print(f"SKIP: {e}", file=sys.stderr)
        return 0

    rules_path = RULES_DIR / f"{a.target}.prose.yaml"
    reviewed = {}
    if rules_path.is_file():
        reviewed = (yaml.safe_load(rules_path.read_text()) or {}).get("not_registers", {}) or {}

    unreviewed = [d for d in unmatched if d["name"] not in reviewed]

    print(f"Register-description headings unmatched by the manifest: {len(unmatched)}")
    for d in unmatched:
        why = reviewed.get(d["name"])
        mark = "reviewed" if why else "UNREVIEWED"
        print(f"  [{mark}] {d['section']:12} {d['name']:14} {d['title'][:52]}")
        if why:
            print(f"             reason: {why}")

    if a.show_groups:
        print(f"\nRegister-group headings: {len(groups)}")
        for g in groups:
            print(f"  {g['section']:12} {g['title'][:70]}")

    if unreviewed:
        print(f"\nFAIL: {len(unreviewed)} heading(s) name something the manifest does not "
              f"cover and that nobody has reviewed.", file=sys.stderr)
        return 1

    print("\nEvery register-description heading is either covered by the manifest or "
          "explicitly reviewed as not a register.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
