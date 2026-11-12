# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT

"""The update path's error type and exit codes.

A module of its own so that :mod:`~healthypi.fw.update` and
:mod:`~healthypi.fw.esp32c6` can share them without importing each other.

Exit codes of ``healthypi fw update`` (the CLI adds 3 = no device, 4 = device
stack not installed, 130 = interrupted):

====  =====================================================================
0     done, or nothing to do
1     applied, but the device does not report what the bundle carries
2     refused or failed; the message says what is running now
5     ESP32-C6: the new image failed its self-test and the C6 rolled back
6     ESP32-C6: outcome not confirmed in time; the C6 was left powered
7     ESP32-C6: its firmware predates the update path (one USB flash needed)
====  =====================================================================
"""

from __future__ import annotations

EXIT_OK = 0
EXIT_UNVERIFIED = 1
EXIT_FAILED = 2
EXIT_C6_ROLLED_BACK = 5
EXIT_C6_UNCONFIRMED = 6
EXIT_C6_NEEDS_USB_FLASH = 7


class UpdateError(Exception):
    """The update could not proceed. The message says what the device state is.

    ``code`` is the process exit status the CLI uses for it.
    """

    def __init__(self, msg: str, code: int = EXIT_FAILED):
        super().__init__(msg)
        self.code = code
