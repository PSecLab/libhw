#!/usr/bin/env python3
"""
Scan a manual for prose-defined state and account for every candidate.

The scanner never adds anything to the manifest. It produces candidates; each
one is classified as state, alias, field, block name, an array family already
covered, already-covered names, or a false positive. Candidates that cannot be
resolved automatically are listed in spec/rules/<target>.prose_candidates.yaml
with a reason, and anything left unclassified fails CI.

  python3 tools/state_prose_candidates.py --target armv7m            # report
  python3 tools/state_prose_candidates.py --target armv7m --seed     # seed review file
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import yaml

from state.generate import COVERAGE_DIR, load_manifest
from state.prose import (field_names, front_matter_pages, instruction_mnemonics, scan)
from state.sources import REPO_ROOT, SourceError, get_source

RULES_DIR = REPO_ROOT / "spec" / "rules"

TARGET_SOURCE = {"armv7m": "armv7m_arm", "cortex_m7_r0p2": "cortex_m7_trm",
                 "cortex_m4_r0p0": "cortex_m4_trm"}

VALID = {"state", "alias", "field", "block_name", "covered_family",
         "already_covered", "false_positive"}

# Chapters that describe the instruction set. A register name appearing there
# is an operand of an instruction, not a definition of new state.
INSTRUCTION_SECTION_RE = re.compile(r"^A[4-7]\.")

# Prose names a register file in words as often as by mnemonic. Each of these
# maps to a family the manifest already covers.
FILE_PHRASES = {
    r"arm core register": "R0-R12, SP, LR, PC",
    r"(?:fp )?extension register": "S0-S31 / D0-D15",
    r"single-precision register": "S0-S31",
    r"double-precision register": "D0-D15",
    r"special-purpose register": "PRIMASK, BASEPRI, FAULTMASK, CONTROL, xPSR",
    r"comparator register": "DWT_COMPn",
    r"stimulus port register": "ITM_STIM0-255",
}


def family_base(name: str) -> str:
    return re.sub(r"(?:[nx]|\d+)$", "", name)  # only a lowercase placeholder, not a capital N


def auto_classify(c, known: set[str], families: set[str],
                  mnemonics: set[str], fields: set[str],
                  front_matter: int) -> tuple[str, str] | None:
    names = [n.upper() for n in c.names]

    if c.page <= front_matter:
        return ("false_positive",
                "document front matter (title, legal and revision text), before the first chapter")

    # Both vocabularies are derived from this document: mnemonics from the
    # instruction chapters' own headings, field names from its 'bit
    # assignments' tables. Neither is a hand-written list.
    if names and all(n in mnemonics for n in names):
        return ("false_positive",
                "instruction mnemonics, named by the instruction chapters' own headings")
    if names and all(n in fields for n in names):
        return ("field",
                "bit fields named by the manual's 'bit assignments' tables, not independent state")
    if names and all(n in mnemonics or n in fields for n in names):
        return ("false_positive",
                "a mix of instruction mnemonics and bit-field names; no register named")

    if names and all(n in known for n in names):
        return ("already_covered",
                f"all {len(names)} name(s) are already state entries in the manifest")

    if names and all(family_base(n) in families for n in names):
        return ("covered_family",
                f"members of the {family_base(names[0])}* array, already covered")

    if c.kind == "family" and names:
        if family_base(names[0]) in families or names[0] in known:
            return ("covered_family",
                    f"parameterised family already covered as {family_base(names[0])}*")

    if c.kind in ("range", "through", "comma_set") and names:
        covered = [n for n in names if n in known or family_base(n) in families]
        if covered and len(covered) == len(names):
            return ("already_covered", "every member already present")
        if not covered and INSTRUCTION_SECTION_RE.match(c.section):
            return ("false_positive",
                    "register list in an instruction description; an operand range, "
                    "not a definition of state")

    if not names and c.kind in ("count", "phrase"):
        # These name no register; they state the size or existence of a file.
        # Resolve them against the register names in their own context.
        ctx = {t for t in re.findall(r"\b[A-Z][A-Z0-9_]{1,15}\b", c.context)}
        covered = {t for t in ctx if t in known or family_base(t) in families}
        if covered:
            return ("covered_family",
                    f"states the size or existence of a register file already covered "
                    f"({', '.join(sorted(covered)[:3])})")
        blob = f"{c.raw} {c.context}".lower()
        for rx, covers in FILE_PHRASES.items():
            if re.search(rx, blob):
                return ("covered_family",
                        f"names a register file in words; already covered as {covers}")
        if not (ctx - mnemonics - fields):
            return ("false_positive",
                    "a count or phrase about registers that names none; no state identified")
        return None

    if names and re.search(r"instruction", c.section, re.I) and \
            not any(n in known or family_base(n) in families for n in names):
        return ("false_positive",
                "named in an instruction-set section; instruction mnemonics, not registers")

    BLOCKS = {"DWT", "ITM", "TPIU", "FPB", "NVIC", "MPU", "SCB", "SCS", "CTI", "ETM", "DCB"}
    if names and all(n in BLOCKS for n in names):
        return ("block_name",
                "names debug/system blocks, not registers; their registers are enumerated "
                "in their own summary tables")

    if c.kind == "instr_ref" and names:
        if names[0] in known or family_base(names[0]) in families:
            return ("already_covered", "special register already present in the manifest")

    return None


def rules_path(target: str) -> pathlib.Path:
    return RULES_DIR / f"{target}.prose_candidates.yaml"


def load_reviewed(target: str) -> dict:
    p = rules_path(target)
    if not p.is_file():
        return {}
    return (yaml.safe_load(p.read_text()) or {}).get("candidates", {}) or {}


def run(target: str, seed: bool) -> int:
    src = get_source(TARGET_SOURCE[target])
    cands = scan(str(src.verify()))

    doc = load_manifest(target)
    known = {e["canonical_name"].upper() for e in doc["entries"]}
    # Strip the placeholder from the original spelling, then fold: uppercasing
    # first turns 'DWT_COMPn' into 'DWT_COMPN', whose trailing N is not a
    # placeholder, and the family never matches.
    families = {family_base(e["canonical_name"]).upper() for e in doc["entries"]}

    path = str(src.verify())
    mnemonics = instruction_mnemonics(path)
    fields = field_names(path)
    front_matter = front_matter_pages(path)

    reviewed = load_reviewed(target)
    rows, unresolved_ranges, unclassified = [], [], []

    for c in cands:
        cls = reviewed.get(c.cid)
        if cls:
            kind, reason, how = cls.get("class"), cls.get("reason", ""), "reviewed"
        else:
            auto = auto_classify(c, known, families, mnemonics, fields, front_matter)
            kind, reason, how = (auto[0], auto[1], "auto") if auto else (None, "", "-")

        if kind is not None and kind not in VALID:
            print(f"ERROR: {c.cid} has invalid class {kind!r}", file=sys.stderr)
            return 2

        rows.append({"cid": c.cid, "kind": c.kind, "raw": c.raw, "names": c.names,
                     "section": c.section, "page": c.page, "context": c.context,
                     "classification": kind, "reason": reason, "resolved_by": how})

        if kind is None:
            unclassified.append(c)
        # A range that names members we cannot resolve is an unresolved range.
        if c.kind in ("range", "through") and kind is None:
            unresolved_ranges.append(c)

    if seed:
        existing = load_reviewed(target)
        for c in unclassified:
            existing.setdefault(c.cid, {
                "raw": c.raw, "section": c.section, "page": c.page,
                "context": c.context, "names": c.names,
                "class": None,
                "reason": "UNREVIEWED - classify as state/alias/field/block_name/"
                          "covered_family/already_covered/false_positive",
            })
        rules_path(target).parent.mkdir(parents=True, exist_ok=True)
        rules_path(target).write_text(yaml.safe_dump(
            {"schema_version": 1, "target": target,
             "description": ("Prose candidates that could not be resolved against the "
                             "manifest automatically. Every one needs a class and a reason."),
             "candidates": existing}, sort_keys=False, width=100))
        print(f"seeded {rules_path(target)} with {len(unclassified)} candidate(s) to review")
        return 0

    COVERAGE_DIR.mkdir(parents=True, exist_ok=True)
    out = COVERAGE_DIR / f"{target}.prose-candidates.json"
    out.write_text(json.dumps({"target": target,
                               "source": f"{src.document} {src.revision}",
                               "candidates": rows}, indent=2))

    by_class: dict[str, int] = {}
    for r in rows:
        by_class[r["classification"] or "UNCLASSIFIED"] = \
            by_class.get(r["classification"] or "UNCLASSIFIED", 0) + 1

    print(f"Prose candidates:        {len(rows)}")
    for k in sorted(by_class):
        print(f"    {k:22} {by_class[k]}")
    print(f"Unresolved ranges:       {len(unresolved_ranges)}")
    print(f"Artifact:                {out.relative_to(pathlib.Path.cwd())}")

    if unclassified:
        print(f"\nFAIL: {len(unclassified)} unclassified prose candidate(s):", file=sys.stderr)
        for c in unclassified[:15]:
            print(f"  {c.cid}  {c.kind:10} {c.raw[:40]:42} {c.section[:34]} p{c.page}",
                  file=sys.stderr)
        return 1
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", default="armv7m", choices=sorted(TARGET_SOURCE))
    ap.add_argument("--seed", action="store_true")
    a = ap.parse_args()
    try:
        return run(a.target, a.seed)
    except SourceError as e:
        print(f"SKIP: {e}", file=sys.stderr)
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
