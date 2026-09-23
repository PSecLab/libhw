"""
Tests for the libhw Python bindings.

Everything here runs against the mock backend, so no board is needed. The
first group is the one that matters most: ctypes struct definitions that drift
from the C headers do not fail loudly, they silently read the wrong bytes, so
the layout is checked against the compiler rather than assumed.
"""

from __future__ import annotations

import ctypes
import pathlib
import shutil
import subprocess
import sys
import textwrap

import pytest

HERE = pathlib.Path(__file__).resolve().parent
PKG_ROOT = HERE.parent
REPO_ROOT = PKG_ROOT.parents[1]

# Prefer an installed package, so this suite can validate a wheel as well as
# the source tree. Only fall back to the checkout when nothing is installed.
try:
    import libhw  # noqa: F401
except ImportError:
    sys.path.insert(0, str(PKG_ROOT))

libhw = pytest.importorskip("libhw")
from libhw import (Access, EncodingKind, HwError, HwFlashError, Namespace,  # noqa: E402
                   Presence, ReadStatus, connect, database_names, state_db)
from libhw import _ffi  # noqa: E402


# --- ABI ---------------------------------------------------------------------

# (ctypes class, C type name, [field names to check offsets of])
ABI_STRUCTS = [
    (_ffi.HwFlashInfo, "hw_flash_info_t", ["base", "size", "page_size", "write_align"]),
    (_ffi.HwFlashPatchStats, "hw_flash_patch_stats_t",
     ["sectors_total", "sectors_changed", "sectors_erased", "bytes_written"]),
    (_ffi.HwStateDesc, "hw_state_desc_t",
     ["name", "ns", "access", "encoding", "enc_kind", "width", "readable",
      "writable", "snapshot", "feature", "component"]),
    (_ffi.HwStateAlias, "hw_state_alias_t", ["name", "target", "bit_offset", "bit_width"]),
    (_ffi.HwStateOperation, "hw_state_operation_t", ["name", "encoding", "reason"]),
    (_ffi.HwStateComponent, "hw_state_component_t", ["name", "base"]),
    (_ffi.HwStateDbgReg, "hw_state_dbgreg_t", ["name", "regsel", "lsb", "width"]),
    (_ffi.HwStateResult, "hw_state_result_t",
     ["presence", "evidence", "read", "value", "value_is_zero"]),
    (_ffi.HwCpuFeatures, "hw_cpu_features_t",
     ["cpuid", "partno", "variant", "revision", "overlay_revision",
      "revision_matches", "cpu_name", "overlay", "fp_extension", "mpu",
      "mpu_regions", "dwt_numcomp", "nvic_intlinesnum"]),
    (_ffi.HwStateDb, "hw_state_db_t",
     ["name", "source", "states", "state_count", "aliases", "alias_count",
      "operations", "operation_count", "dbgregs", "dbgreg_count",
      "components", "component_count"]),
]


def _c_layout(tmp_path) -> dict[str, dict[str, int]]:
    """Ask the compiler for the real sizeof/offsetof of every bound struct."""
    lines = ['#include <stdio.h>', '#include <stddef.h>',
             '#include "hw.h"', '#include "hw_state.h"', "int main(void) {"]
    for _cls, cname, fields in ABI_STRUCTS:
        lines.append(f'    printf("{cname} sizeof %zu\\n", sizeof({cname}));')
        for f in fields:
            lines.append(
                f'    printf("{cname} {f} %zu\\n", offsetof({cname}, {f}));')
    lines += ["    return 0;", "}"]

    src = tmp_path / "abi_probe.c"
    src.write_text("\n".join(lines))
    exe = tmp_path / "abi_probe"
    subprocess.run(
        ["cc", "-std=c11", f"-I{REPO_ROOT / 'include'}", str(src), "-o", str(exe)],
        check=True, capture_output=True)
    out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout

    layout: dict[str, dict[str, int]] = {}
    for line in out.splitlines():
        cname, key, value = line.rsplit(" ", 2)[0], line.split()[1], int(line.split()[2])
        layout.setdefault(cname, {})[key] = value
    return layout


@pytest.mark.skipif(shutil.which("cc") is None, reason="no C compiler to check the ABI against")
def test_ctypes_structs_match_the_c_abi(tmp_path):
    """
    Every bound struct must have the compiler's size and field offsets.

    A drifted ctypes definition reads the wrong bytes without any error, so
    this is the check that keeps the bindings honest when a header changes.
    """
    layout = _c_layout(tmp_path)
    problems = []
    for cls, cname, fields in ABI_STRUCTS:
        want = layout[cname]
        if ctypes.sizeof(cls) != want["sizeof"]:
            problems.append(f"{cname}: sizeof {ctypes.sizeof(cls)} != C {want['sizeof']}")
        for f in fields:
            got = getattr(cls, f).offset
            if got != want[f]:
                problems.append(f"{cname}.{f}: offset {got} != C {want[f]}")
    assert problems == [], "ctypes definitions have drifted from the headers:\n  " + \
        "\n  ".join(problems)


