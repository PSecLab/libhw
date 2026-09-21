"""
Generate .def macro tables and coverage artifacts from normalized manifests.

The .def files are an *output*. They are never edited by hand: the manifest, and
behind it the pinned manual, is the database.
"""

from __future__ import annotations

import json
import pathlib
from typing import Any

import yaml

from .model import Classification
from .sources import REPO_ROOT

GEN_DIR = REPO_ROOT / "generated"
COVERAGE_DIR = REPO_ROOT / "build" / "state-coverage"

# Classifications that become a state descriptor in generated code.
EMITTING = {Classification.EMITTED_STATE.value, Classification.CONDITIONAL_STATE.value}


def load_manifest(target: str) -> dict[str, Any]:
    p = REPO_ROOT / "spec" / "arm" / f"{target}.yaml"
    if not p.is_file():
        raise FileNotFoundError(f"No manifest for {target} at {p}. Run tools/state_import.py.")
    return yaml.safe_load(p.read_text())


def _c_name(e: dict[str, Any]) -> str:
    base = e["canonical_name"]
    comp = e.get("component") or ""
    if comp and not base.upper().startswith(comp.upper()):
        base = f"{comp}_{base}"
    return "".join(ch if ch.isalnum() or ch == "_" else "_" for ch in base).upper()


def generate_def(target: str) -> pathlib.Path:
    doc = load_manifest(target)
    GEN_DIR.mkdir(parents=True, exist_ok=True)
    out = GEN_DIR / f"{target}.def"

    src = doc["source"]
    lines = [
        "/*",
        f" * Generated from {src['document']} {src['revision']} by tools/state_gen.py.",
        " * Do not edit. Edit the manifest's source rules and re-import instead.",
        " *",
        " * HW_STATE(id, name, ns, access, encoding, enc_kind, width, readable, writable, snapshot, feature)",
        " * HW_ALIAS(id, target, bit_offset, bit_width)",
        " * HW_OPERATION(id, name, encoding, reason)",
        " * HW_DBGREG(id, name, regsel, lsb, width)   -- DCRSR/DCRDR access path",
        " */",
        "",
        "#ifndef HW_STATE",
        "#define HW_STATE(id, name, ns, access, enc, ekind, width, rd, wr, snap, feat)",
        "#endif",
        "#ifndef HW_ALIAS",
        "#define HW_ALIAS(id, target, off, width)",
        "#endif",
        "#ifndef HW_OPERATION",
        "#define HW_OPERATION(id, name, enc, reason)",
        "#endif",
        "#ifndef HW_DBGREG",
        "#define HW_DBGREG(id, name, regsel, lsb, width)",
        "#endif",
        "",
    ]

    for e in doc["entries"]:
        cls = e["classification"]
        cid = _c_name(e)
        enc = e.get("encoding") or ""
        if cls in EMITTING:
            lines.append(
                f'HW_STATE({cid}, "{e["canonical_name"]}", HW_NS_{e["namespace"].upper()}, '
                f'HW_ACC_{(e.get("access") or "memory_mapped").upper()}, "{enc}", '
                f'HW_ENC_{(e.get("encoding_kind") or "absolute").upper()}, '
                f'{e.get("width") or 32}, {int(bool(e["readable"]))}, {int(bool(e["writable"]))}, '
                f'{int(bool(e["snapshot"]))}, "{e.get("feature_requirement") or ""}")'
            )
        elif cls == Classification.ALIAS.value and e.get("alias"):
            a = e["alias"]
            lines.append(f'HW_ALIAS({cid}, {a["target"]}, {a["bit_offset"]}, {a["bit_width"]})')
        elif cls == Classification.NON_STATE_OPERATION.value:
            reason = (e.get("reason") or "").replace('"', "'")[:90]
            lines.append(f'HW_OPERATION({cid}, "{e["canonical_name"]}", "{enc}", "{reason}")')

    for e in doc["entries"]:
        if e.get("debug_regsel") is None:
            continue
        if e["classification"] not in EMITTING:
            continue
        lines.append(
            f'HW_DBGREG({_c_name(e)}, "{e["canonical_name"]}", {e["debug_regsel"]}, '
            f'{e.get("debug_lsb", 0)}, {e.get("debug_width", 32)})')

    lines += ["", "#undef HW_STATE", "#undef HW_ALIAS", "#undef HW_OPERATION",
              "#undef HW_DBGREG", ""]
    out.write_text("\n".join(lines))
    return out


def coverage(target: str) -> dict[str, Any]:
    doc = load_manifest(target)
    counts: dict[str, int] = {}
    for e in doc["entries"]:
        counts[e["classification"]] = counts.get(e["classification"], 0) + 1

    unclassified = [e["canonical_name"] for e in doc["entries"] if not e.get("classification")]
    missing_reason = [
        e["canonical_name"] for e in doc["entries"]
        if e["classification"] in (Classification.EXPLICIT_EXCLUSION.value,
                                   Classification.NON_STATE_OPERATION.value)
        and not (e.get("reason") or "").strip()
    ]
    return {
        "target": target,
        "architecture": doc["architecture"],
        "source": doc["source"],
        "total_entries": len(doc["entries"]),
        "counts": counts,
        "unclassified": unclassified,
        "exclusions_without_reason": missing_reason,
        "snapshot_state": sum(1 for e in doc["entries"] if e.get("snapshot")),
        "entries": [
            {
                "state_id": e["state_id"], "name": e["canonical_name"],
                "namespace": e["namespace"], "classification": e["classification"],
                "encoding": e.get("encoding"), "reason": e.get("reason"),
                "feature_requirement": e.get("feature_requirement"),
                "provenance": e["provenance"],
            }
            for e in doc["entries"]
        ],
    }


def write_coverage(target: str) -> pathlib.Path:
    COVERAGE_DIR.mkdir(parents=True, exist_ok=True)
    out = COVERAGE_DIR / f"{target}.json"
    out.write_text(json.dumps(coverage(target), indent=2))
    return out


def format_report(cov: dict[str, Any]) -> str:
    c = cov["counts"]
    src = cov["source"]
    w = 34
    out = [
        f"Architecture: {cov['architecture']}",
        f"Source:       {src['document']} {src['revision']}",
        f"{'Source records accounted for:':<{w}} {cov['total_entries']}",
        "Classified:",
        f"{'    state':<{w}} {c.get('emitted_state', 0)}",
        f"{'    conditional state':<{w}} {c.get('conditional_state', 0)}",
        f"{'    aliases':<{w}} {c.get('alias', 0)}",
        f"{'    architecture-defined duplicates':<{w}} {c.get('architecture_defined', 0)}",
        f"{'    non-state operations':<{w}} {c.get('non_state_operation', 0)}",
        f"{'    explicit exclusions':<{w}} {c.get('explicit_exclusion', 0)}",
        f"{'Unclassified:':<{w}} {len(cov['unclassified'])}",
        f"{'Exclusions without a reason:':<{w}} {len(cov['exclusions_without_reason'])}",
        f"{'In whole-CPU snapshot:':<{w}} {cov['snapshot_state']}",
    ]
    return "\n".join(out)
