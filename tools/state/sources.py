"""
Pinned authoritative sources.

A completeness claim is only meaningful against an exact version of an exact
document, so every source is recorded with its identity (document, revision,
SHA-256, size) in spec/sources.lock.yaml. The importers refuse to run against
anything that does not match the lock.

The documents themselves are licensed Arm material and are not redistributed
with this repository; the lock pins their identity so that any machine holding
a legitimate copy can verify it is working from the same bytes.
"""

from __future__ import annotations

import dataclasses
import hashlib
import os
import pathlib
from typing import Any

import yaml

# Lock formats this code has actually been reviewed against. Refusing an
# unknown version is better than silently misreading a newer layout.
SUPPORTED_LOCK_SCHEMA_VERSIONS = {1}

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_LOCK = REPO_ROOT / "spec" / "sources.lock.yaml"

# Lets CI point at its own copies without editing the lock.
SOURCE_DIR_ENV = "HW_SPEC_SOURCE_DIR"


class SourceError(RuntimeError):
    """Raised for any problem that makes a completeness claim untrustworthy."""


@dataclasses.dataclass
class Source:
    key: str
    kind: str
    document: str
    revision: str
    title: str
    architecture: str
    sha256: str
    path: str
    bytes: int | None = None
    pages: int | None = None
    product_revision: str | None = None
    acquired: str | None = None
    redistributable: bool = False
    url: str | None = None
    notes: str | None = None

    def resolve(self) -> pathlib.Path:
        """Find the document on this machine."""
        candidates: list[pathlib.Path] = []

        override = os.environ.get(SOURCE_DIR_ENV)
        if override:
            candidates.append(pathlib.Path(override).expanduser() / pathlib.Path(self.path).name)

        p = pathlib.Path(self.path).expanduser()
        candidates.append(p if p.is_absolute() else REPO_ROOT / p)

        for c in candidates:
            if c.is_file():
                return c

        raise SourceError(
            f"Source '{self.key}' ({self.document} {self.revision}) not found.\n"
            f"  Looked in: {', '.join(str(c) for c in candidates)}\n"
            f"  This is licensed material that is not redistributed with the repo.\n"
            f"  Obtain a legitimate copy and either place it at the locked path or\n"
            f"  set {SOURCE_DIR_ENV} to the directory holding it."
        )

    def verify(self) -> pathlib.Path:
        """Resolve the document and prove it is byte-identical to the pin."""
        path = self.resolve()

        digest = hashlib.sha256()
        size = 0
        with path.open("rb") as fh:
            for chunk in iter(lambda: fh.read(1 << 20), b""):
                digest.update(chunk)
                size += len(chunk)
        actual = digest.hexdigest()

        if actual != self.sha256:
            raise SourceError(
                f"Source '{self.key}' does not match the pin.\n"
                f"  Document: {self.document} {self.revision}\n"
                f"  Path:     {path}\n"
                f"  Expected: {self.sha256}\n"
                f"  Actual:   {actual}\n"
                f"  Refusing to build a state database from an unpinned document."
            )
        if self.bytes is not None and size != self.bytes:
            raise SourceError(
                f"Source '{self.key}' size {size} != locked {self.bytes}."
            )
        return path


def load_sources(lock_path: pathlib.Path | None = None) -> dict[str, Source]:
    lock_path = lock_path or DEFAULT_LOCK
    if not lock_path.is_file():
        raise SourceError(f"No source lock at {lock_path}.")

    data: dict[str, Any] = yaml.safe_load(lock_path.read_text()) or {}

    version = data.get("schema_version")
    if version not in SUPPORTED_LOCK_SCHEMA_VERSIONS:
        raise SourceError(
            f"Source lock schema version {version!r} is not supported by this "
            f"importer (known: {sorted(SUPPORTED_LOCK_SCHEMA_VERSIONS)}).\n"
            f"  The importer has not been reviewed for this version. Review it "
            f"rather than generating a possibly incomplete state table."
        )

    sources: dict[str, Source] = {}
    for key, raw in (data.get("sources") or {}).items():
        known = {f.name for f in dataclasses.fields(Source)}
        unknown = set(raw) - known - {"key"}
        if unknown:
            raise SourceError(f"Source '{key}' has unknown keys: {sorted(unknown)}")
        sources[key] = Source(key=key, **raw)

    if not sources:
        raise SourceError(f"Source lock {lock_path} pins no sources.")
    return sources


def get_source(key: str, lock_path: pathlib.Path | None = None) -> Source:
    sources = load_sources(lock_path)
    if key not in sources:
        raise SourceError(
            f"No source pinned under key '{key}'. Pinned: {sorted(sources)}"
        )
    return sources[key]
