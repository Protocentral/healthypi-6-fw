# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT

"""smpgroup -- declarative custom MCUmgr management groups.

Describe a Zephyr device's custom SMP group once, as data. Then:

* generate ``smpclient`` request/response classes from it (:mod:`smpgroup.build`)
* check it against the firmware that implements it (:mod:`smpgroup.drift`)

:mod:`smpgroup.spec` and :mod:`smpgroup.drift` are stdlib-only; only
:mod:`smpgroup.build` needs an SMP stack, and it is imported on demand.
"""

from .spec import (
    USER_GROUP_ID_START,
    Command,
    Field,
    Group,
    Op,
    Status,
    T,
)

__version__ = "0.1.0"

__all__ = [
    "Command",
    "Field",
    "Group",
    "Op",
    "Status",
    "T",
    "USER_GROUP_ID_START",
    "__version__",
    "build",
    "drift",
]


def __getattr__(name: str):
    """Lazy access so ``import smpgroup`` never pulls in the SMP stack.

    Uses importlib rather than ``from . import build``. The latter recurses
    forever: ``from package import submodule`` resolves by calling
    ``getattr(package, "submodule")`` first, which re-enters this function,
    which runs the same statement again. Nothing in this repository hit it --
    every caller says ``from smpgroup.build import ...``, which addresses the
    submodule directly -- but ``import smpgroup; smpgroup.build`` and
    ``from smpgroup import build`` are the obvious things for anyone else to
    write, and both hung until this used importlib.
    """
    if name in ("build", "drift"):
        import importlib

        return importlib.import_module(f".{name}", __name__)
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")
