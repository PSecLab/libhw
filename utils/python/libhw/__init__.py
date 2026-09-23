"""
Python bindings for libhw.

    from libhw import connect

    with connect("mock") as hw:
        hw.flash.update(0x08000000, image)
        stats = hw.flash.patch(0x08000000, old_image, new_image)
        print(stats.sectors_changed, "sectors changed")

The bindings load out/libhw.so through ctypes, so there is nothing to compile;
build the library at the repository root first, or point LIBHW_LIBRARY at it.
"""

from ._ffi import BUNDLED_DIR, HwLibraryNotFound, LIBRARY_ENV
from .core import Flash, FlashInfo, Hw, PatchStats, connect
from .errors import (HwConnectError, HwError, HwFlashError, HwStateError)
from .state import (Access, Alias, Component, CpuFeatures, DebugReg,
                    EncodingKind, Namespace, Operation, Presence, QueryResult,
                    ReadStatus, StateDb, StateDesc, database_names, state_db)

__all__ = [
    "connect", "Hw", "Flash", "FlashInfo", "PatchStats",
    "HwError", "HwConnectError", "HwFlashError", "HwStateError",
    "HwLibraryNotFound", "LIBRARY_ENV", "BUNDLED_DIR",
    "state_db", "database_names", "StateDb", "StateDesc",
    "Namespace", "Access", "EncodingKind", "Presence", "ReadStatus",
    "CpuFeatures", "QueryResult", "Alias", "Operation", "DebugReg", "Component",
]

__version__ = "0.1.0"
