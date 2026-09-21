"""
ctypes bindings to libhw.

These declarations mirror include/hw.h and include/hw_state.h exactly. A
mismatch between the two does not fail loudly -- it silently reads the wrong
bytes -- so the structs here are written field for field in the same order as
the headers, and tests/test_bindings.py checks the layout against the C
library rather than trusting that they stayed in step.
"""

from __future__ import annotations

import ctypes
import ctypes.util
import os
import pathlib

LIBRARY_ENV = "LIBHW_LIBRARY"


class HwLibraryNotFound(RuntimeError):
    """libhw.so could not be located."""


def _candidate_paths() -> list[pathlib.Path]:
    out: list[pathlib.Path] = []

    override = os.environ.get(LIBRARY_ENV)
    if override:
        out.append(pathlib.Path(override).expanduser())

    # The build tree: utils/python/libhw/_ffi.py -> repo root -> out/libhw.so
    here = pathlib.Path(__file__).resolve()
    for parent in here.parents:
        candidate = parent / "out" / "libhw.so"
        if candidate.is_file():
            out.append(candidate)
            break

    found = ctypes.util.find_library("hw")
    if found:
        out.append(pathlib.Path(found))
    return out


def load_library() -> ctypes.CDLL:
    tried = _candidate_paths()
    for path in tried:
        if path.is_file() or not path.is_absolute():
            try:
                return ctypes.CDLL(str(path))
            except OSError:
                continue
    raise HwLibraryNotFound(
        "Could not load libhw.so.\n"
        f"  Tried: {', '.join(str(p) for p in tried) or '(nothing)'}\n"
        f"  Build it with 'make' at the repository root, or set {LIBRARY_ENV} "
        f"to the shared library."
    )


# --- types mirroring include/hw.h ------------------------------------------

class HwFlashInfo(ctypes.Structure):
    _fields_ = [
        ("base", ctypes.c_uint),
        ("size", ctypes.c_uint),
        ("page_size", ctypes.c_uint),
        ("write_align", ctypes.c_uint),
    ]


class HwFlashPatchStats(ctypes.Structure):
    _fields_ = [
        ("sectors_total", ctypes.c_uint),
        ("sectors_changed", ctypes.c_uint),
        ("sectors_erased", ctypes.c_uint),
        ("bytes_written", ctypes.c_uint),
    ]


# --- types mirroring include/hw_state.h ------------------------------------

class HwStateDesc(ctypes.Structure):
    _fields_ = [
        ("name", ctypes.c_char_p),
        ("ns", ctypes.c_int),
        ("access", ctypes.c_int),
        ("encoding", ctypes.c_char_p),
        ("enc_kind", ctypes.c_int),
        ("width", ctypes.c_uint16),
        ("readable", ctypes.c_uint8),
        ("writable", ctypes.c_uint8),
        ("snapshot", ctypes.c_uint8),
        ("feature", ctypes.c_char_p),
        ("component", ctypes.c_char_p),
    ]


class HwStateAlias(ctypes.Structure):
    _fields_ = [
        ("name", ctypes.c_char_p),
        ("target", ctypes.c_char_p),
        ("bit_offset", ctypes.c_uint16),
        ("bit_width", ctypes.c_uint16),
    ]


class HwStateOperation(ctypes.Structure):
    _fields_ = [
        ("name", ctypes.c_char_p),
        ("encoding", ctypes.c_char_p),
        ("reason", ctypes.c_char_p),
    ]


class HwStateComponent(ctypes.Structure):
    _fields_ = [
        ("name", ctypes.c_char_p),
        ("base", ctypes.c_uint32),
    ]


class HwStateDbgReg(ctypes.Structure):
    _fields_ = [
        ("name", ctypes.c_char_p),
        ("regsel", ctypes.c_uint8),
        ("lsb", ctypes.c_uint8),
        ("width", ctypes.c_uint8),
    ]


class HwCpuFeatures(ctypes.Structure):
    _fields_ = [
        ("cpuid", ctypes.c_uint32),
        ("partno", ctypes.c_uint16),
        ("variant", ctypes.c_uint8),
        ("revision", ctypes.c_uint8),
        ("overlay_revision", ctypes.c_char_p),
        ("revision_matches", ctypes.c_uint8),
        ("cpu_name", ctypes.c_char_p),
        ("overlay", ctypes.c_char_p),
        ("fp_extension", ctypes.c_uint8),
        ("mpu", ctypes.c_uint8),
        ("mpu_regions", ctypes.c_uint8),
        ("dwt_numcomp", ctypes.c_uint8),
        ("nvic_intlinesnum", ctypes.c_uint8),
    ]


class HwStateResult(ctypes.Structure):
    _fields_ = [
        ("presence", ctypes.c_int),
        ("evidence", ctypes.c_char_p),
        ("read", ctypes.c_int),
        ("value", ctypes.c_uint32),
        ("value_is_zero", ctypes.c_uint8),
    ]


class HwStateDb(ctypes.Structure):
    _fields_ = [
        ("name", ctypes.c_char_p),
        ("source", ctypes.c_char_p),
        ("states", ctypes.POINTER(HwStateDesc)),
        ("state_count", ctypes.c_size_t),
        ("aliases", ctypes.POINTER(HwStateAlias)),
        ("alias_count", ctypes.c_size_t),
        ("operations", ctypes.POINTER(HwStateOperation)),
        ("operation_count", ctypes.c_size_t),
        ("dbgregs", ctypes.POINTER(HwStateDbgReg)),
        ("dbgreg_count", ctypes.c_size_t),
        ("components", ctypes.POINTER(HwStateComponent)),
        ("component_count", ctypes.c_size_t),
    ]


