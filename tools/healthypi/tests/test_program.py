# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT

"""Programming a module's ID EEPROM through the device.

No hardware and no SMP stack: the device is replaced by a 256-byte bytearray
behind the same two commands the firmware routes, which is enough to exercise
what actually breaks -- chunking, the readback comparison, and refusing an
image that is not 256 bytes.
"""

from __future__ import annotations

import asyncio
from types import SimpleNamespace

import pytest

from healthypi.hw import eeprom, program


class FakeSlot:
    """One slot's EEPROM, answering module_eeprom_read/write."""

    def __init__(self, initial: bytes = b"\xff" * eeprom.EEPROM_SIZE):
        self.mem = bytearray(initial)
        self.writes = 0
        self.short_by = 0   # bytes to drop from each write, to fake a bad chip

    # -- the generated command set ----------------------------------------
    def module_eeprom_read(self, *, slot, off, len):  # noqa: A002 (wire key)
        return SimpleNamespace(kind="read", slot=slot, off=off, n=len)

    def module_eeprom_write(self, *, slot, off, data):
        return SimpleNamespace(kind="write", slot=slot, off=off, data=data)

    def module_power(self, *, slot, on):
        return SimpleNamespace(kind="power", slot=slot, on=on)

    # -- the connection ----------------------------------------------------
    async def request(self, req):
        if req.kind == "read":
            return SimpleNamespace(off=req.off, data=bytes(self.mem[req.off : req.off + req.n]))
        if req.kind == "power":
            return SimpleNamespace(ok=True)
        self.writes += 1
        data = req.data[: len(req.data) - self.short_by]
        self.mem[req.off : req.off + len(data)] = data
        return SimpleNamespace(off=req.off, len=len(data))


def run(coro):
    return asyncio.run(coro)


@pytest.fixture
def dev():
    return FakeSlot()


def test_slot_names_resolve():
    assert program.resolve_slot("a") == 0
    assert program.resolve_slot("B") == 1
    assert program.resolve_slot(1) == 1
    with pytest.raises(program.ProgramError):
        program.resolve_slot("c")
    with pytest.raises(program.ProgramError):
        program.resolve_slot(2)


def test_write_then_read_round_trips(dev):
    image = eeprom.create_image(module_id=0x000A, name="GPIO breakout")
    run(program.write_image(dev, dev, 0, image))
    assert bytes(dev.mem) == image
    assert run(program.read_image(dev, dev, 0)) == image


def test_write_is_chunked_not_one_giant_command(dev):
    """The firmware caps a command at 64 B; a 256-byte image cannot be one."""
    image = eeprom.create_image(module_id=0x000A, name="GPIO breakout")
    run(program.write_image(dev, dev, 0, image, chunk=32))
    assert dev.writes == eeprom.EEPROM_SIZE // 32


def test_readback_mismatch_is_reported_with_its_offset(dev):
    """A chip that swallows part of every write must not read as success."""
    dev.short_by = 1
    image = eeprom.create_image(module_id=0x000A, name="GPIO breakout")
    with pytest.raises(program.ProgramError) as exc:
        run(program.write_image(dev, dev, 0, image))
    assert "readback differs at" in str(exc.value)


def test_wrong_sized_image_is_refused_before_any_write(dev):
    with pytest.raises(program.ProgramError):
        run(program.write_image(dev, dev, 0, b"\x00" * 128))
    assert dev.writes == 0


def test_blank_chip_reads_as_blank(dev):
    found = run(program.read_existing(dev, dev, 0))
    assert found.blank and not found.valid
    assert "blank" in found.describe()


def test_programmed_chip_reads_back_its_identity(dev):
    image = eeprom.create_image(module_id=0x000A, name="GPIO breakout", serial=7)
    run(program.write_image(dev, dev, 0, image))
    found = run(program.read_existing(dev, dev, 0))
    assert found.valid
    assert found.info.module_id == 0x000A
    assert found.info.module_name == "GPIO"
    assert found.info.serial == 7


def test_empty_slot_says_so_instead_of_a_bare_error_code(dev):
    """NOT_READY from these two commands has exactly one meaning."""
    async def refuse(req):
        return SimpleNamespace(err=SimpleNamespace(group=64, rc=256))

    dev.request = refuse
    with pytest.raises(program.ProgramError) as exc:
        run(program.read_image(dev, dev, 0))
    msg = str(exc.value)
    assert "nothing acknowledged" in msg
    assert "0x50" in msg, "the message should name the address that stayed quiet"


def test_other_errors_are_still_reported(dev):
    async def refuse(req):
        return SimpleNamespace(err=SimpleNamespace(group=64, rc=257))

    dev.request = refuse
    with pytest.raises(program.ProgramError):
        run(program.read_image(dev, dev, 0))


def test_gpio_claims_no_interface_bit():
    """Bits 0-7 are exclusive across slots in the firmware's arbiter: a passive
    breakout claiming one would lock a real module out of the other slot."""
    caps = eeprom.DEFAULT_CAPABILITIES[eeprom.MODULE_IDS["GPIO"]]
    assert caps & 0xFF == 0


# --- a reply the client cannot parse ---------------------------------------
#
# Always a firmware bug, and one that used to surface as a TypeError from
# inside smpclient's own error handling -- naming neither the device, the
# command, nor the reply. The transport translates it; these lock that in.


def test_unparseable_reply_names_the_command_and_the_likely_cause():
    import asyncio as _asyncio

    from healthypi.transport import serial_smp

    class Boom:
        async def request(self, req):
            raise TypeError(
                "ValidationError.__new__() missing 1 required positional "
                "argument: 'line_errors'"
            )

    from healthypi.smp.group64 import g

    conn = serial_smp.Connection(client=Boom(), port="/dev/null")
    with pytest.raises(serial_smp.UnparseableReplyError) as exc:
        _asyncio.run(conn.request(g.module_eeprom_read(slot=0, off=0, len=8)))

    msg = str(exc.value)
    assert "group 64 command 83" in msg, msg      # 0x53
    assert "smp_add_cmd_err" in msg, "should point at the usual cause"


def test_an_unrelated_TypeError_is_not_swallowed():
    import asyncio as _asyncio

    from healthypi.transport import serial_smp
    from healthypi.smp.group64 import g

    class Boom:
        async def request(self, req):
            raise TypeError("something else entirely")

    conn = serial_smp.Connection(client=Boom(), port="/dev/null")
    with pytest.raises(TypeError, match="something else entirely"):
        _asyncio.run(conn.request(g.module_eeprom_read(slot=0, off=0, len=8)))
