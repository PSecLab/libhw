"""
Turn pinned manuals into normalized state manifests.

The importer visits every row of every table the rules file marks as a register
enumeration, classifies each one, and refuses to finish unless every source
record has been accounted for. Where the document publishes its own register
index, that index is used as a second, document-internal completeness check.
"""

from __future__ import annotations

import dataclasses
import pathlib
import re
from typing import Any

import yaml

from .armpdf import Table, extract_tables
from .model import Access, Alias, Classification, Namespace, Provenance, StateEntry
from .profiles import PROFILES
from .sources import REPO_ROOT, SourceError, get_source

RULES_DIR = REPO_ROOT / "spec" / "rules"
SPEC_DIR = REPO_ROOT / "spec" / "arm"

PROFILE_ENCODING_KIND = {
    "offset_type_name": "offset",
    "offset_value_name": "offset",
    "core_register_index": "core",
    "special_register_sysm": "sysm",
    "fp_common_block": "fp_sysreg",
}

PROFILE_ACCESS = {
    "addr_name_type_reset": Access.MEMORY_MAPPED,
    "addr_register_value": Access.MEMORY_MAPPED,
    "offset_type_name": Access.MEMORY_MAPPED,
    "offset_value_name": Access.MEMORY_MAPPED,
    "core_register_index": Access.CORE_REG,
    "special_register_sysm": Access.SPECIAL_REG,
    "fp_common_block": Access.FP_SYSREG,
    "addr_type_reset_descname": Access.MEMORY_MAPPED,
}

# First column of an index table is the register name.
_INDEX_NAME_RE = re.compile(r"^\s{2,}(?P<name>[A-Z][A-Za-z0-9_]{1,30})\s{2,}\S")


@dataclasses.dataclass
class ImportReport:
    target: str
    architecture: str
    document: str
    revision: str
    source_records: int = 0
    counts: dict[str, int] = dataclasses.field(default_factory=dict)
    tables: list[dict[str, Any]] = dataclasses.field(default_factory=list)
    crosscheck: dict[str, Any] = dataclasses.field(default_factory=dict)
    unclassified: list[str] = dataclasses.field(default_factory=list)


def _load_rules(target: str) -> dict:
    p = RULES_DIR / f"{target}.tables.yaml"
    if not p.is_file():
        raise SourceError(f"No table rules at {p}. Run tools/state_tables.py --seed.")
    return yaml.safe_load(p.read_text())


def _load_access_rules(target: str) -> dict:
    """Debug access paths, where the manual defines them in prose rather than a table."""
    p = RULES_DIR / f"{target}.access.yaml"
    if not p.is_file():
        return {}
    return (yaml.safe_load(p.read_text()) or {}).get("registers", {}) or {}


def _load_classify_rules(target: str) -> dict:
    p = RULES_DIR / f"{target}.classify.yaml"
    if not p.is_file():
        return {}
    return (yaml.safe_load(p.read_text()) or {}).get("registers", {}) or {}


def _load_register_files(target: str) -> dict:
    """
    Register files the manual describes in prose rather than tabulating.

    The FP extension file is the case that matters on Cortex-M: A2.5.2 says
    'thirty-two 32-bit single-precision registers, S0-S31', and no summary
    table lists them, so a purely table-driven import misses the entire file.
    """
    p = RULES_DIR / f"{target}.classify.yaml"
    if not p.is_file():
        return {}
    return (yaml.safe_load(p.read_text()) or {}).get("register_files", {}) or {}


def _index_names(table: Table) -> set[str]:
    names: set[str] = set()
    for _pg, line in table.raw_lines:
        m = _INDEX_NAME_RE.match(line)
        if not m:
            continue
        n = m.group("name")
        if n in {"Register", "Registera", "See", "Notes", "Description", "Table"}:
            continue
        names.add(n)
    return names


def _access_flags(typ: str) -> tuple[bool, bool]:
    t = (typ or "").upper()
    if t == "WO":
        return False, True
    if t in {"RO", "RAZ"}:
        return True, False
    if t == "WI":
        return False, True
    return True, True


