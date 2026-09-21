"""
CI checks for the architectural-state database.

These run against the committed manifests, so they work on a machine that does
not hold the licensed Arm documents. The checks that need the documents
themselves are skipped when the sources are unavailable rather than passing
vacuously -- and 'skipped' is visible in CI output, unlike a silent success.
"""

from __future__ import annotations

import collections
import pathlib
import re
import shutil
import subprocess
import sys

import pytest
import yaml

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

from state.generate import GEN_DIR, coverage, load_manifest          # noqa: E402
from state.model import Classification                                # noqa: E402
from state.sources import SourceError, load_sources                   # noqa: E402

TARGETS = ["armv7m", "cortex_m7_r0p2"]
ARCH_TARGET = "armv7m"
CPU_TARGETS = ["cortex_m7_r0p2"]

# Every classification a source record may carry. Set equality against this is
# what turns "the list looks complete" into "the list is accounted for".
ALL_CLASSES = {c.value for c in Classification}


def manifest(target: str) -> dict:
    return load_manifest(target)


def entries(target: str) -> list[dict]:
    return manifest(target)["entries"]


def sources_available() -> bool:
    try:
        for s in load_sources().values():
            s.verify()
        return True
    except SourceError:
        return False


# --- source integrity -------------------------------------------------------

def test_source_schema_valid():
    """The lock parses, pins a known schema version, and names real documents."""
    sources = load_sources()
    assert sources, "source lock pins nothing"
    for key, s in sources.items():
        assert re.fullmatch(r"[0-9a-f]{64}", s.sha256), f"{key}: malformed SHA-256"
        assert s.document and s.revision, f"{key}: missing document/revision"


@pytest.mark.skipif(not sources_available(), reason="licensed Arm documents not present on this machine")
def test_pinned_sources_match_sha256():
    for s in load_sources().values():
        s.verify()          # raises if the bytes differ from the pin


