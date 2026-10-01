# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT

"""Generated group-64 wire classes.

Two things are checked here:

1. the generated classes encode the headers and CBOR the firmware expects, and
2. a set of requests still produces **exactly** the bytes recorded below.

(2) is a regression guard on the wire encoding. The golden bytes were taken
from the hand-written classes the generated set replaced, and those were the
bytes on the wire when the update path was validated on v5 hardware (July
2026, and again with the generated classes on 2026-10-01). They are data, not a
second implementation: the catalog is the only schema.
"""

from __future__ import annotations

import pytest

pytest.importorskip("smp", reason="SMP stack not installed")

import cbor2  # noqa: E402
from smp.header import OP, Flag, Header, Version  # noqa: E402

from healthypi.smp import catalog  # noqa: E402
from healthypi.smp.group64 import g  # noqa: E402


def _reply(cls, payload: dict, cmd_id: int, op=OP.READ_RSP):
    body = cbor2.dumps(payload, canonical=True)
    hdr = Header(
        op=op,
        version=Version.V2,
        flags=Flag(0),
        length=len(body),
        group_id=64,
        sequence=0,
        command_id=cmd_id,
    )
    return cls.loads(bytes(hdr) + body)


def test_every_routed_command_is_generated():
    routed = {c.name for c in catalog.dispatchable()}
    generated = {b.command.name for b in g}
    assert generated == routed
    # both-op commands appear twice, under <name> and <name>_write
    assert "transfer_mode" in g and "transfer_mode_write" in g
    assert "enter_recovery" in g and "enter_recovery_write" in g


def test_no_unreachable_command_is_generated():
    for cmd in catalog.HPI_GROUP.unreachable():
        assert cmd.name not in g, f"{cmd.name} cannot be answered by the device"


def test_headers_carry_group_64_and_the_right_id():
    for bound in g:
        req = bound.Request() if not bound.command.schema(bound.op)[0] else None
        if req is None:
            continue
        assert req.header.group_id == 64
        assert req.header.command_id == bound.cmd_id


def test_device_info_response():
    r = _reply(
        g.device_info.Response,
        {
            "sn": "HP6-0001",
            "fw": "1.0.0",
            "gv": catalog.SCHEMA_VERSION,
            "br": "v5",
            "hw": b"\xde\xad",
            "m4fw": "1.0.0",
            "espfw": "1.2.3",
            "up": 3600,
        },
        0x0001,
    )
    assert r.sn == "HP6-0001" and r.gv == catalog.SCHEMA_VERSION
    assert r.hw == b"\xde\xad" and r.up == 3600


def test_telemetry_signed_fields():
    r = _reply(
        g.telemetry.Response,
        {
            "vbat_mv": 4100,
            "ibat_ma": -250,
            "soc": 88,
            "tc_x10": -32768,
            "charge": 1,
            "usb": True,
            "batt": False,
            "ok": True,
        },
        0x0030,
    )
    assert r.ibat_ma == -250 and r.tc_x10 == -32768


def test_module_list_is_a_map_keyed_by_slot():
    r = _reply(
        g.module_list.Response,
        {
            "a": {"state": 2, "id": 5, "pwr": True, "name": "NPU"},
            "b": {"state": 0, "id": 0, "pwr": False, "name": ""},
        },
        0x0050,
    )
    assert r.a.state == 2 and r.a.name == "NPU"
    assert r.b.state == 0


def test_selftest_nested_health_map():
    r = _reply(
        g.diag_run_selftest.Response,
        {
            "suite_ver": 1,
            "sd": True,
            "batt": True,
            "ecg": True,
            "ppg": True,
            "m4": True,
            "qspi": True,
            "pass": 6,
            "fail": 0,
            "health_overall": 0,
            "health": {"acq": 0, "m4": 0, "stream": 0, "rec": 0, "hl": 1},
        },
        0x0080,
    )
    assert r.health.hl == 1 and r.m4 is True


def test_stream_start_encodes_masks():
    req = g.stream_start(ch=0x03, ann=0x00)
    assert req.header.op == OP.WRITE
    assert cbor2.loads(req.BYTES[8:]) == {"ch": 3, "ann": 0}


def test_both_op_command_has_distinct_schemas():
    assert cbor2.loads(g.transfer_mode().BYTES[8:]) == {}
    w = g.transfer_mode_write(on=True)
    assert w.header.op == OP.WRITE
    assert cbor2.loads(w.BYTES[8:]) == {"on": True}


def test_m4fw_begin_signature_is_optional():
    assert cbor2.loads(g.m4fw_begin(len=1024, sha=b"\x00" * 32).BYTES[8:]).keys() == {
        "len",
        "sha",
    }
    signed = cbor2.loads(g.m4fw_begin(len=1024, sha=b"\x00" * 32, sig=b"\x01" * 64).BYTES[8:])
    assert set(signed) == {"len", "sha", "sig"}


