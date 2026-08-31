# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT

"""FROZEN BASELINE -- do not edit, do not import outside tests.

A verbatim copy of the hand-written group-64 classes from
the updater's private ``smp_g64.py`` as it stood when the update path was
validated on v5 hardware (July 2026: prod signed boot, suite 16/0/1, a full
bundle cycle with hash-verified install).

``test_group64.py`` asserts that the classes generated from
``healthypi.smp.catalog`` still put **the same bytes on the wire** as these. That
is what lets the generated set replace hand-written code without re-running the
hardware campaign -- and what will catch it if a future catalog edit silently
changes an encoding.
"""

from __future__ import annotations

import sys
from typing import Any

try:
    from smp import error as smperror
    from smp.message import ReadRequest, ReadResponse, WriteRequest, WriteResponse
except ImportError:
    sys.exit("smpclient not installed. Run: pip install smpclient")

GROUP_ID = 64


# smp's ErrorV1 carries no _GROUP_ID, and _MessageBase.loads() dereferences
# cls._GROUP_ID before decoding — binding the raw class raises AttributeError
# instead of reporting the device's error. That path is taken whenever the device
# answers in the legacy form, which for group 64 mostly means "this command ID
# does not exist" (MGMT_ERR_ENOTSUP) — so it stayed hidden until someone sent an
# unknown command, and then hid the very message they needed.
class HpiErrorV1(smperror.ErrorV1):
    _GROUP_ID = GROUP_ID
    _COMMAND_ID = 0


class HpiErrorV2(smperror.ErrorV2[int]):
    _GROUP_ID = GROUP_ID
    _COMMAND_ID = 0


def is_error(resp: Any) -> bool:
    return type(resp).__name__.endswith(("ErrorV1", "ErrorV2"))


def fmt_error(resp: Any) -> str:
    """ErrorV1 has a flat `rc`; ErrorV2 nests it under `err.rc`/`err.group`."""
    if hasattr(resp, "rc") and not hasattr(resp, "err"):
        return f"rc={resp.rc}{_hint(int(resp.rc))}"
    if hasattr(resp, "err") and resp.err is not None:
        return (f"err.rc={resp.err.rc} err.group={resp.err.group}"
                f"{_hint(int(resp.err.rc))}")
    try:
        return f"raw={resp.model_dump(exclude={'header', 'smp_data'})}"
    except Exception:  # noqa: BLE001
        return repr(resp)


# enum hpi_mgmt_err. A raw number is useless to whoever is holding the board.
_ERR_TEXT = {
    256: "NOT_READY",
    257: "HW_FAULT",
    258: "CHANNEL_NOT_AVAILABLE",
    259: "INSUFFICIENT_STORAGE",
    260: "TRANSFER_INVALID",
    261: "VERSION_MISMATCH",
    264: "LOCK_CHALLENGE_MISSING",
    265: "LOCK_HMAC_INVALID",
    266: "LOCK_EXPIRED",
    267: "NO_MEDIA",
    268: "IMAGE_INVALID (bad digest or signature)",
    269: "IMAGE_TOO_LARGE",
    270: "BUSY (an update is already in flight)",
    271: "IMAGE_NOT_M4 (verifies, but is not an M4 image)",
}


def _hint(code: int) -> str:
    text = _ERR_TEXT.get(code)
    return f" ({text})" if text else ""


# --- 0x0031 hpi/fw_versions ------------------------------------------------

class _FwVersionsReq(ReadRequest):
    _GROUP_ID = GROUP_ID
    _COMMAND_ID = 0x0031


class _FwVersionsResp(ReadResponse):
    _GROUP_ID = GROUP_ID
    _COMMAND_ID = 0x0031
    m7fw: str
    m4fw: str
    espfw: str
    # HealthyLink slot A/B module firmware. Always "" today (the module contract
    # has no version field yet) but the device does emit the keys, and
    # extra="forbid" means omitting them here would reject every reply.
    mod_a_fw: str
    mod_b_fw: str


class FwVersions(_FwVersionsReq):
    _Response = _FwVersionsResp
    _ErrorV1 = HpiErrorV1
    _ErrorV2 = HpiErrorV2


# --- 0x0021 hpi/stream_stop ------------------------------------------------

class _StreamStopReq(WriteRequest):
    _GROUP_ID = GROUP_ID
    _COMMAND_ID = 0x0021


class _StreamStopResp(WriteResponse):
    _GROUP_ID = GROUP_ID
    _COMMAND_ID = 0x0021


class StreamStop(_StreamStopReq):
    _Response = _StreamStopResp
    _ErrorV1 = HpiErrorV1
    _ErrorV2 = HpiErrorV2


