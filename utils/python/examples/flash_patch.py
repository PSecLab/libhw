#!/usr/bin/env python3
"""
Differential flash update.

Writes an image, then patches it to a second image and reports what the patch
actually had to touch. Runs against the mock backend by default, so it needs
no hardware:

    python3 examples/flash_patch.py            # mock
    python3 examples/flash_patch.py stlink     # a real board
"""

import sys
import pathlib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from libhw import connect


def main() -> int:
    backend = sys.argv[1] if len(sys.argv) > 1 else "mock"

    with connect(backend) as hw:
        info = hw.flash.info()
        print(f"flash: 0x{info.base:08X} + {info.size} bytes, "
              f"{info.page_size}-byte pages, {info.write_align}-byte writes")

        addr = info.base
        old = bytes((i * 7 + 11) & 0xFF for i in range(4 * info.page_size))

        print(f"--> writing {len(old)} bytes")
        hw.flash.mass_erase()
        hw.flash.update(addr, old)
        assert hw.flash.verify(addr, old)

        # Two edits: one that only clears bits, one that needs a bit set.
        new = bytearray(old)
        new[64] &= 0xF0                      # clears bits: no erase needed
        new[2 * info.page_size + 8] |= 0xFF  # may need a bit raised

        print("--> patching")
        stats = hw.flash.patch(addr, old, bytes(new))
        print(f"    sectors spanned : {stats.sectors_total}")
        print(f"    sectors changed : {stats.sectors_changed}")
        print(f"    sectors erased  : {stats.sectors_erased}")
        print(f"    bytes written   : {stats.bytes_written}")

        whole = len(new)
        print(f"    (a blind rewrite would have programmed {whole} bytes)")

        assert hw.flash.read(addr, len(new)) == bytes(new)
        print("--> verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
