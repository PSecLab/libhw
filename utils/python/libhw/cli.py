"""
`libhw-probe`: identify a target and report its architectural state.

Presence and readability are reported separately. A successful read is not
evidence that a register is implemented, because an unimplemented word in the
Private Peripheral Bus generally reads as zero rather than faulting.
"""

from __future__ import annotations

import argparse
import collections
import sys

from . import __version__
from ._ffi import HwLibraryNotFound
from .core import connect
from .errors import HwError
from .state import database_names


def _report_target(hw) -> object:
    f = hw.identify()
    print("=== target ===")
    print(f"  CPUID            0x{f.cpuid:08X}  (PARTNO 0x{f.partno:03X})")
    print(f"  part             {f.cpu_name or 'not recognised'} {f.part_revision}")
    print(f"  overlay          {f.overlay or 'none pinned'}")
    if f.overlay and not f.revision_matches:
        print(f"  NOTE             the pinned TRM documents {f.overlay_revision}; "
              f"this part is {f.part_revision}.")
        print(f"                   Differences introduced after "
              f"{f.overlay_revision} are not covered.")
    print(f"  FP extension     {'implemented' if f.fp_extension else 'not implemented'}")
    print(f"  MPU regions      {f.mpu_regions}")
    print(f"  DWT comparators  {f.dwt_numcomp}")
    print(f"  interrupt lines  {f.interrupt_lines}")
    return f


def _report_state(hw, f, verbose: bool) -> None:
    for db in hw.databases_for(f):
        presence = collections.Counter()
        reads = collections.Counter()
        read_ok_unknown = 0

        for desc in db:
            r = hw.query(desc, f)
            presence[str(r.presence)] += 1
            reads[str(r.read)] += 1
            if r.read.name == "OK" and not r.is_implemented:
                read_ok_unknown += 1
            if verbose and r.read.name == "OK":
                print(f"    {desc.name:18} {desc.encoding or '-':14} "
                      f"0x{r.value:08X}  {r.presence}")

        print(f"\n=== {db.name} ({db.source}): {len(db)} state elements ===")
        print("  implemented (evidence-based):")
        for k, n in presence.most_common():
            print(f"      {k:24} {n}")
        print("  read outcome (independent of the above):")
        for k, n in reads.most_common():
            print(f"      {k:24} {n}")
        if read_ok_unknown:
            print(f"  read succeeded with no presence evidence: {read_ok_unknown}")
            print("      Not counted as implemented: an unimplemented word in the")
            print("      PPB generally reads as zero rather than faulting.")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        prog="libhw-probe",
        description="Identify a target and report its architectural state.")
    ap.add_argument("backend", nargs="?", default="mock",
                    help="stlink, openocd or mock (default: mock)")
    ap.add_argument("--host", default=None, help="for network backends")
    ap.add_argument("--port", type=int, default=0, help="for network backends")
    ap.add_argument("--verbose", action="store_true", help="print every value read")
    ap.add_argument("--resume", action="store_true",
                    help="let the target run again afterwards")
    ap.add_argument("--version", action="version", version=f"libhw {__version__}")
    a = ap.parse_args(argv)

    print(f"built-in databases: {', '.join(database_names())}\n")

    try:
        with connect(a.backend, a.host, a.port) as hw:
            if not hw.halted:
                print("--> halting the target (core register reads need Debug state)\n")
                hw.halt()
            features = _report_target(hw)
            _report_state(hw, features, a.verbose)
            if a.resume:
                hw.run()
                print("\n--> target resumed")
            else:
                print("\n--> leaving the target halted (pass --resume to let it run)")
    except HwLibraryNotFound as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    except HwError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
