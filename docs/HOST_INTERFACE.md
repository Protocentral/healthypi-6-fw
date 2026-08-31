# HealthyPi 6 host interface

The contract between HealthyPi 6 firmware and anything that talks to it over
USB: the two CDC ports, the MCUmgr SMP control protocol and its HealthyPi
command group, the `.HP6` file and stream container, the settings schema, the
event catalog and the error codes.

This is the document to build a host library, a CLI or a desktop application
against. For how the firmware itself is put together, see
[`ARCHITECTURE.md`](ARCHITECTURE.md).

---

## 1. Architecture

### 1.1. Transport topology

```
┌──────────────┐     USB CDC 0  (stream, one-way)     ┌──────────────┐
│    Host      │◄──────────────────────────────────── │  HealthyPi 6 │
│   (Python    │                                       │   STM32H757  │
│    library)  │     USB CDC 1  (MCUmgr SMP, bidir)    │     M7 core  │
│              │◄──────────────────────────────────── │              │
└──────┬───────┘                                       └───────┬──────┘
       │                                                       │
       │                                                       │ OpenAMP IPC
       ▼                                                       ▼
  ┌─────────┐                                           ┌──────────────┐
  │  WiFi   │◄── ESP32-C6 ─── HealthyBridge SPI ──────► │   M4 core    │
  │  (TCP)  │                                           │   (algos)    │
  └─────────┘                                           └──────────────┘
```

- **CDC 0 = stream pipe.** High-rate sample frames (ECG, PPG, EEG, respiration, annotations). One-way device → host. Binary wire format authored in the HealthyPi 6 Studio data-model document; outside this PRD.
- **CDC 1 = control pipe.** MCUmgr Simple Management Protocol (SMP) framing, bidirectional request/response + async notifications. All commands, events, and OTA flow here. This PRD covers CDC 1.
- ESP32-C6 bridges WiFi/BLE control. In v1 the MCUmgr transport is USB only; network transports are deferred.
- M4 services queries via the existing OpenAMP IPC channel; the message catalog is in
  [`ARCHITECTURE.md`](ARCHITECTURE.md).
- HealthyLink modules are reached via M7 (I²C EEPROM read, plus module-native protocols).

### 1.2. Role separation between CDC 0 and CDC 1

Each CDC has exactly one role — the two pipes are **not** multiplexed and do not share a framing format.

- CDC 0 carries a one-way binary sample stream. It has no command semantics; opening it yields only device → host data once streaming is enabled (see `hpi/stream_start` in §3.3).
- CDC 1 carries the MCUmgr SMP protocol in both directions. Commands, responses, events (async notifications), and OTA traffic all use SMP framing on this endpoint.

This split means: a host library that only wants to record samples opens CDC 0 and ignores CDC 1. A host tool (like `mcumgr-cli`) that only wants to issue commands opens CDC 1 and ignores CDC 0. The library/CLI combines both but with independent file descriptors — no multiplexing state to maintain.

**Rationale.** The pre-pivot design multiplexed stream and control over a single CDC with a custom 8-byte header. Under load (ECG+PPG streaming at ~40 KB/s combined with 100 Hz ping traffic), macOS reliably bus-reset the single-endpoint device within 30 s. Splitting onto separate endpoints eliminates head-of-line blocking and ring-buffer contention. The incremental hardware/power cost of a second CDC function is negligible on STM32H7 (see security review in §10).

### 1.3. USB identity

HealthyPi 6's M7 presents as a **composite USB device** with two CDC ACM functions.

| Interface | Role | Framing |
|-----------|------|---------|
| CDC 0 (lower interface number) | Sample streaming (one-way D → H) | HealthyPi 6 Studio binary (separate doc) |
| CDC 1 (higher interface number) | MCUmgr SMP control (bidirectional) | Zephyr SMP over serial |

**VID/PID (Phase 0 decision, 2026-04-24):** v1 firmware ships with **VID 0x2FE3 / PID 0x0100** (Zephyr Project development pair, not a registered assignment). Host-side auto-discovery matches this pair. A registered VID/PID will be issued before first customer shipment and this section updated.

**Host-side interface selection.** Two deterministic mechanisms:

1. **By position** — the lower-numbered CDC interface is CDC 0; the higher-numbered is CDC 1. Platform identifiers follow this ordering on all three desktop OSes (macOS: `/dev/cu.usbmodem…1` vs `…3`; Linux: `/dev/ttyACM0` vs `/dev/ttyACM1`; Windows: sequential `COMn`).
2. **By framing probe** — the host may open both and send an `os echo` over SMP; the interface that responds is CDC 1.

The library uses mechanism 1 by default, falling back to 2 if enumeration order is ambiguous on a given host.

## 2. Transport: MCUmgr SMP on CDC 1

### 2.1. Framing

CDC 1 speaks the **Simple Management Protocol (SMP)** as defined by Zephyr's MCUmgr subsystem. This PRD does not redefine the wire encoding — it reuses the standard Zephyr / MCUmgr-cli framing. Relevant references:

