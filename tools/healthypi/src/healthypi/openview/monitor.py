# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT

"""Watch the Wi-Fi stream: rate, sequence gaps and loss.

Replaces three separate scripts (a TCP monitor, a TCP benchmark and a UDP
receiver) that each re-implemented the same parse-and-count loop. They differed
only in transport and in how much they printed, so those are options here.

Sequence numbers make loss measurable rather than estimated: every gap in
``seq`` is exactly the number of packets that went missing.
"""

from __future__ import annotations

import socket
import time
from dataclasses import dataclass, field

from .protocol import PACKET_LEN, OpenViewError, Packet, find_sync, unpack

TCP_PORT = 5000
UDP_PORT = 5001
DEFAULT_HOST = "192.168.4.1"  # the co-processor's SoftAP address


@dataclass(slots=True)
class Stats:
    packets: int = 0
    bytes_received: int = 0
    resync_bytes: int = 0
    bad_packets: int = 0
    gaps: int = 0
    lost: int = 0
    first_seq: int | None = None
    last_seq: int | None = None
    started: float = field(default_factory=time.monotonic)
    ended: float | None = None

    def observe(self, p: Packet) -> None:
        self.packets += 1
        if self.first_seq is None:
            self.first_seq = p.seq
        elif self.last_seq is not None and p.seq != self.last_seq + 1:
            if p.seq > self.last_seq:
                self.gaps += 1
                self.lost += p.seq - self.last_seq - 1
        self.last_seq = p.seq

    @property
    def elapsed(self) -> float:
        return (self.ended or time.monotonic()) - self.started

    @property
    def rate_hz(self) -> float:
        return self.packets / self.elapsed if self.elapsed else 0.0

    @property
    def expected(self) -> int:
        if self.first_seq is None or self.last_seq is None:
            return 0
        return self.last_seq - self.first_seq + 1

    @property
    def loss_pct(self) -> float:
        return 100.0 * self.lost / self.expected if self.expected else 0.0

    def summary(self) -> dict:
        return {
            "packets": self.packets,
            "bytes": self.bytes_received,
            "seconds": round(self.elapsed, 2),
            "rate_hz": round(self.rate_hz, 1),
            "expected": self.expected,
            "lost": self.lost,
            "gaps": self.gaps,
            "loss_pct": round(self.loss_pct, 3),
            "bad_packets": self.bad_packets,
            "resync_bytes": self.resync_bytes,
        }


class _Decoder:
    """Incremental packet extraction with resync."""

    def __init__(self, stats: Stats):
        self.buf = bytearray()
        self.stats = stats

    def feed(self, data: bytes):
        self.buf += data
        self.stats.bytes_received += len(data)
        while True:
            idx = find_sync(self.buf)
            if idx < 0:
                if len(self.buf) > 1:
                    self.stats.resync_bytes += len(self.buf) - 1
                    del self.buf[:-1]
                return
            if idx:
                self.stats.resync_bytes += idx
                del self.buf[:idx]
            if len(self.buf) < PACKET_LEN:
                return
            try:
                pkt = unpack(bytes(self.buf))
            except OpenViewError:
                self.stats.bad_packets += 1
                del self.buf[:1]
                continue
            del self.buf[:PACKET_LEN]
            self.stats.observe(pkt)
            yield pkt


def monitor_tcp(
    host: str = DEFAULT_HOST,
    port: int = TCP_PORT,
    duration: float = 30.0,
    on_packet=None,
) -> Stats:
    stats = Stats()
    dec = _Decoder(stats)
    deadline = time.monotonic() + duration
    with socket.create_connection((host, port), timeout=5.0) as sock:
        sock.settimeout(1.0)
        while time.monotonic() < deadline:
            try:
                data = sock.recv(8192)
            except socket.timeout:
                continue
            if not data:
                break
            for pkt in dec.feed(data):
                if on_packet:
                    on_packet(pkt)
    stats.ended = time.monotonic()
    return stats


def monitor_udp(
    port: int = UDP_PORT, duration: float = 30.0, on_packet=None
) -> Stats:
    stats = Stats()
    dec = _Decoder(stats)
    deadline = time.monotonic() + duration
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind(("", port))
        sock.settimeout(1.0)
        while time.monotonic() < deadline:
            try:
                data, _addr = sock.recvfrom(2048)
            except socket.timeout:
                continue
            # Each datagram is a whole packet, but reuse the decoder so a
            # coalesced or truncated datagram is handled the same way.
            for pkt in dec.feed(data):
                if on_packet:
                    on_packet(pkt)
    stats.ended = time.monotonic()
    return stats
