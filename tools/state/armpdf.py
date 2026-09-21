"""
Systematic register-table extraction from Arm PDF manuals.

The rule from the task spec is that a manual must not be read linearly with
registers added as they happen to appear. So this module enumerates *every*
table caption in the document, records an explicit keep/skip decision with a
reason for each one, and only then parses the kept tables. The skipped tables
stay in the report, which is what makes an omission visible rather than silent.

Text comes from `pdftotext -layout`, which preserves the column geometry the Arm
templates use and emits a form feed per page, so every row keeps a page number.
"""

from __future__ import annotations

import dataclasses
import re
import subprocess
from typing import Any

# A table caption, e.g. "Table 3-1 System control registers (continued)"
TABLE_CAPTION_RE = re.compile(
    r"^\s*Table\s+(?P<id>[A-Z]?\d+-\d+)\s+(?P<caption>\S.*?)\s*(?P<cont>\(continued\))?\s*$"
)

# A numbered section heading, e.g. "3.3    System control registers"
SECTION_RE = re.compile(r"^\s{0,24}(?P<num>[A-Z]?\d+(?:\.\d+){1,3})\s{2,}(?P<title>[A-Z]\S.{2,70})$")

# A table header row. Arm register summaries are keyed on an Address or a Name.
HEADER_TOKENS = ("Address", "Name", "Type", "Reset", "Offset", "Register", "Description")

# Leading cell of a row: a single address or an inclusive address range.
ADDR_RE = re.compile(r"^(0x[0-9A-Fa-f]+)(?:\s*-\s*(0x[0-9A-Fa-f]+))?$")

# Access types Arm prints, longest first so "RW or RO" wins over "RW".
ACCESS_TOKENS = ("RW or RO", "RAZ/WI", "RAZ", "WI", "RO", "RW", "WO")

# Page furniture that must never be mistaken for table content.
FURNITURE_RE = re.compile(
    r"(Copyright\s+©|Non-Confidential|^\s*ARM\s+DDI\s|^\s*ID\d{6}|All rights reserved)"
)

# Captions worth parsing as register enumerations.
REGISTER_CAPTION_RE = re.compile(
    r"\b(register|registers)\b.*\b(summary|summaries)\b"
    r"|\bsummary of\b.*\bregisters?\b"
    r"|\bregisters?\b\s*$"
    r"|\b(register map|register summary)\b",
    re.IGNORECASE,
)


@dataclasses.dataclass
class Row:
    """One parsed row of a register-summary table."""

    cells: dict[str, str]
    page: int
    raw: str

    def get(self, *names: str) -> str | None:
        for n in names:
            v = self.cells.get(n)
            if v:
                return v
        return None


@dataclasses.dataclass
class Table:
    table_id: str
    caption: str
    section: str
    page: int
    columns: list[str] = dataclasses.field(default_factory=list)
    rows: list[Row] = dataclasses.field(default_factory=list)
    # Verbatim body lines, for profiles that need to re-parse the raw shape.
    raw_lines: list[tuple[int, str]] = dataclasses.field(default_factory=list)

    @property
    def label(self) -> str:
        return f"Table {self.table_id} {self.caption}"


@dataclasses.dataclass
class TableDecision:
    """Why a table caption was or was not parsed. Auditable by construction."""

    table_id: str
    caption: str
    section: str
    page: int
    kept: bool
    reason: str
    row_count: int = 0

    def to_dict(self) -> dict[str, Any]:
        return dataclasses.asdict(self)


def pdf_pages(path: str) -> list[str]:
    """Extract per-page layout-preserved text."""
    out = subprocess.run(
        ["pdftotext", "-layout", str(path), "-"],
        check=True, capture_output=True, text=True, errors="replace",
    ).stdout
    return out.split("\f")


def _split_cells(line: str) -> list[str]:
    """Arm's table columns are separated by runs of two or more spaces."""
    return [c.strip() for c in re.split(r"\s{2,}", line.strip()) if c.strip()]


def _is_header(line: str) -> bool:
    cells = _split_cells(line)
    if len(cells) < 2:
        return False
    hits = sum(1 for c in cells if c in HEADER_TOKENS)
    return hits >= 2


def strip_footnote(value: str, known: tuple[str, ...] = ()) -> str:
    """
    Remove Arm's superscript footnote markers, which pdftotext glues to the cell.

    'RWd' -> 'RW', '-a' -> '-', '0xFA050000c' -> '0xFA050000'.
    """
    v = value.strip()
    for tok in known:
        if v == tok:
            return v
        if v.startswith(tok) and v[len(tok):].isalpha() and len(v) - len(tok) <= 2:
            return tok
    if re.fullmatch(r"-[a-z]{1,2}", v):
        return "-"
    m = re.fullmatch(r"(0x[0-9A-Fa-f]+)[a-z]{1,2}", v)
    if m:
        return m.group(1)
    return v


def _looks_like_row(line: str) -> bool:
    cells = _split_cells(line)
    if len(cells) < 2:
        return False
    if FURNITURE_RE.search(line):
        return False
    first = cells[0]
    if ADDR_RE.match(first):
        return True
    # Name-keyed tables (no address column).
    return bool(re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]{1,30}(\s*-\s*[A-Za-z_][A-Za-z0-9_]{1,30})?", first))