# --- 0x00A0-0x00A4 M4 firmware update --------------------------------------

class _M4FwBeginReq(WriteRequest):
    _GROUP_ID = GROUP_ID
    _COMMAND_ID = 0x00A0
    len: int
    sha: bytes
    # ECDSA-P256 over `sha`, raw r||s. Omitted from the CBOR when None
    # (_MessageBase dumps with exclude_unset), so an unsigned upload is
    # byte-identical to what older firmware expects. Whether the device ACCEPTS
    # one is its decision (CONFIG_HPI_M4_UPDATE_REQUIRE_SIGNATURE), reported by
    # M4FwStatus.sig.
    sig: bytes | None = None


class _M4FwBeginResp(WriteResponse):
    _GROUP_ID = GROUP_ID
    _COMMAND_ID = 0x00A0


class M4FwBegin(_M4FwBeginReq):
    _Response = _M4FwBeginResp
    _ErrorV1 = HpiErrorV1
    _ErrorV2 = HpiErrorV2


class _M4FwChunkReq(WriteRequest):
    _GROUP_ID = GROUP_ID
    _COMMAND_ID = 0x00A1
    off: int
    data: bytes


class _M4FwChunkResp(WriteResponse):
    _GROUP_ID = GROUP_ID
    _COMMAND_ID = 0x00A1
    off: int          # next expected offset, so a client can resync without a status call


class M4FwChunk(_M4FwChunkReq):
    _Response = _M4FwChunkResp
    _ErrorV1 = HpiErrorV1
    _ErrorV2 = HpiErrorV2


class _M4FwCommitReq(WriteRequest):
    _GROUP_ID = GROUP_ID
    _COMMAND_ID = 0x00A2


class _M4FwCommitResp(WriteResponse):
    _GROUP_ID = GROUP_ID
    _COMMAND_ID = 0x00A2
    rst: bool


class M4FwCommit(_M4FwCommitReq):
    _Response = _M4FwCommitResp
    _ErrorV1 = HpiErrorV1
    _ErrorV2 = HpiErrorV2


class _M4FwStatusReq(ReadRequest):
    _GROUP_ID = GROUP_ID
    _COMMAND_ID = 0x00A3


class _M4FwStatusResp(ReadResponse):
    _GROUP_ID = GROUP_ID
    _COMMAND_ID = 0x00A3
    st: int
    len: int
    rx: int
    err: int
    rst: bool
    # Does this firmware require a signed M4 image? Defaulted so the tool still
    # talks to firmware built before the field existed.
    sig: bool = False


class M4FwStatus(_M4FwStatusReq):
    _Response = _M4FwStatusResp
    _ErrorV1 = HpiErrorV1
    _ErrorV2 = HpiErrorV2


class _M4FwAbortReq(WriteRequest):
    _GROUP_ID = GROUP_ID
    _COMMAND_ID = 0x00A4


class _M4FwAbortResp(WriteResponse):
    _GROUP_ID = GROUP_ID
    _COMMAND_ID = 0x00A4


class M4FwAbort(_M4FwAbortReq):
    _Response = _M4FwAbortResp
    _ErrorV1 = HpiErrorV1
    _ErrorV2 = HpiErrorV2


# --- 0x00A5 hpi/enter_recovery ---------------------------------------------

class _RecoveryStateReq(ReadRequest):
    _GROUP_ID = GROUP_ID
    _COMMAND_ID = 0x00A5


class _RecoveryStateResp(ReadResponse):
    _GROUP_ID = GROUP_ID
    _COMMAND_ID = 0x00A5
    av: bool
    armed: bool


class RecoveryState(_RecoveryStateReq):
    _Response = _RecoveryStateResp
    _ErrorV1 = HpiErrorV1
    _ErrorV2 = HpiErrorV2


class _EnterRecoveryReq(WriteRequest):
    _GROUP_ID = GROUP_ID
    _COMMAND_ID = 0x00A5
    arm: bool = True
    rst: bool = True


class _EnterRecoveryResp(WriteResponse):
    _GROUP_ID = GROUP_ID
    _COMMAND_ID = 0x00A5
    armed: bool
    rst: bool


class EnterRecovery(_EnterRecoveryReq):
    _Response = _EnterRecoveryResp
    _ErrorV1 = HpiErrorV1
    _ErrorV2 = HpiErrorV2


# Mirrors enum hpi_m4fw_state in services/m4_update_service.h.
M4FW_STATE = {
    0: "idle",
    1: "receiving",
    2: "verified",
    3: "committed (reset required)",
    4: "failed",
}
