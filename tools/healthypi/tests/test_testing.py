# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT

"""The acceptance harness, exercised without hardware.

What is checked here is the harness's own logic -- capability gating, the
outcome arithmetic, and that a crashing case cannot take the suite down with
it. The cases themselves need a device; that is the bench gate.

The gating tests matter more than they look: the failure mode they prevent is a
suite that reports PASS for a command it never actually ran, which is worse
than no suite at all.
"""

from __future__ import annotations

import asyncio

import pytest

from healthypi import testing
from healthypi.testing.suite import (
    REQ_SD,
    REQ_SIGNED,
    REQ_UNLOCK,
    Capabilities,
    Outcome,
    Report,
    Result,
)


# ------------------------------------------------------------- registry ----


def test_cases_are_registered():
    reg = testing.registry()
    assert len(reg) >= 15
    names = {c.name for c in reg}
    assert "os/echo" in names
    assert "device_info" in names


def test_case_names_are_unique():
    names = [c.name for c in testing.registry()]
    assert len(names) == len(set(names)), "duplicate case name"


def test_registry_is_stable_across_calls():
    """registry() imports the cases module for its side effect; calling it
    twice must not register everything twice."""
    assert len(testing.registry()) == len(testing.registry())


def test_every_case_declares_a_known_group():
    assert set(testing.groups()) <= {"core", "stream", "recording", "system", "fw"}


def test_destructive_cases_are_marked():
    """Anything that changes device state must be opt-in. A default `hpi test
    run` is expected to be safe against a unit that is mid-recording."""
    by_name = {c.name: c for c in testing.registry()}
    for name in ("stream start/stop", "record start/stop"):
        assert by_name[name].destructive, f"{name} mutates state but is not marked"


#: Destructive cases that the DEVICE does not gate behind the security unlock.
#: Declaring REQ_UNLOCK on these would make them skip on a locked unit where
#: they would in fact run, and a false skip is the failure mode this suite is
#: built to avoid. Both are still opt-in via --destructive.
_NOT_UNLOCK_GATED = {
    "m4fw rejects a bad digest",
    # Stock MCUmgr os/datetime write -- not part of group 64, so the firmware's
    # lock state has no bearing on it.
    "datetime write",
}


def test_state_changing_cases_require_unlock():
    for c in testing.registry():
        if c.destructive and c.name not in _NOT_UNLOCK_GATED:
            assert REQ_UNLOCK in c.requires, f"{c.name} is destructive but ungated"


# ------------------------------------------------------------- gating -----


@pytest.mark.parametrize(
    "caps,requires,expected",
    [
        (Capabilities(unlocked=True), (REQ_UNLOCK,), None),
        (Capabilities(unlocked=False), (REQ_UNLOCK,), "locked"),
        (Capabilities(signed_build=True), (REQ_SIGNED,), None),
        (Capabilities(signed_build=False), (REQ_SIGNED,), "signed build"),
        (Capabilities(sd_present=True), (REQ_SD,), None),
        (Capabilities(sd_present=False), (REQ_SD,), "SD card"),
        (Capabilities(unlocked=True, sd_present=False), (REQ_UNLOCK, REQ_SD), "SD card"),
    ],
)
def test_unmet_requirements_are_named(caps, requires, expected):
    got = caps.unmet(requires)
    if expected is None:
        assert got is None
    else:
        assert got is not None and expected in got


def test_no_requirements_always_runs():
    assert Capabilities().unmet(()) is None


# -------------------------------------------------------------- report ----


def test_report_arithmetic():
    r = Report(results=[
        Result("a", Outcome.PASS),
        Result("b", Outcome.PASS),
        Result("c", Outcome.FAIL, "boom"),
        Result("d", Outcome.SKIP, "no card"),
    ])
    assert (r.passed, r.failed, r.skipped) == (2, 1, 1)
    assert not r.ok
    assert r.summary() == "2/1/1 (pass/fail/skip)"


def test_a_skip_is_not_a_failure():
    """A locked device or a dev build must not read as broken."""
    r = Report(results=[Result("a", Outcome.PASS), Result("b", Outcome.SKIP, "locked")])
    assert r.ok and r.summary() == "1/0/1 (pass/fail/skip)"


def test_empty_report_is_ok():
    assert Report().ok


def test_to_dict_round_trips():
    d = Report(results=[Result("a", Outcome.FAIL, "why", 12.34)]).to_dict()
    assert d["failed"] == 1 and d["ok"] is False
    assert d["results"][0] == {"name": "a", "outcome": "FAIL",
                               "detail": "why", "ms": 12.3}


# --------------------------------------------------------------- runner ---


