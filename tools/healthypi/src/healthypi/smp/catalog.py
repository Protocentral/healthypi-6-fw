# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT

"""MCUmgr group 64 (``hpi``) -- the HealthyPi command catalog.

**This is the single source of truth for the host side.** Every request class,
CLI verb and test in this package derives from the spec below; nothing
re-declares a command id or a CBOR key anywhere else.

The firmware is the ultimate authority. This file is a transcription of:

* ``app_m7/src/control/mcumgr_hpi/hpi_mgmt_group.h``  -- ids and error codes
* ``app_m7/src/control/mcumgr_hpi/hpi_mgmt_group.c``  -- the dispatch table
* the handler files                                   -- the actual CBOR keys

``tests/test_catalog_drift.py`` re-parses those sources with ``smpgroup.drift``
and fails if this file disagrees, so the transcription cannot silently rot.
Prose version with the rationale:
docs/MCUMGR_COMMANDS.md.

The generic machinery lives in :mod:`smpgroup`; this module is only the data.
Stdlib only -- the drift check must run without a device stack installed.
"""

from __future__ import annotations

from smpgroup import Command, Field, Group, Op, Status, T

GROUP_ID = 64

#: HPI_MGMT_SCHEMA_VERSION reported by device_info's `gv` field.
SCHEMA_VERSION = 0x0001

# --- group-64 extension error codes (MGMT_ERR_USER_START = 256) -------------

ERRORS: dict[int, tuple[str, str]] = {
    256: ("NOT_READY", "busy, or a prerequisite is missing (no card, dev build)"),
    257: ("HW_FAULT", "hardware or co-processor link failure"),
    258: ("CHANNEL_NOT_AVAILABLE", "this device cannot produce that stream channel"),
    259: ("INSUFFICIENT_STORAGE", "not enough space"),
    260: ("TRANSFER_INVALID", "transfer state is invalid"),
    261: ("VERSION_MISMATCH", "version mismatch"),
    262: ("CONFIG_KEY_UNKNOWN", "unknown settings key"),
    263: ("CONFIG_TYPE_MISMATCH", "settings value has the wrong type"),
    264: ("LOCK_CHALLENGE_MISSING", "unlock_response without a prior challenge"),
    265: ("LOCK_HMAC_INVALID", "wrong unlock secret"),
    266: ("LOCK_EXPIRED", "challenge or grant TTL elapsed"),
    267: ("NO_MEDIA", "no SD card present"),
    268: ("IMAGE_INVALID", "digest or signature mismatch"),
    269: ("IMAGE_TOO_LARGE", "exceeds staging or bank 2"),
    270: ("BUSY", "an update is already in flight"),
    271: ("IMAGE_NOT_M4", "verifies, but is not an M4 image"),
}

# --- stock-group error codes we can also be answered with -------------------
#
# NOT group 64, and deliberately not in ERRORS above: that table is checked
# key-for-key against ``enum hpi_mgmt_err`` in the firmware header, and an entry
# with no counterpart there is drift by definition.
#
# We still need these names. The device serves the stock MCUmgr *os* group on
# the same connection (echo, datetime, reset), and os codes live in os's own
# namespace starting at 0 -- so os/4 is RTC_NOT_SET, while 4 read against the
# group-64 table is nothing at all. That mismatch is what printed
# "read refused: UNKNOWN (4)" for a device whose only sin was an unset clock.
#
# Source: ``enum os_mgmt_err_code_t``,
# zephyr/include/zephyr/mgmt/mcumgr/grp/os_mgmt/os_mgmt.h. Checked against that
# header by tests/test_catalog_drift.py when a Zephyr tree is present.

OS_GROUP_ID = 0
OS_ERR_RTC_NOT_SET = 4

