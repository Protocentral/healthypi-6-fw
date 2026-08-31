# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT

"""smpgroup tests.

The spec and drift tests are stdlib-only. The build tests need the SMP stack and
skip without it, so the package stays testable in a bare environment.
"""

from __future__ import annotations

import textwrap

import pytest

from smpgroup import Command, Field, Group, Op, Status, T

R, W = Op.READ, Op.WRITE

DEMO = Group(
    group_id=64,
    name="demo",
    schema_version=1,
    commands=(
        Command(
            0x0001,
            "info",
            (R,),
            response=(
                Field("sn", T.TSTR),
                Field("up", T.UINT),
                Field("hw", T.BSTR),
            ),
        ),
        Command(
            0x0002,
            "set_thing",
            (W,),
            meta={"unlock": True},
            request=(Field("name", T.TSTR), Field("on", T.BOOL)),
            response=(Field("ok", T.BOOL),),
        ),
        Command(
            0x0003,
            "both",
            (R, W),
            response=(Field("armed", T.BOOL),),
            write_request=(Field("on", T.BOOL),),
            write_response=(Field("armed", T.BOOL),),
        ),
        Command(
            0x0004,
            "nested",
            (R,),
            response=(
                Field("top", T.UINT),
                Field(
                    "health",
                    T.MAP,
                    nested=(Field("a", T.UINT), Field("b", T.UINT)),
                ),
            ),
        ),
        Command(0x0005, "reserved_thing", (), status=Status.UNREACHABLE),
        Command(0x0006, "stubbed", (R,), status=Status.STUB),
    ),
    errors={256: ("NOT_READY", "busy"), 257: ("HW_FAULT", "hardware")},
)


# --- spec ------------------------------------------------------------------


def test_lookup_and_partitions():
    assert DEMO.by_id(0x0002).name == "set_thing"
    assert DEMO.by_name("info").cmd_id == 1
    assert len(DEMO.routed()) == 5
    assert len(DEMO.live()) == 4
    assert [c.name for c in DEMO.unreachable()] == ["reserved_thing"]
    assert [c.name for c in DEMO.tagged("unlock")] == ["set_thing"]


def test_group_id_must_be_in_the_user_range():
    with pytest.raises(ValueError, match="reserved by MCUmgr"):
        Group(group_id=0, name="os", commands=())


def test_duplicate_command_ids_are_rejected():
    with pytest.raises(ValueError, match="duplicate command id"):
        Group(
            group_id=64,
            name="x",
            commands=(Command(1, "a", (R,)), Command(1, "b", (R,))),
        )


def test_unreachable_must_declare_no_ops():
    with pytest.raises(ValueError, match="must declare no ops"):
        Command(1, "x", (R,), status=Status.UNREACHABLE)


def test_routed_must_declare_an_op():
    with pytest.raises(ValueError, match="must declare an op"):
        Command(1, "x", ())


def test_write_schema_needs_a_write_op():
    with pytest.raises(ValueError, match="write schema"):
        Command(1, "x", (R,), write_request=(Field("a", T.UINT),))


def test_schema_selects_per_op():
    both = DEMO.by_name("both")
    assert both.schema(R) == ((), (Field("armed", T.BOOL),))
    assert both.schema(W)[0] == (Field("on", T.BOOL),)
    info = DEMO.by_name("info")
    assert info.schema(W) == info.schema(R)  # no write-specific schema


def test_error_lookup():
    assert DEMO.err_name(267) == "UNKNOWN"
    assert DEMO.err_name(256) == "NOT_READY"
    assert DEMO.err_hint(257) == "hardware"


def test_describe_marks_status():
    out = DEMO.describe()
    assert "group 64 (demo), schema 0x0001" in out
    assert "x 0x0005 reserved_thing" in out
    assert "~ 0x0006 stubbed" in out


# --- drift -----------------------------------------------------------------

HEADER = """
enum demo_cmd {
    DEMO_CMD_INFO      = 0x0001,
    DEMO_CMD_SET_THING = 0x0002,
    DEMO_CMD_BOTH      = 0x0003,
    DEMO_CMD_NESTED    = 0x0004,
    DEMO_CMD_RESERVED_THING = 0x0005,
    DEMO_CMD_STUBBED   = 0x0006,
};
enum demo_err {
    DEMO_ERR_NOT_READY = 256,
    DEMO_ERR_HW_FAULT  = 257,
};
"""

