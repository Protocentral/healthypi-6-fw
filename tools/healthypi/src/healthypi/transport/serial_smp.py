# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT

"""CDC 1 (control) transport: connect, autodetect, survive a reset.

The device enumerates two CDC ports from one cable. CDC 0 streams ``.HP6`` and
answers nothing; CDC 1 speaks SMP. That asymmetry is the only reliable way to
tell them apart -- port numbering differs by OS and by how many devices are
plugged in -- so autodetect works by asking each candidate for an ``os echo``
and keeping whichever replies.
"""

from __future__ import annotations

import asyncio
import glob
import sys
from dataclasses import dataclass

try:
    from smpclient import SMPClient
    from smpclient.requests.os_management import EchoWrite
    from smpclient.transport.serial import SMPSerialTransport
except ImportError as exc:  # pragma: no cover
    raise ImportError(
        "the device stack is not installed. Run: pip install 'healthypi[device]'"
    ) from exc

#: Max encoded SMP frame. Must not exceed the device's MCUmgr UART MTU -- a
#: larger host frame is **silently dropped** by the device, not rejected. 256 is
#: the value the update path was validated with on v5 hardware.
DEFAULT_FRAME_SIZE = 256
DEFAULT_BAUD = 115200

#: Serial device patterns worth probing, by platform.
_PATTERNS = {
    "darwin": ("/dev/cu.usbmodem*",),
    "linux": ("/dev/ttyACM*", "/dev/ttyUSB*"),
    "win32": (),  # COM ports are enumerated, not globbed -- see candidates()
}


class NoDeviceError(RuntimeError):
    """No port answered SMP."""


@dataclass(slots=True)
class Connection:
    """A live CDC 1 session."""

    client: SMPClient
    port: str
    #: The transport backing ``client``. Kept because the group-64 upload path
    #: sizes its chunks from ``transport.max_unencoded_size``, and reaching into
    #: the client for it would mean touching a private attribute.
    transport: SMPSerialTransport | None = None

    async def request(self, req, timeout_s: float | None = None):
        if timeout_s is None:
            return await self.client.request(req)
        return await asyncio.wait_for(self.client.request(req), timeout=timeout_s)


def candidates() -> list[str]:
    """Serial ports that could be a HealthyPi, most likely first."""
    if sys.platform == "win32":  # pragma: no cover - platform specific
        try:
            from serial.tools import list_ports

            return [p.device for p in list_ports.comports()]
        except ImportError:
            return []
    found: list[str] = []
    for pattern in _PATTERNS.get(sys.platform, ("/dev/ttyACM*",)):
        found.extend(sorted(glob.glob(pattern)))
    # CDC 1 is the higher-numbered interface on the same device, so probing the
    # highest first usually hits on the first try.
    return sorted(set(found), reverse=True)


def make_transport(
    *, baud: int = DEFAULT_BAUD, frame_size: int = DEFAULT_FRAME_SIZE
) -> SMPSerialTransport:
    """Build the transport with the line geometry smpclient requires.

    ``max_smp_encoded_frame_size`` must equal ``line_length * line_buffers``;
    deriving it here rather than exposing both means a caller cannot produce the
    mismatched pair that smpclient warns about and the device silently drops.
    """
    return SMPSerialTransport(
        baudrate=baud,
        max_smp_encoded_frame_size=frame_size,
        line_length=frame_size // 2,
        line_buffers=2,
    )


async def _answers_smp(port: str, *, baud: int, timeout_s: float) -> bool:
    try:
        async with SMPClient(make_transport(baud=baud), port) as client:
            resp = await asyncio.wait_for(
                client.request(EchoWrite(d="hpi")), timeout=timeout_s
            )
            return getattr(resp, "r", None) == "hpi"
    except Exception:
        return False


async def autodetect(*, baud: int = DEFAULT_BAUD, timeout_s: float = 2.0) -> str:
    """Return the first port that answers ``os echo``."""
    ports = candidates()
    if not ports:
        raise NoDeviceError(
            "no serial ports found. Is the device plugged in?"
        )
    for port in ports:
        if await _answers_smp(port, baud=baud, timeout_s=timeout_s):
            return port
    raise NoDeviceError(
        "no port answered SMP. Tried:\n  "
        + "\n  ".join(ports)
        + "\nCDC 0 (the sample stream) never answers -- make sure the device is "
          "not in Transfer Mode or recovery, and pass --port to skip detection."
    )


async def connect(
    port: str | None = None,
    *,
    baud: int = DEFAULT_BAUD,
    frame_size: int = DEFAULT_FRAME_SIZE,
    timeout_s: float = 2.5,
) -> Connection:
    """Open CDC 1, autodetecting the port when not given.

    The caller owns the connection: ``await conn.client.__aexit__(...)`` or use
    :func:`session`.
    """
    resolved = port or await autodetect(baud=baud)
    transport = make_transport(baud=baud, frame_size=frame_size)
    client = SMPClient(transport, resolved, timeout_s=timeout_s)
    await client.__aenter__()
    return Connection(client=client, port=resolved, transport=transport)


class session:
    """``async with session(port) as conn:``"""

    def __init__(self, port: str | None = None, **kw):
        self._port = port
        self._kw = kw
        self._conn: Connection | None = None

    async def __aenter__(self) -> Connection:
        self._conn = await connect(self._port, **self._kw)
        return self._conn

    async def __aexit__(self, exc_type, exc, tb) -> None:
        if self._conn is not None:
            await self._conn.client.__aexit__(exc_type, exc, tb)
