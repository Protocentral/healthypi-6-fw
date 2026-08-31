# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT

"""MCUmgr SMP -- HealthyPi group 64 and the stock groups.

``catalog`` is the single source of truth for the group-64 surface and is
stdlib-only, so the firmware drift check runs anywhere. The classes that put
those commands on the wire need ``smpclient`` (the ``device`` extra) and live in
``group64``, which imports lazily for that reason.
"""

from . import catalog  # noqa: F401  (stdlib-only)
from .catalog import (  # noqa: F401
    COMMANDS,
    ERRORS,
    EVENTS,
    GROUP_ID,
    SCHEMA_VERSION,
    BY_ID,
    BY_NAME,
    HPI_GROUP,
    Command,
    Field,
    Op,
    Status,
    T,
    dispatchable,
    err_hint,
    err_name,
    live,
    signed_build_only,
    unlock_gated,
)

__all__ = [
    "BY_ID",
    "BY_NAME",
    "COMMANDS",
    "ERRORS",
    "EVENTS",
    "GROUP_ID",
    "HPI_GROUP",
    "SCHEMA_VERSION",
    "Command",
    "Field",
    "Op",
    "Status",
    "T",
    "catalog",
    "dispatchable",
    "err_hint",
    "err_name",
    "live",
    "signed_build_only",
    "unlock_gated",
]