DISPATCH = """
static const struct mgmt_handler handlers[] = {
    [DEMO_CMD_INFO]      = { .mh_read = info_read },
    [DEMO_CMD_SET_THING] = { .mh_write = set_thing },
    [DEMO_CMD_BOTH]      = { .mh_read = both_r, .mh_write = both_w },
    [DEMO_CMD_NESTED]    = { .mh_read = nested_read },
    [DEMO_CMD_STUBBED]   = { .mh_read = stubbed },
};
"""

HANDLERS = """
zcbor_tstr_put_lit(zse, "sn"); zcbor_tstr_put_lit(zse, "up");
zcbor_tstr_put_lit(zse, "hw"); zcbor_tstr_put_lit(zse, "ok");
ZCBOR_MAP_DECODE_KEY_DECODER("name", d, &n);
ZCBOR_MAP_DECODE_KEY_DECODER("on", d, &o);
zcbor_tstr_put_lit(zse, "armed"); zcbor_tstr_put_lit(zse, "top");
zcbor_tstr_put_lit(zse, "health"); zcbor_tstr_put_lit(zse, "a");
zcbor_tstr_put_lit(zse, "b");
"""


@pytest.fixture
def fw(tmp_path):
    from smpgroup.drift import Sources

    (tmp_path / "demo.h").write_text(textwrap.dedent(HEADER))
    (tmp_path / "demo.c").write_text(textwrap.dedent(DISPATCH))
    (tmp_path / "demo_handlers.c").write_text(textwrap.dedent(HANDLERS))
    return Sources.zephyr(
        tmp_path,
        header="demo.h",
        dispatch="demo.c",
        id_prefix="DEMO_CMD_",
        err_prefix="DEMO_ERR_",
        err_enum="enum demo_err",
    )


def test_clean_spec_passes(fw):
    from smpgroup.drift import check

    rep = check(DEMO, fw)
    assert rep.ok, str(rep)
    assert len(rep.declared) == 6 and len(rep.routed) == 5


def test_missing_sources_is_reported_not_raised(tmp_path):
    from smpgroup.drift import Sources, check

    src = Sources.zephyr(tmp_path / "nope", id_prefix="X_", err_prefix="Y_")
    rep = check(DEMO, src)
    assert not rep.ok and "not found" in rep.problems[0]


@pytest.mark.parametrize(
    "mutate,expect",
    [
        (lambda c: {"cmd_id": 0x00FF}, "spec id"),
        (lambda c: {"status": Status.UNREACHABLE, "ops": ()}, "firmware routes it"),
        (lambda c: {"response": (Field("nope", T.UINT),)}, "appears in no handler"),
        (lambda c: {"name": "ghost"}, "not declared by the firmware"),
    ],
)
def test_drift_is_caught(fw, mutate, expect):
    import dataclasses

    from smpgroup.drift import check

    target = DEMO.by_name("info")
    broken = dataclasses.replace(target, **mutate(target))
    g = dataclasses.replace(
        DEMO, commands=tuple(broken if c.name == target.name else c for c in DEMO.commands)
    )
    rep = check(g, fw)
    assert not rep.ok
    assert any(expect in p for p in rep.problems), rep.problems


def test_firmware_command_missing_from_spec_is_caught(fw):
    import dataclasses

    from smpgroup.drift import check

    g = dataclasses.replace(
        DEMO, commands=tuple(c for c in DEMO.commands if c.name != "nested")
    )
    rep = check(g, fw)
    assert any("missing from the spec" in p for p in rep.problems), rep.problems


def test_error_table_drift_is_caught(fw):
    import dataclasses

    from smpgroup.drift import check

    g = dataclasses.replace(DEMO, errors={256: ("NOT_READY", ""), 258: ("MADE_UP", "")})
    rep = check(g, fw)
    assert any("MADE_UP" in p for p in rep.problems)
    assert any("HW_FAULT" in p for p in rep.problems)  # in firmware, not the spec


# --- build (needs the SMP stack) -------------------------------------------

smp = pytest.importorskip("smp", reason="SMP stack not installed")


@pytest.fixture(scope="module")
def bound():
    from smpgroup.build import build

    return build(DEMO)


def test_generates_one_entry_per_routed_op(bound):
    assert set(bound.names()) == {
        "info",
        "set_thing",
        "both",
        "both_write",
        "nested",
        "stubbed",
    }
    assert "reserved_thing" not in bound  # never generate an unroutable command