- [Zephyr MCUmgr documentation](https://docs.zephyrproject.org/latest/services/device_mgmt/mcumgr.html)
- [MCUmgr SMP protocol spec](https://docs.zephyrproject.org/latest/services/device_mgmt/smp_protocol.html)
- [MCUmgr-cli tool (Go)](https://github.com/apache/mynewt-mcumgr-cli)

Summary of the framing (for orientation only, not a re-specification):

- Each SMP transaction is a base64-encoded CBOR body delimited by start-of-packet markers (`0x06 0x09` for first chunk, `0x04 0x14` for continuation) and a newline terminator. A CRC-16-XMODEM over the decoded payload protects integrity.
- CBOR-encoded body carries an 8-byte SMP header (op, flags, len, **group**, seq, id) plus the request/response payload.
- Responses echo the request's `seq`. One request in flight at a time per transport is the mcumgr-cli convention; the device accepts concurrent requests and serializes them via its MCUmgr workqueue.
- Async notifications (events) use the same framing but with the response op code and no corresponding request.

### 2.2. Management groups

MCUmgr commands are organized into **groups**, each a 16-bit namespace. Within a group, a **command ID** (16-bit) selects the handler. HealthyPi 6 v1 exposes the following groups on CDC 1:

| Group ID | Name | Source | Purpose |
|----------|------|--------|---------|
| 0 | `os` | stock Zephyr | Reset, echo, MCUmgr parameters |
| 1 | `img` | stock Zephyr | **OTA** — image upload/test/confirm/erase (see §10 for signing policy) |
| 2 | `stat` | stock Zephyr | Counter statistics (debugging) |
| 3 | `config` | stock Zephyr | **Settings schema** (§6) |
| **64** | **`hpi`** | **custom** | **HealthyPi-specific commands** (see §3) |

Groups explicitly **not enabled** in customer builds, for security reasons (§10):

- `fs` (group 8) — stock filesystem access. `.hpi` recording download is served by a bespoke `hpi` group command (§3.3) with access-control gating. Enabling stock `fs` would expose SD card contents to any host with CDC 1 access.
- `log` (group 4) — stock log group. HealthyPi logs contain PHI (heart rates, event timestamps). Log access is served by a bespoke `hpi/log_*` command with redaction.
- `shell` (group 9) — remote shell. Never enabled in customer builds.

Development builds may enable any of the above behind an explicit `CONFIG_HPI_DEV_MODE=y` Kconfig. Production builds assert-fail at link time if any of those groups is registered.

### 2.3. Command semantics

- **Request / response.** Every command in §3 is a request/response pair. The response carries an MCUmgr return code (see §4) and a CBOR result payload defined per-command.
- **Notifications.** The device pushes async events (battery, module insertion, self-test progress, etc.) as MCUmgr notifications on group 64. See §5. Hosts that open CDC 1 without subscribing will still receive notifications — the subscription model of the old custom protocol is replaced by Kconfig-gated event production on the device (an event is produced only if its producer is enabled).
- **Locking.** Destructive commands require the device to be in the **unlocked** state (§10). Commands issued while locked return `MGMT_ERR_EACCES` (see §4). `os reset`, `img upload`, `config_reset`, `hpi/sd_format`, and `hpi/sd_delete` are gated by this.
- **Authentication.** MCUmgr SMP itself has no transport-level authentication. The HealthyPi 6 device lock (§10) provides access control at the application layer. Non-destructive commands (device info, telemetry, lead-off, etc.) are available without unlock — they do not reveal patient data.

### 2.4. CBOR schemas

Each command in §3.3 specifies its CBOR request and response as a list of (key, type) pairs. Keys are short ASCII strings to keep frames small. Types are CBOR primitives (tstr, bstr, uint, int, bool, array, map).

Implementations should use zcbor — the Zephyr-maintained CBOR codec that MCUmgr already depends on — for encode/decode on both device and host. The Python host library should use `cbor2` or `zcbor`'s Python bindings.

## 3. Command Reference

### 3.1. Stock MCUmgr groups used

| Group | Command ID | Name | Locked-when | Purpose |
|-------|-----------|------|-------------|---------|
| 0 (os) | 0 | `echo` | never | **Liveness / loopback** — replaces what would have been `hpi/ping` |
| 0 (os) | 5 | `reset` | locked | **Normal reboot** — replaces `hpi/reboot mode=0` |
| 0 (os) | 6 | `mcumgr_params` | never | MCUmgr buffer sizes |
| 0 (os) | 8 | `datetime_get` | never | **RTC unix time** — replaces `hpi/time_get` |
| 0 (os) | 9 | `datetime_set` | locked | **Set RTC** — replaces `hpi/time_set` |
| 1 (img) | 0 | `state` | never (read) | List slot states |
| 1 (img) | 1 | `upload` | locked | **OTA image upload** (see §10.5 for signing) |
| 1 (img) | 2 | `erase` | locked | Erase pending image |
| 2 (stat) | 0 | `read` | never | Counter statistics |
| 2 (stat) | 1 | `list` | never | Enumerate stat groups |
| 3 (config) | 0 | `config_read` | depends on key (see §6) | Read a setting |
| 3 (config) | 1 | `config_write` | locked for sensitive keys | Write a setting |

**Principle:** if a stock MCUmgr command exists that correctly serves a HealthyPi need, we use the stock command rather than reimplementing it in group 64. This keeps our custom surface small and benefits from Zephyr's tested code + mcumgr-cli native support.

Stock command documentation: [Zephyr MCUmgr command reference](https://docs.zephyrproject.org/latest/services/device_mgmt/smp_groups/smp_groups.html). This PRD does not re-specify stock handlers — it only documents where they fit in HealthyPi's security model.

### 3.2. HealthyPi group 64 — command catalog

The `hpi` group (group ID **64**) carries HealthyPi-specific commands. Command IDs are allocated in subranges by subsystem; all requests and responses are CBOR maps as described below.

| Cmd ID range | Subsystem | Locked-when |
|--------------|-----------|-------------|
| 0x0000 – 0x000F | System / metadata (device info only — ping/reboot/time are stock) | never |
| 0x0010 – 0x001F | Unlock & security (§10) | n/a — unlock is the gate |
| 0x0020 – 0x002F | Streaming control | locked |
| 0x0030 – 0x003F | Telemetry | never |
| 0x0040 – 0x004F | *(reserved — previously Time/RTC, now served by `os/datetime_*`)* | — |
| 0x0050 – 0x005F | HealthyLink modules | `power` is locked |
| 0x0060 – 0x006F | SD card / recordings | all destructive ops locked |
| 0x0070 – 0x007F | WiFi | `set` / `forget` locked |
| 0x0080 – 0x008F | Diagnostics / self-test | never (diagnostic read-only) |
| 0x0090 – 0x009F | Logs | `subscribe` locked |
| 0x00A0 – 0x00FF | Reserved — do not reassign | |
| 0x0100+ | Future / vendor | |

Each command below specifies request and response CBOR schemas. Types are CBOR primitives (`tstr` = text string, `bstr` = byte string, `uint` = unsigned int, `int` = signed int, `bool`, `array`, `map`). All maps use short ASCII keys.

### 3.3. System commands (0x0000 – 0x000F)

Liveness is served by stock `os/echo`. Normal reboot is served by stock `os/reset`. This range carries only HealthyPi-specific metadata not available via stock groups.

**`hpi/device_info` (0x0001)** — locked-when: never

- Request: `{}`
- Response:
  - `sn` : tstr — device serial number (derived from STM32 UID hwinfo)
  - `fw` : tstr — M7 firmware semver (e.g. `"1.2.0"`)
  - `gv` : uint — HealthyPi group-64 schema version (see §9)
  - `br` : tstr — board revision (`"v3"`, `"v4"`, ...)
  - `hw` : bstr[16] — raw STM32 unique device ID
  - `m4fw` : tstr — M4 firmware semver (empty if unavailable)
  - `espfw` : tstr — ESP32-C6 firmware semver (empty if no bridge)
  - `up` : uint — uptime in seconds
- Rationale: stock `os/info` returns a single free-form string; we need structured fields for multi-core firmware versions, board revision, and group-64 schema version.

> **Removed in cleanup (2026-04-25):** `hpi/ping` (0x0000) → use `os/echo`; `hpi/reboot` (0x0002) → use `os/reset`. If bootloader-entry or safe-mode reboot flavors become required later, reintroduce as `hpi/reboot_ext` in this range.

### 3.4. Unlock & security (0x0010 – 0x001F)

Defined in detail in §10. Summary:

- `hpi/unlock_challenge` (0x0010) — request a 16-byte nonce
- `hpi/unlock_response` (0x0011) — submit HMAC(nonce, shared_secret) to unlock
- `hpi/lock` (0x0012) — explicitly relock
- `hpi/lock_state` (0x0013) — query current lock state

### 3.5. Streaming control (0x0020 – 0x002F)

**`hpi/stream_start` (0x0020)** — locked-when: locked

- Request:
  - `ch` : uint — channel bitmask (bit 0=ECG, 1=PPG, 2=RESP, 3=EEG)
  - `ann` : uint — annotation bitmask (bit 0=QRS, 1=HR, 2=SpO2, 3=resp_rate, 4=beat_class)
- Response: `{}` on success
- Behavior: enables CDC 0 TX for the selected channels/annotations. Returns `MGMT_ERR_HPI_CHANNEL_NOT_AVAILABLE` if EEG requested without the EEG module seated and powered.

**`hpi/stream_stop` (0x0021)** — locked-when: never (allowing lockdown mid-session)

- Request: `{}`
- Response: `{}`
- Behavior: stops all channels. CDC 0 frames cease within 50 ms.

**`hpi/stream_status` (0x0022)** — locked-when: never

- Request: `{}`
- Response:
  - `active` : bool
  - `ch` : uint — active channel mask
  - `ann` : uint — active annotation mask
  - `sent` : uint — cumulative frames sent since boot
  - `dropped` : uint — frames dropped due to host backpressure

### 3.6. Telemetry (0x0030 – 0x003F)

**`hpi/telemetry` (0x0030)** — locked-when: never

- Request: `{}`
- Response:
  - `vbat_mv` : uint — Li-Po cell voltage, mV
  - `ibat_ma` : int — signed battery current, mA (+ = charging)
  - `soc` : uint — state-of-charge % (0–100, from MAX17048)
  - `tc_x10` : int — STM32 internal temperature °C × 10
  - `charge` : uint — 0=discharging, 1=charging, 2=full, 3=fault
  - `usb` : bool — USB plugged
  - `batt` : bool — on-battery power

Sources: MP2696B IB via ADC12_IN15 (PC5), MAX17048 via I²C, STM32 internal temp sensor. `fault` flag OR'd from MP2696B INT status.

**`hpi/fw_versions` (0x0031)** — locked-when: never

- Request: `{}`
- Response:
  - `m7fw` : tstr
  - `m4fw` : tstr
  - `espfw` : tstr
  - `mod_a_fw` : tstr (empty if no module)
  - `mod_b_fw` : tstr (empty if no module)

### 3.7. Time / RTC

Time is handled by stock `os/datetime_get` (group 0, cmd 8) and `os/datetime_set` (group 0, cmd 9). HealthyPi does not add custom time commands.

- Get: `mcumgr os datetime get` — returns ISO-8601 string from the battery-backed RTC. Returns an empty string if the RTC has never been set.
- Set: `mcumgr os datetime set "2026-04-25T12:34:56"` — requires unlock (see §10.3).

**RTC policy (Phase 0 decision, 2026-04-24, unchanged):** HealthyPi 6 v3 and v4 carry a battery-backed RTC (coin cell). Once `os/datetime_set` succeeds, the RTC survives power cycles. The `time/rtc_set_at_boot` settings-schema bool (§6) is set to `true` by firmware the first time a set succeeds and persists across reboots; the host library uses it to decide whether to auto-sync on connect.

**Kconfig:** `CONFIG_MCUMGR_GRP_OS_DATETIME=y` + `zephyr,rtc` chosen node pointing at the STM32 RTC (already done in the v3 board DTS).

### 3.8. HealthyLink modules (0x0050 – 0x005F)

**`hpi/module_list` (0x0050)** — locked-when: never

- Request: `{}`
- Response:
  - `slots` : array of map
    - `slot` : uint (0 = A, 1 = B)
    - `present` : bool
    - `type` : uint — see module type enum below
    - `rev` : tstr
    - `sn` : tstr
    - `fw` : tstr
    - `active` : bool — slot LDO enabled

Module type enum:

| Code | Name |
|------|------|
| 0x0000 | None / empty |
| 0x0100 | EEG ADS1299 |
| 0x0200 | NPU STM32N657 |
| 0x0300 | CAN bridge |
| 0x0400 | Trigger I/O |
| 0xFFFF | Unknown / future |

**`hpi/module_info` (0x0051)** — locked-when: never

- Request: `{ "slot": uint }`
- Response: `{ "eeprom": bstr[256], "status": map }`  (status fields TBD in Phase 5)

**`hpi/module_power` (0x0052)** — locked-when: locked

- Request: `{ "slot": uint, "enable": bool }`
- Response: `{}`
- Behavior: controls the firmware-gated LDO per power architecture doc §4.

### 3.9. SD card / recordings (0x0060 – 0x006F)

All SD commands return `MGMT_ERR_HPI_NOT_READY` if the filesystem isn't mounted.

**`hpi/sd_status` (0x0060)** — locked-when: never

- Request: `{}`
- Response:
  - `mounted` : bool
  - `total_mb` : uint
  - `free_mb` : uint
  - `files` : uint
  - `rec_active` : bool
  - `rec_name` : tstr
  - `rec_bytes` : uint
  - `rec_s` : uint — recording duration in seconds

**`hpi/sd_record_start` (0x0061)** — locked-when: locked

- Request:
  - `ch` : uint — channel mask
  - `ann` : uint — annotation mask
  - `name` : tstr (optional; empty = firmware generates timestamped name)
- Response: `{ "name": tstr }`

**`hpi/sd_record_stop` (0x0062)** — locked-when: never (always allow stop)

- Request: `{}`
- Response: `{ "name": tstr, "bytes": uint, "duration_s": uint }`

**`hpi/sd_list` (0x0063)** — locked-when: never

- Request: `{ "offset": uint, "count": uint }` (max count 16 per response)
- Response:
  - `total` : uint
  - `entries` : array of map
    - `name` : tstr
    - `bytes` : uint
    - `start_ms` : uint — recording start unix-ms
    - `duration_s` : uint
    - `ch` : uint

**`hpi/sd_download_begin` (0x0064)** — locked-when: **locked** (PHI access)

- Request: `{ "name": tstr, "offset": uint, "max_chunk": uint }`
- Response: `{ "xfer": uint, "total": uint, "chunk": uint }`
- Note: replaces stock `fs download` because the stock group is disabled in customer builds (§10.2).

**`hpi/sd_download_chunk` (0x0065)** — inherits unlock from `begin`

- Request: `{ "xfer": uint, "idx": uint }`
- Response: `{ "xfer": uint, "idx": uint, "data": bstr }`

**`hpi/sd_download_end` (0x0066)** — inherits unlock from `begin`

- Request: `{ "xfer": uint }`
- Response: `{ "crc32": uint }`

**`hpi/sd_delete` (0x0067)** — locked-when: locked

- Request: `{ "name": tstr }`
- Response: `{}`
- Note: the 0xA5 "confirm byte" from pre-pivot revisions is removed; unlock (§10) is the authoritative gate.

**`hpi/sd_format` (0x0068)** — locked-when: locked

- Request: `{}`
- Response: `{}` (progress is reported via `hpi/evt_sd_format_progress` notifications)

### 3.10. WiFi (0x0070 – 0x007F)

All WiFi commands route M7 → HealthyBridge SPI → ESP32-C6. M7 blocks until ESP32 responds or times out.

**`hpi/wifi_status` (0x0070)** — locked-when: never

- Request: `{}`
- Response:
  - `state` : uint — 0=off, 1=disconnected, 2=connecting, 3=connected, 4=error
  - `ssid` : tstr — last-known SSID (cached on M7; the ESP32 holds the authoritative copy)
  - `rssi` : int
  - `ipv4` : bstr[4]
  - `mac` : bstr[6]

**`hpi/wifi_scan` (0x0071)** — locked-when: never

- Request: `{ "max": uint }` (≤ 16)
- Response:
  - `aps` : array of map
    - `ssid` : tstr
    - `rssi` : int
    - `auth` : uint — 0=open, 1=WEP, 2=WPA2, 3=WPA3, 4=WPA2-enterprise
    - `ch` : uint

**`hpi/wifi_set` (0x0072)** — locked-when: locked

- Request: `{ "ssid": tstr, "pw": tstr }`
- Response: `{}` (state transition reported via `hpi/evt_wifi_state`)

**`hpi/wifi_forget` (0x0073)** — locked-when: locked

- Request: `{}`
- Response: `{}`

### 3.11. Diagnostics (0x0080 – 0x008F)

**`hpi/diag_run_selftest` (0x0080)** — locked-when: never (read-only diagnostics)

- Request: `{ "mask": uint }` (0 = run all)
- Response: `{}` on accept; results stream as `hpi/evt_diag_result` notifications with a final `hpi/evt_diag_complete`. See §8.

**`hpi/diag_lead_off` (0x0081)** — locked-when: never

- Request: `{}`
- Response:
  - `ecg_ra` : bool
  - `ecg_la` : bool
  - `ecg_ll` : bool
  - `ecg_v` : bool
  - `ppg_finger` : bool — true = not detected
  - `eeg_z_kohm` : array[8] of uint — per-channel impedance in kΩ, or 0xFFFF if module absent

**`hpi/diag_signal_stats` (0x0082)** — locked-when: never

- Request: `{ "duration_s": uint, "ch": uint }`
- Response: `{ "mean": float, "std": float, "snr_db": float, "min": float, "max": float }`

**Memory statistics** — served by the stock `stat` group (group 2).

Firmware exposes named stat groups:
- `hpi_mem_m7` — `flash_used`, `flash_free`, `sram_used`, `sram_free`
- `hpi_mem_m4` — same four
- `hpi_mem_ext` — `sdram_used`, `sdram_free`, `qspi_used`, `qspi_free`

Host reads each via `mcumgr stat <group_name>` (stock `stat/read`). This replaces the originally-planned `hpi/diag_memory` (0x0083) with a stock-command implementation. Group 64 cmd ID 0x0083 is reserved — do not reassign.

### 3.12. Logs (0x0090 – 0x009F)

Stock MCUmgr `log` group (group 4) is **disabled** in customer builds because HealthyPi logs contain PHI and redacting at the message level is unreliable. The commands below provide scoped, redacted access.

**`hpi/log_subscribe` (0x0090)** — locked-when: locked

- Request: `{ "min_level": uint, "modules": uint }` (modules bitmask; 0 = all)
- Response: `{}`
- Behavior: streams redacted log lines via `hpi/evt_log` notifications. PHI-tagged log lines (heart rate, beat timing, lead-off transitions) are filtered out regardless of level.

**`hpi/log_unsubscribe` (0x0091)** — locked-when: never

- Request: `{}`
- Response: `{}`

**`hpi/log_get_buffered` (0x0092)** — locked-when: locked

- Request: `{ "since_ms": uint }` (0 = all)
- Response: chunked via multiple notifications. Same PHI filtering as `subscribe`.

### 3.13. Firmware / OTA

**Stock MCUmgr `img` group handles OTA.** No HealthyPi-specific commands needed. Host workflow:

```
mcumgr -t serial -c /dev/tty.usbmodem…3 image upload fw.bin
mcumgr -t serial -c /dev/tty.usbmodem…3 image test <hash>
mcumgr -t serial -c /dev/tty.usbmodem…3 reset
# device boots new image; confirm after validation:
mcumgr -t serial -c /dev/tty.usbmodem…3 image confirm
```

**Signing.** `CONFIG_BOOT_SIGNATURE_TYPE_ECDSA_P256=y` is required in all builds. Unsigned images fail the swap. Production signing key management is specified in §10.4.

**Anti-rollback.** `CONFIG_BOOT_SECURITY_COUNTER=y` required. Firmware images carry a security counter; MCUboot refuses to boot an image whose counter is below the current running one.

**Non-M7 targets.**

- **M4**: the M4 image ships inside the M7 dual-slot layout; `os reset` triggers M4 re-boot via OpenAMP restart. Transparent to host.
- **ESP32-C6**: upload via `mcumgr fs upload /lfs/ota/esp32c6.bin …`, then trigger via `hpi/esp32_ota_commit` (TBD, Phase 9 minor version). Requires unlock. `fs upload` to this specific path is whitelisted in §10.2.
- **HealthyLink modules**: expansion-module commands are not part of the v1 host contract.

## 4. Error Codes

MCUmgr returns a numeric error code per response, plus an optional CBOR `err` map with a detailed message. HealthyPi uses the standard MCUmgr codes for transport-level issues and defines **group-64 extension codes** starting at `MGMT_ERR_USER_START` (256) for HealthyPi-specific errors.

### 4.1. Standard MCUmgr codes (used as-is)

| Code | Name | Used when |
|------|------|-----------|
| 0 | `MGMT_ERR_EOK` | Success |
| 1 | `MGMT_ERR_EUNKNOWN` | Unspecified internal error |
| 2 | `MGMT_ERR_ENOMEM` | Out of memory |
| 3 | `MGMT_ERR_EINVAL` | Malformed CBOR, missing required key, invalid value |
| 4 | `MGMT_ERR_ETIMEOUT` | Sub-system (M4 via IPC, ESP32 via SPI) didn't respond |
| 5 | `MGMT_ERR_ENOENT` | Referenced object not found (filename, module slot) |
| 6 | `MGMT_ERR_EBADSTATE` | Operation doesn't apply in current state |
| 7 | `MGMT_ERR_EMSGSIZE` | Request too large for transport |
| 8 | `MGMT_ERR_ENOTSUP` | Command not implemented in this firmware version |
| 9 | `MGMT_ERR_ECORRUPT` | Integrity check failed (image hash, file CRC) |
| 10 | `MGMT_ERR_EBUSY` | Resource in use; retry later |
| 11 | `MGMT_ERR_EACCES` | **Access denied** — device locked (§10), or caller lacks permission |
| 12 | `MGMT_ERR_EPERUSER` | Group-specific error: see extension codes below |

### 4.2. HealthyPi group-64 extension codes

| Code | Name | Meaning |
|------|------|---------|
| 256 | `MGMT_ERR_HPI_NOT_READY` | Subsystem (SD mount, WiFi init, M4 IPC) not yet available |
| 257 | `MGMT_ERR_HPI_HW_FAULT` | Sensor init failed, SD card missing, bridge comm error |
| 258 | `MGMT_ERR_HPI_CHANNEL_NOT_AVAILABLE` | Requested stream channel needs a module that isn't present or active |
| 259 | `MGMT_ERR_HPI_INSUFFICIENT_STORAGE` | SD card full, NVS full |
| 260 | `MGMT_ERR_HPI_TRANSFER_INVALID` | Unknown `xfer` in download chunk/end |
| 261 | `MGMT_ERR_HPI_VERSION_MISMATCH` | OTA image doesn't match board revision |
| 262 | `MGMT_ERR_HPI_CONFIG_KEY_UNKNOWN` | Settings key not in schema (§6) |
| 263 | `MGMT_ERR_HPI_CONFIG_TYPE_MISMATCH` | Settings value type wrong for key |
| 264 | `MGMT_ERR_HPI_LOCK_CHALLENGE_MISSING` | Caller must request `unlock_challenge` first |
| 265 | `MGMT_ERR_HPI_LOCK_HMAC_INVALID` | `unlock_response` HMAC did not verify |
| 266 | `MGMT_ERR_HPI_LOCK_EXPIRED` | Unlock grant TTL expired; re-unlock required |

Vendors extending group 64 in derivative products should start at 512 to avoid collisions with future HealthyPi additions.

### 4.3. Error detail

MCUmgr responses may include an `err` CBOR map with `rc` (int) and `rsn` (tstr) — a human-readable reason. HealthyPi handlers should populate `rsn` for any non-OK return to aid debugging. mcumgr-cli prints this automatically; the Python library surfaces it as `exception.detail`.

## 5. Events (MCUmgr async notifications)

Events are asynchronous notifications from the device to the host, emitted as MCUmgr SMP responses with no matching request on group 64. Each event carries a CBOR map with an `evt` type string plus event-specific fields and a `t` device-monotonic timestamp (uint, ms).

### 5.1. Event catalog

| Cmd ID | `evt` string | Fields | Description |
|--------|-------------|--------|-------------|
| 0x00F0 | `charge_state` | `{ state: uint }` — 0=dis, 1=charging, 2=full, 3=fault | MP2696B charge state transition |
| 0x00F1 | `batt_low` | `{ vbat_mv: uint }` | Battery voltage below threshold |
| 0x00F2 | `usb_plug` | `{ plugged: bool }` | USB connect/disconnect |
| 0x00F3 | `module_inserted` | `{ slot: uint, type: uint }` | HealthyLink module plugged in |
| 0x00F4 | `module_removed` | `{ slot: uint }` | HealthyLink module unplugged |
| 0x00F5 | `wifi_state` | `{ state: uint }` | WiFi state transition |
| 0x00F6 | `sd_inserted` | `{}` | SD card hot-insert detected |
| 0x00F7 | `sd_removed` | `{}` | SD card hot-removal detected |
| 0x00F8 | `sd_error` | `{ rc: int }` | FS or I/O error |
| 0x00F9 | `lead_off` | `{ mask: uint }` | ECG lead-off bitmask changed |
| 0x00FA | `button` | `{ duration_ms: uint }` | Button press detected |
| 0x00FB | `over_temp` | `{ tc_x10: int }` | STM32 or battery over-temperature |
| 0x00FC | `diag_result` | see §8.3 | Self-test individual result |
| 0x00FD | `diag_complete` | see §8.3 | Self-test batch finished |
| 0x00FE | `reboot_pending` | `{ reason: uint }` | Device is about to reset |
| 0x00FF | `sd_format_progress` | `{ pct: uint }` | SD format progress |
| 0x0100+ | reserved | | Future expansion |

### 5.2. Subscription model

Unlike the pre-pivot design, **there is no explicit subscribe/unsubscribe command.** MCUmgr notifications are pushed whenever the device produces them, and hosts that don't want a given event simply ignore it.

Which events a device produces is controlled at compile time by Kconfig (each producer is gated) and at runtime by the device lock state (some events, like `sd_format_progress`, only occur after an unlocked command triggers them).

This simplifies the host library and removes the subscription mask bookkeeping that caused bugs in the pre-pivot reference implementation.

### 5.3. Notification transport detail

Per the SMP spec, notifications use op code `MGMT_OP_WRITE_RSP` (or `MGMT_OP_READ_RSP`) with a `seq` field of zero. Zephyr's MCUmgr exposes `mgmt_callback_register()` plus `smp_client` APIs that let handlers emit notifications from any thread context.

HealthyPi firmware emits notifications via a small helper in the hpi management group (`hpi_mgmt_notify(evt_name, cbor_fn)`), which serializes the CBOR map, wraps it in an SMP response, and queues it onto the MCUmgr TX workqueue. Producers do not block on transport availability — if CDC 1 is not connected, notifications are dropped silently (consistent with the "device idle when no host" principle).

## 6. Settings Schema

Stored in Zephyr settings subsystem on QSPI LittleFS partition, accessed via the stock MCUmgr `config` group (3). Keys are case-sensitive, UTF-8, max 32 characters.

### 6.1. Keys

"Sensitive" keys require the device to be **unlocked** for `config_write` (§10.3). "Read-public" keys can be read while locked; "read-sensitive" keys cannot.

| Key | Type | Default | Read | Write | Description |
|-----|------|---------|------|-------|-------------|
| `device/name` | string | `"healthypi6"` | public | sensitive | User-visible device name |
| `device/location` | string | `""` | public | sensitive | Optional location tag (lab, clinic) |
| `stream/default_channels` | uint (bitmask) | `0x03` (ECG+PPG) | public | sensitive | Auto-start channel set |
| `stream/annotations_enabled` | bool | `true` | public | sensitive | Include QRS/HR/SpO2 in stream by default |
| `wifi/ssid` | string | `""` | public | sensitive | ESP32-C6 NVS authoritative; M7 caches last-known SSID only. No password stored on M7. `hpi/wifi_forget` clears NVS and cache. |
| `wifi/autostart` | bool | `false` | public | sensitive | Connect to stored WiFi on boot |
| `tcp/port` | uint | `7000` | public | sensitive | TCP listen port for streaming |
| `tcp/autostart` | bool | `false` | public | sensitive | Start TCP server on WiFi connect |
| `display/brightness` | uint | `80` | public | public | 0–100, backlight PWM (non-sensitive UX setting) |
| `display/timeout_s` | uint | `300` | public | public | Backlight off after N seconds idle |
| `power/auto_off_s` | uint | `0` | public | sensitive | Power off after N seconds on battery idle (0 = never) |
| `time/rtc_set_at_boot` | bool | `false` | public | internal | Set by firmware after first `os/datetime_set`; host cannot write directly |
| `locale/temperature_unit` | string | `"C"` | public | public | `"C"` or `"F"` |
| `hl/slot_a_autoenable` | bool | `false` | public | sensitive | Enable slot A LDO on boot |
| `hl/slot_b_autoenable` | bool | `false` | public | sensitive | Enable slot B LDO on boot |
| `ml/beat_classifier_enable` | bool | `true` | public | sensitive | Run TFLite on M4 when ECG streaming |
| `security/unlock_grant_s` | uint | `600` | **sensitive** | sensitive | TTL in seconds for an unlock grant (range 60–3600). See §10.4. |

**Extensibility:** firmware team may add keys by updating this table and bumping the group-64 minor schema version (`gv`). Host library treats unknown keys as opaque.

**Migration policy (Phase 0 decision, 2026-04-24):** lazy. When firmware boots with a LittleFS image written by an older build:

- Stored keys not in the current schema are ignored (not deleted, so a downgrade can still read them).
- Schema keys not present in storage read as their default value.
- Type changes on an existing key require an explicit per-key migration; this has not happened in v1 but the policy is: add a new key with the new type, deprecate the old one per §9.3, never silently reinterpret stored bytes.

## 7. SD File Container Format

> **⚠️ Superseded — do not implement this section.** It specifies an early
> container design that the firmware never shipped. What the device actually
> writes and streams is the `DBLK` frame format documented in
> **[HP6_DATA_FORMAT.md](HP6_DATA_FORMAT.md)**: extension `.HP6`, magic
> `"HPI6"`, version `0x0300`, a 256-byte header and per-block CRC-32 — not the
> `.hpi` / `"HPI\0"` / 128-byte-header / channel-table layout below. The section
> is retained only until this document's next revision.

Format name: **HPI** (HealthyPi recording). File extension: `.hpi`. Endianness: **little-endian** for all multi-byte fields.

### 7.1. File Layout

```
┌──────────────────────┐
│  File Header (128 B) │  fixed size
├──────────────────────┤
│  Channel Table       │  variable, N channels × 32 B
├──────────────────────┤
│  Metadata Block      │  variable, JSON or key=value
├──────────────────────┤
│  Data Blocks         │  repeated, each 4 KB typical
│  ...                 │
├──────────────────────┤
│  Footer (64 B)       │  total stats, whole-file CRC
└──────────────────────┘
```

### 7.2. File Header (128 bytes)

```
Offset  Size  Field
------  ----  -----
 0      4     Magic "HPI\0"
 4      2     Format version (currently 1)
 6      2     Header size (128)
 8      8     Creation time, unix ms
16      16    Device serial number (null-padded)
32      16    Firmware version (null-padded)
48      4     Board revision code (e.g. 0x00000003 for v3)
52      4     Channel count
56      4     Channel table offset (from file start)
60      4     Channel table size
64      4     Metadata offset
68      4     Metadata size
72      4     First data block offset
76      4     Data block size (typical; variable allowed)
80      8     Duration estimate, ms (updated by footer on close; 0 if file is mid-write)
88      36    Reserved (zero-filled)
124     4     Header CRC32 (over offsets 0–123)
```

### 7.3. Channel Table Entry (32 bytes each)

```
 0      1     Channel type (1=ECG, 2=PPG, 3=RESP, 4=EEG, 5=ANNOTATION)
 1      1     Sub-index (0..3 for ECG leads, 0..1 for PPG Red/IR, etc.)
 2      2     Sample rate Hz
 4      1     Sample type (0=s24, 1=s16, 2=s32, 3=f32)
 5      1     Bytes per sample
 6      2     Flags (bit 0: signed, bit 1: big-endian sample)
 8      16    Channel name (null-padded, UTF-8)
24      4     Physical scale numerator (to convert raw → physical unit)
28      4     Physical scale denominator
```

### 7.4. Metadata Block

UTF-8 JSON, up to 4 KB. Required fields: `device_serial`, `firmware_version`, `start_time_iso`, `channels` (array mirroring channel table), `patient_id` (optional), `session_notes` (optional).

### 7.5. Data Block (typical 4 KB)

```
 0      4     Magic "DBLK"
 4      4     Block length including header (bytes)
 8      4     Block sequence number (monotonic)
12      8     Start timestamp, device monotonic ms
20      1     Channel ID (index into channel table)
21      1     Flags (bit 0: last block for this channel)
22      2     Sample count
24      4     Reserved
28      N     Samples (sample_count × bytes_per_sample)
28+N    4     Block CRC32 (over offsets 0 .. 27+N)
```

Multiple channels interleave in the file — each data block belongs to one channel. A reader reconstructs per-channel time series by sorting blocks per channel-id and concatenating.

### 7.6. Footer (64 bytes)

```
 0      4     Magic "HPIE"
 4      8     Recording stop time, unix ms
12      8     Total duration ms
20      4     Total data blocks
24      4     Total samples across all channels
28      32    Reserved
60      4     Whole-file CRC32 (header + channel table + metadata + all data blocks)
```

If the footer is absent (magic byte mismatch at end of file), the file was not closed cleanly. Readers may still process the data blocks; duration is inferred from last block's timestamp.

## 8. Self-Test Specification

`hpi/diag_run_selftest` (§3.11) runs a fixed sequence of on-device checks. Each check reports via an `hpi/evt_diag_result` notification; a final `hpi/evt_diag_complete` signals the end.

### 8.1. Test Definition

Each test has:
- `id` (uint) — stable identifier, u16 range
- `name` (tstr) — human-readable
- `required` (bool) — must pass for overall PASS
- `timeout_ms` (uint) — max execution time

### 8.2. Standard Test List (v1)

| ID | Name | Required | What it does |
|-----|------|----------|--------------|
| 0x0001 | `smp_loopback` | Yes | `os echo` round-trip (inherently passes if SMP dispatcher runs) |
| 0x0002 | `sdram_sweep` | Yes | Walks 10 test patterns across 32 MB SDRAM; reads back |
| 0x0003 | `qspi_jedec_id` | Yes | Reads 0xEF4021 expected for W25Q01JV |
| 0x0004 | `flash_xip` | Yes | Executes a short code block from QSPI XIP region |
| 0x0005 | `ads1294r_present` | Yes | Reads ID register; expects 0x9X |
| 0x0006 | `afe4400_present` | Yes | Reads ID register; expects 0x00 |
| 0x0007 | `max17048_present` | Yes | I²C probe; expects ack |
| 0x0008 | `mp2696b_present` | Yes | I²C probe |
| 0x0009 | `ipc_heartbeat` | Yes | Ping M4 via OpenAMP, expects response < 50 ms |
| 0x000A | `m4_algorithms_alive` | Yes | Queries M4 for QRS detector state; expects non-error |
| 0x000B | `esp32_bridge_alive` | No | HealthyBridge SPI handshake |
| 0x000C | `display_backlight` | No | Toggles backlight GPIO; result is "manual" (check visually) |
| 0x000D | `touch_controller_present` | No | I²C probe GT911 |
| 0x000E | `sd_card_mounted` | No | LittleFS mount status |
| 0x000F | `gt911_touch_controller` | No | I²C probe + firmware version |
| 0x0010 | `rtc_running` | Yes | Compares RTC tick across two reads 10 ms apart |
| 0x0011 | `battery_voltage_sane` | Yes | Reads MAX17048; expects 3.0 – 4.3 V |
| 0x0012 | `adc_vref_internal` | Yes | STM32 internal VREFINT reads expected band |
| 0x0013 | `hl_slot_a_eeprom` | No | If module seated, reads EEPROM; else marks SKIP |
| 0x0014 | `hl_slot_b_eeprom` | No | Same for slot B |
| 0x0015 | `ecg_signal_presence` | No | 2 s ECG capture; RMS above noise floor |
| 0x0016 | `ppg_signal_presence` | No | 2 s PPG capture; RMS above noise floor |
| 0x0017 | `temperature_sane` | Yes | STM32 temperature sensor reads –10 to +80 °C |

### 8.3. Notification payloads

`hpi/evt_diag_result` (cmd ID 0x00FC) — emitted once per test:
```
  {
    evt: "diag_result",
    t:   uint,      # device-monotonic ms
    id:  uint,      # test ID from §8.2
    rc:  uint,      # 0=PASS, 1=FAIL, 2=SKIP, 3=MANUAL
    dur_ms: uint,
    detail: tstr    # human-readable, <=128 chars
  }
```

`hpi/evt_diag_complete` (cmd ID 0x00FD) — emitted once at end:
```
  {
    evt: "diag_complete",
    t: uint,
    total: uint,
    passed: uint,
    failed: uint,
    skipped: uint,
    overall: uint,   # 0=PASS, 1=FAIL
    dur_ms: uint,
    suite_ver: uint, # bumped on every change to §8.2
    flavor: uint     # 0=basic (customer), 1=factory
  }
```

### 8.4. Test Versioning and shipped-build policy

**Test suite versioning (Phase 0 decision, 2026-04-24):** every change to the §8.2 table bumps `suite_ver` by 1. Manufacturing pins to a specific `suite_ver` for EOL acceptance; QA automation asserts on this value so upgrades don't silently change the pass criteria.

**Shipped build flavor (Phase 0 decision, 2026-04-24):** two build flavors:

- **Basic** (`CONFIG_HPI_SELFTEST_BASIC=y`, default for customer firmware): only the **required** tests in §8.2 (marked "Yes") compile in. 15 tests. Keeps flash footprint bounded given the M7 is at 88 %.
- **Factory** (`CONFIG_HPI_SELFTEST_FULL=y`): full §8.2 list including optional/manual tests. Used for manufacturing and for field diagnostics builds.

The `flavor` field in `evt_diag_complete` lets the host library confirm which flavor produced a given report.

## 9. Protocol Versioning

MCUmgr SMP itself is versioned by Zephyr (currently SMP v1). HealthyPi layers its own **group-64 schema version** on top to track additive changes to our command catalog.

### 9.1. Schema version

- `gv` (group-version) is a uint returned by `hpi/device_info` (§3.3).
- Convention: **major.minor** encoded as `(major << 8) | minor`.
  - Major bumps: removing a command, changing a CBOR schema incompatibly, renaming a key.
  - Minor bumps: adding a command, adding an optional key, adding an extension error code, adding a settings-schema key.
- Current PRD specifies `gv = 0x0001` (v1.0 of the HealthyPi management group, post-pivot).

### 9.2. Compatibility policy

- Firmware must accept commands from hosts speaking the same major and any minor ≤ its own.
- Host library supports current major and one prior major.
- Host queries `hpi/device_info` once after connect to learn `gv`; on major mismatch it refuses to proceed and reports `FirmwareVersionError`.
- Unknown commands return `MGMT_ERR_ENOTSUP`, never silently no-op.
- Unknown CBOR keys in a request are ignored by the handler; unknown keys in a response are ignored by the library. This gives both sides room to add fields without breaking older peers.

### 9.3. Deprecation process

When a command is deprecated:
1. Mark it as deprecated in the next minor version; handler still works.
2. Library emits a warning when the command is used.
3. Remove in the next major version.
4. A replacement command must exist before deprecation begins.

## 10. Security model

This section covers the HealthyPi 6 control-plane security posture for customer shipments. Hardware-level countermeasures (STM32 RDP levels, JTAG lockout, enclosure tamper, supply-chain signing-key provenance) are **not specified anywhere yet** — earlier drafts of this section pointed at a companion `SYSTEM_SECURITY.md` that was never written.

### 10.1. Threat model

| Attacker | Access | Scenario |
|----------|--------|----------|
| Physical USB attacker | Unattended device, USB cable | Extract recorded patient data, install malicious firmware, pivot to network |
| Evil maid | Brief USB access during clinical session | Install persistent malware, exfiltrate credentials |
| Supply-chain intermediary | Between manufacture and customer | Backdoor firmware, subvert signing key |
| WiFi/BLE-local attacker | Same-network proximity | Intercept streaming, DoS, pivot via OTA (BLE/Wi-Fi SMP is **deferred to future** — not a v1 surface) |
| Lost/stolen device | Full physical | Access historical `.hpi` recordings on SD |

HealthyPi 6 handles biomedical signals — in most jurisdictions PHI — even outside formal medical-device clearance. Reasonable security hygiene is mandatory regardless of regulatory regime.

### 10.2. Enabled vs disabled MCUmgr groups in customer builds

Customer firmware enables only the groups listed in §3.1. Specifically:

**Enabled groups** (stock): `os`, `img`, `stat`, `config` — plus custom group `hpi` (64).

**Explicitly disabled groups:**
- **`fs` (group 8)**. Stock FS access would expose SD card contents, including `.hpi` recordings. `.hpi` download is served by `hpi/sd_download_*` with explicit lock-gating (§3.9). If future needs require `fs`, gate it behind `CONFIG_MCUMGR_GRP_FS_FILE_ACCESS_HOOK=y` with an allowlist limited to `/lfs/ota/` and nothing in `/sd/`.
- **`log` (group 4)**. HealthyPi logs contain PHI (heart rates, beat timing, event timestamps). Scoped/redacted log access is served by `hpi/log_*` (§3.12).
- **`shell` (group 9)**. Never enabled in customer builds. Behind `CONFIG_HPI_DEV_MODE=y` in developer builds only.

Production-build Kconfig asserts at link time that none of these groups is registered; see §10.7.

### 10.3. Device lock state machine

The device boots **locked**. In this state it serves only read-only information (device info, telemetry, lead-off, scan, status) and responds to locked destructive commands with `MGMT_ERR_EACCES` plus an `rsn` of `"device locked; call hpi/unlock_challenge + hpi/unlock_response"`.

**Commands that require the device to be unlocked:**

| Command | Category |
|---------|----------|
| `os reset` | reboot |
| `img upload` / `img erase` / `img test` | OTA |
| `config_write` (for keys tagged "sensitive" in §6.1) | config |
| `os/datetime_set` | RTC write |
| `hpi/stream_start` | stream (PHI protection) |
| `hpi/module_power` | power rails |
| `hpi/sd_record_start` | records PHI |
| `hpi/sd_download_begin` | reads PHI |
| `hpi/sd_delete` / `hpi/sd_format` | destructive |
| `hpi/wifi_set` / `hpi/wifi_forget` | WiFi credentials |
| `hpi/log_subscribe` / `hpi/log_get_buffered` | may leak debug info |

**Commands that remain available while locked** (no PHI, non-destructive, never secret):
`os echo`, `os mcumgr_params`, `os datetime_get`, `stat read`, `stat list`, `img state`, `config_read` (non-sensitive keys), `hpi/device_info`, `hpi/fw_versions`, `hpi/telemetry`, `hpi/stream_stop`, `hpi/stream_status`, `hpi/module_list`, `hpi/module_info`, `hpi/sd_status`, `hpi/sd_list`, `hpi/sd_record_stop` (allow stop-in-progress), `hpi/wifi_status`, `hpi/wifi_scan`, `hpi/diag_*`, `hpi/log_unsubscribe`, `hpi/lock`, `hpi/lock_state`, `hpi/unlock_challenge`, `hpi/unlock_response`.

### 10.4. Unlock protocol

Challenge–response using a device-unique shared secret provisioned at manufacture.

```
Host                                             Device (locked)
  ──── hpi/unlock_challenge ───────────────────► generates nonce[16] = CSPRNG
                                                  stores (nonce, expiry = now + 60 s)
  ◄──── { nonce: bstr[16], ttl_ms: 60000 } ────
HMAC-SHA256(shared_secret, nonce) = tag[32]
  ──── hpi/unlock_response { tag } ────────────► if HMAC matches AND within TTL:
                                                     state = UNLOCKED
                                                     expires_at = now + grant_ttl
                                                     zeroize nonce
                                                  else: MGMT_ERR_HPI_LOCK_HMAC_INVALID
  ◄──── { expires_at_ms: uint, ttl_ms: uint } ─
```

**Properties:**
- Nonce is single-use — each `unlock_challenge` invalidates any prior outstanding nonce on the same transport. Prevents replay.
- Nonce TTL: 60 seconds. If the host doesn't respond in time, a new challenge is required.
- Unlock grant TTL: **10 minutes** default; configurable via `security/unlock_grant_s` setting (range 60–3600 s).
- Unlock grant expires on: TTL elapse, `hpi/lock` call, CDC 1 DTR-down (host disconnect), or device reboot.
- HMAC-SHA256 over raw bytes. Zephyr's mbedTLS backend provides this.

**Shared secret:**
- 256-bit random value, provisioned at manufacture, stored in STM32H7 OTP/protected flash region (`CONFIG_HPI_UNLOCK_SECRET_SOURCE=otp`).
- Printed as a QR code on a sticker inside the device enclosure so the owner has physical recovery. A lost sticker means RMA.
- A development-build fallback allows the secret to live in Kconfig for dev boards; flagged at boot with a prominent log line `[WARN] dev-mode secret from Kconfig; do not ship`.

### 10.5. MCUboot signing and anti-rollback

Required Kconfig for all customer builds:

```kconfig
CONFIG_BOOT_SIGNATURE_TYPE_ECDSA_P256=y
CONFIG_BOOT_SIGNATURE_TYPE_NONE=n
CONFIG_BOOT_ENCRYPT_EC256=y          # encrypted image payload in transit
CONFIG_BOOT_SECURITY_COUNTER=y       # anti-rollback
CONFIG_BOOT_WATCHDOG_FEED=y          # do not brick on bad boot
CONFIG_BOOT_MAX_IMG_SECTORS=512      # sized for this platform
```

**Key management:**
- Production signing key stored in HSM or air-gapped signing server.
- Never on developer laptops or CI runners.
- Dev builds use a separate dev key (`CONFIG_MCUBOOT_IMGTOOL_SIGN_KEY_FILE` pointing to a file gitignored).
- The boot verification path is **always** enabled — there is no "skip signature" configuration in either build flavor. A build that skips verification will never accidentally ship.

**Anti-rollback:**
- Each image carries a monotonically increasing security counter in the image header.
- MCUboot refuses to boot an image whose counter is less than the one stored in the anti-rollback OTP region.
- Counter bumps happen only when there is a known security fix; not every minor release.

### 10.6. Transport confidentiality

USB CDC is a physical-layer channel. Contents on the wire between the laptop and the device are not encrypted but are also not observable to a remote attacker. Accept this as the model.

Future transports (BLE, WiFi SMP) will require transport-level encryption:
- BLE: `CONFIG_BT_SMP=y` + LE Secure Connections pairing, encrypted link required.
- WiFi SMP: `CONFIG_MCUMGR_TRANSPORT_UDP` with TLS preferred; if unavailable, at minimum require the app-layer unlock (§10.4) and reject unauthenticated writes.

### 10.7. Production vs development build matrix

Two Kconfig fragments gate the two flavors:

```kconfig
# prj.prod.conf — customer shipping build
CONFIG_HPI_DEV_MODE=n
CONFIG_MCUMGR_GRP_FS=n
CONFIG_MCUMGR_GRP_LOG=n
CONFIG_MCUMGR_GRP_SHELL=n
CONFIG_HPI_SELFTEST_BASIC=y
CONFIG_HPI_SELFTEST_FULL=n
CONFIG_LOG_MODE_MINIMAL=y
CONFIG_LOG_DEFAULT_LEVEL=2          # warn/err only
CONFIG_HPI_UNLOCK_SECRET_SOURCE=otp
CONFIG_BOOT_SIGNATURE_TYPE_ECDSA_P256=y
CONFIG_BOOT_SECURITY_COUNTER=y
```

```kconfig
# prj.dev.conf — developer workstation build
CONFIG_HPI_DEV_MODE=y
CONFIG_MCUMGR_GRP_FS=y              # allowed in dev only
CONFIG_MCUMGR_GRP_LOG=y
CONFIG_MCUMGR_GRP_SHELL=y
CONFIG_HPI_SELFTEST_FULL=y
CONFIG_LOG_DEFAULT_LEVEL=4          # info
CONFIG_HPI_UNLOCK_SECRET_SOURCE=kconfig
CONFIG_BOOT_SIGNATURE_TYPE_ECDSA_P256=y   # even dev builds sign
```

**Enforcement:** `hpi_mgmt_group_register()` checks `CONFIG_HPI_DEV_MODE` at runtime and refuses to register the dev group commands in prod builds, with a `BUILD_ASSERT` mirror at compile time.

### 10.8. Surface-area summary

Compared to pre-pivot custom protocol:

| Aspect | Pre-pivot | Post-pivot (this PRD) |
|--------|-----------|-----------------------|
| OTA integrity | would have been custom (unaudited) | MCUboot ECDSA signing + anti-rollback |
| Destructive-op gate | 0xA5 magic byte (security theater) | HMAC-SHA256 unlock with nonce + TTL |
| Stream exposure | always on, anyone with USB | `stream_start` requires unlock |
| SD download | custom command, always available | `sd_download_*` requires unlock; stock `fs` disabled |
| Log PHI leakage | filterable at subscribe time | stock `log` disabled; custom redacted `hpi/log_*` requires unlock |
| Fuzzing surface | custom framer + CRC + parser | well-audited Zephyr SMP + zcbor |
| Replay | none | per-transport single-use nonce |
| Transport auth | none | none (physical USB); app-layer unlock required |

### 10.9. Residual risks (acknowledged)

- **Shared-secret QR sticker inside enclosure.** Lost access = RMA. Trade-off chosen for simplicity over TOTP/web-unlock; revisit if RMA rate is high.
- **WiFi password transits USB cleartext during setup.** Local USB only; user must provision in trusted environment. BLE provisioning (future) will solve this.
- **Physical teardown gives the secret.** Acceptable: an attacker with physical disassembly access has many other attack paths (direct flash readback, fault injection). Mitigation: STM32 RDP level 2, which is not yet specified or enabled.
- **Kconfig hardening is fragile against misbuild.** Mitigation: CI must build `prj.prod.conf` and run a test that asserts all disabled groups are absent from the registration table.

---

## 11. Writing a host application

```
Studio launches             → (no ports open)
User picks a device         → open CDC 1
                              query hpi/device_info, telemetry, etc.
                              show "connected" UI
User clicks "Start Live"    → send hpi/stream_start on CDC 1
                              open CDC 0, start a reader task
                              render incoming sample frames
User clicks "Stop"          → send hpi/stream_stop on CDC 1
                              close CDC 0
User disconnects            → close CDC 1
```

**Rules of the road:**
- CDC 1 stays open as long as the user has a device "connected" in the UI.
- CDC 0 is open **only while actively streaming**, and **must always have a reader pulling bytes** while open. Without a reader, kernel-side buffers fill, the device's ring fills, and stream frames are silently dropped at the device.
- Always send `hpi/stream_stop` *before* closing CDC 0 — never close CDC 0 with the stream still enabled.
- Apps that only configure or update the device (no live waveform display) only ever need to open CDC 1.

---

## 12. A worked example

The repo ships [`tools/healthypi/`](../tools/healthypi/) — a pip-installable library plus the `healthypi` / `hpi` CLI that talks to a device over CDC 1. Use it as:

- A worked example of how to connect and issue commands from Python
  (`healthypi.transport.serial_smp` for the connection, `healthypi.smp.group64` for the commands)
- A reference for CBOR schema mapping — [`smp/catalog.py`](../tools/healthypi/src/healthypi/smp/catalog.py)
  declares every group-64 command and the wire classes are generated from it, so
  there is one description to read rather than a hand-written class per tool
- A decoder for what comes back on CDC 0 — `healthypi.hp6` parses the same
  `.HP6` frames from a live stream or a recorded file

```bash
pip install -e tools/smpgroup -e tools/healthypi
healthypi catalog                       # every group-64 command and its status
healthypi device info --port <CDC1>
```

---

## 13. Pitfalls

- ❌ Open CDC 0 without a reader task draining bytes — backpressure into the device
- ❌ Close CDC 0 while stream is still enabled — frames keep being produced and dropped
- ❌ Use CDC 0 for control — it's one-way; anything written to it is silently discarded
- ❌ Mix MCUmgr and stream framing on a single port — they live on separate CDCs by design
- ❌ Hold CDC 1 open across a `mcumgr reset` without re-enumerating — the USB stack tears down on reboot
- ❌ Assume `m4fw` / `espfw` / `mod_*_fw` strings in `device_info` are populated in Phase 2 — they're empty placeholders until Phases 4/6/10 wire them in

---