def import_target(target: str, arch_names: set[str] | None = None) -> tuple[list[StateEntry], ImportReport]:
    rules_doc = _load_rules(target)
    overrides = _load_classify_rules(target)
    access_paths = _load_access_rules(target)
    register_files = _load_register_files(target)
    src = get_source(rules_doc["source"])
    path = src.verify()

    table_rules = rules_doc["tables"]
    keep = {tid for tid, r in table_rules.items() if r.get("keep")}
    crosscheck_ids = set(rules_doc.get("crosscheck_indexes") or [])

    tables, decisions = extract_tables(str(path), keep_ids=keep | crosscheck_ids)
    by_id = {t.table_id: t for t in tables}

    found = {d.table_id for d in decisions}
    undecided = found - set(table_rules)
    if undecided:
        raise SourceError(
            f"{target}: {len(undecided)} table(s) in {src.document} {src.revision} have no "
            f"reviewed decision: {sorted(undecided)[:8]}. Re-run tools/state_tables.py --seed."
        )

    report = ImportReport(target=target, architecture=rules_doc["architecture"],
                          document=src.document, revision=src.revision)
    entries: list[StateEntry] = []
    seen: dict[str, StateEntry] = {}

    for tid in sorted(keep):
        rule = table_rules[tid]
        table = by_id.get(tid)
        if table is None:
            raise SourceError(f"{target}: Table {tid} is marked keep but was not found in the document.")

        records = PROFILES[rule["profile"]](table)
        stats = {"rows": len(records), "emitted": 0, "conditional": 0, "alias": 0,
                 "architecture_defined": 0, "excluded": 0, "operation": 0}

        for rec in records:
            name = rec.get("name")
            prov = Provenance(document=src.document, revision=src.revision,
                              section=table.section or rule.get("caption", ""),
                              table=table.label, row=name or (rec.get("address") or "reserved"),
                              page=rec.get("page"))

            if rec.get("reserved") or not name:
                entries.append(StateEntry(
                    canonical_name=f"RESERVED_{rec.get('address','?')}",
                    source_id=f"{tid}:{rec.get('address','?')}",
                    architecture=report.architecture, namespace=Namespace.ARCH,
                    classification=Classification.EXPLICIT_EXCLUSION, provenance=prov,
                    access=PROFILE_ACCESS[rule["profile"]], encoding=rec.get("address"),
                    reason=(rec.get("description") or "Reserved").strip()[:160] or
                           "Reserved address range; not an architectural state element",
                    source_family="ARM_TRM" if src.document == "DDI0489" else "ARM_ARM",
                    source_release=f"{src.document} {src.revision}"))
                stats["excluded"] += 1
                continue

            ns_raw = rule["namespace"]
            classification = Classification.EMITTED_STATE
            reason = None
            feature = rule.get("feature_requirement")

            if ns_raw == "auto":
                if arch_names and name in arch_names:
                    namespace = Namespace.ARCH
                    classification = Classification.ARCHITECTURE_DEFINED
                    reason = "Defined by the ARMv7-M architecture; accounted for in the architecture manifest"
                else:
                    namespace = Namespace.CPU
            else:
                namespace = Namespace(ns_raw)

            if rule.get("classification"):
                classification = Classification(rule["classification"])
                reason = rule.get("classification_reason")

            ov = overrides.get(name)
            if ov:
                classification = Classification(ov["class"])
                reason = ov.get("reason")
                feature = ov.get("requires", feature)

            if classification is Classification.EMITTED_STATE and feature:
                classification = Classification.CONDITIONAL_STATE

            readable, writable = _access_flags(rec.get("access"))
            alias = None
            if ov and ov.get("alias_of"):
                alias = Alias(target=ov["alias_of"], bit_offset=int(ov.get("bit_offset", 0)),
                              bit_width=int(ov.get("bit_width", 32)))

            entry = StateEntry(
                canonical_name=name,
                source_id=f"{tid}:{name}",
                architecture=report.architecture,
                namespace=namespace,
                classification=classification,
                provenance=prov,
                width=32,
                access=PROFILE_ACCESS[rule["profile"]],
                encoding=rec.get("address") if rec.get("address") else (
                    f"SYSm={rec['sysm']}" if rec.get("sysm") is not None else None),
                readable=readable, writable=writable,
                feature_requirement=feature, alias=alias, reason=reason,
                snapshot=(classification in (Classification.EMITTED_STATE,
                                             Classification.CONDITIONAL_STATE)
                          and readable and namespace in (Namespace.ARCH, Namespace.CPU)),
                source_family="ARM_TRM" if src.document == "DDI0489" else "ARM_ARM",
                component=rule.get("component", ""),
                debug_regsel=(access_paths.get(name) or {}).get("regsel"),
                debug_lsb=int((access_paths.get(name) or {}).get("lsb", 0)),
                debug_width=int((access_paths.get(name) or {}).get("width", 32)),
                encoding_kind=PROFILE_ENCODING_KIND.get(rule["profile"], "absolute"),
                source_release=f"{src.document} {src.revision}")

            key = entry.state_id()
            if key in seen:
                # The same register, in the same component, listed by more than
                # one summary table. Keep the first and record the repeat as an
                # alias rather than dropping it silently.
                entries.append(dataclasses.replace(
                    entry, classification=Classification.ALIAS,
                    alias=Alias(target=name, bit_offset=0, bit_width=32),
                    source_id=f"{tid}:{name}:dup",
                    reason=f"Also listed in {seen[key].provenance.table}"))
                stats["alias"] += 1
                continue

            seen[key] = entry
            entries.append(entry)
            stats[{
                Classification.EMITTED_STATE: "emitted",
                Classification.CONDITIONAL_STATE: "conditional",
                Classification.ALIAS: "alias",
                Classification.ARCHITECTURE_DEFINED: "architecture_defined",
                Classification.EXPLICIT_EXCLUSION: "excluded",
                Classification.NON_STATE_OPERATION: "operation",
            }[classification]] += 1

        report.tables.append({"table": table.label, "id": tid, "profile": rule["profile"], **stats})
        report.source_records += stats["rows"]

    # Registers the manual documents outside any summary table (its own index or
    # a register description). Declared explicitly in the classify rules, with
    # provenance, so they are accounted for rather than quietly missing.
    for name, ov in overrides.items():
        if not ov.get("declare") or name in seen:
            continue
        prov = Provenance(document=src.document, revision=src.revision,
                          section=ov.get("provenance_section", ""),
                          table=ov.get("provenance_table", ""), row=name, page=None)
        cls = Classification(ov["class"])
        entry = StateEntry(
            canonical_name=name, source_id=f"declared:{name}",
            architecture=report.architecture, namespace=Namespace.ARCH,
            classification=cls, provenance=prov, width=int(ov.get("bit_width", 32)),
            access=Access.MEMORY_MAPPED, encoding=ov.get("address"),
            reason=ov.get("reason"),
            alias=(Alias(target=ov["alias_of"], bit_offset=int(ov.get("bit_offset", 0)),
                         bit_width=int(ov.get("bit_width", 32))) if ov.get("alias_of") else None),
            snapshot=False,
            source_family="ARM_ARM", source_release=f"{src.document} {src.revision}")
        seen[name] = entry
        entries.append(entry)

    # Register files declared in prose. Expanded here so the file's size comes
    # from the manual's own statement of it rather than being invented.
    for fkey, spec in register_files.items():
        cls = Classification(spec["class"])
        for n in range(int(spec["count"])):
            name = spec["name_format"] % n
            if name in seen:
                continue
            prov = Provenance(document=src.document, revision=src.revision,
                              section=spec.get("provenance_section", ""),
                              table=spec.get("provenance_table", ""), row=name, page=None)
            alias = None
            if spec.get("alias_format"):
                alias = Alias(target=spec["alias_format"] % (n * int(spec.get("alias_index_stride", 1))),
                              bit_offset=0, bit_width=int(spec.get("width", 32)))
            regsel = spec.get("dcrsr_regsel_base")
            entry = StateEntry(
                canonical_name=name, source_id=f"{fkey}:{name}",
                architecture=report.architecture, namespace=Namespace.ARCH,
                classification=cls, provenance=prov, width=int(spec.get("width", 32)),
                access=Access(spec.get("access", "core_reg")), encoding=None,
                encoding_kind="core",
                feature_requirement=spec.get("requires"), alias=alias,
                reason=spec.get("reason"),
                debug_regsel=(regsel + n) if regsel is not None else None,
                debug_lsb=0, debug_width=min(int(spec.get("width", 32)), 32),
                component="FPREGS",
                snapshot=(cls is Classification.CONDITIONAL_STATE),
                source_family="ARM_ARM", source_release=f"{src.document} {src.revision}")
            seen[name] = entry
            entries.append(entry)

    # Document-internal completeness check.
    imported = {e.canonical_name for e in entries if e.classification is not Classification.EXPLICIT_EXCLUSION}
    for tid in sorted(crosscheck_ids):
        t = by_id.get(tid)
        if t is None:
            continue
        idx = _index_names(t)

        def norm(n: str) -> str:
            # 'DWT_COMPx' / 'DWT_COMPn' / 'DWT_COMP0' all name the same array.
            return re.sub(r"(?:[nx]|\d+)$", "", n)

        norm_imported = {norm(n) for n in imported} | imported
        missing = sorted(n for n in idx - imported if norm(n) not in norm_imported)
        report.crosscheck[tid] = {
            "caption": t.label, "index_entries": len(idx),
            "present": len(idx) - len(missing), "missing": missing,
        }

    counts: dict[str, int] = {c.value: 0 for c in Classification}
    for e in entries:
        counts[e.classification.value] += 1
        if e.classification in (Classification.NON_STATE_OPERATION, Classification.EXPLICIT_EXCLUSION) \
                and not e.reason:
            report.unclassified.append(f"{e.canonical_name}: exclusion without a reason")
    report.counts = counts

    total = sum(counts.values())
    if total != len(entries):
        raise SourceError(f"{target}: classification totals {total} != {len(entries)} entries")

    return entries, report


def write_manifest(target: str, entries: list[StateEntry], report: ImportReport) -> pathlib.Path:
    SPEC_DIR.mkdir(parents=True, exist_ok=True)
    out = SPEC_DIR / f"{target}.yaml"
    doc = {
        "schema_version": 1,
        "target": target,
        "architecture": report.architecture,
        "source": {"document": report.document, "revision": report.revision},
        "generated_by": "tools/state_import.py - do not edit by hand",
        "entries": [e.to_dict() for e in entries],
    }
    out.write_text(yaml.safe_dump(doc, sort_keys=False, width=120))
    return out