def test_enum_values_match_the_headers():
    """Enum members must line up with the C enumerations, in order."""
    header = (REPO_ROOT / "include" / "hw_state.h").read_text()
    for py_enum, prefix in ((Namespace, "HW_NS_"), (Access, "HW_ACC_"),
                            (EncodingKind, "HW_ENC_"), (Presence, "HW_PRESENCE_"),
                            (ReadStatus, "HW_READ_")):
        names = [m.name for m in py_enum]
        for i, name in enumerate(names):
            token = f"{prefix}{name}"
            assert token in header, f"{token} is not in hw_state.h"
            assert py_enum[name].value == i, \
                f"{token} must be {i} to match the C enumeration order"


# --- connection --------------------------------------------------------------

def test_unknown_backend_raises():
    with pytest.raises(libhw.HwConnectError):
        connect("no_such_backend")


def test_context_manager_closes():
    with connect("mock") as hw:
        assert not hw.closed
    assert hw.closed
    with pytest.raises(HwError):
        hw.read32(0x20000000)


def test_double_close_is_safe():
    hw = connect("mock")
    hw.close()
    hw.close()


# --- memory ------------------------------------------------------------------

def test_memory_round_trip():
    with connect("mock") as hw:
        hw.write32(0x20000000, 0xDEADBEEF)
        assert hw.read32(0x20000000) == 0xDEADBEEF
        hw.write8(0x20000010, 0xA5)
        assert hw.read8(0x20000010) == 0xA5


def test_execution_control():
    with connect("mock") as hw:
        hw.halt()
        assert hw.halted
        hw.run()
        assert not hw.halted
        hw.reset()
        assert hw.halted        # the mock resets into a halted state


# --- flash -------------------------------------------------------------------

def test_flash_geometry_and_sector():
    with connect("mock") as hw:
        info = hw.flash.info()
        assert info.base == 0x08000000
        assert info.page_size == 1024
        assert info.end == info.base + info.size
        base, size = hw.flash.sector(info.base + 1500)
        assert (base, size) == (info.base + 1024, 1024)


def test_flash_update_and_verify():
    with connect("mock") as hw:
        hw.flash.mass_erase()
        assert hw.flash.read(0x08000000, 16) == b"\xff" * 16
        image = bytes(range(256)) * 8
        hw.flash.update(0x08000000, image)
        assert hw.flash.read(0x08000000, len(image)) == image
        assert hw.flash.verify(0x08000000, image)


def test_flash_write_cannot_raise_bits():
    """Programming clears bits; it cannot set them. The mock models this."""
    with connect("mock") as hw:
        hw.flash.mass_erase()
        hw.flash.write(0x08000000, b"\x0f\x0f\x0f\x0f")
        assert hw.flash.read(0x08000000, 4) == b"\x0f" * 4
        hw.flash.write(0x08000000, b"\xff\xff\xff\xff")
        assert hw.flash.read(0x08000000, 4) == b"\x0f" * 4


def test_patch_skips_the_erase_when_only_clearing_bits():
    with connect("mock") as hw:
        hw.flash.mass_erase()
        old = bytes([0xFF] * 4096)
        hw.flash.update(0x08000000, old)
        new = bytearray(old)
        new[64:68] = b"\xf0\xf0\xf0\xf0"        # only clears bits
        stats = hw.flash.patch(0x08000000, old, bytes(new))
        assert stats.sectors_changed == 1
        assert stats.sectors_erased == 0
        assert stats.bytes_written < 1024
        assert hw.flash.read(0x08000000, 4096) == bytes(new)


def test_patch_erases_when_a_bit_must_be_set():
    with connect("mock") as hw:
        hw.flash.mass_erase()
        old = bytearray([0xFF] * 4096)
        old[2048:2052] = b"\x00\x00\x00\x00"
        hw.flash.update(0x08000000, bytes(old))
        new = bytearray(old)
        new[2048:2052] = b"\x01\x01\x01\x01"    # needs a bit raised
        stats = hw.flash.patch(0x08000000, bytes(old), bytes(new))
        assert stats.sectors_erased == 1
        assert hw.flash.read(0x08000000, 4096) == bytes(new)


