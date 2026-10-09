# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT

"""Program a HealthyLink module's ID EEPROM *through the HealthyPi*.

The device is already wired to both slot EEPROMs (I2C3, 0x50 for slot A, 0x51
for slot B), so identifying a module needs nothing but the USB cable that is
already attached: group-64 ``module_eeprom_read``/``module_eeprom_write`` move
raw bytes, and everything about what those bytes *mean* -- the header layout,
the CRC, which module ID is which -- stays here in :mod:`healthypi.hw.eeprom`.
That split is deliberate: a module type that did not exist when the firmware
shipped can be programmed without new firmware.

    from healthypi.hw import eeprom, program

    image = eeprom.create_image(module_id=0x000A, name="GPIO breakout")
    await program.write_image(conn, g, slot=0, image=image)

The firmware handles the 24AA02's 8-byte pages and its 5 ms write cycle, so a
chunk here only has to fit the SMP frame.

Every write is verified by reading the image back, because the failure this
guards against is not a refused write -- it is a write that half-lands and
leaves a module that enumerates as something it is not.
"""

from __future__ import annotations

from dataclasses import dataclass

from .eeprom import EEPROM_SIZE, EepromError, EepromInfo, parse_image

#: Bytes per command. The firmware caps a chunk at 64 (HPI_EEPROM_CHUNK_MAX);
#: 32 also fits comfortably inside the default 256-byte SMP frame.
CHUNK = 32

#: Slot names the CLI and the firmware agree on.
SLOTS = {"a": 0, "b": 1, "0": 0, "1": 1}


class ProgramError(RuntimeError):
    """The device refused an EEPROM operation, or the readback disagreed."""


def resolve_slot(spec: str | int) -> int:
    """``"a"`` / ``"B"`` / ``0`` / ``1`` -> the wire value."""
    if isinstance(spec, int):
        if spec not in (0, 1):
            raise ProgramError(f"no slot {spec}: this board has slots A and B")
        return spec
    key = str(spec).strip().lower()
    if key not in SLOTS:
        raise ProgramError(f"no slot {spec!r}: expected a or b")
    return SLOTS[key]


def slot_name(slot: int) -> str:
    return "AB"[slot]


#: group-64 NOT_READY, which for these two commands means one specific thing.
_NOT_READY = 256


def _group64_code(resp) -> int | None:
    """The group-64 error code in a reply, if that is what this is."""
    from ..smp import catalog

    err = getattr(resp, "err", None)
    if err is None:
        return None
    try:
        if int(getattr(err, "group", -1)) != catalog.GROUP_ID:
            return None
        return int(err.rc)
    except (TypeError, ValueError):
        return None


def _check(resp, what: str, slot: int | None = None):
    """Raise on an SMP error reply; otherwise hand the response back."""
    from ..smp.group64 import fmt_error, is_error

    if not is_error(resp):
        return resp

    if _group64_code(resp) == _NOT_READY:
        where = f"slot {slot_name(slot)}" if slot is not None else "the slot"
        raise ProgramError(
            f"{what}: nothing acknowledged at {where}'s ID EEPROM address "
            f"(0x{0x50 + (slot or 0):02X}). Either no module is seated, or the "
            "one that is has no ID EEPROM answering — the device retries with "
            "the slot powered before reporting this, so a module whose EEPROM "
            "sits behind the load switch has already been given a chance. The "
            "boot log's `slot X: ...` line says what detection saw."
        )
    raise ProgramError(f"{what}: {fmt_error(resp)}")


async def read_image(conn, g, slot: int, *, size: int = EEPROM_SIZE,
                     chunk: int = CHUNK) -> bytes:
    """Read ``size`` bytes of a slot's ID EEPROM."""
    out = bytearray()
    while len(out) < size:
        n = min(chunk, size - len(out))
        resp = _check(
            await conn.request(g.module_eeprom_read(slot=slot, off=len(out), len=n)),
            f"read at 0x{len(out):02X}",
            slot,
        )
        data = bytes(resp.data)
        if not data:
            raise ProgramError(f"slot {slot_name(slot)}: empty reply at 0x{len(out):02X}")
        out += data[:n]
    return bytes(out)