def test_request_encodes_the_right_header(bound):
    req = bound.info()
    assert req.header.group_id == 64
    assert req.header.command_id == 0x0001
    assert req.header.op == smp.header.OP.READ


def test_write_request_carries_its_payload(bound):
    import cbor2

    req = bound.set_thing(name="x", on=True)
    assert req.header.op == smp.header.OP.WRITE
    assert cbor2.loads(req.BYTES[8:]) == {"name": "x", "on": True}


def test_both_ops_get_distinct_schemas(bound):
    import cbor2

    assert cbor2.loads(bound.both().BYTES[8:]) == {}
    w = bound.both_write(on=True)
    assert cbor2.loads(w.BYTES[8:]) == {"on": True}
    assert w.header.op == smp.header.OP.WRITE


def _reply(cls, payload: dict, cmd_id: int, op):
    import cbor2
    from smp.header import Flag, Header, Version

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


def test_response_round_trip(bound):
    r = _reply(
        bound.info.Response,
        {"sn": "SN1", "up": 42, "hw": b"\x01\x02"},
        0x0001,
        smp.header.OP.READ_RSP,
    )
    assert (r.sn, r.up, r.hw) == ("SN1", 42, b"\x01\x02")


def test_nested_map_response(bound):
    r = _reply(
        bound.nested.Response,
        {"top": 1, "health": {"a": 2, "b": 3}},
        0x0004,
        smp.header.OP.READ_RSP,
    )
    assert r.top == 1 and r.health.a == 2 and r.health.b == 3


def test_unknown_key_is_rejected(bound):
    """The contract smpgroup exists to protect: a device field the host does not
    declare must fail loudly, not silently."""
    from pydantic import ValidationError

    with pytest.raises(ValidationError):
        _reply(
            bound.info.Response,
            {"sn": "S", "up": 1, "hw": b"", "surprise": 9},
            0x0001,
            smp.header.OP.READ_RSP,
        )


def test_nested_map_also_forbids_unknown_keys(bound):
    from pydantic import ValidationError

    with pytest.raises(ValidationError):
        _reply(
            bound.nested.Response,
            {"top": 1, "health": {"a": 2, "b": 3, "c": 4}},
            0x0004,
            smp.header.OP.READ_RSP,
        )


def test_optional_field_may_be_omitted():
    import cbor2

    from smpgroup.build import build

    g = build(
        Group(
            group_id=64,
            name="opt",
            commands=(
                Command(
                    1,
                    "begin",
                    (W,),
                    request=(
                        Field("len", T.UINT),
                        Field("sig", T.BSTR, optional=True),
                    ),
                ),
            ),
        )
    )
    assert cbor2.loads(g.begin(len=10).BYTES[8:]) == {"len": 10}
    assert cbor2.loads(g.begin(len=10, sig=b"\x01").BYTES[8:]) == {
        "len": 10,
        "sig": b"\x01",
    }


def test_error_formatting():
    from smpgroup.build import format_error, is_error

    class FakeErr:
        class err:
            rc = 257

    assert is_error(FakeErr())
    assert format_error(DEMO, FakeErr()) == "HW_FAULT (257) -- hardware"


def test_error_from_another_group_is_not_named_from_ours():
    """An `err` map says which group the code belongs to. A code from a stock
    group means nothing in this group's table, and reading it there is how
    os/RTC_NOT_SET (group 0, rc 4) got reported as "UNKNOWN (4)"."""
    from smpgroup.build import format_error

    class Foreign:
        class err:
            group = 0
            rc = 4

    out = format_error(DEMO, Foreign())
    # Unnamed without a table, but honestly scoped: it says whose 4 this is
    # instead of borrowing a meaning from our own table.
    assert out == "UNKNOWN (group 0, rc 4)"
    assert "HW_FAULT" not in out

    named = format_error(
        DEMO, Foreign(), foreign={0: {4: ("RTC_NOT_SET", "clock never set")}}
    )
    assert named == "RTC_NOT_SET (group 0, rc 4) -- clock never set"


def test_foreign_code_is_named_from_the_typed_rc():
    """``smp`` types an error's rc as its own group's IntEnum, so a stock-group
    error names itself even with no table supplied."""
    from enum import IntEnum

    from smpgroup.build import format_error

    class OsRc(IntEnum):
        RTC_NOT_SET = 4

    class Typed:
        class err:
            group = 0
            rc = OsRc.RTC_NOT_SET

    assert format_error(DEMO, Typed()) == "RTC_NOT_SET (group 0, rc 4)"
