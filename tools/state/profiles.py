"""
Row parsers for the table shapes the Arm manuals actually use.

Which profile applies to which table is recorded in spec/rules/<target>.tables.yaml
rather than guessed, so each table's treatment is reviewable.
"""

from __future__ import annotations

import re

from .armpdf import ACCESS_TOKENS, ADDR_RE, Table, expand_range, strip_footnote

RESERVED_WORDS = {"-", "reserved", "--"}


class RawRecord(dict):
    """A source row reduced to fields the normalizer understands."""


def _clean_name(value: str, description: str = "") -> str:
    """
    Strip Arm's superscript footnote letters glued onto a register name.

    'IEBR0k' -> 'IEBR0'. A trailing 'n' or 'x' is kept when the description
    repeats the name verbatim, because there it is an array index placeholder
    ('DWT_COMPn on page C1-745') rather than a footnote marker.
    """
    v = value.strip()
    m = re.fullmatch(r"([A-Z][A-Z0-9_]*?[0-9A-Z_])([a-z])", v)
    if not m:
        return v
    if m.group(2) in ("n", "x") and re.search(rf"\b{re.escape(v)}\b", description):
        return v
    return m.group(1)


def _is_reserved(name: str, desc: str) -> bool:
    return name.strip().lower() in RESERVED_WORDS or desc.strip().lower().startswith("reserved")


def _merge_split_ranges(table: Table) -> dict[str, str]:
    """
    Some range rows wrap, putting the end address alone on the next line:

        0xE0000000 -   ITM_STIMx  RW  UNKNOWN  Stimulus Port registers...
        0xE00003FC

    Return {start_address_cell: full_range} so rows can be repaired.
    """
    fixes: dict[str, str] = {}
    pages = [pg for pg, _ in table.raw_lines]
    lines = [l for _, l in table.raw_lines]
    for i, line in enumerate(lines):
        m = re.match(r"^\s*(0x[0-9A-Fa-f]+)\s*-\s{2,}", line)
        if not m:
            continue
        for nxt in lines[i + 1: i + 3]:
            m2 = re.fullmatch(r"\s*(0x[0-9A-Fa-f]+)\s*", nxt)
            if m2:
                fixes[m.group(1)] = f"{m.group(1)} - {m2.group(1)}"
                break
    return fixes


def _expand_placeholder(name: str, desc: str, addr_cell: str) -> list[tuple[str, str]]:
    """
    Expand an 'x'-suffixed array name using the range named in its description.

    'ITM_STIMx' + 'Stimulus Port registers, ITM_STIM0-ITM_STIM255' -> 256 entries.
    """
    m = re.search(r"\b([A-Z][A-Z0-9_]*?)(\d+)\s*-\s*([A-Z][A-Z0-9_]*?)(\d+)\b", desc)
    if not m or m.group(1) != m.group(3):
        return []
    base, lo, hi = m.group(1), int(m.group(2)), int(m.group(4))
    am = ADDR_RE.match(addr_cell.strip())
    if not am:
        return []
    start = int(am.group(1), 16)
    end = int(am.group(2), 16) if am.group(2) else start
    count = hi - lo + 1
    stride = (end - start) // (count - 1) if count > 1 and end > start else 4
    return [(f"0x{start + k * stride:08X}", f"{base}{lo + k}") for k in range(count)]


# Access types as Arm prints them. Some carry a qualifier ('RW clear'), and a
# few rows leave the column empty, so the qualifier and the column are optional.
_ACCESS_ALT = r"RW or RO|RAZ/WI|RO|RW|WO|RAZ|WI|-"
_ACCESS = r"(?:" + _ACCESS_ALT + r")(?:\s+(?:clear|only|once))?"
# An address cell: a plain address, an inline range, or a strided array base
# such as '0xE0002008+4n'.
# A range separator is attached to the address or single-spaced from it.
# Columns are separated by two or more spaces, so anything further away is the
# next column -- treating a Name column's '-' as a range dash makes the row
# swallow the line below it.
_ADDR = (r"0x[0-9A-Fa-f]+(?:\s*\+\s*\d*[a-z])?"
         r"(?:[ ]?(?:-|to)[ ]?(?:0x[0-9A-Fa-f]+)?)?")
