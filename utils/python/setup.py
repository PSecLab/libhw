"""
Build hooks for the libhw bindings.

The bindings are ctypes, so there is nothing to compile here -- but a package
that installs cleanly and then fails on the first call because it cannot find
libhw.so is worse than one that refuses to build. So the wheel carries its own
copy of the shared library, built from the repository if it is not already
there, and the wheel is tagged for the platform rather than as pure Python.

The sdist is deliberately not self-contained: the C sources live in the
repository above this directory, and copying them in would mean maintaining a
second copy. Building therefore needs the repository, which is how this
package is actually used.
"""

from __future__ import annotations

import pathlib
import shutil
import subprocess
import sys

from setuptools import setup
from setuptools.command.build_py import build_py as _build_py
from setuptools.dist import Distribution

HERE = pathlib.Path(__file__).resolve().parent
LIBRARY_NAME = "libhw.so"
BUNDLE_DIR = HERE / "libhw" / "_lib"


def _repo_root() -> pathlib.Path | None:
    """The libhw checkout this package lives inside, if it is still there."""
    for parent in HERE.parents:
        if (parent / "Makefile").is_file() and (parent / "include" / "hw.h").is_file():
            return parent
    return None


def _built_library(root: pathlib.Path) -> pathlib.Path | None:
    lib = root / "out" / LIBRARY_NAME
    return lib if lib.is_file() else None


def ensure_library() -> pathlib.Path:
    """Return a path to libhw.so, building it if the repository is present."""
    root = _repo_root()
    if root is None:
        raise SystemExit(
            "Cannot find the libhw repository from " + str(HERE) + ".\n"
            "These bindings are built from the C library in the repository that\n"
            "contains them. Install from a checkout, or set LIBHW_LIBRARY at\n"
            "runtime to point at an existing libhw.so."
        )

    lib = _built_library(root)
    if lib is not None:
        return lib

    print(f"libhw: {LIBRARY_NAME} not built yet, running make in {root}")
    try:
        subprocess.run(["make", "out/" + LIBRARY_NAME], cwd=root, check=True)
    except (OSError, subprocess.CalledProcessError) as exc:
        raise SystemExit(
            f"Could not build {LIBRARY_NAME}: {exc}\n"
            "The C library needs a compiler and the libstlink development\n"
            "headers (libstlink-dev on Debian/Ubuntu). Build it by hand with\n"
            "'make' at the repository root and try again."
        ) from exc

    lib = _built_library(root)
    if lib is None:
        raise SystemExit(f"make finished but {root / 'out' / LIBRARY_NAME} is missing")
    return lib


class build_py(_build_py):
    """Copy the shared library into the package before it is collected."""

    def run(self) -> None:
        lib = ensure_library()
        BUNDLE_DIR.mkdir(parents=True, exist_ok=True)
        target = BUNDLE_DIR / LIBRARY_NAME
        shutil.copy2(lib, target)
        print(f"libhw: bundled {lib} -> {target}")
        super().run()


class BinaryDistribution(Distribution):
    """Marks the distribution as carrying a compiled object.

    This puts the package in platlib rather than purelib, so the files sit at
    the root of the wheel instead of under a .data/purelib directory, and it
    stops the wheel being tagged as pure Python.
    """

    def has_ext_modules(self) -> bool:
        return True

    def is_pure(self) -> bool:
        return False


# The wheel carries a compiled object, so it must not be tagged pure Python.
# It is loaded through ctypes and so is not tied to a CPython ABI, which is
# why the tag stays py3-none-<platform> rather than cp3xx-cp3xx-<platform>.
try:
    try:
        from setuptools.command.bdist_wheel import bdist_wheel as _bdist_wheel
    except ImportError:
        from wheel.bdist_wheel import bdist_wheel as _bdist_wheel

    class bdist_wheel(_bdist_wheel):
        def finalize_options(self) -> None:
            super().finalize_options()
            self.root_is_pure = False

        def get_tag(self):
            _python, _abi, plat = super().get_tag()
            return "py3", "none", plat

    cmdclass = {"build_py": build_py, "bdist_wheel": bdist_wheel}
except ImportError:      # building an sdist, or no wheel support present
    cmdclass = {"build_py": build_py}


setup(cmdclass=cmdclass, distclass=BinaryDistribution)
