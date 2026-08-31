# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT

"""HealthyLink module identification EEPROM (24AA02, 256 B at I2C 0x50).

Every HealthyLink module carries one. The M7's arbiter reads it to learn what
the module is and which interfaces it needs to claim, so the image here is the
contract between a module's hardware and
``app_m7/src/healthylink/`` -- see ``docs/HEALTHYLINK.md``.

Layout, little-endian, 256 bytes exactly::

    0x00  4  magic "HLNK"
    0x04  1  header version (1)
    0x05  1  header length (0x60)
    0x06  2  module id
    0x08  1  hw rev major
    0x09  1  hw rev minor -- low nibble rev, HIGH nibble stack position
    0x0A  2  minimum compatible firmware version
    0x0C  4  capability bits
    0x10 32  name, NUL-padded ASCII
    0x30 32  manufacturer, NUL-padded ASCII
    0x50  4  serial number
    0x54 12  reserved
    0x60 32  config blob
    0x80 126 extended blob
    0xFE  2  CRC-16-CCITT over [0x00, 0xFE)

**There is no authentication.** The CRC catches a corrupt read, not a forged
module -- anyone can mint an image the arbiter accepts. That is a deliberate
consequence of an open-hardware expansion bus, not an oversight, but do not
build a trust decision on the contents of this EEPROM.

Writing an image to real hardware is out of scope for this module: it needs an
FT232H, a Raspberry Pi's I2C bus, a CH341A or a Bus Pirate, and the vendor tool
that comes with whichever you have. ``hpi hl eeprom generate`` produces the
256-byte file; program it with e.g. ``ch341eeprom -w eeprom.bin`` or
``i2ctransfer`` on a Pi. (The predecessor script documented a ``program``
subcommand with five wiring diagrams; it was never implemented.)
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

EEPROM_SIZE = 256
EEPROM_MAGIC = b"HLNK"
HEADER_VERSION = 0x01
HEADER_SIZE = 0x60

_HEADER_STRUCT = "<4sBBHBBHI32s32sI12s"

#: Each stacked module gets +2 on its I2C address; 4 deep is the mechanical limit.
STACK_ADDRESS_INCREMENT = 0x02
MAX_STACK_DEPTH = 4

MODULE_IDS: dict[str, int] = {
    "INVALID": 0x0000,
    "EEG-8CH": 0x0001,
    "EMG-4CH": 0x0002,
    "TRIGGER-IO": 0x0003,
    "CAN-INTERFACE": 0x0004,
    "AI-ACCELERATOR": 0x0005,
    "HIGH-RES-ADC": 0x0006,
    "STIM-OUTPUT": 0x0007,
    "SYNC-MASTER": 0x0008,
    "GSR-RESPIRATION": 0x0009,
}

CAPABILITIES: dict[str, int] = {
    "REQUIRES_SPI4": 0x01,
    "REQUIRES_SPI6": 0x02,
    "REQUIRES_USART2": 0x04,
    "REQUIRES_FDCAN": 0x08,
    "REQUIRES_I2C1": 0x10,
    "REQUIRES_ADC": 0x20,
    "REQUIRES_GPIO": 0x40,
    "REQUIRES_PWM": 0x80,
    "HAS_EXT_CLK": 0x100,
    "HAS_PWR_SWITCH": 0x200,
    "HOT_PLUGGABLE": 0x400,
    "DMA_CAPABLE": 0x800,
    "REALTIME_STREAM": 0x1000,
    "POWER_LOW": 0x00000,
    "POWER_MED": 0x10000,
    "POWER_HIGH": 0x20000,
}

DEFAULT_CAPABILITIES: dict[int, int] = {
    0x0001: CAPABILITIES["REQUIRES_SPI4"]
    | CAPABILITIES["REQUIRES_GPIO"]
    | CAPABILITIES["REALTIME_STREAM"]
    | CAPABILITIES["POWER_LOW"],
    0x0002: CAPABILITIES["REQUIRES_SPI4"]
    | CAPABILITIES["REQUIRES_GPIO"]
    | CAPABILITIES["REALTIME_STREAM"]
    | CAPABILITIES["POWER_LOW"],
    0x0003: CAPABILITIES["REQUIRES_GPIO"] | CAPABILITIES["POWER_LOW"],
    0x0004: CAPABILITIES["REQUIRES_FDCAN"] | CAPABILITIES["POWER_MED"],
    0x0005: CAPABILITIES["REQUIRES_SPI6"]
    | CAPABILITIES["REQUIRES_GPIO"]
    | CAPABILITIES["DMA_CAPABLE"]
    | CAPABILITIES["POWER_HIGH"],
}


class EepromError(ValueError):
    """The image is not a valid HealthyLink EEPROM."""


def crc16_ccitt(data: bytes) -> int:
    """CRC-16-CCITT, polynomial 0x1021, init 0xFFFF, non-reflected.

    Implemented directly rather than via ``crcmod``: it is nine lines, it runs
    once per 256-byte image, and an optional dependency that silently changes
    the checksum if absent is a worse trade than the loop.
    """
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else (crc << 1)
            crc &= 0xFFFF
    return crc


def resolve_module_id(spec: str | int) -> int:
    """Accept a name (``EEG-8CH``), ``0x0001`` or a decimal string."""
    if isinstance(spec, int):
        return spec
    if spec in MODULE_IDS:
        return MODULE_IDS[spec]
    return int(spec, 0)


def resolve_capabilities(specs) -> int:
    """OR together capability names, ``0x…`` literals or decimal strings."""
    bits = 0
    for cap in specs or ():
        if isinstance(cap, int):
            bits |= cap
        elif cap in CAPABILITIES:
            bits |= CAPABILITIES[cap]
        else:
            bits |= int(cap, 0)
    return bits


def create_image(
    module_id: int,
    name: str,
    manufacturer: str = "ProtoCentral",
    serial: int = 1,
    *,
    hw_rev_major: int = 1,
    hw_rev_minor: int = 0,
    fw_compat_min: int = 0x0110,
    capabilities: int = 0,
    config: bytes = b"",
    extended: bytes = b"",
    stack_position: int = 0,
) -> bytes:
    """Build a complete 256-byte EEPROM image."""
    if not 0 <= stack_position < MAX_STACK_DEPTH:
        raise EepromError(
            f"stack position {stack_position} out of range (0-{MAX_STACK_DEPTH - 1})"
        )

    if capabilities == 0 and module_id in DEFAULT_CAPABILITIES:
        capabilities = DEFAULT_CAPABILITIES[module_id]

    name_bytes = name.encode("ascii")[:31].ljust(32, b"\x00")
    manufacturer_bytes = manufacturer.encode("ascii")[:31].ljust(32, b"\x00")
    config_bytes = config[:32].ljust(32, b"\x00")
    extended_bytes = extended[:126].ljust(126, b"\x00")

    # Stack position rides in the upper nibble of hw_rev_minor.
    if stack_position > 0:
        hw_rev_minor = (hw_rev_minor & 0x0F) | ((stack_position & 0x0F) << 4)

    header = struct.pack(
        _HEADER_STRUCT,
        EEPROM_MAGIC,
        HEADER_VERSION,
        HEADER_SIZE,
        module_id,
        hw_rev_major,
        hw_rev_minor,
        fw_compat_min,
        capabilities,
        name_bytes,
        manufacturer_bytes,
        serial,
        b"\x00" * 12,
    )
    if len(header) != 96:  # pragma: no cover -- struct format is fixed
        raise EepromError(f"header size mismatch: {len(header)} != 96")

    image = header + config_bytes + extended_bytes
    image += struct.pack("<H", crc16_ccitt(image))
    if len(image) != EEPROM_SIZE:  # pragma: no cover
        raise EepromError(f"image size {len(image)} != {EEPROM_SIZE}")
    return image


@dataclass
class EepromInfo:
    magic: str
    version: int
    header_len: int
    module_id: int
    hw_rev_major: int
    hw_rev_minor: int
    stack_position: int
    fw_compat_min: int
    capabilities: int
    name: str
    manufacturer: str
    serial: int
    crc_stored: int
    crc_calculated: int
    crc_valid: bool
    config: bytes = b""
    extended: bytes = b""

    @property
    def module_name(self) -> str:
        """The catalog name for :attr:`module_id`, or ``?`` if unknown."""
        for k, v in MODULE_IDS.items():
            if v == self.module_id:
                return k
        return "?"

    def capability_names(self) -> list[str]:
        return [n for n, bit in CAPABILITIES.items() if bit and self.capabilities & bit]

    def describe(self) -> str:
        lines = [
            f"module id      0x{self.module_id:04X} ({self.module_name})",
            f"name           {self.name}",
            f"manufacturer   {self.manufacturer}",
            f"hw revision    {self.hw_rev_major}.{self.hw_rev_minor}",
            f"stack position {self.stack_position} "
            f"({'base' if self.stack_position == 0 else 'stacked'}, "
            f"I2C 0x{0x50 + self.stack_position:02X})",
            f"fw compat min  0x{self.fw_compat_min:04X}",
            f"serial         {self.serial}",
            f"capabilities   0x{self.capabilities:08X}"
            + (f"  {', '.join(self.capability_names())}" if self.capability_names() else ""),
            f"crc-16         0x{self.crc_stored:04X} "
            f"({'OK' if self.crc_valid else f'INVALID, computed 0x{self.crc_calculated:04X}'})",
        ]
        if any(self.config):
            lines.append(f"config         {self.config.hex()}")
        return "\n".join(lines)

    def to_dict(self) -> dict:
        d = {
            k: v
            for k, v in self.__dict__.items()
            if k not in ("config", "extended")
        }
        d["module_name"] = self.module_name
        d["capability_names"] = self.capability_names()
        return d


def parse_image(data: bytes) -> EepromInfo:
    """Parse a 256-byte image. Raises :class:`EepromError` on magic mismatch.

    A bad CRC is reported in :attr:`EepromInfo.crc_valid`, not raised -- reading
    a mis-programmed EEPROM to find out *why* it is rejected is the main reason
    to run this at all.
    """
    if len(data) < EEPROM_SIZE:
        raise EepromError(f"EEPROM data too short: {len(data)} bytes")

    magic = data[0:4]
    if magic != EEPROM_MAGIC:
        raise EepromError(
            f"invalid magic {magic.hex()} (expected {EEPROM_MAGIC.hex()}) — "
            "this is not a HealthyLink EEPROM image"
        )

    header = struct.unpack(_HEADER_STRUCT, data[0:96])
    stored_crc = struct.unpack("<H", data[254:256])[0]
    calculated_crc = crc16_ccitt(data[0:254])

    hw_rev_minor_raw = header[5]
    return EepromInfo(
        magic=header[0].decode("ascii"),
        version=header[1],
        header_len=header[2],
        module_id=header[3],
        hw_rev_major=header[4],
        hw_rev_minor=hw_rev_minor_raw & 0x0F,
        stack_position=(hw_rev_minor_raw >> 4) & 0x0F,
        fw_compat_min=header[6],
        capabilities=header[7],
        name=header[8].rstrip(b"\x00").decode("ascii", "replace"),
        manufacturer=header[9].rstrip(b"\x00").decode("ascii", "replace"),
        serial=header[10],
        crc_stored=stored_crc,
        crc_calculated=calculated_crc,
        crc_valid=stored_crc == calculated_crc,
        config=data[96:128],
        extended=data[128:254],
    )


@dataclass
class StackEntry:
    """One module's place in a stack, as produced by :func:`build_stack`."""

    index: int
    module_id: int
    name: str
    serial: int
    image: bytes
    filename: str = field(default="")

    def __post_init__(self) -> None:
        if not self.filename:
            self.filename = f"stack_{self.index:02d}_0x{self.module_id:04X}.bin"


def build_stack(specs: list[str], *, base_serial: int = 10000) -> list[StackEntry]:
    """Build images for a stack from ``ID[:Name[:Manufacturer[:Serial]]]`` specs.

    Position in the list *is* the stack position, which is what sets both the
    encoded nibble and the I2C address the module must be strapped to.
    """
    if len(specs) > MAX_STACK_DEPTH:
        raise EepromError(
            f"{len(specs)} modules requested, the stack holds {MAX_STACK_DEPTH}"
        )
    out: list[StackEntry] = []
    for i, spec in enumerate(specs):
        parts = spec.split(":")
        module_id = resolve_module_id(parts[0])
        name = parts[1] if len(parts) > 1 else f"Module {module_id:04X}"
        manufacturer = parts[2] if len(parts) > 2 else "ProtoCentral"
        serial = int(parts[3]) if len(parts) > 3 else base_serial + i
        out.append(
            StackEntry(
                index=i,
                module_id=module_id,
                name=name,
                serial=serial,
                image=create_image(
                    module_id=module_id,
                    name=name,
                    manufacturer=manufacturer,
                    serial=serial,
                    stack_position=i,
                ),
            )
        )
    return out
