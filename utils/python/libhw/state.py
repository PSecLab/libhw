"""The generated architectural-state database, as Python objects."""

from __future__ import annotations

import ctypes
import dataclasses
import enum
from typing import Iterator

from . import _ffi
from .errors import HwStateError


class Namespace(enum.IntEnum):
    ARCH = 0
    CPU = 1
    DEBUG = 2
    CLUSTER = 3
    UNDOCUMENTED_DISCOVERED = 4


class Access(enum.IntEnum):
    MEMORY_MAPPED = 0
    SPECIAL_REG = 1
    CORE_REG = 2
    SYSREG = 3
    CP15 = 4
    CSR = 5
    FP_SYSREG = 6


class EncodingKind(enum.IntEnum):
    ABSOLUTE = 0
    OFFSET = 1
    SYSM = 2
    CORE = 3
    FP_SYSREG = 4


class Presence(enum.IntEnum):
    """Whether the target implements a piece of state.

    Deliberately separate from whether a read of it succeeded: an
    unimplemented word in the Private Peripheral Bus generally reads as zero
    rather than faulting, so a clean read is not evidence of implementation.
    """

    UNKNOWN = 0
    ARCHITECTURAL = 1
    CONFIG_CONFIRMED = 2
    CONFIG_DENIED = 3
    DISCOVERED = 4
    DISCOVERY_ABSENT = 5

    @property
    def is_implemented(self) -> bool:
        """True only on positive evidence. UNKNOWN is not implemented."""
        return self in (Presence.ARCHITECTURAL, Presence.CONFIG_CONFIRMED,
                        Presence.DISCOVERED)

    def __str__(self) -> str:
        return _ffi.lib().hw_presence_name(int(self)).decode()


class ReadStatus(enum.IntEnum):
    NOT_ATTEMPTED = 0
    OK = 1
    BACKEND_UNSUPPORTED = 2
    UNSAFE = 3
    ACCESS_DENIED = 4
    FAILED = 5

    def __str__(self) -> str:
        return _ffi.lib().hw_read_status_name(int(self)).decode()


def _s(p) -> str | None:
    return p.decode() if p else None


@dataclasses.dataclass(frozen=True)
class StateDesc:
    """One architectural or implementation-defined state element."""

    name: str
    namespace: Namespace
    access: Access
    encoding: str | None
    encoding_kind: EncodingKind
    width: int
    readable: bool
    writable: bool
    snapshot: bool
    feature: str | None
    component: str | None
    _ptr: object = dataclasses.field(repr=False, compare=False, default=None)

    @classmethod
    def _from_c(cls, c, ptr=None) -> "StateDesc":
        feature = _s(c.feature)
        return cls(
            name=_s(c.name) or "",
            namespace=Namespace(c.ns),
            access=Access(c.access),
            encoding=_s(c.encoding) or None,
            encoding_kind=EncodingKind(c.enc_kind),
            width=int(c.width),
            readable=bool(c.readable),
            writable=bool(c.writable),
            snapshot=bool(c.snapshot),
            feature=feature or None,
            component=_s(c.component) or None,
            _ptr=ptr,
        )


@dataclasses.dataclass(frozen=True)
class Alias:
    name: str
    target: str
    bit_offset: int
    bit_width: int


@dataclasses.dataclass(frozen=True)
class Operation:
    """A memory-mapped location whose write performs an action."""

    name: str
    encoding: str | None
    reason: str | None


@dataclasses.dataclass(frozen=True)
class DebugReg:
    """A DCRSR/DCRDR access path. REGSEL is not the same as MRS/MSR's SYSm."""

    name: str
    regsel: int
    lsb: int
    width: int


@dataclasses.dataclass(frozen=True)
class Component:
    name: str
    base: int