def test_newer_firmware_reply_still_parses():
    """A field the catalog does not know yet is dropped, not fatal. Found on
    hardware 2026-09-30: firmware that added `usb_att` to the telemetry reply
    made this tool fail every telemetry read. The drift test, not the runtime,
    is what keeps the catalog complete."""
    r = _reply(
        g.stream_status.Response,
        {"active": True, "ch": 3, "ann": 0, "sent": 1, "dropped": 0, "new": 1},
        0x0022,
    )
    assert (r.active, r.sent) == (True, 1)


def test_error_formatting_uses_the_group_table():
    from healthypi.smp.group64 import fmt_error

    class E:
        class err:
            rc = 267

    assert fmt_error(E()) == "NO_MEDIA (267) -- no SD card present"


def test_error_from_a_stock_group_is_named_not_UNKNOWN():
    """os/RTC_NOT_SET is rc 4 in the *os* group, and 4 means nothing in ours.

    The regression this locks: an unset clock was reported as
    "read refused: UNKNOWN (4)" because the group-64 table was consulted for a
    group-0 code.
    """
    from healthypi.smp.group64 import fmt_error

    class E:
        class err:
            group = 0
            rc = 4

    out = fmt_error(E())
    assert "RTC_NOT_SET" in out
    assert "group 0" in out and "rc 4" in out
    assert "UNKNOWN" not in out


def test_plain_rc_errors_still_name_themselves():
    """A handler returning non-zero answers `{"rc": n}` in the protocol-wide
    MGMT_ERR namespace -- a different shape, and it must keep working."""
    from healthypi.smp.group64 import fmt_error

    from smp.error import MGMT_ERR

    class Rc:
        rc = MGMT_ERR.ENOTSUP
        rsn = None

    assert fmt_error(Rc()) == "ENOTSUP (8)"


# --- golden wire bytes -------------------------------------------------------

# (command, kwargs, SMP header bytes 0-5, bytes 7-end). Byte 6 is the
# per-request sequence number and is not compared.
GOLDEN = [
    ("fw_versions", {}, "080000010040", "31a0"),
    ("stream_stop", {}, "0a0000010040", "21a0"),
    ("m4fw_status", {}, "080000010040", "a3a0"),
    ("m4fw_abort", {}, "0a0000010040", "a4a0"),
    ("m4fw_chunk", {"off": 512, "data": b"\xaa\xbb"}, "0a0000100040", "a1a2636f6666190200646461746142aabb"),
    ("m4fw_commit", {}, "0a0000010040", "a2a0"),
    ("enter_recovery_write", {"arm": True, "rst": True}, "0a00000b0040", "a5a26361726df563727374f5"),
]


@pytest.mark.parametrize("name,kwargs,head,tail", GOLDEN, ids=[c[0] for c in GOLDEN])
def test_requests_put_the_recorded_bytes_on_the_wire(name, kwargs, head, tail):
    """A catalog edit must not silently change an encoding the device relies on."""
    wire = g[name](**kwargs).BYTES
    assert wire[:6].hex() == head, f"{name}: header differs"
    assert wire[7:].hex() == tail, f"{name}: command id or payload differs"


def test_unparseable_reply_names_the_command_not_a_TypeError():
    """smpclient mishandles a reply that fits no schema and raises
    `TypeError: ValidationError.__new__() missing ... 'line_errors'`, naming
    neither the command nor the cause. Seen on hardware 2026-09-30 and
    2026-10-01."""
    import asyncio

    from healthypi.transport.serial_smp import Connection, UnparseableReplyError

    class Client:
        async def request(self, req, timeout_s=None):
            raise TypeError("ValidationError.__new__() missing 1 required positional argument: 'line_errors'")

    conn = Connection(client=Client(), port="/dev/null")
    with pytest.raises(UnparseableReplyError) as exc:
        asyncio.run(conn.request(g.m4fw_begin(len=1, sha=b"\x00" * 32)))
    msg = str(exc.value)
    assert "M4FwBegin" in msg and "command 160" in msg
    assert "healthypi device versions" in msg and "smp_add_cmd_err" in msg


def test_an_unrelated_TypeError_is_not_relabelled():
    import asyncio

    from healthypi.transport.serial_smp import Connection

    class Client:
        async def request(self, req, timeout_s=None):
            raise TypeError("a genuine programming error")

    conn = Connection(client=Client(), port="/dev/null")
    with pytest.raises(TypeError, match="genuine"):
        asyncio.run(conn.request(g.device_info()))