class _FakeConn:
    """Answers every request with a stub; enough to drive the runner."""

    def __init__(self, responses=None):
        self.port = "/dev/fake"
        self._responses = responses or {}
        self.sent = []

    async def request(self, req, timeout_s=None):
        self.sent.append(type(req).__name__)
        return self._responses.get(type(req).__name__, object())


def test_a_crashing_case_fails_without_killing_the_suite():
    """One bad case must not cost the information in the others."""
    from healthypi.testing import suite as S

    saved = list(S._REGISTRY)
    try:
        S._REGISTRY.clear()

        @S.case("explodes")
        async def boom(conn, g, is_error, fmt_error):
            raise RuntimeError("kaboom")

        @S.case("fine")
        async def fine(conn, g, is_error, fmt_error):
            return S.ok("fine")

        async def go():
            conn = _FakeConn()
            # Probe tolerates the stub connection; requirements resolve open.
            return await S.run(conn)

        report = asyncio.run(go())
        by = {r.name: r for r in report.results}
        assert by["explodes"].outcome is Outcome.FAIL
        assert "kaboom" in by["explodes"].detail
        assert by["fine"].outcome is Outcome.PASS
    finally:
        S._REGISTRY[:] = saved


def test_destructive_cases_skipped_by_default():
    from healthypi.testing import suite as S

    saved = list(S._REGISTRY)
    try:
        S._REGISTRY.clear()

        @S.case("wipes things", destructive=True)
        async def wipe(conn, g, is_error, fmt_error):
            return S.fail("wipes things", "should not have run")

        report = asyncio.run(S.run(_FakeConn()))
        assert report.results[0].outcome is Outcome.SKIP
        assert "destructive" in report.results[0].detail
        assert report.ok
    finally:
        S._REGISTRY[:] = saved


def test_datetime_case_skips_on_an_unset_clock():
    """An unset RTC is a device state, not a defect.

    Zephyr's os handler answers a datetime read with os/RTC_NOT_SET until the
    STM32 calendar has been written once -- true of every board fresh off the
    line. That is an unmet precondition, and this suite's contract is that an
    unmet precondition is a SKIP with a reason. It used to read
    "[FAIL] datetime  read refused: UNKNOWN (4)".
    """
    from healthypi.smp.group64 import fmt_error, is_error
    from healthypi.testing import cases as C

    class RtcNotSet:
        class err:
            group = 0
            rc = 4

    class Conn:
        async def request(self, req, timeout_s=None):
            return RtcNotSet()

    res = asyncio.run(C.datetime_rw(Conn(), None, is_error, fmt_error))
    assert res.outcome is Outcome.SKIP
    assert "RTC_NOT_SET" in res.detail
    assert "datetime --set now" in res.detail


def test_datetime_case_still_fails_on_a_real_refusal():
    """Only RTC_NOT_SET is excused; anything else is still a failure."""
    from healthypi.smp.group64 import fmt_error, is_error
    from healthypi.testing import cases as C

    class RtcBroken:
        class err:
            group = 0
            rc = 5  # RTC_COMMAND_FAILED

    class Conn:
        async def request(self, req, timeout_s=None):
            return RtcBroken()

    res = asyncio.run(C.datetime_rw(Conn(), None, is_error, fmt_error))
    assert res.outcome is Outcome.FAIL
    assert "RTC_COMMAND_FAILED" in res.detail


def test_group_filter():
    from healthypi.testing import suite as S

    saved = list(S._REGISTRY)
    try:
        S._REGISTRY.clear()

        @S.case("a", group="one")
        async def a(conn, g, is_error, fmt_error):
            return S.ok("a")

        @S.case("b", group="two")
        async def b(conn, g, is_error, fmt_error):
            return S.ok("b")

        report = asyncio.run(S.run(_FakeConn(), group="one"))
        assert [r.name for r in report.results] == ["a"]
    finally:
        S._REGISTRY[:] = saved


# ------------------------------------------------------------- smpgroup ---


def test_smpgroup_submodule_access_does_not_recurse():
    """Regression: `from smpgroup import build` used to hang forever.

    __getattr__ resolved it with `from . import build`, which itself resolves
    by calling __getattr__ again. Nothing in this repository hit it -- every
    caller imports smpgroup.build directly -- but it is the obvious spelling
    for anyone else, and this package is meant to be published.
    """
    import smpgroup

    assert smpgroup.build is not None
    assert smpgroup.drift is not None

    from smpgroup import build, drift

    assert build.__name__ == "smpgroup.build"
    assert drift.__name__ == "smpgroup.drift"

    with pytest.raises(AttributeError):
        smpgroup.does_not_exist