def extract_tables(path: str, keep_ids: set[str] | None = None) -> tuple[list[Table], list[TableDecision]]:
    """
    Walk the whole document, decide on every table caption, parse the kept ones.

    Returns (tables, decisions). `decisions` covers every caption seen, so the
    coverage report can show what was skipped and why.
    """
    pages = pdf_pages(path)

    tables: dict[str, Table] = {}
    decisions: dict[str, TableDecision] = {}
    section = ""

    for page_no, page in enumerate(pages, start=1):
        lines = page.splitlines()
        i = 0
        while i < len(lines):
            line = lines[i]

            sm = SECTION_RE.match(line)
            if sm and not TABLE_CAPTION_RE.match(line):
                section = f"{sm.group('num')} {sm.group('title').strip()}"

            m = TABLE_CAPTION_RE.match(line)
            if not m:
                i += 1
                continue

            tid = m.group("id")
            caption = m.group("caption").strip()
            # A reference like "Table 3-1 shows ..." is prose, not a caption.
            if re.match(r"^(shows|lists|summari[sz]es|describes|gives|provides|defines|on page)\b", caption, re.I):
                i += 1
                continue

            if keep_ids is not None:
                keep = tid in keep_ids
            else:
                keep = bool(REGISTER_CAPTION_RE.search(caption))
            if tid not in decisions:
                decisions[tid] = TableDecision(
                    table_id=tid, caption=caption, section=section, page=page_no,
                    kept=keep,
                    reason=("register-summary table" if keep
                            else "caption does not describe a register enumeration"),
                )
            if not keep:
                i += 1
                continue

            table = tables.get(tid)
            if table is None:
                table = Table(table_id=tid, caption=caption, section=section, page=page_no)
                tables[tid] = table

            i = _parse_table_body(lines, i + 1, page_no, table)

    for tid, t in tables.items():
        decisions[tid].row_count = len(t.rows)

    return list(tables.values()), list(decisions.values())


def _parse_table_body(lines: list[str], start: int, page_no: int, table: Table) -> int:
    """
    Capture a table body from just after its caption to the end of the page.

    These tables run across page boundaries and Arm repeats the caption with
    '(continued)', so the outer walk re-enters this function once per page and
    appends. Capture is deliberately permissive -- page furniture and stray
    prose are harmless because the profile parsers match rows by pattern, and
    being permissive is what stops a continued page from being silently lost.
    """
    i = start
    while i < len(lines):
        line = lines[i]
        stripped = line.strip()

        if not stripped:
            i += 1
            continue

        m = TABLE_CAPTION_RE.match(line)
        if m and m.group("id") != table.table_id:
            break                       # a different table starts here
        if m:
            i += 1                      # our own '(continued)' caption
            continue

        # A footnote block ('a. Power-on reset only.') ends the table on this page.
        if re.match(r"^\s*[a-z]\.\s+\S", line):
            break

        if FURNITURE_RE.search(line):
            i += 1
            continue

        if not table.columns and _is_header(line):
            table.columns = _split_cells(line)
            i += 1
            continue

        table.raw_lines.append((page_no, line.rstrip()))
        if table.columns and _looks_like_row(line):
            cells = _split_cells(line)
            if len(cells) > len(table.columns):
                head = cells[: len(table.columns) - 1]
                tail = " ".join(cells[len(table.columns) - 1:])
                cells = head + [tail]
            mapping = {table.columns[j]: cells[j] for j in range(min(len(table.columns), len(cells)))}
            table.rows.append(Row(cells=mapping, page=page_no, raw=line.rstrip()))

        i += 1

    return i


def expand_range(addr_cell: str, name_cell: str) -> list[tuple[str, str]]:
    """
    Expand an Arm range row into individual registers.

    '0xE000E100 - 0xE000E11C' / 'NVIC_ISER0 - NVIC_ISER7' becomes eight
    (address, name) pairs. Ranges are how Arm prints register arrays, and each
    element is independent state, so they must not be counted as one entry.
    """
    am = ADDR_RE.match(addr_cell.strip())
    if not am:
        return []
    lo = int(am.group(1), 16)
    hi = int(am.group(2), 16) if am.group(2) else lo

    nm = re.fullmatch(
        r"(?P<base>[A-Za-z_][A-Za-z0-9_]*?)(?P<lo>\d+)\s*-\s*(?P<base2>[A-Za-z_][A-Za-z0-9_]*?)(?P<hi>\d+)",
        name_cell.strip(),
    )
    if not nm or nm.group("base") != nm.group("base2"):
        return [(f"0x{lo:08X}", name_cell.strip())] if lo == hi else []

    first, last = int(nm.group("lo")), int(nm.group("hi"))
    count = last - first + 1
    if count <= 0:
        return []
    span = hi - lo
    stride = span // (count - 1) if count > 1 else 4
    return [
        (f"0x{lo + k * stride:08X}", f"{nm.group('base')}{first + k}")
        for k in range(count)
    ]
