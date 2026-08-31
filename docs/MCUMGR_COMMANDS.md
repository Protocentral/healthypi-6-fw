# HealthyPi 6 MCUmgr command reference

How to control a HealthyPi 6 from a host, using MCUmgr — the same management
protocol Zephyr devices use for firmware update and device management.

The device speaks **standard SMP**. Stock commands (`echo`, `reset`, `datetime`,
firmware upload) work with any off-the-shelf MCUmgr client. HealthyPi-specific
functionality lives in **management group 64**, which needs a client that can
send a custom group ID — most can.

- [1. Connecting](#1-connecting)
- [2. Choosing a client](#2-choosing-a-client)
- [3. The wire protocol](#3-the-wire-protocol)
- [4. Stock MCUmgr groups](#4-stock-mcumgr-groups)
- [5. Group 64 — HealthyPi commands](#5-group-64--healthypi-commands)
- [6. Error codes](#6-error-codes)
- [7. Constants and enumerations](#7-constants-and-enumerations)
- [8. Reserved command IDs](#8-reserved-command-ids)
- [9. Compatibility](#9-compatibility)

Firmware 1.0.0 · group schema version `0x0001`.

---

## 1. Connecting

Plugging in one USB cable enumerates **two serial ports**:

| Port | Purpose | Direction |
|---|---|---|
| **CDC 0** | `.HP6` sample stream | device → host only |
| **CDC 1** | **MCUmgr / SMP control** | bidirectional |

**Everything in this document happens on CDC 1.** CDC 0 never answers a
command; it emits sample data and nothing else.

The device enumerates as *HealthyPi 6* (Protocentral). On macOS and Linux both
ports appear as `/dev/tty.usbmodem*` or `/dev/ttyACM*`; on Windows, as two COM
ports.

**Identifying CDC 1:** send an `os echo` to each candidate — only CDC 1 replies.
On macOS the control port is usually the higher-numbered device
(`…usbmodem31103` vs `…usbmodem31101`), but do not rely on numbering. Baud rate
is irrelevant (USB CDC ignores it); clients typically open at 115200.

> **If you also open CDC 0, you must continuously read from it.** The device
> drops sample frames when the host stops draining the endpoint — it will never
> stall data acquisition to wait. Always send `stream_stop` *before* closing
> CDC 0.

---

## 2. Choosing a client

| Client | Stock groups | Group 64 |
|---|---|---|
| [`mcumgr` CLI (Go)](https://github.com/apache/mynewt-mcumgr-cli) | ✅ | ❌ no custom-group support |
| [`smpclient` (Python)](https://pypi.org/project/smpclient/) | ✅ | ✅ define your own request classes |
| [`smpmgr` (Python CLI)](https://pypi.org/project/smpmgr/) | ✅ | ✅ via plugins |
| [`protocentral-healthypi`](https://pypi.org/project/protocentral-healthypi/) | ✅ | ✅ every command below, pre-defined |
| Your own | — | ✅ see §3 |

The simplest path for group 64:

```bash
pip install protocentral-healthypi
healthypi device info --port /dev/tty.usbmodem31103
healthypi telemetry --port /dev/tty.usbmodem31103 --json
```

Or directly with `smpclient`:

```python
import asyncio
from smpclient import SMPClient
from smpclient.transport.serial import SMPSerialTransport
from smp.message import ReadRequest, ReadResponse

class DeviceInfoResponse(ReadResponse):
    _GROUP_ID = 64
    _COMMAND_ID = 0x0001
    sn: str; fw: str; gv: int; br: str
    hw: bytes; m4fw: str; espfw: str; up: int

class DeviceInfo(ReadRequest):
    _GROUP_ID = 64
    _COMMAND_ID = 0x0001
    _Response = DeviceInfoResponse

async def main():
    async with SMPClient(SMPSerialTransport(), "/dev/tty.usbmodem31103") as c:
        print(await c.request(DeviceInfo()))

asyncio.run(main())
```

> Response models in `smpclient` reject unknown keys. **Declare every field the
> device sends** — the tables in §5 list them all — or the reply fails to parse.

---

## 3. The wire protocol

HealthyPi changes nothing here; this is standard Zephyr SMP, summarized so you
can implement a client in any language.

### SMP header — 8 bytes, big-endian

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | `op` — 0 read, 1 read-response, 2 write, 3 write-response |
| 1 | 1 | flags (0) |
| 2 | 2 | payload length |
| 4 | 2 | **group id — 64 (`0x0040`) for HealthyPi commands** |
| 6 | 1 | sequence number, echoed in the reply |
| 7 | 1 | command id within the group |

The payload is a **CBOR map**. A request with no arguments sends an empty map
(`0xA0`). Replies are a CBOR map of the response keys, or an error map.

### Serial framing

Each SMP packet is wrapped for the serial link:

1. Compute **CRC-16/CCITT** (polynomial `0x1021`, initial value `0x0000`) over
   the SMP packet.
2. Build `[uint16 BE total_length][SMP packet][uint16 BE crc]`, where
   `total_length` is the SMP packet length **plus 2** for the CRC.
3. Base64-encode that byte string.
4. Emit it as frames of at most **127 bytes** including the marker and newline.
   The first frame starts with `0x06 0x09`; each continuation frame starts with
   `0x04 0x14`. Every frame ends with `\n`.

Responses arrive in the same framing. One request in flight at a time is the
conventional discipline.

### Sizing

The device accepts SMP packets up to **512 bytes**. Query the exact limits at
runtime with `os mcumgr_params` rather than hard-coding them.

---

## 4. Stock MCUmgr groups

These work with any MCUmgr client.

| Group | Command | Notes |
|---|---|---|
| `os` (0) | `echo` | Connectivity check — the way to identify CDC 1 |
| `os` (0) | `reset` | Reboot the device |
| `os` (0) | `mcumgr_params` | Buffer size and count |
| `os` (0) | `datetime` read/write | Sets the real-time clock — **do this before recording**, or recordings carry no wall-clock time |
| `settings` (3) | read/write/delete | Device settings |
| `img` (1) | `state`, `upload` | **Signed builds only** — firmware update |

```bash
mcumgr --conntype serial --connstring dev=/dev/tty.usbmodem31103,baud=115200 echo hello
mcumgr ... datetime write "2026-07-30T09:15:00"
mcumgr ... image list
```

The `img` group is present only in signed production builds. On a development
build it returns "group not found" — that is expected, not a fault.

---

## 5. Group 64 — HealthyPi commands

**Group ID 64 (`0x0040`).** `Op` is the SMP operation to use — sending the wrong
one returns `ENOTSUP` even for an implemented command.

A **lock** marker means the command requires the device to be unlocked when the
access-control gate is enabled (see [DEVICE_LOCK.md](DEVICE_LOCK.md)). In the shipping
default configuration the gate is off and these succeed without an unlock.

### 5.1. Device information

#### `0x0001` device_info — read

Request `{}` →

| Key | Type | Meaning |
|---|---|---|
| `sn` | tstr | Serial number |
| `fw` | tstr | M7 firmware version |
| `gv` | uint | Group schema version |
| `br` | tstr | Board revision |
| `hw` | **bstr** | Hardware identifier (byte string, not text) |
| `m4fw` | tstr | Algorithm-core firmware version |
| `espfw` | tstr | Wi-Fi co-processor firmware version |
| `up` | uint | Uptime, seconds |

#### `0x0031` fw_versions — read

Request `{}` → `m7fw`, `m4fw`, `espfw`, `mod_a_fw`, `mod_b_fw` — all tstr.

### 5.2. Telemetry

#### `0x0030` telemetry — read

Request `{}` →

| Key | Type | Meaning |
|---|---|---|
| `vbat_mv` | uint | Battery voltage, mV |
| `ibat_ma` | **int** | Battery current, mA — signed; negative while charging |
| `soc` | uint | State of charge, % |
| `tc_x10` | **int** | Temperature × 10 — **always the unavailable sentinel in 1.0.0** |
| `charge` | uint | Charge state |
| `usb` | bool | USB power present |
| `batt` | bool | Running on battery — currently just `!usb` |
| `ok` | bool | Reading is valid |

### 5.3. Live streaming

Sample data flows on **CDC 0**, not in these replies. Format:
[HP6_DATA_FORMAT.md](HP6_DATA_FORMAT.md).

#### `0x0020` stream_start — write · 🔒 lock

| Key | Type | Meaning |
|---|---|---|
| `ch` | uint | Channel mask — see §7.1 |
| `ann` | uint | Annotation mask — see §7.2 |

Empty map `{}` on success. Returns **258** if a requested channel is not
available (e.g. EEG with no module fitted). Unknown mask bits are ignored rather
than rejected, so a newer client cannot break older firmware.

Two behaviours to know before you design around this:

- **Vitals blocks are streamed whenever streaming is active**, regardless of
  `ch`. There is no bit to disable them; they are low-rate.
- **`ann` is accepted, stored and echoed back by `stream_status`, but does not
  filter anything in firmware 1.0.0.** Send it for forward compatibility;
  do not expect it to change what arrives on CDC 0.

#### `0x0021` stream_stop — write

Request `{}` → `{}`. Idempotent; safe to send at any time.

#### `0x0022` stream_status — read

Request `{}` → `active` bool, `ch` uint, `ann` uint, `sent` uint,
`dropped` uint.

`dropped` counts frames the device discarded because the host wasn't reading
CDC 0 fast enough. A rising `dropped` is a host-side problem.

### 5.4. Recording to SD card

#### `0x0060` sd_status — read

Request `{}` → `active` bool, `bytes` uint, `dur` uint (ms), `ecg` uint,
`ppg` uint (samples written), `path` tstr.

#### `0x0061` sd_record_start — write · 🔒 lock

| Key | Type | Meaning |
|---|---|---|
| `name` | tstr | Optional session name; the device auto-names if omitted |

Reply: `path` tstr — where the recording is being written.

Returns **256** for *both* "already recording" and "no card present". Call
`sd_status` to tell them apart.

> Set the clock with `os datetime write` **before** starting a recording.
> Without it the file's start timestamp is 0 and the recording carries no
> wall-clock reference.

#### `0x0062` sd_record_stop — write

Request `{}` → `{}`.

### 5.5. Retrieving recordings — Transfer Mode

#### `0x0069` transfer_mode — read and write · write 🔒 lock

| Op | Request | Response |
|---|---|---|
| read | `{}` | `armed` bool |
| write | `on` bool | `armed` bool |

Arming exposes the SD card to the host as a **USB mass-storage device**.

> **Arming re-enumerates USB.** The CDC ports disappear and a disk appears, so
> your open CDC 1 connection *will* drop — that is expected behaviour, not a
> failure. Mount the disk, copy files, unmount, then disarm to get the serial
> ports back.

Returns **267** (`NO_MEDIA`) if no card is present; in that case the device does
*not* re-enumerate and your connection stays alive. The card must be inserted
before the device boots — there is no hot-insert detection.

**Transfer Mode is currently the only way to retrieve recordings.** There is no
file-download command over SMP; see §8.

### 5.6. Wi-Fi and BLE

> **The device boots with both radios off and the co-processor powered down, and
> it does not remember being switched on.** Every command in this section other
> than `0x0075` therefore answers "disconnected" on a freshly booted device until
> something enables connectivity. This is deliberate — bringing the radios up
> during boot browns out a USB-powered unit — so treat "off" as the normal
> resting state, not a fault. Use `0x0075 conn_enable` first, and `0x0077
> conn_status` to tell "off because it was asked to be" from "not responding".

#### `0x0070` wifi_status — read

Request `{}` → `state` uint (§7.4), `rssi` **int** (signed dBm), `ssid` tstr,
`ip` tstr. Returns **257** if the co-processor does not respond.

A powered-down or still-starting co-processor answers a well-formed
`state = 0` (disconnected) rather than an error, because neither is a fault. Read
`0x0077` if you need to distinguish them.

#### `0x0071` wifi_scan — read

**Not implemented in 1.0.0** — always returns `ENOTSUP`. Network selection
happens through the provisioning portal (`0x0074`). Handle the `ENOTSUP` as an
expected outcome.

#### `0x0072` wifi_set — write · 🔒 lock

`ssid` tstr, `pw` tstr → `ok` bool.

#### `0x0073` wifi_forget — write · 🔒 lock

`{}` → `ok` bool.

#### `0x0074` wifi_softap — write

`{}` → `ok` bool. Opens the co-processor's captive-portal access point for
network provisioning. Deliberately **not** lock-gated — it stores no secret, and
it is how a locked device gets onto a network in the first place. Returns
**257** on co-processor failure.

Powers the co-processor and lifts the Wi-Fi gate itself, so it works on a freshly
booted device with no preceding `conn_enable`.

#### `0x0075` conn_enable — write

`radios` uint → `ok` bool. Bitmask: **bit0 = Wi-Fi, bit1 = BLE**. Powers the
co-processor if it is not already, then brings up the radios named.

`radios = 0` is legal and means *powered, no radio*. Use it before flashing the
co-processor: it is held in reset by default and does not enumerate its
USB-Serial/JTAG port in that state.

The reply means **"request accepted", not "radio up"** — powering the part takes
the better part of a second and associating takes longer. Poll `0x0077` for the
outcome. Returns **257** if the request could not be queued.

Deliberately **not** lock-gated, for the same reason as `0x0074`: it stores no
secret, and a locked device must still be able to get itself online.

#### `0x0076` conn_disable — write

`{}` → `ok` bool. Radios down and the co-processor back into reset. Idempotent.

#### `0x0077` conn_status — read

`{}` → `link` uint, `radios` uint, `state` uint (§7.4), `rssi` int, `ssid` tstr,
`ip` tstr, `ble_adv` bool, `ble_conn` bool.

`link` is the field `wifi_status` cannot express:

| `link` | meaning |
|---|---|
| 0 | off — the co-processor is powered down because that was requested |
| 1 | starting — powered, not yet answering |
| 2 | up — answering |
| 3 | fault — powered past its budget and still silent |

`radios` echoes the mask most recently requested. Answered from a cache the
device refreshes about every 2 s, so it never costs a round-trip to the
co-processor and is safe to poll.

### 5.7. Expansion modules

#### `0x0050` module_list — read

Request `{}` → a map **keyed by slot**, not an array:

```
{ "a": {"state": u, "id": u, "pwr": bool, "name": tstr},
  "b": {"state": u, "id": u, "pwr": bool, "name": tstr} }
```

`state` values in §7.5.

#### `0x0052` module_power — write · 🔒 lock

`slot` uint, `on` bool → `ok` bool.

### 5.8. Diagnostics

#### `0x0080` diag_run_selftest — read

Request `{}` →

| Key | Type | Meaning |
|---|---|---|
| `suite_ver` | uint | Test suite version |
| `sd`, `batt`, `ecg`, `ppg`, `m4`, `qspi` | bool | Per-test pass/fail |
| `pass`, `fail` | uint | Totals |
| `health_overall` | uint | Aggregate subsystem health |
| `health` | **map** | `{acq, m4, stream, rec, hl}`, each uint |

`health` is a **nested map** — a flat parser will fail on it. Note that
top-level `m4` (bool, the self-test result) and `health.m4` (uint, the subsystem
state) are different things.

#### `0x0081` diag_lead_off — read

Request `{}` →

| Key | Type | Meaning |
|---|---|---|
| `mask` | uint | Electrodes off: `1` RA, `2` LA, `4` LL, `8` V1 |
| `ra`, `la`, `ll`, `v1` | bool | The same mask, unpacked |
| `age_ms` | uint | Since acquisition last updated the mask |
| `ok` | bool | The state is fresh (the ECG front end is producing samples) |
| `hr` | uint | Latest heart rate, `0` = none |
| `hr_src` | uint | `0` = ECG, `1` = PPG |
| `vflags` | uint | Raw vitals flags (see [HP6_DATA_FORMAT.md](HP6_DATA_FORMAT.md)) |

The mask is debounced (~100 ms) and named by **electrode**, not by AFE channel.
A large `age_ms` with `ok` false means the ECG front end has gone quiet — which
is *not* the same as "all electrodes connected", and the mask should be treated
as stale rather than as good news.

`hr_src` is the point of this command: the device reports one heart rate but has
two detectors, and a PPG pulse rate must not be recorded or displayed as an ECG
heart rate. While `mask` is non-zero the ECG rate is suppressed at the source,
so `hr` is either a PPG rate (`hr_src` = 1) or `0`.

### 5.9. Access control

Only relevant when the access-control gate is enabled; see
[DEVICE_LOCK.md](DEVICE_LOCK.md).

| ID | Command | Op | Request | Response |
|---|---|---|---|---|
| `0x0010` | unlock_challenge | read | `{}` | `nonce` bstr, `ttl_ms` uint |
| `0x0011` | unlock_response | write | `tag` bstr | `state` uint, `ttl_ms` uint |
| `0x0012` | lock | write | `{}` | `state` uint |
| `0x0013` | lock_state | read | `{}` | `state` uint |

Request a challenge, compute the HMAC tag over the nonce with the device secret,
send it back within the TTL. Errors: **264** no prior challenge, **265** bad
tag, **266** expired. `0x0010` returns `ENOTSUP` when no secret is provisioned.

### 5.10. Algorithm-core firmware update

Present only in **signed** builds; a development build returns **256** for all
of these. The M7 firmware itself updates through the stock `img` group.

| ID | Command | Op | 🔒 | Request | Response |
|---|---|---|---|---|---|
| `0x00A0` | m4fw_begin | write | 🔒 | `len` uint, `sha` bstr, `sig` bstr (optional) | `off` uint |
| `0x00A1` | m4fw_chunk | write | 🔒 | `off` uint, `data` bstr | `off` uint |
| `0x00A2` | m4fw_commit | write | 🔒 | `{}` | `rst` bool |
| `0x00A3` | m4fw_status | read | | `{}` | `st`, `len`, `rx` uint, `err` int, `rst`, `sig` bool |
| `0x00A4` | m4fw_abort | write | | `{}` | `{}` |

**Call `m4fw_status` first.** Its `sig` field tells you whether this firmware
requires a signature before you spend minutes uploading an image it will reject.
`off` in the `begin` reply is a resume offset, so an interrupted upload can
continue rather than restart.

The target flash is written only after the whole image has arrived *and* its
SHA-256 matches *and*, when required, its signature verifies — so an interrupted
or corrupt upload cannot leave the device unbootable. `m4fw_abort` discards an
in-flight upload; the running firmware is untouched either way.

Errors: **268** invalid image, **269** too large, **270** update already in
progress, **271** verifies but is not a valid image for this core.

### 5.11. Recovery mode

#### `0x00A5` enter_recovery — read and write

| Op | Request | Response |
|---|---|---|
| read | `{}` | `av` bool (available), `armed` bool |
| write | `arm` bool, `rst` bool | `armed` bool, `rst` bool |

Reboots into the bootloader's serial recovery mode, used to rescue a device
whose application will not start. In recovery the device enumerates as a
**single** CDC port named *HealthyPi 6 Recovery* and only the bootloader's `img`
group is available — everything in this document is gone until the device is
reflashed. Returns **256** if the mechanism is unavailable.

---

## 6. Error codes

Standard MCUmgr codes apply: `0` OK, `2` unknown, `3` invalid argument,
`4` message too large, `8` not supported, `11` access denied (locked).

HealthyPi adds codes from 256 upward, returned in the reply's error map as
`{"group": 64, "rc": <code>}`:

| Code | Name | Meaning |
|---|---|---|
| 256 | `NOT_READY` | Busy, or a precondition is missing — no card, wrong build, already running |
| 257 | `HW_FAULT` | Hardware or co-processor failure; commonly the Wi-Fi module not responding |
| 258 | `CHANNEL_NOT_AVAILABLE` | Requested a stream channel this device cannot produce |
| 259 | `INSUFFICIENT_STORAGE` | Not enough space |
| 260 | `TRANSFER_INVALID` | Transfer state invalid |
| 261 | `VERSION_MISMATCH` | Version mismatch |
| 262 | `CONFIG_KEY_UNKNOWN` | Unknown settings key |
| 263 | `CONFIG_TYPE_MISMATCH` | Settings value has the wrong type |
| 264 | `LOCK_CHALLENGE_MISSING` | Unlock attempted without a challenge |
| 265 | `LOCK_HMAC_INVALID` | Wrong unlock credential |
| 266 | `LOCK_EXPIRED` | Challenge or unlock grant expired |
| 267 | `NO_MEDIA` | No SD card present |
| 268 | `IMAGE_INVALID` | Firmware image digest or signature mismatch |
| 269 | `IMAGE_TOO_LARGE` | Image exceeds the target region |
| 270 | `BUSY` | An update is already in progress |
| 271 | `IMAGE_NOT_M4` | Image verifies but is not valid for that core |

Replies may also carry `rsn`, a human-readable string. Show it when present.

---

## 7. Constants and enumerations

### 7.1. Stream channel mask (`ch`)

| Bit | Value | Channel | Effect in 1.0.0 |
|---|---|---|---|
| 0 | `0x01` | ECG | ✅ selects ECG blocks — respiration is a field inside them |
| 1 | `0x02` | PPG | ✅ selects PPG blocks |
| 2 | `0x04` | Respiration | ⚠️ no effect — respiration is carried inside ECG, never as its own block |
| 3 | `0x08` | EEG | ✅ selects EEG blocks; **258** if no module is fitted |

Bits above 3 are masked off and ignored. There is no vitals bit: vitals blocks
are emitted whenever streaming is active.

A typical request is `ch = 0x03` (ECG + PPG).

### 7.2. Annotation mask (`ann`)

| Bit | Value | Annotation |
|---|---|---|
| 0 | `0x01` | QRS detection |
| 1 | `0x02` | Heart rate |
| 2 | `0x04` | SpO₂ |
| 3 | `0x08` | Respiration rate |
| 4 | `0x10` | Beat classification |

Bits above 4 are masked off. **These bits are reserved in firmware 1.0.0**: the
value is stored and reported by `stream_status`, but it does not currently
filter the stream. Derived values arrive in vitals blocks either way.

### 7.3. Lock state

`0` locked · `1` unlocked

### 7.4. Wi-Fi state

`0` disconnected · `1` connecting · `2` connected · `3` access-point
(provisioning) mode · `255` error

### 7.5. Module slot state

`0` empty · `1` module present but unsupported · `2` active · `3` error during
probe or claim · `4` quarantined after a fault

---

## 8. Reserved command IDs

The following IDs are allocated but **not implemented in firmware 1.0.0**. They
return `ENOTSUP`. Do not build against them:

- `0x0051` module_info
- `0x0063`–`0x0068` — SD file listing, download, delete and format
- `0x0082` — signal statistics
- `0x0090`–`0x0092` — log subscription and retrieval
- `0x00F0`–`0x00FF` — asynchronous event notifications (allocated; no events are
  emitted yet)

The most consequential gap is file download: **use Transfer Mode (§5.5) to get
recordings off the device.**

---

## 9. Compatibility

`device_info.gv` reports the group schema version, currently `0x0001`. Within a
major schema version:

- Command IDs and their meanings do not change.
- Existing response keys keep their name, type and meaning.
- **New keys may be added to any response.** Write clients that ignore keys they
  do not recognize. If your client library rejects unknown fields — `smpclient`
  does by default — you will need to update it when the device firmware gains a
  field.
- Commands may move from reserved (§8) to implemented. Detect capability by
  calling and handling `ENOTSUP`, not by comparing firmware version strings.

---

## See also

- [HP6_DATA_FORMAT.md](HP6_DATA_FORMAT.md) — the sample data format
- [HOST_INTERFACE.md](HOST_INTERFACE.md) — the full host interface contract
- [DEVICE_LOCK.md](DEVICE_LOCK.md) — the access-control gate
- [ARCHITECTURE.md § 9](ARCHITECTURE.md#9-firmware-update-and-recovery) — firmware update and recovery
