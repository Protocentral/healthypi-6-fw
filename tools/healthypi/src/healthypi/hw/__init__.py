# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT

"""Hardware-adjacent host tooling: things that make artifacts for a board.

Stdlib-only. Nothing here talks to a HealthyPi over USB -- that is
:mod:`healthypi.smp` and :mod:`healthypi.transport`.
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
