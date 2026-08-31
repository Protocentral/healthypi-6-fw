# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT

"""Wire classes for HealthyPi group 64.

Generated from :data:`healthypi.smp.catalog.HPI_GROUP` by :mod:`smpgroup.build`
-- there are no hand-written request/response classes here, and there must never
be. Hand-written copies are what made a device-side field addition break three
host tools at once; a generated set cannot drift from the catalog, and the
catalog is drift-checked against the firmware.

    from healthypi.smp import group64

    async with SMPClient(SMPSerialTransport(), port) as client:
        info = await client.request(group64.g.device_info())
        await client.request(group64.g.stream_start(ch=0x03, ann=0))
        armed = await client.request(group64.g.transfer_mode_write(on=True))

Needs the SMP stack: ``pip install 'healthypi[device]'``.
"""

from __future__ import annotations

from smpgroup.build import build, format_error, is_error

from .catalog import HPI_GROUP, STOCK_ERRORS

#: Every routed command in group 64, addressable by name.
g = build(HPI_GROUP)

#: A command implementing both ops exposes the write side as ``<name>_write``.
__all__ = ["g", "fmt_error", "is_error", "HPI_GROUP"]


def fmt_error(response) -> str:
    """Render an SMP error reply as a sentence.

    Group-64 codes come from the catalog's table. An error raised by a stock
    group on the same connection -- ``os`` datetime is the one this package
    actually issues -- is named from :data:`~healthypi.smp.catalog.STOCK_ERRORS`
    instead, because those codes are in *that* group's namespace and mean
    nothing in ours.
    """
    return format_error(HPI_GROUP, response, foreign=STOCK_ERRORS)