# A name cell: an identifier, an inline name range, or an array slice such as
# 'CTIINEN[7:0]'. A trailing '_' absorbs a space the PDF inserted ('ID_ MMFR1').
# The second half of a name range must not be an access token: without the
# guard, 'NVIC_ISER0-          RW' parses as one name and the row is lost.
_NAME = (r"-|[A-Za-z_][A-Za-z0-9_]*_?(?:[ ]?[A-Za-z0-9_]+){0,2}"
         r"(?:\[\d+:\d+\])?"
         r"(?:\s*-\s*(?!(?:RW|RO|WO|RAZ|WI)\b)[A-Za-z_][A-Za-z0-9_]*)?")
_ROW_RE = re.compile(
    r"^\s*(?P<addr>" + _ADDR + r")(?P<adash>[ ]?-(?!\s*0x))?\s{2,}"
    r"(?P<name>" + _NAME + r")(?P<ndash>[ ]?-)?"
    r"(?:\s{2,}(?P<type>" + _ACCESS + r")[a-z]?)?"
    r"(?:\s{2,}(?P<rest>.*))?$"
)
_RESERVED_RE = re.compile(r"^\s*(?P<addr>" + _ADDR + r")[ ]?-?\s{2,}[-…]\s{2,}[-…]\s{2,}")
_CONT_RE = re.compile(r"^\s*(?P<addr>0x[0-9A-Fa-f]+)\s{2,}(?P<name>[A-Za-z_][A-Za-z0-9_]*)\b")

# 'CTIINEN[7:0]' is eight registers, the same as an explicit range.
_SLICE_RE = re.compile(r"^(?P<base>[A-Za-z_][A-Za-z0-9_]*)\[(?P<hi>\d+):(?P<lo>\d+)\]$")


def _expand_slice(name: str, addr_cell: str) -> list[tuple[str, str]]:
    m = _SLICE_RE.match(name.strip())
    if not m:
        return []
    lo, hi = int(m.group("lo")), int(m.group("hi"))
    am = ADDR_RE.match(addr_cell.strip())
    if not am:
        return []
    start = int(am.group(1), 16)
    end = int(am.group(2), 16) if am.group(2) else start
    count = hi - lo + 1
    stride = (end - start) // (count - 1) if count > 1 and end > start else 4
    return [(f"0x{start + k * stride:08X}", f"{m.group('base')}{lo + k}") for k in range(count)]