@pytest.mark.skipif(not sources_available(), reason="licensed Arm documents not present on this machine")
@pytest.mark.parametrize("target", TARGETS)
def test_all_tables_reviewed(target):
    """Every table caption in the document has an explicit keep/skip decision."""
    proc = subprocess.run(
        [sys.executable, str(ROOT / "tools" / "state_tables.py"), "--target", target, "--check"],
        capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr


# --- classification invariants ---------------------------------------------

@pytest.mark.parametrize("target", TARGETS)
def test_source_classification_complete(target):
    """source == emitted u aliases u operations u conditional u exclusions."""
    cov = coverage(target)
    assert cov["unclassified"] == [], f"unclassified source entries: {cov['unclassified']}"
    assert sum(cov["counts"].values()) == cov["total_entries"]
    assert set(cov["counts"]) <= ALL_CLASSES


@pytest.mark.parametrize("target", TARGETS)
def test_no_generic_exclusions(target):
    """Every exclusion and every operation carries a reason."""
    bad = [e["canonical_name"] for e in entries(target)
           if e["classification"] in (Classification.EXPLICIT_EXCLUSION.value,
                                      Classification.NON_STATE_OPERATION.value)
           and not (e.get("reason") or "").strip()]
    assert bad == [], f"exclusions without a reason: {bad[:10]}"


@pytest.mark.parametrize("target", TARGETS)
def test_no_duplicate_state_ids(target):
    ids = [e["state_id"] for e in entries(target)]
    dupes = [i for i, n in collections.Counter(ids).items() if n > 1]
    assert dupes == [], f"duplicate state ids: {dupes[:10]}"


@pytest.mark.parametrize("target", TARGETS)
def test_no_duplicate_encoding_unless_alias(target):
    """Two entries may share an address only if one is a declared alias."""
    seen: dict[tuple[str, str], str] = {}
    clashes = []
    for e in entries(target):
        enc = e.get("encoding")
        if not enc or e.get("encoding_kind") != "absolute":
            continue
        if e["classification"] in (Classification.ALIAS.value,
                                   Classification.EXPLICIT_EXCLUSION.value,
                                   Classification.ARCHITECTURE_DEFINED.value):
            continue
        key = (e.get("component") or "", enc)
        if key in seen:
            clashes.append(f"{enc} shared by {seen[key]} and {e['canonical_name']}")
        seen[key] = e["canonical_name"]
    assert clashes == [], clashes[:10]


@pytest.mark.parametrize("target", TARGETS)
def test_alias_targets_exist(target):
    names = {e["canonical_name"] for e in entries(target)}
    missing = [e["canonical_name"] for e in entries(target)
               if e["classification"] == Classification.ALIAS.value
               and e.get("alias") and e["alias"]["target"] not in names]
    assert missing == [], f"aliases pointing at unknown registers: {missing}"


@pytest.mark.parametrize("target", TARGETS)
def test_feature_references_exist(target):
    """Every feature_requirement resolves to a declared feature."""
    fp = ROOT / "spec" / "arm" / "armv7m.features.yaml"
    declared = set((yaml.safe_load(fp.read_text()) or {}).get("features", {}))
    used = {e["feature_requirement"] for e in entries(target) if e.get("feature_requirement")}
    assert used <= declared, f"undeclared features: {sorted(used - declared)}"


# --- generated artifacts ----------------------------------------------------

@pytest.mark.parametrize("target", TARGETS)
def test_generated_def_matches_manifest(target):
    """The .def table is exactly the manifest's emitting entries -- no drift."""
    d = GEN_DIR / f"{target}.def"
    assert d.is_file(), f"{d} has not been generated"
    text = d.read_text()
    emitted = {e["canonical_name"] for e in entries(target)
               if e["classification"] in (Classification.EMITTED_STATE.value,
                                          Classification.CONDITIONAL_STATE.value)}
    in_def = set(re.findall(r'^HW_STATE\([A-Z0-9_]+,\s*"([^"]+)"', text, re.M))
    assert in_def == emitted, (
        f"generated .def drifted from the manifest: "
        f"only-in-def={sorted(in_def - emitted)[:5]} only-in-manifest={sorted(emitted - in_def)[:5]}")


@pytest.mark.parametrize("target", TARGETS)
def test_snapshot_coverage(target):
    """Snapshot-required state ids == the state ids marked snapshot in the .def."""
    required = {e["canonical_name"] for e in entries(target) if e.get("snapshot")}
    text = (GEN_DIR / f"{target}.def").read_text()
    in_def = set(re.findall(r'^HW_STATE\([A-Z0-9_]+,\s*"([^"]+)".*,\s*1,\s*"[^"]*"\)\s*$', text, re.M))
    assert in_def == required, (
        f"snapshot set mismatch: missing={sorted(required - in_def)[:5]} "
        f"extra={sorted(in_def - required)[:5]}")


# --- CPU overlay ------------------------------------------------------------

@pytest.mark.parametrize("target", CPU_TARGETS)
def test_cpu_overlay_no_duplicate_state(target):
    """
    A CPU overlay must not re-emit state the architecture already owns; such
    rows have to be classified as architecture-defined instead.
    """
    arch = {e["canonical_name"] for e in entries(ARCH_TARGET)
            if e["classification"] in (Classification.EMITTED_STATE.value,
                                       Classification.CONDITIONAL_STATE.value)
            and e["namespace"] == "arch"}
    dupes = [e["canonical_name"] for e in entries(target)
             if e["namespace"] == "arch"
             and e["classification"] == Classification.EMITTED_STATE.value
             and e["canonical_name"] in arch]
    assert dupes == [], f"overlay re-emits architectural state: {dupes[:10]}"


@pytest.mark.parametrize("target", TARGETS)
def test_every_entry_has_provenance(target):
    """Nothing enters the database without a traceable source."""
    bad = []
    for e in entries(target):
        p = e.get("provenance") or {}
        if not p.get("document") or not p.get("revision") or not (p.get("table") or p.get("section")):
            bad.append(e["canonical_name"])
    assert bad == [], f"entries without provenance: {bad[:10]}"


@pytest.mark.skipif(not sources_available(), reason="licensed Arm documents not present on this machine")
def test_cpu_manifest_complete():
    """The manual's own register index must be fully covered by the manifest."""
    from state.importer import import_target
    _, report = import_target(ARCH_TARGET)
    for tid, cc in report.crosscheck.items():
        assert not cc["missing"], f"{tid}: index entries absent from the manifest: {cc['missing']}"


# --- independent encoding oracle -------------------------------------------

@pytest.mark.skipif(shutil.which("llvm-mc") is None, reason="llvm-mc not installed")
def test_special_register_encoder_against_assembler():
    """SYSm values read from Table B5-1 must match what an assembler emits."""
    proc = subprocess.run(
        [sys.executable, str(ROOT / "tools" / "state_encoding_check.py"), "--target", ARCH_TARGET],
        capture_output=True, text=True)
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "Mismatches:         0" in proc.stdout, proc.stdout
