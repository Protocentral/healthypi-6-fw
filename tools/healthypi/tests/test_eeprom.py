# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT

"""HealthyLink module EEPROM images.

The image is a contract with the M7 arbiter, so the offsets are pinned
byte-by-byte here rather than merely round-tripped: a field that moves silently
would make a module report as a different module.
"""

from __future__ import annotations

import struct

import pytest

from healthypi.hw import eeprom


def test_layout_is_pinned():
    """Offsets, not just round-trip. These are what the firmware reads."""
    img = eeprom.create_image(
        module_id=0x0005,
        name="AI-ACCELERATOR",
        manufacturer="ProtoCentral",
        serial=0x01020304,
        hw_rev_major=2,
        hw_rev_minor=3,
        fw_compat_min=0x0110,
        capabilities=0x00021042,
    )
    assert len(img) == 256
    assert img[0:4] == b"HLNK"
    assert img[4] == 1  # header version
    assert img[5] == 0x60  # header length
    assert struct.unpack_from("<H", img, 6)[0] == 0x0005
    assert img[8] == 2
    assert img[9] == 3  # stack position 0 -> upper nibble clear
    assert struct.unpack_from("<H", img, 0x0A)[0] == 0x0110
    assert struct.unpack_from("<I", img, 0x0C)[0] == 0x00021042
    assert img[0x10:0x1E] == b"AI-ACCELERATOR"
    assert img[0x30:0x3C] == b"ProtoCentral"
    assert struct.unpack_from("<I", img, 0x50)[0] == 0x01020304
    assert struct.unpack_from("<H", img, 0xFE)[0] == eeprom.crc16_ccitt(img[:254])


def test_roundtrip():
    img = eeprom.create_image(module_id=0x0001, name="EEG-8CH", serial=10001)
    info = eeprom.parse_image(img)
    assert info.crc_valid
    assert info.module_id == 0x0001
    assert info.module_name == "EEG-8CH"
    assert info.name == "EEG-8CH"
    assert info.manufacturer == "ProtoCentral"
    assert info.serial == 10001


def test_default_capabilities_applied():
    """A known module with no explicit capabilities gets the catalog's set --
    the EEG module must claim SPI4, or the arbiter will not wire it up."""
    info = eeprom.parse_image(eeprom.create_image(module_id=0x0001, name="EEG-8CH"))
    assert "REQUIRES_SPI4" in info.capability_names()
    assert "REALTIME_STREAM" in info.capability_names()


def test_explicit_capabilities_win():
    img = eeprom.create_image(module_id=0x0001, name="EEG-8CH", capabilities=0x40)
    assert eeprom.parse_image(img).capabilities == 0x40


@pytest.mark.parametrize("pos", [0, 1, 2, 3])
def test_stack_position_in_the_high_nibble(pos):
    img = eeprom.create_image(
        module_id=0x0003, name="TRIGGER-IO", hw_rev_minor=7, stack_position=pos
    )
    info = eeprom.parse_image(img)
    assert info.stack_position == pos
    assert info.hw_rev_minor == 7  # the revision survives the packing
    assert img[9] == (7 | (pos << 4))


def test_stack_position_out_of_range():
    with pytest.raises(eeprom.EepromError, match="out of range"):
        eeprom.create_image(module_id=1, name="x", stack_position=4)


def test_long_strings_truncated_not_overflowed():
    img = eeprom.create_image(module_id=1, name="N" * 60, manufacturer="M" * 60)
    assert len(img) == 256
    info = eeprom.parse_image(img)
    assert len(info.name) == 31 and len(info.manufacturer) == 31


def test_bad_magic_raises():
    with pytest.raises(eeprom.EepromError, match="invalid magic"):
        eeprom.parse_image(b"XXXX" + b"\x00" * 252)


def test_short_image_raises():
    with pytest.raises(eeprom.EepromError, match="too short"):
        eeprom.parse_image(b"HLNK" + b"\x00" * 100)


def test_bad_crc_is_reported_not_raised():
    """Reading a mis-programmed EEPROM to find out why it is rejected is the
    main reason to run this, so a bad CRC must not be fatal."""
    img = bytearray(eeprom.create_image(module_id=1, name="EEG-8CH"))
    img[0xFE] ^= 0xFF
    info = eeprom.parse_image(bytes(img))
    assert not info.crc_valid
    assert info.crc_stored != info.crc_calculated
    assert "INVALID" in info.describe()


def test_crc16_vectors():
    """CRC-16-CCITT, poly 0x1021, init 0xFFFF, non-reflected."""
    assert eeprom.crc16_ccitt(b"") == 0xFFFF
    assert eeprom.crc16_ccitt(b"123456789") == 0x29B1  # the standard check value


def test_resolve_module_id_forms():
    assert eeprom.resolve_module_id("EEG-8CH") == 1
    assert eeprom.resolve_module_id("0x0005") == 5
    assert eeprom.resolve_module_id("9") == 9
    assert eeprom.resolve_module_id(7) == 7


def test_resolve_capabilities_forms():
    got = eeprom.resolve_capabilities(["REQUIRES_SPI4", "0x40", "2"])
    assert got == (0x01 | 0x40 | 0x02)
    assert eeprom.resolve_capabilities(None) == 0


def test_build_stack_positions_and_addresses():
    entries = eeprom.build_stack(["EEG-8CH", "TRIGGER-IO:Trig", "0x0005"],
                                base_serial=500)
    assert [e.index for e in entries] == [0, 1, 2]
    assert [e.serial for e in entries] == [500, 501, 502]
    assert entries[1].name == "Trig"
    for e in entries:
        assert eeprom.parse_image(e.image).stack_position == e.index
        assert f"0x{e.module_id:04X}" in e.filename


def test_stack_depth_limited():
    with pytest.raises(eeprom.EepromError, match="stack holds"):
        eeprom.build_stack(["EEG-8CH"] * 5)


def test_describe_mentions_the_i2c_address():
    info = eeprom.parse_image(
        eeprom.create_image(module_id=1, name="EEG-8CH", stack_position=2)
    )
    assert "0x52" in info.describe()  # 0x50 + position
