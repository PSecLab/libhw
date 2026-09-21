#!/usr/bin/env python3
"""Generate .def tables and the coverage report from normalized manifests."""

from __future__ import annotations

import argparse
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from state.generate import coverage, format_report, generate_def, write_coverage

TARGETS = ["armv7m", "cortex_m7_r0p2"]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", action="append", help="defaults to every known target")
    ap.add_argument("--coverage-only", action="store_true")
    a = ap.parse_args()

    rc = 0
    for t in (a.target or TARGETS):
        cov = coverage(t)
        if not a.coverage_only:
            d = generate_def(t)
            print(f"generated {d.relative_to(pathlib.Path.cwd())}")
        j = write_coverage(t)
        print(format_report(cov))
        print(f"{'Coverage artifact:':<34} {j.relative_to(pathlib.Path.cwd())}")
        if cov["unclassified"] or cov["exclusions_without_reason"]:
            print("FAIL: unaccounted-for source entries", file=sys.stderr)
            rc = 1
        print()
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
