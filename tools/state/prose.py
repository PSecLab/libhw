"""
Systematic extraction of state that a manual defines in prose.

Register-summary tables and a document's own index are not sufficient. The
ARMv7-M FP register file is the proof: A2.5.2 defines S0-S31 and D0-D15 in
running text, no summary table lists them, and the memory-mapped indexes in
Appendix D8 cannot catch them either.

This scanner finds prose that names state and emits *candidates*. It never adds
anything to the manifest on its own -- every candidate must be classified by
hand in spec/rules/<target>.prose_candidates.yaml as state, alias, field, block
name, an array family already covered, or a false positive. Unclassified
candidates fail CI.
"""

from __future__ import annotations

import dataclasses
import hashlib
import re
from typing import Any

from .armpdf import pdf_pages

# --- number words Arm uses when stating the size of a register file ---------
NUMBER_WORDS = {
    "two": 2, "three": 3, "four": 4, "five": 5, "six": 6, "seven": 7, "eight": 8,
    "nine": 9, "ten": 10, "twelve": 12, "sixteen": 16, "thirty-two": 32,
    "sixty-four": 64, "thirty two": 32, "sixty four": 64,
}

SECTION_RE = re.compile(r"^(?P<sec>[A-Z]?\d+(?:\.\d+){1,3})\s+(?P<title>[A-Z]\S.{2,70})$")

# A register name: uppercase stem, optional digits, optional _suffix.
_REG = r"[A-Z][A-Z0-9]*(?:_[A-Z0-9]+)*"

PATTERNS: list[tuple[str, re.Pattern]] = [
    # S0-S31, R0-R15, D0-D15, Q0-Q7, NVIC_IPR0-NVIC_IPR123
    ("range", re.compile(rf"\b(?P<base>{_REG}?)(?P<lo>\d+)\s*[-–—]\s*(?P<base2>{_REG}?)(?P<hi>\d+)\b")),
    # 'registers R0 through R12'
    ("through", re.compile(rf"\b(?P<base>{_REG}?)(?P<lo>\d+)\s+(?:through|to)\s+(?P<base2>{_REG}?)(?P<hi>\d+)\b")),
    # A statement of a register file's size: 'Thirty-two 32-bit
    # single-precision registers, S0-S31', 'a bank of 8 registers'. A bare
    # 'two registers' in running prose is not a definition and is excluded by
    # requiring a qualifier that marks a file, or a naming of its members.
    ("count", re.compile(
        r"\b(?P<count>thirty-two|sixty-four|sixteen|twelve|eight|seven|six|five|four|three|two|\d{1,3})\b"
        r"(?P<mid>[\w\-, ]{0,44}?)"
        r"\b(?:single-precision|double-precision|general-purpose|special-purpose|banked|"
        r"extension|core|doubleword|word|comparator|stimulus)\b"
        r"[\w\-, ]{0,24}?\bregisters?\b", re.I)),
    # 'a bank of 8 registers', 'register file', 'banked registers'
    ("phrase", re.compile(r"\b(?:bank of \w+ registers?|register file|register bank|"
                          r"banked registers?|extension registers?|shadow registers?)\b", re.I)),
    # DWT_COMP<n>, FP_COMPn, ITM_STIMx
    ("family", re.compile(rf"\b(?P<name>{_REG})(?:<[nNxX]>|(?<=[0-9A-Z_])[nx])\b")),
    # Special registers named as the operand of a system instruction.
    ("instr_ref", re.compile(
        rf"\b(?:MRS|MSR|VMRS|VMSR)\b(?!\s+on\s+page)[^.\n]{{0,60}}?\b(?P<name>{_REG})\b")),
    # 'R0, R1, R2, R3' -- three or more comma-separated register names.
    ("comma_set", re.compile(rf"\b(?P<first>{_REG}\d*)(?:\s*,\s*{_REG}\d*){{2,}}")),
]

# Words that look like register names but are not.
STOPWORDS = {
    "THE", "AND", "FOR", "SEE", "ALL", "ARM", "THIS", "EACH", "ANY", "NOT", "BIT",
    "USE", "ONE", "TWO", "NOTE", "WHEN", "IF", "IS", "ARE", "TO", "IN", "ON", "OF",
    "RAZ", "WI", "RO", "RW", "WO", "UNKNOWN", "UNPREDICTABLE", "IMPLEMENTATION",
    "DEFINED", "RESERVED", "SBZ", "SBO", "UNP", "PE", "PPB", "SCS", "AHB", "APB",
    "DAP", "MVA", "PMSA", "ARMV", "ISA", "ID", "LSB", "MSB", "CPU", "FIFO", "TRM",
}

MAX_CONTEXT = 150


@dataclasses.dataclass
class ProseCandidate:
    cid: str
    kind: str
    raw: str
    names: list[str]
    section: str
    page: int
    context: str

    def to_dict(self) -> dict[str, Any]:
        return dataclasses.asdict(self)


def _expand(base: str, lo: int, hi: int, limit: int = 512) -> list[str]:
    if not base or hi < lo or (hi - lo) > limit:
        return []
    return [f"{base}{i}" for i in range(lo, hi + 1)]


def _cid(kind: str, raw: str, section: str) -> str:
    h = hashlib.sha1(f"{kind}|{raw}|{section}".encode()).hexdigest()[:10]
    return f"{kind}:{h}"


