# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT

"""The test harness: a registry, a runner, and result types.

Importable, unlike the shell script this replaces. That is the whole point --
``release.sh`` can gate a bundle on a hardware pass, another test can reuse a
single case, and CI can run the offline subset, none of which was possible when
the suite was 908 lines that only a human with a port could start.

A case is a coroutine taking a live :class:`~healthypi.transport.serial_smp.
Connection` and the generated command set, and returning a :class:`Result`.
Cases declare what they need (an unlocked device, a signed build, an SD card)
rather than assuming it, so an unmet requirement is reported as SKIP with the
reason -- never as a failure, and never as a false pass.
"""

from __future__ import annotations

import time
from dataclasses import dataclass, field
from enum import Enum
from typing import Awaitable, Callable, Iterable


class Outcome(str, Enum):
    PASS = "PASS"
    FAIL = "FAIL"
    SKIP = "SKIP"


@dataclass
class Result:
    name: str
    outcome: Outcome
    detail: str = ""
    ms: float = 0.0

    @property
    def ok(self) -> bool:
        return self.outcome is not Outcome.FAIL


@dataclass
class Report:
    results: list[Result] = field(default_factory=list)

    @property
    def passed(self) -> int:
        return sum(r.outcome is Outcome.PASS for r in self.results)

    @property
    def failed(self) -> int:
        return sum(r.outcome is Outcome.FAIL for r in self.results)

    @property
    def skipped(self) -> int:
        return sum(r.outcome is Outcome.SKIP for r in self.results)

    @property
    def ok(self) -> bool:
        return self.failed == 0

    def summary(self) -> str:
        """The 16/0/1 line the bench acceptance run is quoted in."""
        return f"{self.passed}/{self.failed}/{self.skipped} (pass/fail/skip)"

    def to_dict(self) -> dict:
        return {
            "passed": self.passed,
            "failed": self.failed,
            "skipped": self.skipped,
            "ok": self.ok,
            "results": [
                {"name": r.name, "outcome": r.outcome.value,
                 "detail": r.detail, "ms": round(r.ms, 1)}
                for r in self.results
            ],
        }


# --------------------------------------------------------------------------
# registry
# --------------------------------------------------------------------------

#: Requirement tags a case may declare. Unmet -> SKIP with the reason, so a
#: locked device or a dev build produces an honest report rather than a wall of
#: failures that trains people to ignore the suite.
REQ_UNLOCK = "unlock"
REQ_SIGNED = "signed_build"
REQ_SD = "sd"

CaseFn = Callable[..., Awaitable[Result]]


@dataclass
class Case:
    name: str
    fn: CaseFn
    requires: tuple[str, ...] = ()
    group: str = "general"
    destructive: bool = False   #: mutates device state; opt-in only


_REGISTRY: list[Case] = []


def case(name: str, *, requires: Iterable[str] = (), group: str = "general",
         destructive: bool = False):
    """Register a test case."""

    def deco(fn: CaseFn) -> CaseFn:
        _REGISTRY.append(Case(name=name, fn=fn, requires=tuple(requires),
                              group=group, destructive=destructive))
        return fn

    return deco


def registry() -> list[Case]:
    from . import cases  # noqa: F401  -- import registers them

    return list(_REGISTRY)


def groups() -> list[str]:
    return sorted({c.group for c in registry()})


# --------------------------------------------------------------------------
# helpers available to cases
# --------------------------------------------------------------------------


def ok(name: str, detail: str = "") -> Result:
    return Result(name, Outcome.PASS, detail)


def fail(name: str, detail: str) -> Result:
    return Result(name, Outcome.FAIL, detail)


def skip(name: str, detail: str) -> Result:
    return Result(name, Outcome.SKIP, detail)


# --------------------------------------------------------------------------
# runner
# --------------------------------------------------------------------------


@dataclass
class Capabilities:
    """What the attached device can actually be asked to do.

    Probed once, before any case runs, so requirements resolve against the real
    device instead of a flag someone remembered to pass.
    """

    unlocked: bool = False
    signed_build: bool = False
    sd_present: bool = False
    notes: list[str] = field(default_factory=list)

    def unmet(self, requires: tuple[str, ...]) -> str | None:
        if REQ_UNLOCK in requires and not self.unlocked:
            return "device is locked (hpi lock unlock)"
        if REQ_SIGNED in requires and not self.signed_build:
            return "not a signed build (no M4-update/recovery support)"
        if REQ_SD in requires and not self.sd_present:
            return "no SD card present"
        return None


async def probe(conn, g, is_error) -> Capabilities:
    """Work out what this device supports, tolerating every failure."""
    caps = Capabilities()

    # lock_state.state: 0 = LOCKED, 1 = UNLOCKED (enum hpi_lock_state). A build
    # with CONFIG_HPI_SECURITY=n does not route the command at all and answers
    # an error -- which for our purposes is "nothing is gated", i.e. unlocked.
    try:
        st = await conn.request(g.lock_state())
        caps.unlocked = True if is_error(st) else (getattr(st, "state", 1) == 1)
        if is_error(st):
            caps.notes.append("no lock gate in this build (CONFIG_HPI_SECURITY=n)")
    except Exception:  # noqa: BLE001
        caps.unlocked = True
        caps.notes.append("lock_state unavailable; assuming ungated")

    # m4fw_status answers only where CONFIG_HPI_M4_UPDATE is built in, which is
    # the signed layer -- so a successful reply IS the signed-build detection.
    try:
        m4 = await conn.request(g.m4fw_status())
        caps.signed_build = not is_error(m4)
    except Exception:  # noqa: BLE001
        caps.signed_build = False

    # There is no "is a card present" command in 1.0.0. sd_status reports the
    # *recording* state and answers fine with no card. The self-test's `sd`
    # boolean is the only readiness signal the device exposes.
    try:
        stest = await conn.request(g.diag_run_selftest())
        caps.sd_present = (not is_error(stest)) and bool(getattr(stest, "sd", False))
    except Exception:  # noqa: BLE001
        caps.sd_present = False
        caps.notes.append("self-test unavailable; treating SD as absent")

    return caps


async def run(conn, *, group: str | None = None, include_destructive: bool = False,
              on_result: Callable[[Result], None] | None = None) -> Report:
    """Run the suite against an open connection."""
    from ..smp.group64 import fmt_error, g, is_error

    report = Report()
    caps = await probe(conn, g, is_error)

    for c in registry():
        if group and c.group != group:
            continue
        if c.destructive and not include_destructive:
            report.results.append(
                skip(c.name, "destructive; pass --destructive to include"))
            if on_result:
                on_result(report.results[-1])
            continue

        reason = caps.unmet(c.requires)
        if reason:
            report.results.append(skip(c.name, reason))
            if on_result:
                on_result(report.results[-1])
            continue

        t0 = time.monotonic()
        try:
            res = await c.fn(conn, g, is_error, fmt_error)
        except Exception as exc:  # noqa: BLE001 -- a crashing case is a failure,
            # not a crashed suite: the remaining cases still carry information.
            res = fail(c.name, f"{type(exc).__name__}: {exc}")
        res.ms = (time.monotonic() - t0) * 1000.0
        report.results.append(res)
        if on_result:
            on_result(res)

    return report
