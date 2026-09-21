"""The connection to a target, and everything reachable through it."""

from __future__ import annotations

import ctypes
from typing import Iterator

from . import _ffi
from .errors import HwConnectError, HwError, HwFlashError, HwStateError
from .state import (CpuFeatures, Presence, QueryResult, ReadStatus, StateDb,
                    StateDesc, state_db)


class FlashInfo:
    """Geometry of the target's program flash."""

    __slots__ = ("base", "size", "page_size", "write_align")

    def __init__(self, c) -> None:
        self.base = int(c.base)
        self.size = int(c.size)
        self.page_size = int(c.page_size)
        self.write_align = int(c.write_align)

    @property
    def end(self) -> int:
        return self.base + self.size

    def __repr__(self) -> str:
        return (f"FlashInfo(base=0x{self.base:08X}, size={self.size}, "
                f"page_size={self.page_size}, write_align={self.write_align})")


class PatchStats:
    """What a patch actually touched."""

    __slots__ = ("sectors_total", "sectors_changed", "sectors_erased", "bytes_written")

    def __init__(self, c) -> None:
        self.sectors_total = int(c.sectors_total)
        self.sectors_changed = int(c.sectors_changed)
        self.sectors_erased = int(c.sectors_erased)
        self.bytes_written = int(c.bytes_written)

    def __repr__(self) -> str:
        return (f"PatchStats(sectors_total={self.sectors_total}, "
                f"sectors_changed={self.sectors_changed}, "
                f"sectors_erased={self.sectors_erased}, "
                f"bytes_written={self.bytes_written})")


class Flash:
    """The flash API, bound to one target.

    Reached as `hw.flash`. Mirrors the C API, including the part that surprises
    people: `write` does not erase, so on its own it can only clear bits. Use
    `update` to erase and reprogram, or `patch` to apply only what changed.
    """

    def __init__(self, hw: "Hw") -> None:
        self._hw = hw

    def info(self) -> FlashInfo:
        c = _ffi.HwFlashInfo()
        if _ffi.lib().hw_flash_info(self._hw._ctx, ctypes.byref(c)) != 0:
            raise HwFlashError("hw_flash_info failed: the backend reported no geometry")
        return FlashInfo(c)

    def sector(self, addr: int) -> tuple[int, int]:
        """The (base, size) of the erase block containing `addr`."""
        base = ctypes.c_uint(0)
        size = ctypes.c_uint(0)
        if _ffi.lib().hw_flash_sector(self._hw._ctx, addr,
                                      ctypes.byref(base), ctypes.byref(size)) != 0:
            raise HwFlashError(f"no erase block covering 0x{addr:08X}")
        return int(base.value), int(size.value)

    def read(self, addr: int, length: int) -> bytes:
        if length <= 0:
            return b""
        buf = ctypes.create_string_buffer(length)
        if _ffi.lib().hw_flash_read(self._hw._ctx, addr, buf, length) != 0:
            raise HwFlashError(f"flash read of {length} bytes at 0x{addr:08X} failed")
        return buf.raw

    def write(self, addr: int, data: bytes) -> None:
        """Program without erasing. Bits can only go 1 -> 0."""
        if not data:
            return
        if _ffi.lib().hw_flash_write(self._hw._ctx, addr, data, len(data)) != 0:
            raise HwFlashError(f"flash write of {len(data)} bytes at 0x{addr:08X} failed")

    def erase(self, addr: int, length: int) -> None:
        """Erase every block overlapping the range. Whole blocks go."""
        if length <= 0:
            return
        if _ffi.lib().hw_flash_erase(self._hw._ctx, addr, length) != 0:
            raise HwFlashError(f"flash erase of {length} bytes at 0x{addr:08X} failed")

    def mass_erase(self) -> None:
        if _ffi.lib().hw_flash_mass_erase(self._hw._ctx) != 0:
            raise HwFlashError("mass erase failed")

    def verify(self, addr: int, data: bytes) -> bool:
        if not data:
            return True
        return _ffi.lib().hw_flash_verify(self._hw._ctx, addr, data, len(data)) == 0

    def update(self, addr: int, data: bytes) -> None:
        """Erase and reprogram the range, then verify.

        Bytes sharing an erase block with the range but outside it are read
        first and written back. Halts the target and leaves it halted.
        """
        if not data:
            return
        if _ffi.lib().hw_flash_update(self._hw._ctx, addr, data, len(data)) != 0:
            raise HwFlashError(f"flash update of {len(data)} bytes at 0x{addr:08X} failed")

    def patch(self, addr: int, old: bytes | None, new: bytes) -> PatchStats:
        """Apply the minimal set of operations that turns `old` into `new`.

        Blocks identical in both images are never touched, and with `old`
        supplied they are never even read. A changed block is erased only if
        some bit has to go 0 -> 1. Passing `old=None` reads the current
        contents instead, which is correct but costs a full read.
        """
        if old is not None and len(old) != len(new):
            raise ValueError("old and new images must describe the same range")
        if not new:
            return PatchStats(_ffi.HwFlashPatchStats())

        stats = _ffi.HwFlashPatchStats()
        rc = _ffi.lib().hw_flash_patch(self._hw._ctx, addr, old, new, len(new),
                                       ctypes.byref(stats))
        if rc != 0:
            raise HwFlashError(f"flash patch of {len(new)} bytes at 0x{addr:08X} failed")
        return PatchStats(stats)


