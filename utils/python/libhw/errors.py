"""Exceptions raised by the libhw bindings."""

from __future__ import annotations


class HwError(Exception):
    """A libhw call failed.

    The C API reports failure as a -1 return and prints the detail to stderr,
    so the message here says which operation failed rather than inventing a
    reason the library did not give.
    """


class HwConnectError(HwError):
    """hw_connect() returned NULL: no such backend, or it could not attach."""


class HwFlashError(HwError):
    """A flash operation failed."""


class HwStateError(HwError):
    """A state database lookup or query failed."""
