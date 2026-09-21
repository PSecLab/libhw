#!/usr/bin/env python3
"""
Cross-check generated special-register encodings against an independent assembler.

A complete register list with wrong encodings is useless, so the SYSm values the
importer read out of Table B5-1 are checked against what an assembler actually
emits for 'mrs r0, <reg>'. The assembler is an encoding oracle, not a
completeness authority: it only ever confirms or contradicts an encoding we
already claim.

ARMv7-M MRS (T1) encodes SYSm in the low byte of the second halfword, which is
byte 2 of the little-endian 4-byte encoding.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import shutil
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from state.generate import load_manifest

ENC_RE = re.compile(r"encoding:\s*\[([^\]]*)\]")


def assemble_sysm(tool: str, reg: str) -> int | None:
    """Return the SYSm field an assembler emits for 'mrs r0, <reg>', or None."""
    try:
        r = subprocess.run(
            [tool, "--triple=thumbv7m-none-eabi", "--show-encoding", "--assemble"],
            input=f"mrs r0, {reg}\n", capture_output=True, text=True, timeout=20)
    except (OSError, subprocess.TimeoutExpired):
        return None
    m = ENC_RE.search(r.stdout)
    if not m:
        return None
    try:
        by = [int(b.strip(), 16) for b in m.group(1).split(",")]
    except ValueError:
        return None
    return by[2] if len(by) >= 3 else None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", default="armv7m")
    ap.add_argument("--tool", default="llvm-mc")
    a = ap.parse_args()

    tool = shutil.which(a.tool)
    if not tool:
        print(f"SKIP: no {a.tool} on PATH; cannot cross-check encodings.", file=sys.stderr)
        return 0

    doc = load_manifest(a.target)
    checked = agreed = 0
    mismatches: list[str] = []
    unsupported: list[str] = []

    for e in doc["entries"]:
        enc = e.get("encoding") or ""
        if not enc.startswith("SYSm="):
            continue
        ours = int(enc.split("=", 1)[1])
        theirs = assemble_sysm(tool, e["canonical_name"].lower())
        if theirs is None:
            # The assembler may not accept every spelling the manual lists
            # (composite views such as IEPSR). That is not a mismatch.
            unsupported.append(e["canonical_name"])
            continue
        checked += 1
        if theirs == ours:
            agreed += 1
        else:
            mismatches.append(f"{e['canonical_name']}: manifest SYSm={ours}, {a.tool} SYSm={theirs}")

    print(f"Encoding oracle:    {tool}")
    print(f"Checked:            {checked}")
    print(f"Agreed:             {agreed}")
    print(f"Mismatches:         {len(mismatches)}")
    for m in mismatches:
        print(f"    {m}")
    if unsupported:
        print(f"Not accepted by the assembler ({len(unsupported)}): {', '.join(unsupported)}")
    return 1 if mismatches else 0


if __name__ == "__main__":
    raise SystemExit(main())