class Hw:
    """A connection to a target through one libhw backend."""

    def __init__(self, backend: str, host: str | None = None, port: int = 0) -> None:
        ctx = _ffi.lib().hw_connect(backend.encode(),
                                    host.encode() if host else None, port)
        if not ctx:
            raise HwConnectError(
                f"could not connect with backend {backend!r}"
                + (f" to {host}:{port}" if host else "")
            )
        self._ctx = ctx
        self._backend = backend
        self.flash = Flash(self)

    # --- lifetime ----------------------------------------------------------

    @property
    def closed(self) -> bool:
        return self._ctx is None

    def close(self) -> None:
        if self._ctx is not None:
            _ffi.lib().hw_close(self._ctx)
            self._ctx = None

    def __enter__(self) -> "Hw":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def _check(self) -> None:
        if self._ctx is None:
            raise HwError("this connection is closed")

    # --- memory ------------------------------------------------------------

    def read32(self, addr: int) -> int:
        self._check()
        v = ctypes.c_uint(0)
        if _ffi.lib().hw_read32(self._ctx, addr, ctypes.byref(v)) != 0:
            raise HwError(f"read32 at 0x{addr:08X} failed")
        return int(v.value)

    def write32(self, addr: int, value: int) -> None:
        self._check()
        if _ffi.lib().hw_write32(self._ctx, addr, value) != 0:
            raise HwError(f"write32 at 0x{addr:08X} failed")

    def read8(self, addr: int) -> int:
        self._check()
        v = ctypes.c_uint8(0)
        if _ffi.lib().hw_read8(self._ctx, addr, ctypes.byref(v)) != 0:
            raise HwError(f"read8 at 0x{addr:08X} failed")
        return int(v.value)

    def write8(self, addr: int, value: int) -> None:
        self._check()
        if _ffi.lib().hw_write8(self._ctx, addr, value) != 0:
            raise HwError(f"write8 at 0x{addr:08X} failed")

    # --- execution ---------------------------------------------------------

    @property
    def halted(self) -> bool:
        self._check()
        return bool(_ffi.lib().hw_board_halted(self._ctx))

    def halt(self) -> None:
        self._check()
        if _ffi.lib().hw_board_halt(self._ctx) != 0:
            raise HwError("halt failed")

    def run(self) -> None:
        self._check()
        if _ffi.lib().hw_board_run(self._ctx) != 0:
            raise HwError("run failed")

    def step(self) -> None:
        self._check()
        if _ffi.lib().hw_board_step(self._ctx) != 0:
            raise HwError("single step failed")

    def reset(self) -> None:
        self._check()
        if _ffi.lib().hw_board_reset(self._ctx) != 0:
            raise HwError("reset failed, or this backend does not implement it")

    def read_reg(self, reg: int) -> int:
        self._check()
        return int(_ffi.lib().hw_read_reg(self._ctx, reg))

    def write_reg(self, reg: int, value: int) -> None:
        self._check()
        _ffi.lib().hw_write_reg(self._ctx, reg, value)

    # --- architectural state -----------------------------------------------

    def identify(self) -> CpuFeatures:
        """Read the target's ID registers and derive what it implements."""
        self._check()
        c = _ffi.HwCpuFeatures()
        if _ffi.lib().hw_state_identify(self._ctx, ctypes.byref(c)) != 0:
            raise HwStateError("could not read the target's ID registers")
        return CpuFeatures._from_c(c)

    def state_db(self, name: str) -> StateDb:
        return state_db(name)

    def databases_for(self, features: CpuFeatures) -> list[StateDb]:
        """The architecture database, plus a CPU overlay when one is pinned."""
        dbs = [state_db("armv7m")]
        if features.overlay:
            dbs.append(state_db(features.overlay))
        return dbs

    def query(self, desc: StateDesc, features: CpuFeatures | None = None) -> QueryResult:
        """Decide presence, and separately attempt a read.

        The read outcome never changes the presence verdict: a clean read is
        not evidence that a register is implemented.
        """
        self._check()
        if desc._ptr is None:
            raise HwStateError(f"{desc.name} was not obtained from a state database")
        out = _ffi.HwStateResult()
        fp = ctypes.byref(features._raw) if features is not None else None
        if _ffi.lib().hw_state_query(self._ctx, desc._ptr, fp, ctypes.byref(out)) != 0:
            raise HwStateError(f"query of {desc.name} failed")
        return QueryResult(
            presence=Presence(out.presence),
            evidence=out.evidence.decode() if out.evidence else None,
            read=ReadStatus(out.read),
            value=int(out.value) if out.read == ReadStatus.OK else None,
            value_is_zero=bool(out.value_is_zero),
        )

    def snapshot(self, features: CpuFeatures | None = None) -> dict[str, int]:
        """Read every snapshot-marked element that the target implements."""
        self._check()
        feats = features if features is not None else self.identify()
        out: dict[str, int] = {}
        for db in self.databases_for(feats):
            for desc in db:
                if not desc.snapshot:
                    continue
                r = self.query(desc, feats)
                if r.read == ReadStatus.OK and r.is_implemented:
                    out[f"{db.name}:{desc.name}"] = r.value
        return out

    def __repr__(self) -> str:
        return f"<Hw backend={self._backend!r} {'closed' if self.closed else 'open'}>"


def connect(backend: str, host: str | None = None, port: int = 0) -> Hw:
    """Connect to a target. Usable as a context manager."""
    return Hw(backend, host, port)
