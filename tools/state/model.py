"""
Normalized libhw architectural-state model.

Nothing downstream of the importers understands Arm's or RISC-V's native data
formats. Importers produce StateEntry records; generators, the coverage report
and the CI checks consume only those. That separation is what lets a new CPU be
added by pointing an importer at a new manual rather than by inventing another
enumeration methodology.
"""

from __future__ import annotations

import dataclasses
import enum
import re
from typing import Any


class Classification(str, enum.Enum):
    """
    How a source record has been accounted for.

    Every record drawn from an authoritative source must land in exactly one of
    these. There is deliberately no generic IGNORE: an exclusion without a
    reason is indistinguishable from an oversight.
    """

    EMITTED_STATE = "emitted_state"
    # A CPU-overlay row that restates state the architecture manifest already
    # accounts for. Section 14 of the task spec requires this bucket so a TRM's
    # row count can be balanced against the architecture it implements.
    ARCHITECTURE_DEFINED = "architecture_defined"
    ALIAS = "alias"
    NON_STATE_OPERATION = "non_state_operation"
    CONDITIONAL_STATE = "conditional_state"
    EXPLICIT_EXCLUSION = "explicit_exclusion"


class Namespace(str, enum.Enum):
    """
    Which state universe an entry belongs to.

    Keeping these apart is what stops the meaning of a "CPU snapshot" from
    drifting as a function of how many peripherals a given TRM happens to
    document.
    """

    ARCH = "arch"        # ISA-defined PE state
    CPU = "cpu"          # implementation-defined state for a specific CPU
    DEBUG = "debug"      # CoreSight / DWT / ITM / FPB / CTI / debug block
    CLUSTER = "cluster"  # SCU, private timers, GIC CPU interface
    # Experimentally discovered but absent from any authoritative manual. Never
    # counted towards the documented-completeness claim.
    UNDOCUMENTED = "undocumented_discovered"


class Access(str, enum.Enum):
    """The mechanism by which the state is reached."""

    MEMORY_MAPPED = "memory_mapped"   # PPB/SCS and other MMIO
    SPECIAL_REG = "special_reg"       # Cortex-M MRS/MSR special registers
    CORE_REG = "core_reg"             # R0-R15, xPSR via debug core register file
    SYSREG = "sysreg"                 # AArch64 MRS/MSR system registers
    CP15 = "cp15"                     # AArch32 coprocessor
    CSR = "csr"                       # RISC-V control/status registers
    FP_SYSREG = "fp_sysreg"           # VFP/FP system registers (VMRS/VMSR)


@dataclasses.dataclass(frozen=True)
class Provenance:
    """
    Where an entry came from, precisely enough for someone else to re-derive it.

    Recording only the document name is not enough: a reviewer has to be able to
    open the manual at the right table and check the row.
    """

    document: str            # e.g. DDI0489
    revision: str            # e.g. B  (product revision r0p2)
    section: str             # e.g. "3.3 System control registers"
    table: str               # e.g. "Table 3-1 System control registers"
    row: str                 # the register name as printed in that table
    page: int | None = None  # 1-based PDF page the row was read from

    def to_dict(self) -> dict[str, Any]:
        return dataclasses.asdict(self)


@dataclasses.dataclass(frozen=True)
class Alias:
    """A named view onto storage that another entry owns."""

    target: str       # canonical name of the owning entry
    bit_offset: int
    bit_width: int


@dataclasses.dataclass
class StateEntry:
    """One architectural or implementation-defined state element."""

    canonical_name: str
    source_id: str                 # stable ID within the source, for diffing
    architecture: str              # ARMv7-M, AArch64, RV32I, ...
    namespace: Namespace
    classification: Classification
    provenance: Provenance

    width: int | None = None       # bits
    access: Access | None = None
    encoding: str | None = None    # "0xE000ED00", "S3_0_C1_C0_0", "p15,0,c1,c0,0"
    # "absolute" | "offset" | "sysm" | "core" | "fp_sysreg". An offset is
    # meaningless without its component base, so it must not be compared
    # against absolute addresses.
    encoding_kind: str = "absolute"
    readable: bool = True
    writable: bool = True

    # Populated for CONDITIONAL_STATE. A runtime feature resolver turns the
    # implemented feature set into the expected state set.
    feature_requirement: str | None = None

    alias: Alias | None = None     # populated for ALIAS

    # Why an entry is not emitted state. Mandatory for NON_STATE_OPERATION and
    # EXPLICIT_EXCLUSION; a bare exclusion is rejected by the classifier.
    reason: str | None = None

    # Whether this element belongs in a whole-CPU snapshot. Reserved/WO/
    # side-effecting registers generally do not.
    snapshot: bool = False

    # Which pass produced this entry: a register-summary table, a declaration
    # for state the manual documents only in prose, or an expanded register
    # file. Reported separately so the coverage figures are not one opaque
    # number.
    derivation: str = "table"      # table | declared | register_file | prose

    source_family: str = ""        # AARCHMRS, ARM_TRM, ARM_ARM, RISCV_OPCODES
    source_release: str = ""       # pinned release/revision of that source

    # Debug access path (DCRSR REGSEL), where one exists. This is a second,
    # independent way to reach the same state: an instruction uses SYSm, a
    # debugger uses REGSEL, and the two encodings differ.
    debug_regsel: int | None = None
    debug_lsb: int = 0
    debug_width: int = 32

    # Owning block (SCB, NVIC, DWT, ITM, ...). CoreSight ID registers repeat the
    # same names in every component at different addresses, so the component is
    # part of a register's identity rather than decoration.
    component: str = ""

    def state_id(self) -> str:
        """Stable, unique identifier used by generated code and CI checks."""
        comp = f"{self.component}:" if self.component else ""
        return f"{self.architecture}:{self.namespace.value}:{comp}{self.canonical_name}"

    def c_name(self) -> str:
        """Collision-free C identifier for the generated descriptors."""
        base = self.canonical_name
        if self.component and not base.upper().startswith(self.component.upper()):
            base = f"{self.component}_{base}"
        return c_identifier(base)

    def to_dict(self) -> dict[str, Any]:
        d = dataclasses.asdict(self)
        d["namespace"] = self.namespace.value
        d["classification"] = self.classification.value
        d["access"] = self.access.value if self.access else None
        d["state_id"] = self.state_id()
        return d


_C_IDENT_RE = re.compile(r"[^A-Za-z0-9_]")


def c_identifier(name: str) -> str:
    """Turn a register name as printed in a manual into a C identifier."""
    ident = _C_IDENT_RE.sub("_", name.strip()).strip("_")
    ident = re.sub(r"_{2,}", "_", ident)
    if ident and ident[0].isdigit():
        ident = "_" + ident
    return ident.upper()
