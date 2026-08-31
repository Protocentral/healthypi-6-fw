# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT

"""The group-64 acceptance suite, as a library.

The 16/0/1 bench gate lives here rather than in a script, so that
``scripts/release.sh`` can require a hardware pass before it emits a bundle,
and so a single case can be reused from another test.

    from healthypi.testing import run, registry
    report = await run(conn)
    print(report.summary())          # "16/0/1 (pass/fail/skip)"

Needs the device stack (``pip install 'protocentral-healthypi[device]'``);
:func:`registry` alone does not.
"""

from __future__ import annotations

from .suite import (
    REQ_SD,
    REQ_SIGNED,
    REQ_UNLOCK,
    Capabilities,
    Case,
    Outcome,
    Report,
    Result,
    case,
    groups,
    probe,
    registry,
    run,
)

__all__ = [
    "REQ_SD",
    "REQ_SIGNED",
    "REQ_UNLOCK",
    "Capabilities",
    "Case",
    "Outcome",
    "Report",
    "Result",
    "case",
    "groups",
    "probe",
    "registry",
    "run",
]
