# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT

"""Hardware-adjacent host tooling: things that make artifacts for a board.

:mod:`~healthypi.hw.eeprom` builds and parses HealthyLink ID EEPROM images and
is stdlib-only. :mod:`~healthypi.hw.program` writes one to a module *through a
connected HealthyPi* (group-64 0x0053/0x0054) and is imported on demand,
because that half needs the SMP stack.
"""

from __future__ import annotations

from .eeprom import (
    CAPABILITIES,
    MODULE_IDS,
    EepromError,
    EepromInfo,
    build_stack,
    create_image,
    parse_image,
)

__all__ = [
    "CAPABILITIES",
    "MODULE_IDS",
    "EepromError",
    "EepromInfo",
    "build_stack",
    "create_image",
    "parse_image",
]