STOCK_ERRORS: dict[int, dict[int, tuple[str, str]]] = {
    OS_GROUP_ID: {
        0: ("OK", ""),
        1: ("UNKNOWN", "the os group reports an unspecified failure"),
        2: ("INVALID_FORMAT", "the provided format value is not valid"),
        3: ("QUERY_YIELDS_NO_ANSWER", "the query was not recognised"),
        4: ("RTC_NOT_SET", "the clock has never been set on this device "
                           "(healthypi device datetime --set now)"),
        5: ("RTC_COMMAND_FAILED", "the RTC rejected the command"),
        6: ("QUERY_RESPONSE_VALUE_NOT_VALID",
            "the query was recognised but has no valid value"),
        7: ("HEAP_STATS_FETCH_FAILED", "heap statistics could not be read"),
    },
}

R, W = Op.READ, Op.WRITE

# --- the catalog -----------------------------------------------------------

COMMANDS: tuple[Command, ...] = (
    # -- system ------------------------------------------------------------
    Command(
        0x0001,
        "device_info",
        (R,),
        response=(
            Field("sn", T.TSTR, "serial number"),
            Field("fw", T.TSTR, "M7 firmware version"),
            Field("gv", T.UINT, "group schema version"),
            Field("br", T.TSTR, "board revision"),
            Field("hw", T.BSTR, "hardware id blob"),
            Field("m4fw", T.TSTR),
            Field("espfw", T.TSTR),
            Field("up", T.UINT, "uptime, seconds"),
        ),
    ),
    # -- security ----------------------------------------------------------
    Command(
        0x0010,
        "unlock_challenge",
        (R,),
        response=(Field("nonce", T.BSTR), Field("ttl_ms", T.UINT)),
        doc="ENOTSUP when no unlock secret is provisioned",
    ),
    Command(
        0x0011,
        "unlock_response",
        (W,),
        request=(Field("tag", T.BSTR, "HMAC over the challenge nonce"),),
        response=(Field("state", T.UINT), Field("ttl_ms", T.UINT, "grant remaining")),
        errors=(264, 265, 266),
    ),
    Command(0x0012, "lock", (W,), response=(Field("state", T.UINT),)),
    Command(0x0013, "lock_state", (R,), response=(Field("state", T.UINT),)),
    # -- streaming ---------------------------------------------------------
    Command(
        0x0020,
        "stream_start",
        (W,),
        meta={"unlock": True},
        request=(
            Field("ch", T.UINT, "channel mask, masked to 0x0F"),
            Field("ann", T.UINT, "annotation mask, masked to 0x1F"),
        ),
        errors=(258,),
        doc="empty response on success; data lands on CDC0",
    ),
    Command(0x0021, "stream_stop", (W,), doc="idempotent"),
    Command(
        0x0022,
        "stream_status",
        (R,),
        response=(
            Field("active", T.BOOL),
            Field("ch", T.UINT),
            Field("ann", T.UINT),
            Field("sent", T.UINT),
            Field("dropped", T.UINT),
        ),
    ),
    # -- telemetry ---------------------------------------------------------
    Command(
        0x0030,
        "telemetry",
        (R,),
        response=(
            Field("vbat_mv", T.UINT),
            Field("ibat_ma", T.INT),
            Field("soc", T.UINT, "state of charge, %"),
            Field("tc_x10", T.INT, "always the unavailable sentinel in 1.0.0"),
            Field("charge", T.UINT),
            Field("usb", T.BOOL),
            Field("batt", T.BOOL, "literally !usb"),
            Field("ok", T.BOOL),
        ),
    ),
    Command(
        0x0031,
        "fw_versions",
        (R,),
        response=(
            Field("m7fw", T.TSTR),
            Field("m4fw", T.TSTR),
            Field("espfw", T.TSTR),
            Field("mod_a_fw", T.TSTR),
            Field("mod_b_fw", T.TSTR),
        ),
    ),
    # -- HealthyLink modules ----------------------------------------------
    Command(
        0x0050,
        "module_list",
        (R,),
        response=(
            Field(
                "a",
                T.MAP,
                "slot A",
                nested=(
                    Field("state", T.UINT),
                    Field("id", T.UINT),
                    Field("pwr", T.BOOL),
                    Field("name", T.TSTR),
                ),
            ),
            Field(
                "b",
                T.MAP,
                "slot B",
                nested=(
                    Field("state", T.UINT),
                    Field("id", T.UINT),
                    Field("pwr", T.BOOL),
                    Field("name", T.TSTR),
                ),
            ),
        ),
        doc="keyed by slot, NOT an array",
    ),
    Command(0x0051, "module_info", (), status=Status.UNREACHABLE),
    Command(
        0x0052,
        "module_power",
        (W,),
        meta={"unlock": True},
        request=(Field("slot", T.UINT), Field("on", T.BOOL)),
        response=(Field("ok", T.BOOL),),
    ),
    # -- recording ---------------------------------------------------------
    Command(
        0x0060,
        "sd_status",
        (R,),
        response=(
            Field("active", T.BOOL),
            Field("bytes", T.UINT),
            Field("dur", T.UINT, "ms"),
            Field("ecg", T.UINT, "samples"),
            Field("ppg", T.UINT, "samples"),
            Field("path", T.TSTR),
        ),
    ),
    Command(
        0x0061,
        "sd_record_start",
        (W,),
        meta={"unlock": True},
        request=(Field("name", T.TSTR, "session name", optional=True),),
        response=(Field("path", T.TSTR),),
        errors=(256,),
        doc="256 covers BOTH already-recording and no-card; call sd_status to tell them apart",
    ),
    Command(0x0062, "sd_record_stop", (W,)),
    Command(0x0063, "sd_list", (), status=Status.UNREACHABLE),
    Command(0x0064, "sd_download_begin", (), status=Status.UNREACHABLE),
    Command(0x0065, "sd_download_chunk", (), status=Status.UNREACHABLE),
    Command(0x0066, "sd_download_end", (), status=Status.UNREACHABLE),
    Command(0x0067, "sd_delete", (), status=Status.UNREACHABLE),
    Command(0x0068, "sd_format", (), status=Status.UNREACHABLE),
    Command(
        0x0069,
        "transfer_mode",
        (R, W),
        meta={"unlock": True},
        response=(Field("armed", T.BOOL),),
        write_request=(Field("on", T.BOOL),),
        write_response=(Field("armed", T.BOOL),),
        errors=(267, 257),
        doc="arming re-enumerates USB and kills the open CDC1 connection",
    ),
    # -- WiFi --------------------------------------------------------------
    Command(
        0x0070,
        "wifi_status",
        (R,),
        response=(
            Field("state", T.UINT),
            Field("rssi", T.INT),
            Field("ssid", T.TSTR),
            Field("ip", T.TSTR),
        ),
        errors=(257,),
    ),
    Command(
        0x0071,
        "wifi_scan",
        (R,),
        status=Status.STUB,
        doc="always ENOTSUP: provisioning goes through the SoftAP portal",
    ),
    Command(
        0x0072,
        "wifi_set",
        (W,),
        meta={"unlock": True},
        request=(Field("ssid", T.TSTR), Field("pw", T.TSTR)),
        response=(Field("ok", T.BOOL),),
    ),
    Command(0x0073, "wifi_forget", (W,), meta={"unlock": True}, response=(Field("ok", T.BOOL),)),
    Command(
        0x0074,
        "wifi_softap",
        (W,),
        response=(Field("ok", T.BOOL),),
        errors=(257,),
        doc="deliberately NOT unlock-gated: it is how a locked device gets online",
    ),
    # -- co-processor power ------------------------------------------------
    # The device boots with the ESP32-C6 held in reset and both radios down, so
    # something has to be able to turn it on; the shell adapter that could is
    # compiled out of production builds. Not unlock-gated, for the same reason
    # wifi_softap is not.
    Command(
        0x0075,
        "conn_enable",
        (W,),
        request=(Field("radios", T.UINT),),
        response=(Field("ok", T.BOOL),),
        errors=(257,),
        doc="radios bitmask: bit0 WiFi, bit1 BLE; 0 = powered with no radio "
        "(needed to flash the C6, which cannot enumerate while held in reset). "
        "Answers 'accepted', not 'radio up' -- poll conn_status for the outcome",
    ),
    Command(
        0x0076,
        "conn_disable",
        (W,),
        response=(Field("ok", T.BOOL),),
        doc="radios down and the co-processor back into reset",
    ),
    Command(
        0x0077,
        "conn_status",
        (R,),
        response=(
            Field("link", T.UINT),
            Field("radios", T.UINT),
            Field("state", T.UINT),
            Field("rssi", T.INT),
            Field("ssid", T.TSTR),
            Field("ip", T.TSTR),
            Field("ble_adv", T.BOOL),
            Field("ble_conn", T.BOOL),
        ),
        doc="link: 0 off (by request) 1 starting 2 up 3 fault -- the distinction "
        "wifi_status cannot express, between a radio that is off because it was "
        "asked to be and a co-processor that should be answering and is not",
    ),
    # -- diagnostics -------------------------------------------------------
    Command(
        0x0080,
        "diag_run_selftest",
        (R,),
        response=(
            Field("suite_ver", T.UINT),
            Field("sd", T.BOOL),
            Field("batt", T.BOOL),
            Field("ecg", T.BOOL),
            Field("ppg", T.BOOL),
            Field("m4", T.BOOL),
            Field("qspi", T.BOOL),
            Field("pass", T.UINT),
            Field("fail", T.UINT),
            Field("health_overall", T.UINT),
            Field(
                "health",
                T.MAP,
                "per-subsystem state",
                nested=(
                    Field("acq", T.UINT),
                    Field("m4", T.UINT),
                    Field("stream", T.UINT),
                    Field("rec", T.UINT),
                    Field("hl", T.UINT),
                ),
            ),
        ),
        doc="`health` is a NESTED map; top-level `m4` (bool) differs from health.m4 (uint)",
    ),
    Command(
        0x0081,
        "diag_lead_off",
        (R,),
        response=(
            Field("mask", T.UINT, "electrode bits: 1 RA, 2 LA, 4 LL, 8 V1"),
            Field("ra", T.BOOL, "RA electrode is off"),
            Field("la", T.BOOL),
            Field("ll", T.BOOL),
            Field("v1", T.BOOL),
            Field("age_ms", T.UINT, "since acquisition last updated the mask"),
            Field("ok", T.BOOL, "the state is fresh (the ECG front end is live)"),
            Field("hr", T.UINT, "last heart rate, 0 = none"),
            Field("hr_src", T.UINT, "0 = ECG, 1 = PPG"),
            Field("vflags", T.UINT, "raw hp6_vitals.flags"),
        ),
        doc="a large age_ms means the ECG went quiet -- NOT the same as leads on",
    ),
    Command(0x0082, "diag_signal_stats", (), status=Status.UNREACHABLE),
    # -- logs (none implemented) -------------------------------------------
    Command(0x0090, "log_subscribe", (), status=Status.UNREACHABLE),
    Command(0x0091, "log_unsubscribe", (), status=Status.UNREACHABLE),
    Command(0x0092, "log_get_buffered", (), status=Status.UNREACHABLE),
    # -- M4 firmware update ------------------------------------------------
    Command(
        0x00A0,
        "m4fw_begin",
        (W,),
        meta={"unlock": True, "signed_build": True},
        request=(
            Field("len", T.UINT, "image size"),
            Field("sha", T.BSTR, "SHA-256 of the image"),
            Field("sig", T.BSTR, "ECDSA-P256 r||s", optional=True),
        ),
        response=(Field("off", T.UINT, "resume offset"),),
        errors=(256, 269, 270),
    ),
    Command(
        0x00A1,
        "m4fw_chunk",
        (W,),
        meta={"unlock": True, "signed_build": True},
        request=(Field("off", T.UINT), Field("data", T.BSTR)),
        response=(Field("off", T.UINT, "next expected offset"),),
        errors=(256, 260),
    ),
    Command(
        0x00A2,
        "m4fw_commit",
        (W,),
        meta={"unlock": True, "signed_build": True},
        response=(Field("rst", T.BOOL, "a reset is required"),),
        errors=(268, 269, 270, 271),
    ),
    Command(
        0x00A3,
        "m4fw_status",
        (R,),
        meta={"signed_build": True},
        response=(
            Field("st", T.UINT, "state"),
            Field("len", T.UINT, "total size"),
            Field("rx", T.UINT, "bytes received"),
            Field("err", T.INT),
            Field("rst", T.BOOL, "reset required"),
            Field("sig", T.BOOL, "this firmware requires a signature"),
        ),
        doc="call FIRST: `sig` says whether an unsigned upload will be rejected",
    ),
    Command(0x00A4, "m4fw_abort", (W,), meta={"signed_build": True}),
    # -- recovery ----------------------------------------------------------
    Command(
        0x00A5,
        "enter_recovery",
        (R, W),
        response=(Field("av", T.BOOL, "available"), Field("armed", T.BOOL)),
        write_request=(Field("arm", T.BOOL), Field("rst", T.BOOL, "reset now")),
        write_response=(Field("armed", T.BOOL), Field("rst", T.BOOL, "will reset")),
        errors=(256,),
        doc="reboots into MCUboot serial recovery; group 64 disappears until reflash",
    ),
)