def scan(path: str) -> list[ProseCandidate]:
    """Walk the document and emit every prose construct that names state."""
    pages = pdf_pages(path)
    out: dict[str, ProseCandidate] = {}
    section = ""

    for page_no, page in enumerate(pages, start=1):
        lines = page.splitlines()
        # Join wrapped lines so a construct split across a line break is seen.
        for idx, line in enumerate(lines):
            sm = SECTION_RE.match(line.strip()) if line.strip() else None
            if sm:
                section = f"{sm.group('sec')} {sm.group('title').strip()}"

            window = " ".join(l.strip() for l in lines[idx:idx + 2])
            if not window.strip():
                continue

            for kind, rx in PATTERNS:
                for m in rx.finditer(window):
                    names: list[str] = []

                    if kind in ("range", "through"):
                        base = (m.group("base") or m.group("base2") or "").strip("_")
                        if not base or base in STOPWORDS:
                            continue
                        b2 = (m.group("base2") or "").strip("_")
                        if b2 and b2 != base:
                            continue
                        names = _expand(base, int(m.group("lo")), int(m.group("hi")))
                        if not names:
                            continue
                    elif kind == "count":
                        c = m.group("count").lower()
                        n = NUMBER_WORDS.get(c, int(c) if c.isdigit() else 0)
                        if n < 2 or n > 512:
                            continue
                    elif kind in ("family", "instr_ref"):
                        nm = m.group("name")
                        if nm in STOPWORDS or len(nm) < 2:
                            continue
                        names = [nm]
                    elif kind == "comma_set":
                        toks = [t.strip() for t in re.split(r"\s*,\s*", m.group(0))]
                        names = [t for t in toks if re.fullmatch(rf"{_REG}\d*", t)
                                 and t not in STOPWORDS]
                        if len(names) < 3:
                            continue

                    raw = m.group(0).strip()
                    if len(raw) > 90:
                        continue
                    start = max(0, m.start() - 60)
                    context = window[start:m.end() + 60].strip()

                    cid = _cid(kind, raw, section)
                    if cid in out:
                        continue
                    out[cid] = ProseCandidate(
                        cid=cid, kind=kind, raw=raw, names=sorted(set(names)),
                        section=section, page=page_no, context=context[:MAX_CONTEXT])

    return sorted(out.values(), key=lambda c: (c.kind, c.section, c.raw))


# --- vocabularies derived from the document itself -------------------------
#
# Deciding that a token is an instruction mnemonic or a bit-field name should
# not rest on a hand-written list. Both are already stated by the manual: the
# instruction chapters head each description with its mnemonics, and the
# 'bit assignments' tables name the fields of each register.

# 'A7.7.42  LDMDB, LDMEA' and 'A7.7.40  LDC, LDC2 (immediate)': take the
# mnemonic list before any parenthetical qualifier.
_MNEMONIC_HEAD_RE = re.compile(
    r"^(?P<sec>A[4-7](?:\.\d+){1,3})\s+(?P<title>[A-Z][A-Z0-9]*(?:\s*,\s*[A-Z][A-Z0-9]*)*)"
    r"(?:\s*\(|\s*$)")


def instruction_mnemonics(path: str) -> set[str]:
    """Mnemonics, taken from the instruction chapters' own section headings."""
    out: set[str] = set()
    for page in pdf_pages(path):
        for line in page.splitlines():
            m = _MNEMONIC_HEAD_RE.match(line.strip())
            if not m:
                continue
            for tok in re.split(r"\s*,\s*", m.group("title")):
                tok = tok.strip()
                if re.fullmatch(r"[A-Z][A-Z0-9]{1,9}", tok):
                    out.add(tok)
    return out


_FIELD_TABLE_RE = re.compile(
    r"^\s*Table\s+[A-Z]?\d+-\d+\s+(?P<who>[A-Z][A-Za-z0-9_/ ]{0,40}?)\s+bit\s+(?:assignments|definitions)",
    re.I)
# 'PRIMASK, bit[0]' and 'N, bit[31]': single-letter flags count as fields too.
_FIELD_ROW_RE = re.compile(r"^\s*(?P<name>[A-Z][A-Za-z0-9_]{0,15})\s*(?:,|\s)\s*bits?\s*\[")


def field_names(path: str) -> set[str]:
    """
    Field names, taken from the 'bit assignments' tables and from field
    descriptions of the form 'NAME, bits[x:y]'.
    """
    out: set[str] = set()
    in_field_table = False
    for page in pdf_pages(path):
        for line in page.splitlines():
            if _FIELD_TABLE_RE.match(line):
                in_field_table = True
                continue
            if line.strip().startswith("Table "):
                in_field_table = False

            m = _FIELD_ROW_RE.match(line)
            if m:
                out.add(m.group("name"))
                continue
            if in_field_table:
                cells = [c.strip() for c in re.split(r"\s{2,}", line.strip()) if c.strip()]
                if cells and re.fullmatch(r"[A-Z][A-Za-z0-9_]{0,15}", cells[0]):
                    out.add(cells[0])
    return out


def front_matter_pages(path: str) -> int:
    """Last page before the first numbered chapter; everything before is preamble."""
    for page_no, page in enumerate(pdf_pages(path), start=1):
        if re.search(r"^\s*(?:Chapter\s+)?A1(?:\.1)?\s+\S", page, re.M):
            return page_no - 1
    return 0
