# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT

"""The catalog must not drift from the firmware.

Uses the generic checker in ``smpgroup.drift``, pointed at this repo's MCUmgr
sources. Adding a command or a reply field on the device fails here until the
host catalog is updated -- which is what makes "single source of truth" an
enforceable claim rather than an aspiration.

Skipped (not failed) outside the firmware tree, so the package stays testable
standalone.
"""

from __future__ import annotations

from pathlib import Path

import pytest

from healthypi.smp import catalog
from smpgroup import Status
from smpgroup.drift import Sources, check

HERE = Path(__file__).resolve()
REPO = HERE.parents[3]  # tools/healthypi/tests/ -> repo root
MGMT = REPO / "app_m7" / "src" / "control" / "mcumgr_hpi"

SOURCES = Sources.zephyr(
    MGMT,
    header="hpi_mgmt_group.h",
    dispatch="hpi_mgmt_group.c",
    id_prefix="HPI_MGMT_CMD_",
    err_prefix="HPI_MGMT_ERR_",
    err_enum="enum hpi_mgmt_err",
)

pytestmark = pytest.mark.skipif(
    not SOURCES.exists(), reason="firmware tree not present"
)


@pytest.fixture(scope="module")
def report():
    return check(catalog.HPI_GROUP, SOURCES)


def test_catalog_matches_firmware(report):
    """Ids, routing status, error codes and every CBOR key."""
    assert report.ok, str(report)


def test_firmware_actually_parsed(report):
    """Guard against a silent no-op if the C layout ever changes shape."""
    assert len(report.declared) >= 46, report.declared
    assert len(report.routed) == 35, sorted(report.routed)
    assert len(report.errors) == 16, report.errors


def test_surface_counts():
    """The headline numbers in GROUP64_SSOT.md."""
    g = catalog.HPI_GROUP
    assert len(g.routed()) == 35
    assert len(g.live()) == 34  # wifi_scan is a stub
    assert len(g.unreachable()) == 11
    assert len(g.tagged("unlock")) == 11
    assert len(g.tagged("signed_build")) == 5


def test_stub_is_marked_not_live():
    """wifi_scan is dispatched but returns ENOTSUP -- it must not read as API."""
    assert catalog.BY_NAME["wifi_scan"].status is Status.STUB
    handlers = "\n".join(p.read_text() for p in SOURCES.handlers)
    assert "MGMT_ERR_ENOTSUP" in handlers


def _uncommented(text: str) -> str:
    """C source with comments stripped -- a rule about code, not about prose."""
    import re

    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def test_unlock_gate_still_exists():
    handlers = "\n".join(p.read_text() for p in SOURCES.handlers)
    assert "hpi_security_require_unlocked" in handlers, "gate helper vanished"


def _group_err_offenders(name: str, text: str) -> list[str]:
    """Places in one handler file that return a group-64 code the wrong way."""
    import re

    out = []
    for n, line in enumerate(text.splitlines(), 1):
        if re.search(r"return\s+HPI_MGMT_ERR_", line):
            out.append(f"{name}:{n}: {line.strip()}")
    # A file that names group codes but never encodes one is building them to
    # return some other way -- through a variable, say, which the line check
    # above cannot see.
    names = re.findall(r"HPI_MGMT_ERR_[A-Z0-9_]+", _uncommented(text))
    if names and "smp_add_cmd_err" not in text:
        out.append(
            f"{name}: uses {sorted(set(names))[0]} but never calls "
            "smp_add_cmd_err()"
        )
    return out


def test_group_error_guard_actually_catches_it():
    """A guard that cannot fail proves nothing -- prove it fails."""
    assert _group_err_offenders(
        "bad.c", "int h(void)\n{\n\treturn HPI_MGMT_ERR_HW_FAULT;\n}\n"
    )
    assert _group_err_offenders(
        "sneaky.c",
        "uint16_t code = HPI_MGMT_ERR_NOT_READY;\nreturn code;\n",
    )
    assert not _group_err_offenders(
        "good.c",
        "bool ok = smp_add_cmd_err(zse, HPI_MGMT_GROUP_ID, "
        "HPI_MGMT_ERR_NOT_READY);\nreturn ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;\n",
    )


def test_group_error_codes_are_never_returned_as_protocol_codes():
    """A group-64 code (>= 256) has to go out as an err map, not as `rc`.

    Returning one straight from a handler puts a number on the wire that means
    nothing in the protocol-wide MGMT_ERR namespace, and the reply then matches
    neither the success model nor either error model -- the client cannot even
    name the failure. `smp_add_cmd_err(zse, HPI_MGMT_GROUP_ID, code)` is the
    only correct way out.
    """
    offenders = []
    for path in SOURCES.handlers:
        offenders += _group_err_offenders(path.name, path.read_text())
    assert not offenders, (
        "group-64 error codes returned directly instead of via "
        "smp_add_cmd_err():\n  " + "\n  ".join(offenders)
    )


def test_drift_is_detectable():
    """A checker that cannot fail proves nothing -- prove it fails."""
    import dataclasses

    g = catalog.HPI_GROUP
    cmd = g.by_name("stream_status")
    from smpgroup import Field, T

    broken = dataclasses.replace(
        cmd, response=cmd.response[:-1] + (Field("droppd", T.UINT),)
    )
    mutated = dataclasses.replace(
        g, commands=tuple(broken if c.name == cmd.name else c for c in g.commands)
    )
    rep = check(mutated, SOURCES)
    assert not rep.ok
    assert any("droppd" in p for p in rep.problems), rep.problems


# --- the stock groups we are also answered by ------------------------------
#
# The check above covers group 64 and nothing else, which is correct and was
# also the blind spot: the device serves the stock MCUmgr `os` group on the same
# connection, and an os error is in os's namespace. `catalog.STOCK_ERRORS`
# transcribes that namespace, so it needs the same guard against rot.

ZEPHYR = REPO.parent / "zephyr"
OS_MGMT_H = ZEPHYR / "include/zephyr/mgmt/mcumgr/grp/os_mgmt/os_mgmt.h"


def _parse_c_enum(text: str, enum_name: str, prefix: str) -> dict[str, int]:
    """Values of a C enum, honouring implicit (sequential) numbering."""
    import re

    body = text[text.index(enum_name) :]
    body = body[body.index("{") + 1 : body.index("}")]
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    out: dict[str, int] = {}
    nxt = 0
    for m in re.finditer(re.escape(prefix) + r"([A-Z0-9_]+)\s*(?:=\s*(\d+))?", body):
        val = int(m.group(2)) if m.group(2) is not None else nxt
        out[m.group(1)] = val
        nxt = val + 1
    return out


@pytest.mark.skipif(not OS_MGMT_H.is_file(), reason="Zephyr tree not present")
def test_stock_os_errors_match_zephyr():
    fw = _parse_c_enum(
        OS_MGMT_H.read_text(), "enum os_mgmt_err_code_t", "OS_MGMT_ERR_"
    )
    assert fw, "parsed no os_mgmt error codes -- the enum changed shape"
    spec = {name: code for code, (name, _hint) in catalog.STOCK_ERRORS[0].items()}
    assert spec == fw
    assert fw["RTC_NOT_SET"] == catalog.OS_ERR_RTC_NOT_SET