HwContextP = ctypes.c_void_p
_U32P = ctypes.POINTER(ctypes.c_uint)
_U8P = ctypes.POINTER(ctypes.c_uint8)


def _bind(lib: ctypes.CDLL) -> None:
    """Give every entry point an explicit signature."""
    f = lib

    f.hw_connect.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
    f.hw_connect.restype = HwContextP
    f.hw_close.argtypes = [HwContextP]
    f.hw_close.restype = None

    f.hw_read32.argtypes = [HwContextP, ctypes.c_uint, _U32P]
    f.hw_read32.restype = ctypes.c_int
    f.hw_write32.argtypes = [HwContextP, ctypes.c_uint, ctypes.c_uint]
    f.hw_write32.restype = ctypes.c_int
    f.hw_read8.argtypes = [HwContextP, ctypes.c_uint, _U8P]
    f.hw_read8.restype = ctypes.c_int
    f.hw_write8.argtypes = [HwContextP, ctypes.c_uint, ctypes.c_uint8]
    f.hw_write8.restype = ctypes.c_int

    for name in ("hw_board_halted", "hw_board_halt", "hw_board_step",
                 "hw_board_run", "hw_board_reset", "hw_flash_mass_erase"):
        fn = getattr(f, name)
        fn.argtypes = [HwContextP]
        fn.restype = ctypes.c_int

    f.hw_read_reg.argtypes = [HwContextP, ctypes.c_int]
    f.hw_read_reg.restype = ctypes.c_uint64
    f.hw_write_reg.argtypes = [HwContextP, ctypes.c_int, ctypes.c_uint64]
    f.hw_write_reg.restype = None

    f.hw_flash_info.argtypes = [HwContextP, ctypes.POINTER(HwFlashInfo)]
    f.hw_flash_info.restype = ctypes.c_int
    f.hw_flash_sector.argtypes = [HwContextP, ctypes.c_uint, _U32P, _U32P]
    f.hw_flash_sector.restype = ctypes.c_int
    for name in ("hw_flash_read", "hw_flash_write", "hw_flash_verify",
                 "hw_flash_update"):
        fn = getattr(f, name)
        fn.argtypes = [HwContextP, ctypes.c_uint, ctypes.c_void_p, ctypes.c_size_t]
        fn.restype = ctypes.c_int
    f.hw_flash_erase.argtypes = [HwContextP, ctypes.c_uint, ctypes.c_size_t]
    f.hw_flash_erase.restype = ctypes.c_int
    f.hw_flash_patch.argtypes = [HwContextP, ctypes.c_uint, ctypes.c_void_p,
                                 ctypes.c_void_p, ctypes.c_size_t,
                                 ctypes.POINTER(HwFlashPatchStats)]
    f.hw_flash_patch.restype = ctypes.c_int

    f.hw_state_db.argtypes = [ctypes.c_char_p]
    f.hw_state_db.restype = ctypes.POINTER(HwStateDb)
    f.hw_state_find.argtypes = [ctypes.POINTER(HwStateDb), ctypes.c_char_p]
    f.hw_state_find.restype = ctypes.POINTER(HwStateDesc)
    f.hw_state_dbgreg.argtypes = [ctypes.POINTER(HwStateDb), ctypes.c_char_p]
    f.hw_state_dbgreg.restype = ctypes.POINTER(HwStateDbgReg)
    f.hw_state_identify.argtypes = [HwContextP, ctypes.POINTER(HwCpuFeatures)]
    f.hw_state_identify.restype = ctypes.c_int
    f.hw_state_expected.argtypes = [ctypes.POINTER(HwStateDesc),
                                    ctypes.POINTER(HwCpuFeatures)]
    f.hw_state_expected.restype = ctypes.c_int
    f.hw_state_presence.argtypes = [HwContextP, ctypes.POINTER(HwStateDesc),
                                    ctypes.POINTER(HwCpuFeatures),
                                    ctypes.POINTER(ctypes.c_char_p)]
    f.hw_state_presence.restype = ctypes.c_int
    f.hw_state_query.argtypes = [HwContextP, ctypes.POINTER(HwStateDesc),
                                 ctypes.POINTER(HwCpuFeatures),
                                 ctypes.POINTER(HwStateResult)]
    f.hw_state_query.restype = ctypes.c_int
    f.hw_state_read.argtypes = [HwContextP, ctypes.POINTER(HwStateDesc),
                                ctypes.POINTER(ctypes.c_uint32),
                                ctypes.POINTER(ctypes.c_int)]
    f.hw_state_read.restype = ctypes.c_int
    f.hw_presence_name.argtypes = [ctypes.c_int]
    f.hw_presence_name.restype = ctypes.c_char_p
    f.hw_read_status_name.argtypes = [ctypes.c_int]
    f.hw_read_status_name.restype = ctypes.c_char_p


_lib: ctypes.CDLL | None = None


def lib() -> ctypes.CDLL:
    """The loaded library, bound on first use."""
    global _lib
    if _lib is None:
        _lib = load_library()
        _bind(_lib)
    return _lib


def databases() -> list[HwStateDb]:
    """The NULL-name-terminated hw_state_databases[] array."""
    arr = (HwStateDb * 64).in_dll(lib(), "hw_state_databases")
    out = []
    for entry in arr:
        if not entry.name:
            break
        out.append(entry)
    return out
