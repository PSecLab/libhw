#!/usr/bin/env python3
"""
Identify a target and report its architectural state.

Presence and readability are reported separately: a successful read is not
evidence that a register is implemented, because an unimplemented word in the
Private Peripheral Bus generally reads as zero rather than faulting.

    python3 examples/state_dump.py             # mock
    python3 examples/state_dump.py stlink      # a real board
"""

import collections
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from libhw import connect, database_names


def main() -> int:
    backend = sys.argv[1] if len(sys.argv) > 1 else "mock"
    print(f"built-in databases: {', '.join(database_names())}\n")

    with connect(backend) as hw:
        if not hw.halted:
            hw.halt()

        f = hw.identify()
        print("=== target ===")
        print(f"  CPUID            0x{f.cpuid:08X}  (PARTNO 0x{f.partno:03X})")
        print(f"  part             {f.cpu_name or 'not recognised'} {f.part_revision}")
        print(f"  overlay          {f.overlay or 'none pinned'}")
        if f.overlay and not f.revision_matches:
            print(f"  NOTE             the pinned TRM documents {f.overlay_revision}; "
                  f"this part is {f.part_revision}")
        print(f"  FP extension     {'yes' if f.fp_extension else 'no'}")
        print(f"  MPU regions      {f.mpu_regions}")
        print(f"  DWT comparators  {f.dwt_numcomp}")
        print(f"  interrupt lines  {f.interrupt_lines}")

        for db in hw.databases_for(f):
            presence = collections.Counter()
            reads = collections.Counter()
            for desc in db:
                r = hw.query(desc, f)
                presence[str(r.presence)] += 1
                reads[str(r.read)] += 1

            print(f"\n=== {db.name} ({db.source}): {len(db)} elements ===")
            print("  implemented (evidence-based):")
            for k, n in presence.most_common():
                print(f"      {k:22} {n}")
            print("  read outcome (independent):")
            for k, n in reads.most_common():
                print(f"      {k:22} {n}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