def parse_addr_name_type_reset(table: Table) -> list[RawRecord]:
    """
    Arm's standard address/name/type/reset summary.

    Driven by row pattern rather than detected columns: the header wraps across
    two or three lines in several of these tables and repeats on every page, so
    column detection is not a sound basis for a completeness claim.
    """
    out: list[RawRecord] = []
    consumed: set[int] = set()
    rejected: list[tuple[int, str]] = []
    pages = [pg for pg, _ in table.raw_lines]
    lines = [l for _, l in table.raw_lines]
    for i, line in enumerate(lines):
        # A line already absorbed as the tail of a wrapped range above must not
        # also be parsed on its own: that produces a phantom duplicate of the
        # last element of the range.
        if i in consumed:
            continue

        rm = _RESERVED_RE.match(line)
        if rm and "Reserved" in line:
            addr = rm.group("addr")
            # A reserved range wraps too: absorb its continuation line.
            if re.search(r"(?:-|to)\s*$", addr):
                for k in range(i + 1, min(i + 3, len(lines))):
                    if re.match(r"^\s*0x[0-9A-Fa-f]+", lines[k]):
                        consumed.add(k)
                        break
            out.append(RawRecord(name=None, address=addr, access="-",
                                 reset=None, description="Reserved", reserved=True,
                                 page=pages[i], row=None))
            continue

        m = _ROW_RE.match(line)
        if not m:
            continue

        addr, name = m.group("addr"), m.group("name")
        typ = (m.group("type") or "-").strip()
        rest = (m.group("rest") or "").strip()

        if name.strip() == "-":
            # A reserved or delegated range; absorb its continuation line too.
            if m.group("adash") or re.search(r"(?:-|to)\s*$", addr):
                for k in range(i + 1, min(i + 3, len(lines))):
                    if re.match(r"^\s*0x[0-9A-Fa-f]+", lines[k]):
                        consumed.add(k)
                        break
            out.append(RawRecord(name=None, address=addr, access=typ, reset=None,
                                 description=rest or "Reserved", reserved=True,
                                 page=pages[i], row=None))
            continue

        if m.group("adash") or m.group("ndash") or re.search(r"(?:-|to)\s*$", addr):
            for k in range(i + 1, min(i + 3, len(lines))):
                cm = _CONT_RE.match(lines[k])
                if cm:
                    addr = f"{addr.rstrip('- to')} - {cm.group('addr')}"
                    name = f"{name} - {cm.group('name')}"
                    consumed.add(k)
                    break
                if re.match(r"^\s*0x[0-9A-Fa-f]+\s*$", lines[k]):
                    addr = f"{addr.rstrip('- to')} - {lines[k].strip()}"
                    consumed.add(k)
                    break
                # A wrapped description line belonging to this row.
                if not re.match(r"^\s*0x[0-9A-Fa-f]+", lines[k]):
                    break
                consumed.add(k)
                break

        name = re.sub(r"_\s+", "_", name).strip()        # 'ID_ MMFR1' -> 'ID_MMFR1'
        # A few registers are named in words ('FIFO data 0'); join them into an
        # identifier. Range names keep their ' - ' so they can still expand.
        if "-" not in name:
            name = re.sub(r"\s+", "_", name)
        rest = rest or ""
        parts = rest.split(None, 1)
        reset = strip_footnote(parts[0]) if parts else ""
        desc = parts[1].strip() if len(parts) > 1 else ""

        pairs: list[tuple[str, str]] = []
        if "[" in name:
            pairs = _expand_slice(name, addr)
        if not pairs and "+" in addr:
            # A strided array base such as '0xE0002008+4n': the count is not in
            # this cell, so keep it as one conditional array entry.
            base = addr.split("+")[0].strip()
            pairs = [(base, name)]
        if not pairs and name.endswith("x"):
            pairs = _expand_placeholder(name, desc, addr)
        if not pairs and ("-" in addr or "-" in name):
            pairs = expand_range(addr, name)
        if not pairs:
            pairs = [(addr, _clean_name(name, desc))]

        for a, n in pairs:
            n = n.strip()
            # A name still carrying a separator means range expansion failed;
            # emitting it would invent a register that does not exist. Record
            # the rejection: a row discarded here matched the row pattern, so
            # it would otherwise be invisible to the balance check.
            if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", n):
                rejected.append((pages[i], f"{line.strip()[:100]}  [rejected name {n!r}]"))
                continue
            out.append(RawRecord(name=n, address=a, access=typ, reset=reset,
                                 description=desc, reserved=False, page=pages[i], row=None))

    table.consumed_lines = consumed
    table.rejected_rows = rejected
    return out


def unparsed_rows(table: Table) -> list[tuple[int, str]]:
    """
    Row-like lines the parser did not turn into a record.

    This is the balance check from the task spec: a source row that silently
    fails to parse is invisible, so the importer refuses to finish while any
    remain. Continuation lines of a wrapped range are consumed by the row above
    and are not drops.
    """
    lines = [l for _, l in table.raw_lines]
    consumed = getattr(table, "consumed_lines", set())
    out = list(getattr(table, "rejected_rows", []))
    for i, l in enumerate(lines):
        if i in consumed:
            continue
        if not re.match(r"^\s*0x[0-9A-Fa-f]+", l):
            continue
        if _ROW_RE.match(l) or _RESERVED_RE.match(l):
            continue
        if re.fullmatch(r"\s*0x[0-9A-Fa-f]+\s*", l):
            continue
        out.append((table.raw_lines[i][0], l.strip()))
    return out