async def write_image(conn, g, slot: int, image: bytes, *, chunk: int = CHUNK,
                      verify: bool = True, progress=None) -> None:
    """Write a full image to a slot, then read it back and compare.

    ``progress(written, total)`` is called after each chunk, if given.
    """
    if len(image) != EEPROM_SIZE:
        raise ProgramError(
            f"an EEPROM image is {EEPROM_SIZE} bytes; this one is {len(image)}"
        )

    off = 0
    while off < len(image):
        piece = image[off : off + chunk]
        _check(
            await conn.request(
                g.module_eeprom_write(slot=slot, off=off, data=piece)
            ),
            f"write at 0x{off:02X}",
            slot,
        )
        off += len(piece)
        if progress is not None:
            progress(off, len(image))

    if not verify:
        return

    back = await read_image(conn, g, slot)
    if back != image:
        bad = next(i for i, (a, b) in enumerate(zip(back, image)) if a != b)
        raise ProgramError(
            f"slot {slot_name(slot)}: readback differs at 0x{bad:02X} "
            f"(wrote 0x{image[bad]:02X}, read 0x{back[bad]:02X}). The module is "
            f"now carrying a partial image -- rerun to finish it."
        )


#: The ID EEPROM address a slot's module is expected to answer at. The A0
#: strap belongs to the HOST, per slot, which is what lets one module work in
#: either -- a module that straps A0 itself answers at the wrong slot.
SLOT_EEPROM_ADDR = (0x50, 0x51)


async def bus_scan(conn, g, slot: int, *, powered: bool = False) -> list[int]:
    """The 7-bit addresses answering on the bus this slot's EEPROM is on."""
    resp = _check(
        await conn.request(g.module_i2c_scan(slot=slot, pwr=powered)),
        f"bus scan (rail {'on' if powered else 'off'})",
        slot,
    )
    return sorted(bytes(resp.addrs))


def explain_scan(slot: int, off: list[int], on: list[int]) -> list[str]:
    """Read a scan result out loud. One of these is the actual fault."""
    seen = set(off) | set(on)
    mine, other = SLOT_EEPROM_ADDR[slot], SLOT_EEPROM_ADDR[1 - slot]
    lines: list[str] = []

    if mine in seen:
        lines.append(
            f"0x{mine:02X} answered — slot {slot_name(slot)}'s EEPROM is "
            "reachable; `hl eeprom dump` should work."
        )
        if mine not in off:
            lines.append(
                "  Only with the rail on: its EEPROM is behind the load "
                "switch, so boot detection (which probes unpowered) can never "
                "find it. The EEPROM belongs on the always-on rail."
            )
        return lines

    if other in seen:
        lines.append(
            f"0x{other:02X} answered but 0x{mine:02X} did not, with the module "
            f"in slot {slot_name(slot)}. Either that is the other slot's "
            "module, or this one straps A0 itself instead of leaving it to the "
            f"host — try `hl eeprom dump --slot {slot_name(1 - slot).lower()}`."
        )
        return lines

    if seen:
        lines.append(
            "Something is on the bus, but not at either slot's EEPROM address "
            f"({', '.join(f'0x{a:02X}' for a in sorted(seen))}). The bus works; "
            "the module's EEPROM is strapped somewhere unexpected."
        )
        return lines

    lines.append("Nothing answered at any address, rail off or on.")
    lines.append(
        "  The two slot EEPROMs are the only devices on this bus, so this is "
        "either no module answering or the bus itself. With a known-good "
        "module in the other slot, a scan that still shows nothing points at "
        "the bus: SCL/SDA continuity to the connector, and the pull-ups."
    )
    return lines


@dataclass(frozen=True)
class Existing:
    """What was on the EEPROM before a program run."""

    raw: bytes
    info: EepromInfo | None   # None when the bytes are not a HealthyLink image
    error: str = ""

    @property
    def blank(self) -> bool:
        """A never-programmed chip: all 0xFF, or all 0x00."""
        return self.info is None and (
            self.raw == b"\xff" * len(self.raw) or self.raw == b"\x00" * len(self.raw)
        )

    @property
    def valid(self) -> bool:
        return self.info is not None and self.info.crc_valid

    def describe(self) -> str:
        if self.info is not None:
            return self.info.describe()
        if self.blank:
            return "blank (never programmed)"
        return self.error or "not a HealthyLink image"


async def read_existing(conn, g, slot: int) -> Existing:
    """Read a slot's EEPROM and say what, if anything, is on it.

    Never raises on unparseable content: reading a chip precisely because it
    is *not* right is the main reason to call this.
    """
    raw = await read_image(conn, g, slot)
    try:
        return Existing(raw=raw, info=parse_image(raw))
    except EepromError as exc:
        return Existing(raw=raw, info=None, error=str(exc))