#: Async notification ids. Declared in the header; no emitter exists in 1.0.0.
EVENTS: dict[int, str] = {
    0x00F0: "charge_state",
    0x00F1: "batt_low",
    0x00F2: "usb_plug",
    0x00F3: "module_inserted",
    0x00F4: "module_removed",
    0x00F5: "wifi_state",
    0x00F6: "sd_inserted",
    0x00F7: "sd_removed",
    0x00F8: "sd_error",
    0x00F9: "lead_off",
    0x00FA: "button",
    0x00FB: "over_temp",
    0x00FC: "diag_result",
    0x00FD: "diag_complete",
    0x00FE: "reboot_pending",
    0x00FF: "sd_format_progress",
}

#: The group, as a reusable smpgroup spec. `healthypi.smp.group64` generates the
#: wire classes from this, and the drift test checks it against the firmware.
HPI_GROUP = Group(
    group_id=GROUP_ID,
    name="hpi",
    commands=COMMANDS,
    errors=ERRORS,
    schema_version=SCHEMA_VERSION,
    doc="HealthyPi 6 device control -- streaming, recording, wifi, diagnostics, update",
)

BY_ID: dict[int, Command] = {c.cmd_id: c for c in COMMANDS}
BY_NAME: dict[str, Command] = {c.name: c for c in COMMANDS}


def dispatchable() -> tuple[Command, ...]:
    """Commands the firmware actually routes (live + stub)."""
    return HPI_GROUP.routed()


def live() -> tuple[Command, ...]:
    return HPI_GROUP.live()


def unlock_gated() -> tuple[Command, ...]:
    """Commands that require the security unlock when the gate is enabled."""
    return HPI_GROUP.tagged("unlock")


def signed_build_only() -> tuple[Command, ...]:
    """Commands that exist only in the signed (shippable) build."""
    return HPI_GROUP.tagged("signed_build")


def err_name(code: int) -> str:
    return HPI_GROUP.err_name(code)


def err_hint(code: int) -> str:
    return HPI_GROUP.err_hint(code)