@dataclasses.dataclass(frozen=True)
class CpuFeatures:
    """What a target implements, derived from its ID registers."""

    cpuid: int
    partno: int
    variant: int
    revision: int
    cpu_name: str | None
    overlay: str | None
    overlay_revision: str | None
    revision_matches: bool
    fp_extension: bool
    mpu: bool
    mpu_regions: int
    dwt_numcomp: int
    nvic_intlinesnum: int
    _raw: object = dataclasses.field(repr=False, compare=False, default=None)

    @property
    def part_revision(self) -> str:
        return f"r{self.variant}p{self.revision}"

    @property
    def interrupt_lines(self) -> int:
        return (self.nvic_intlinesnum + 1) * 32

    @classmethod
    def _from_c(cls, c) -> "CpuFeatures":
        return cls(
            cpuid=int(c.cpuid), partno=int(c.partno), variant=int(c.variant),
            revision=int(c.revision), cpu_name=_s(c.cpu_name),
            overlay=_s(c.overlay), overlay_revision=_s(c.overlay_revision),
            revision_matches=bool(c.revision_matches),
            fp_extension=bool(c.fp_extension), mpu=bool(c.mpu),
            mpu_regions=int(c.mpu_regions), dwt_numcomp=int(c.dwt_numcomp),
            nvic_intlinesnum=int(c.nvic_intlinesnum), _raw=c,
        )


@dataclasses.dataclass(frozen=True)
class QueryResult:
    """Presence and read outcome, kept apart on purpose."""

    presence: Presence
    evidence: str | None
    read: ReadStatus
    value: int | None
    value_is_zero: bool

    @property
    def is_implemented(self) -> bool:
        """Never inferred from the read outcome."""
        return self.presence.is_implemented


class StateDb:
    """A generated state database: an architecture, or a CPU overlay."""

    def __init__(self, ptr) -> None:
        if not ptr:
            raise HwStateError("no such state database")
        self._p = ptr
        self._c = ptr.contents if hasattr(ptr, "contents") else ptr

    @property
    def name(self) -> str:
        return _s(self._c.name) or ""

    @property
    def source(self) -> str:
        return _s(self._c.source) or ""

    def __len__(self) -> int:
        return int(self._c.state_count)

    def __iter__(self) -> Iterator[StateDesc]:
        for i in range(int(self._c.state_count)):
            yield StateDesc._from_c(self._c.states[i],
                                    ctypes.pointer(self._c.states[i]))

    @property
    def states(self) -> list[StateDesc]:
        return list(self)

    @property
    def aliases(self) -> list[Alias]:
        return [Alias(_s(a.name) or "", _s(a.target) or "",
                      int(a.bit_offset), int(a.bit_width))
                for a in (self._c.aliases[i] for i in range(int(self._c.alias_count)))]

    @property
    def operations(self) -> list[Operation]:
        return [Operation(_s(o.name) or "", _s(o.encoding), _s(o.reason))
                for o in (self._c.operations[i]
                          for i in range(int(self._c.operation_count)))]

    @property
    def components(self) -> list[Component]:
        return [Component(_s(c.name) or "", int(c.base))
                for c in (self._c.components[i]
                          for i in range(int(self._c.component_count)))]

    @property
    def debug_regs(self) -> list[DebugReg]:
        return [DebugReg(_s(d.name) or "", int(d.regsel), int(d.lsb), int(d.width))
                for d in (self._c.dbgregs[i]
                          for i in range(int(self._c.dbgreg_count)))]

    def find(self, name: str) -> StateDesc | None:
        p = _ffi.lib().hw_state_find(self._p, name.encode())
        if not p:
            return None
        return StateDesc._from_c(p.contents, p)

    def debug_reg(self, name: str) -> DebugReg | None:
        p = _ffi.lib().hw_state_dbgreg(self._p, name.encode())
        if not p:
            return None
        d = p.contents
        return DebugReg(_s(d.name) or "", int(d.regsel), int(d.lsb), int(d.width))

    def snapshot_state(self) -> list[StateDesc]:
        return [d for d in self if d.snapshot]

    def __repr__(self) -> str:
        return f"<StateDb {self.name!r} source={self.source!r} states={len(self)}>"


def state_db(name: str) -> StateDb:
    """Look up a built-in state database by name, e.g. 'armv7m'."""
    p = _ffi.lib().hw_state_db(name.encode())
    if not p:
        raise HwStateError(f"no state database named {name!r}; "
                           f"known: {', '.join(database_names())}")
    return StateDb(p)


def database_names() -> list[str]:
    return [d.name.decode() for d in _ffi.databases()]
