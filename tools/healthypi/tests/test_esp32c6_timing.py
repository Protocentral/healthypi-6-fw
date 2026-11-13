# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT

"""Pins found on the bench (2026-11-12) for the ESP32-C6 update path.

- The M7 must wait for VALID longer than the C6's own self-test window, and
  the host must wait longer than the M7, or an image the C6 rolls back is
  reported as "unconfirmed" (exit 6) instead of "rolled back" (exit 5).
- A corrupt bundle is refused before anything is written: exit 2, not 1.
- A serial write the OS will not complete must end as a timeout, not hang.
"""

from __future__ import annotations

import re
import types
from pathlib import Path

import pytest

from healthypi.fw import esp32c6

ROOT = Path(__file__).resolve().parents[3]
SERVICE = ROOT / "app_m7" / "src" / "services" / "esp32_update_service.c"

#: healthybridge-esp32 CONFIG_HB_OTA_SELFTEST_TIMEOUT_S (its sdkconfig default).
C6_SELFTEST_WINDOW_S = 60


def _define_ms(name: str) -> int:
    m = re.search(rf"#define\s+{name}\s+(\d+)U", SERVICE.read_text())
    assert m, f"{name} not found in {SERVICE}"
    return int(m.group(1))


def test_m7_waits_for_valid_longer_than_the_c6_decides():
    assert _define_ms("ESPFW_VALID_MS") / 1000 > C6_SELFTEST_WINDOW_S


def test_host_waits_longer_than_the_m7():
    m7_s = (_define_ms("ESPFW_REBOOT_MS") + _define_ms("ESPFW_VALID_MS")) / 1000
    assert esp32c6.OUTCOME_BUDGET_S > m7_s


def test_corrupt_bundle_exits_2(tmp_path, capsys):
    from healthypi.cli.main import _bundle_or_fail

    bad = tmp_path / "not-a-bundle.zip"
    bad.write_bytes(b"this is not a zip")
    bundle, code = _bundle_or_fail(types.SimpleNamespace(bundle=str(bad)))
    assert bundle is None
    assert code == 2


def test_stalled_write_is_a_timeout():
    serial_smp = pytest.importorskip("healthypi.transport.serial_smp")
    from serial import SerialTimeoutException

    class Client:
        async def request(self, req, timeout_s=None):
            raise SerialTimeoutException("Write timeout")

    import asyncio

    conn = serial_smp.Connection(client=Client(), port="/dev/null")
    with pytest.raises(TimeoutError):
        asyncio.run(conn.request(object(), timeout_s=1.0))


def test_transport_sets_a_write_timeout():
    serial_smp = pytest.importorskip("healthypi.transport.serial_smp")
    src = Path(serial_smp.__file__).read_text()
    assert "write_timeout=WRITE_TIMEOUT_S" in src
    assert 0 < serial_smp.WRITE_TIMEOUT_S <= 10