_ID_NAME_RE = re.compile(r"(Peripheral|Component)\s*ID\s*(\d+)", re.I)


def parse_addr_register_value(table: Table) -> list[RawRecord]:
    """ID-value tables: 'Address | Register | Value | Description'."""
    out: list[RawRecord] = []
    for row in table.rows:
        addr = (row.get("Address") or "").strip()
        raw_name = (row.get("Register", "Name") or "").strip()
        value = strip_footnote(row.get("Value") or "")
        if not addr or not raw_name:
            continue
        m = _ID_NAME_RE.search(raw_name)
        name = f"{'PIDR' if m.group(1).lower() == 'peripheral' else 'CIDR'}{m.group(2)}" if m else raw_name
        out.append(RawRecord(name=name, address=addr, access="RO", reset=value,
                             description=raw_name, reserved=False, page=row.page, row=row))
    return out


def parse_core_register_index(table: Table) -> list[RawRecord]:
    """
    Table D8-1: 'Register | See', where the name cell holds a comma-separated
    list that can wrap onto the following line:

        R0, R1, R2, R3, R4, R5, R6,    Registers on page B1-516
        R7, R8, R9, R10, R11, R12

    A continuation line has no 'See' column and belongs to the row *above* it.
    Attaching it to the row below instead silently loses the tail of the list --
    which is how R12 and SP_main went missing.
    """
    out: list[RawRecord] = []
    rows: list[tuple[str, str, int]] = []      # (names, see, page)

    for _pg, line in table.raw_lines:
        cells = [c.strip() for c in re.split(r"\s{2,}", line.strip()) if c.strip()]
        if not cells or cells[0] in ("Register", "See"):
            continue
        if len(cells) >= 2:
            rows.append((cells[0], cells[-1], _pg))
        elif rows:
            # Continuation of the row above.
            names, see, pg = rows[-1]
            rows[-1] = (f"{names} {cells[0]}", see, pg)

    for names, see, pg in rows:
        for tok in re.split(r",\s*", names):
            tok = tok.strip().rstrip(",")
            if not tok:
                continue
            m = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)(?:\s*\(([^)]+)\))?$", tok)
            if not m:
                continue
            out.append(RawRecord(name=m.group(1), address=None, access="RW",
                                 reset=None, description=see, reserved=False,
                                 alt_name=m.group(2), page=pg, row=None))
    return out


_SYSM_RE = re.compile(r"(\d+)\s*=\s*0b[01]+:[01]+")


def parse_special_register_sysm(table: Table) -> list[RawRecord]:
    """
    Table B5-1: special-purpose registers reachable by MRS/MSR, with SYSm values.

    The first column may carry both the read and write spellings
    ('APSR, on reads' / 'APSR_<bits>, on writes'); the base name is what matters.
    """
    out: list[RawRecord] = []
    for _pg, line in table.raw_lines:
        m = _SYSM_RE.search(line)
        if not m:
            continue
        head = line[: m.start()].strip()
        cells = [c.strip() for c in re.split(r"\s{2,}", head) if c.strip()]
        if not cells:
            continue
        spec = cells[0]
        spec = re.sub(r",\s*on (reads|writes).*$", "", spec).strip()
        spec = re.sub(r"_<bits>", "", spec).strip()
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", spec):
            continue
        out.append(RawRecord(name=spec.upper(), address=None, access="RW", reset=None,
                             description=cells[1] if len(cells) > 1 else "",
                             reserved=False, sysm=int(m.group(1)), page=_pg, row=None))
    # The table lists read/write spellings on separate lines; keep first occurrence.
    seen, uniq = set(), []
    for r in out:
        if r["name"] in seen:
            continue
        seen.add(r["name"])
        uniq.append(r)
    return uniq


_FP_RE = re.compile(r"^\s*(?P<enc>0b[01]+(?:\s*-\s*0b[01]+)?)\s{2,}(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s{2,}(?P<desc>.*)$")