def test_patch_of_an_identical_image_touches_nothing():
    with connect("mock") as hw:
        hw.flash.mass_erase()
        image = bytes(range(256)) * 16
        hw.flash.update(0x08000000, image)
        stats = hw.flash.patch(0x08000000, image, image)
        assert stats.sectors_changed == 0
        assert stats.bytes_written == 0


def test_patch_with_no_old_image_reads_the_target():
    with connect("mock") as hw:
        hw.flash.mass_erase()
        image = bytes(range(256)) * 8
        hw.flash.update(0x08000000, image)
        new = bytearray(image)
        new[10] &= 0xF0
        stats = hw.flash.patch(0x08000000, None, bytes(new))
        assert stats.sectors_changed == 1
        assert hw.flash.read(0x08000000, len(new)) == bytes(new)


def test_patch_rejects_mismatched_image_lengths():
    with connect("mock") as hw:
        with pytest.raises(ValueError):
            hw.flash.patch(0x08000000, b"\x00" * 4, b"\x00" * 8)


def test_flash_errors_raise():
    with connect("mock") as hw:
        info = hw.flash.info()
        with pytest.raises(HwFlashError):
            hw.flash.read(info.end + 0x1000, 4)
        with pytest.raises(HwFlashError):
            hw.flash.update(info.end + 0x1000, b"\x00\x00\x00\x00")


# --- state database ----------------------------------------------------------

def test_databases_are_available():
    names = database_names()
    assert "armv7m" in names
    db = state_db("armv7m")
    assert db.source.startswith("DDI0403")
    assert len(db) > 400


def test_unknown_database_raises():
    with pytest.raises(libhw.HwStateError):
        state_db("nope")


def test_descriptor_fields_are_decoded():
    db = state_db("armv7m")
    primask = db.find("PRIMASK")
    assert primask is not None
    assert primask.namespace is Namespace.ARCH
    assert primask.access is Access.SPECIAL_REG
    assert primask.encoding == "SYSm=16"
    assert primask.width == 32
    assert db.find("NOT_A_REGISTER") is None


def test_fp_register_file_is_present():
    """The file is defined only in prose; it must still be in the database."""
    db = state_db("armv7m")
    assert all(db.find(f"S{i}") is not None for i in range(32))
    s0 = db.find("S0")
    assert s0.feature == "FP_EXTENSION"


def test_alias_targets_exist():
    db = state_db("armv7m")
    for a in db.aliases:
        assert db.find(a.target) is not None, f"{a.name} aliases a missing {a.target}"


def test_operations_carry_a_reason():
    db = state_db("armv7m")
    assert db.operations
    for op in db.operations:
        assert op.reason and op.reason.strip()


def test_sysm_and_regsel_are_different_encodings():
    db = state_db("armv7m")
    assert db.debug_reg("MSP").regsel == 17
    assert db.find("MSP").encoding == "SYSm=8"
    primask = db.debug_reg("PRIMASK")
    assert (primask.regsel, primask.lsb, primask.width) == (20, 0, 8)


# --- presence is not readability ---------------------------------------------

def test_presence_is_not_inferred_from_a_read():
    with connect("mock") as hw:
        db = state_db("armv7m")
        feats = hw.identify()
        unknown_but_read = 0
        for desc in db:
            r = hw.query(desc, feats)
            if r.presence in (Presence.CONFIG_DENIED, Presence.DISCOVERY_ABSENT):
                assert r.read is ReadStatus.NOT_ATTEMPTED
            if r.read is ReadStatus.OK and r.presence is Presence.UNKNOWN:
                unknown_but_read += 1
                assert not r.is_implemented, \
                    "a successful read must never count as implemented"
        # The mock implements no optional feature and has no CoreSight IDs, so
        # this case must actually occur; otherwise the test proves nothing.
        assert unknown_but_read > 0


def test_offsets_are_never_read_as_addresses():
    with connect("mock") as hw:
        db = state_db("armv7m")
        feats = hw.identify()
        offsets = [d for d in db if d.encoding_kind is EncodingKind.OFFSET]
        assert offsets
        for d in offsets:
            assert hw.query(d, feats).read is not ReadStatus.OK


def test_presence_names_round_trip():
    assert str(Presence.ARCHITECTURAL) == "ARCHITECTURAL"
    assert str(ReadStatus.OK) == "OK"
    assert Presence.ARCHITECTURAL.is_implemented
    assert not Presence.UNKNOWN.is_implemented
    assert not Presence.DISCOVERY_ABSENT.is_implemented
