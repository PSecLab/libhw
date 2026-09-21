"""
Independent cross-check of the Cortex-M state set against CMSIS-Core headers.

CMSIS is not the completeness authority -- the architecture manual and the CPU
TRM are. This module exists so that a disagreement between the two becomes
visible and gets investigated, which is the only useful role a second
implementation can play.

The comparison is made on absolute addresses rather than names, because CMSIS
names its fields relative to a block (SysTick->CTRL) while the manuals use
global names (SYST_CSR). Addresses are the thing both sides must agree on.
"""

from __future__ import annotations

import dataclasses
import pathlib
import re
from typing import Any

# Scalar widths used in CMSIS core structs.
_TYPE_SIZE = {"uint8_t": 1, "uint16_t": 2, "uint32_t": 4, "uint64_t": 8}

_STRUCT_RE = re.compile(r"typedef\s+struct\s*\{(?P<body>.*?)\}\s*(?P<name>\w+)_Type\s*;", re.S)
# An anonymous union member: its footprint is its widest scalar, not their sum.
_UNION_RE = re.compile(r"union\s*\{(?P<body>[^{}]*)\}\s*(?P<name>\w+)\s*(?:\[(?P<count>[^\]]+)\])?\s*;", re.S)
_MEMBER_RE = re.compile(
    r"^\s*(?:__IM|__OM|__IOM|volatile|const)?\s*"
    r"(?P<type>u?int(?:8|16|32|64)_t)\s+(?P<name>\w+)\s*(?:\[(?P<count>[^\]]+)\])?\s*;",
    re.M)
_DEFINE_RE = re.compile(r"^#define\s+(?P<name>\w+_BASE)\s+\((?P<expr>[^)]*)\)", re.M)

# Which CMSIS struct is mapped at which base symbol.
STRUCT_BASE = {
    "SCB": "SCB_BASE", "SysTick": "SysTick_BASE", "NVIC": "NVIC_BASE",
    "ITM": "ITM_BASE", "DWT": "DWT_BASE", "TPI": "TPI_BASE",
    "CoreDebug": "CoreDebug_BASE", "MPU": "MPU_BASE", "FPU": "FPU_BASE",
    "SCnSCB": "SCS_BASE",
}


@dataclasses.dataclass
class CmsisReg:
    address: int
    struct: str
    field: str
    width: int


def _eval_base(expr: str, known: dict[str, int]) -> int | None:
    e = expr.replace("UL", "").replace("U", "").strip()
    for k, v in known.items():
        e = re.sub(rf"\b{k}\b", str(v), e)
    if not re.fullmatch(r"[0-9xXa-fA-F+\-*/() ]+", e):
        return None
    try:
        return int(eval(e, {"__builtins__": {}}, {}))    # arithmetic over literals only
    except Exception:
        return None


def parse_header(path: pathlib.Path) -> dict[int, CmsisReg]:
    text = path.read_text(errors="replace")

    bases: dict[str, int] = {}
    for _ in range(4):                       # resolve symbols defined in terms of others
        for m in _DEFINE_RE.finditer(text):
            if m.group("name") in bases:
                continue
            v = _eval_base(m.group("expr"), bases)
            if v is not None:
                bases[m.group("name")] = v

    out: dict[int, CmsisReg] = {}
    for sm in _STRUCT_RE.finditer(text):
        struct = sm.group("name")
        base_sym = STRUCT_BASE.get(struct)
        if not base_sym or base_sym not in bases:
            continue
        base = bases[base_sym]

        body = sm.group("body")

        # Replace each union with a single scalar of the union's own size, so
        # the running offset stays correct.
        def _flatten(m: re.Match) -> str:
            widest = max((_TYPE_SIZE[t] for t in _TYPE_SIZE
                          if re.search(rf"\b{t}\b", m.group("body"))), default=4)
            tname = {1: "uint8_t", 2: "uint16_t", 4: "uint32_t", 8: "uint64_t"}[widest]
            cnt = f"[{m.group('count')}]" if m.group("count") else ""
            return f"{tname} {m.group('name')}{cnt};"

        body = _UNION_RE.sub(_flatten, body)

        offset = 0
        for mm in _MEMBER_RE.finditer(body):
            size = _TYPE_SIZE[mm.group("type")]
            count = 1
            if mm.group("count"):
                c = _eval_base(mm.group("count"), bases)
                count = c if c else 1
            name = mm.group("name")
            if not re.match(r"R[E]?SERVED|PADDING", name.upper()):
                for k in range(count):
                    addr = base + offset + k * size
                    out[addr] = CmsisReg(addr, struct, name if count == 1 else f"{name}[{k}]", size * 8)
            offset += size * count
    return out


def diff_against(manifest_entries: list[dict[str, Any]], cmsis: dict[int, CmsisReg]) -> dict[str, Any]:
    """Compare address sets. Differences are reported, never auto-resolved."""
    ours: dict[int, str] = {}
    for e in manifest_entries:
        enc = e.get("encoding") or ""
        if not enc.startswith("0x") or e.get("access") != "memory_mapped":
            continue
        if e.get("encoding_kind") != "absolute":
            continue
        if e["classification"] in ("explicit_exclusion",):
            continue
        try:
            ours[int(enc, 16)] = e["canonical_name"]
        except ValueError:
            continue

    only_cmsis = sorted(set(cmsis) - set(ours))
    only_ours = sorted(set(ours) - set(cmsis))
    return {
        "cmsis_registers": len(cmsis),
        "manifest_registers": len(ours),
        "in_both": len(set(cmsis) & set(ours)),
        "only_in_cmsis": [{"address": f"0x{a:08X}", "cmsis": f"{cmsis[a].struct}->{cmsis[a].field}"}
                          for a in only_cmsis],
        "only_in_manifest": [{"address": f"0x{a:08X}", "name": ours[a]} for a in only_ours],
    }
