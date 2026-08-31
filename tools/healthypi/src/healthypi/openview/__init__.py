# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT

"""OpenView -- the Wi-Fi streaming format emitted by the ESP32-C6."""

from .monitor import Stats, monitor_tcp, monitor_udp
from .protocol import (
    EEG_PACKET_LEN,
    HRV_PACKET_LEN,
    PACKET_LEN,
    VERSION_V1,
    VERSION_V2,
    OpenViewError,
    Packet,
    find_sync,
    iter_packets,
    pack,
    peek,
    unpack,
)

__all__ = [
    "EEG_PACKET_LEN",
    "HRV_PACKET_LEN",
    "PACKET_LEN",
    "OpenViewError",
    "Packet",
    "Stats",
    "VERSION_V1",
    "VERSION_V2",
    "find_sync",
    "iter_packets",
    "monitor_tcp",
    "monitor_udp",
    "pack",
    "peek",
    "unpack",
]