def parse_fp_common_block(table: Table) -> list[RawRecord]:
    """Table B1-10: 'System register | Name | Description', reached by VMRS/VMSR."""
    out: list[RawRecord] = []
    for _pg, line in table.raw_lines:
        m = _FP_RE.match(line)
        if not m:
            continue
        name, desc, enc = m.group("name"), m.group("desc").strip(), m.group("enc")
        if _is_reserved(name, desc):
            out.append(RawRecord(name=None, address=enc, access="-", reset=None,
                                 description=desc or "Reserved", reserved=True, page=_pg, row=None))
            continue
        out.append(RawRecord(name=name, address=enc, access="RW", reset=None,
                             description=desc, reserved=False, page=_pg, row=None))
    return out


_OTN_RE = re.compile(
    r"^\s*(?P<off>0x[0-9A-Fa-f]+)\s{2,}(?P<type>RO|RW|WO)\s{2,}(?P<rest>.+?)\s*$")


def parse_offset_type_name(table: Table) -> list[RawRecord]:
    """Table D1-2: 'Address offset | Type | Register name | Notes'."""
    out: list[RawRecord] = []
    for _pg, line in table.raw_lines:
        m = _OTN_RE.match(line)
        if not m:
            continue
        rest = m.group("rest")
        nm = re.search(r"\(([A-Z][A-Z0-9_]*)\)", rest)     # 'Lock Status (LSR)'
        if not nm:
            continue
        out.append(RawRecord(name=nm.group(1), address=m.group("off"), access=m.group("type"),
                             reset=None, description=rest.strip(), reserved=False, page=_pg, row=None))
    return out


def parse_offset_value_name(table: Table) -> list[RawRecord]:
    """Table C1-3: 'Offset | Value | Name | Description' (ROM table entries)."""
    out: list[RawRecord] = []
    for row in table.rows:
        off = (row.get("Offset") or "").strip()
        name = (row.get("Name") or "").strip()
        val = (row.get("Value") or "").strip()
        desc = (row.get("Description") or "").strip()
        if not name or not re.fullmatch(r"[A-Z][A-Z0-9_]*", name):
            continue
        out.append(RawRecord(name=name, address=off, access="RO", reset=val,
                             description=desc, reserved=False, page=row.page, row=row))
    return out


PROFILES = {
    "addr_name_type_reset": parse_addr_name_type_reset,
    "addr_register_value": parse_addr_register_value,
    "core_register_index": parse_core_register_index,
    "special_register_sysm": parse_special_register_sysm,
    "fp_common_block": parse_fp_common_block,
    "offset_type_name": parse_offset_type_name,
    "offset_value_name": parse_offset_value_name,
}


_DESCNAME_RE = re.compile(
    r"^\s*(?P<addr>0x[0-9A-Fa-f]+)\s{2,}(?P<type>RO|RW|WO)\s{2,}(?P<reset>\S+(?:\s\S+)*?)\s{2,}(?P<desc>\S.*)$")


def parse_addr_type_reset_descname(table: Table) -> list[RawRecord]:
    """
    Tables whose register name lives inside the description rather than in a
    column of its own, e.g. Table B4-1:

        0xE000ED40  RO  IMPLEMENTATION DEFINED  Processor Feature Register 0, ID_PFR0 on page B4-646
    """
    out: list[RawRecord] = []
    for _pg, line in table.raw_lines:
        m = _DESCNAME_RE.match(line)
        if not m:
            continue
        desc = m.group("desc").strip()
        # Prefer an explicit ', NAME on page' spelling; fall back to a leading token.
        nm = re.search(r",\s*([A-Z][A-Z0-9_]{2,})\s+on page", desc)
        if not nm:
            nm = re.match(r"([A-Z][A-Z0-9_]{2,})\b", desc)
        if not nm:
            continue
        out.append(RawRecord(name=nm.group(1), address=m.group("addr"), access=m.group("type"),
                             reset=m.group("reset"), description=desc, reserved=False,
                             page=_pg, row=None))
    return out


PROFILES["addr_type_reset_descname"] = parse_addr_type_reset_descname
