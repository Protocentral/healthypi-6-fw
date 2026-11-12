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

Firmware 1.0.0 · group schema version `0x0003`.

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
| `os` (0) | `reset` | Reboot the device. **Signed builds refuse it** with group-64 **274** while an ESP32-C6 update is in flight or its new image is still pending verification (§5.12); `{"force": true}` overrides |
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
| `espfw` | tstr | Wi-Fi co-processor (ESP32-C6) firmware version, e.g. `"1.1.0"` — see below |
| `up` | uint | Uptime, seconds |

#### `0x0031` fw_versions — read

Request `{}` → `m7fw`, `m4fw`, `espfw`, `mod_a_fw`, `mod_b_fw` — all tstr.

`espfw` is read from a cache, never from the co-processor while the request
waits. It is the **empty string** while the ESP32-C6 is held in reset — the
state every boot starts in, until `conn_enable` (`0x0075`) — for the first
moments after it is powered, until it answers, after it restarts until it
answers again, and permanently when its firmware is older than HealthyBridge
1.1.0 (which cannot report a version). An empty `espfw` therefore means "not
known right now", not "no co-processor".

### 5.2. Telemetry

#### `0x0030` telemetry — read

Request `{}` →

| Key | Type | Meaning |
|---|---|---|
| `vbat_mv` | uint | Battery voltage, mV |
| `ibat_ma` | **int** | Battery current, mA — signed; negative while charging |
| `soc` | uint | State of charge, % |
| `tc_x10` | **int** | Temperature × 10 — **always the unavailable sentinel in 1.0.0** |
| `charge` | uint | Charge state, from the charger's status pins: 0 discharging · 1 charging · 2 full (charging terminated; independent of `soc`) · 3 fault (charger safety timer expired) |
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
Refused with **274** while an ESP32-C6 update is in flight or its new image is
still pending verification (§5.12) — a reset then would roll the update back.

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

`state` values in §7.5. `pwr` is the slot's load-switch enable as the
firmware last drove it — the command, not a measurement of the rail. A slot is
powered only while its module is running.

#### `0x0052` module_power — write · 🔒 lock

`slot` uint (`0` = A, `1` = B), `on` bool → `ok` bool.

`on: true` re-detects the slot from its ID EEPROM and starts whatever it finds,
so it is also how to retry a slot in state 3, 4 or 5, and how to rescan an empty
one. `ok` is true only if the slot ends up active (state 2). `on: false` stops
the module, releases its interfaces and cuts the rail. The slot then reads
state 5, or 0 if it was empty; `ok` is true.

#### `0x0053` module_eeprom_read — read · 🔒 lock

`slot` uint (`0` = A, `1` = B), `off` uint (0–255), `len` uint (1–64) →
`off` uint, `data` bstr.

Raw bytes from the slot's identification EEPROM — the 256-byte image described
in [HEALTHYLINK.md §4.3](HEALTHYLINK.md). The device does not parse what it
returns; a request that runs past byte 255, or asks for more than 64 bytes,
is `EINVAL`. A slot with no EEPROM answering (empty, or a module that will not
ACK) is `256 NOT_READY`, and an I2C failure is `257 HW_FAULT`.

Unlock-gated: a NAK on the unpowered probe retries with the slot rail on, which
pulses `EN_MOD_x` — the same privilege as `module_i2c_scan`.

#### `0x0054` module_eeprom_write — write · 🔒 lock

`slot` uint, `off` uint, `data` bstr (1–64 bytes) → `off` uint, `len` uint,
echoing what was written.

The counterpart, and the reason a module can be identified over the same cable
as everything else instead of needing an external I2C programmer. The device
writes the bytes and nothing more: it does not check the magic, recompute the
CRC, or care whether the module ID means anything to this firmware. **That is
deliberate** — it is what lets a module type newer than the firmware be
programmed — and the consequence is that a half-finished write leaves a module
whose identity is neither the old one nor the new one. Write the whole image,
then read it back and compare.

The 24AA02's 8-byte pages and 5 ms write cycle are handled on the device, so a
chunk only has to fit the SMP frame.

A written identity takes effect at the **next detect**: follow with
`module_power {slot, on: true}`. `healthypi hl eeprom program` does all of
this — build, write, verify, rescan — in one command.

#### `0x0055` module_i2c_scan — read · 🔒 lock

`slot` uint, `pwr` bool (optional, default false) → `addrs` bstr, one byte per
responding 7-bit address.

A bring-up instrument. Both slots' ID EEPROMs share one I²C bus and are the only
devices on it, so a slot that answers nothing gives no way to tell a module that
is not responding from a bus that is not working — and "the module is at the
wrong address" looks the same as "there is no module". This separates all three.
The probe is a zero-length write: whatever answers is neither read nor written.

`pwr: true` scans with the slot rail on and puts it back afterwards, which is
what identifies a module whose EEPROM sits behind the load switch instead of on
the always-on rail. That is also why the command is lock-gated — it can energise
a module whose identity is not yet known.

`healthypi hl scan --slot a` runs it both ways and interprets the result.

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
| `0x00A0` | m4fw_begin | write | 🔒 | `len` uint, `sha` bstr, `sig` bstr (optional) | `{}` |
| `0x00A1` | m4fw_chunk | write | 🔒 | `off` uint, `data` bstr | `off` uint |
| `0x00A2` | m4fw_commit | write | 🔒 | `{}` | `rst` bool |
| `0x00A3` | m4fw_status | read | | `{}` | `st`, `len`, `rx` uint, `err` int, `rst`, `sig` bool |
| `0x00A4` | m4fw_abort | write | | `{}` | `{}` |

**Call `m4fw_status` first.** Its `sig` field tells you whether this firmware
requires a signature before you spend minutes uploading an image it will reject.
`begin` replies with an empty map and always starts the upload from offset 0;
an interrupted upload cannot be resumed and must be restarted with `begin`.
Each `chunk` reply's `off` is the next offset the device expects.

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
reflashed. Returns **256** if the mechanism is unavailable, and refuses `arm`
with **274** while an ESP32-C6 update is in flight or pending verification
(§5.12).

### 5.12. Network co-processor (ESP32-C6) firmware update

Present only in **signed** builds; elsewhere every command returns `ENOTSUP`.
The M7 relays the image, chunk by chunk, into the ESP32-C6's **inactive** OTA
slot over its internal link. Nothing is staged on the M7, and the C6 keeps
running its current image until the very end.

> **Not yet run on hardware.** The device side is built and unit-tested against
> the C6's golden frames, but no C6 has been updated through it.

| ID | Command | Op | 🔒 | Request | Response |
|---|---|---|---|---|---|
| `0x00A6` | espfw_begin | write | 🔒 | `len` uint, `sha` bstr (32), `sig` bstr (64, **required**) | `{}` |
| `0x00A7` | espfw_chunk | write | 🔒 | `off` uint, `data` bstr (1–1016 B) | `off` uint (next expected) |
| `0x00A8` | espfw_commit | write | 🔒 | `{}` | `{}` |
| `0x00A9` | espfw_status | read | | `{}` | see below |
| `0x00AA` | espfw_abort | write | | `{}` | `{}` |

**The image** is the C6 *application* binary (`build.hp6/healthybridge_esp32c6.bin`
in healthybridge-esp32), not the merged flash image. `sha` is its SHA-256 and
`sig` an ECDSA-P256 signature over that digest with the **release key** — the
same key and the same raw `r||s` form as `m4fw_begin`'s `sig`. There is no
unsigned mode: the C6 does not check signatures, so the M7 refuses to start
without one.

**Per-request timeout: 5 s for every `espfw_*` command** (the catalog carries it
as `timeout_s`). The device bounds `espfw_chunk` at 1.6 s — a 0.6 s transmit
deadline plus a 1 s wait for the C6's acknowledgement — and the others answer
sooner. The 2.5 s default of some clients is too close.

#### Sequence

1. `espfw_status` — confirms the command set exists (a build without it answers
   `ENOTSUP`) and that `busy` is false.
2. `espfw_begin {len, sha, sig}`. The signature is checked **first**: **268**
   means it did not verify and nothing was sent anywhere. On success the reply
   is empty and immediate; the device then powers the C6 if it was off, takes
   over its link and opens the transfer.
3. Poll `espfw_status` about once a second until `st` = **2** (receiving), or
   **8** (failed — read `err`). Allow ~15 s.
4. `espfw_chunk {off, data}` in order from offset 0. Each chunk is relayed to the
   C6 and acknowledged *before* the reply; `off` in the reply is the next offset.
   - **272** `ESP_NO_REPLY`: the C6 did not answer in time. The session is
     intact — resend **the same chunk**. Resending the chunk accepted last
     (same `off`, same length) is always harmless.
   - **260**: out of order or past `len`; read `rx` from `espfw_status` and
     resume there.
   - **273** `ESP_REFUSED`: the C6 refused; the session is over (`st` 8,
     `c6_err` says why). Start again with `begin`.
   - Do not pause more than **30 s** between chunks: the C6 abandons the
     transfer at 30 s of silence and so does the M7.
5. `espfw_commit`. The device checks that every byte was relayed (**275**
   otherwise) and that their SHA-256 equals the signed `sha` (**268**
   otherwise) — either failure sends ABORT, and the C6 never sees COMMIT.
   On success the reply is immediate and empty. **Never send it twice.**
6. Poll `espfw_status` about once a second until `st` is final:

| `st` | Meaning | Host action |
|---|---|---|
| 6 `DONE` | new image running from the other slot and **VALID** | success |
| 7 `ROLLED_BACK` | new image failed its self-test; old image running (`other` = 3/4) | report; the C6 is fine on its old firmware |
| 8 `FAILED` | see `err`/`c6_err`; the C6's boot slot is unchanged | report; may retry from `begin` |
| 9 `TIMEOUT` | outcome not confirmed in time; the C6 is left powered | keep polling: `st` turns 6 or 7 if the C6 reports later; treat `busy` = true as "do not reset" |

Commit to `DONE` normally takes ~1 s of C6 verification, a restart of a few
seconds and ~3 s of link self-test; the device gives the restart 10 s and the
self-test 75 s, which is longer than the C6's own 60 s self-test window, so an
image that fails it ends as `ROLLED_BACK` rather than `TIMEOUT`. Allow **95 s**
before treating `TIMEOUT` as final.

`espfw_abort` before commit sends ABORT (the C6's running image is untouched)
and returns `st` 0 with `err` 9; after commit it returns **270** — too late,
the outcome is the C6's. On a finished session it clears the result.

#### `espfw_status` reply

| Key | Type | Meaning |
|---|---|---|
| `st` | uint | session state: 0 idle · 1 preparing · 2 receiving · 3 committing · 4 rebooting · 5 verifying · 6 done · 7 rolled back · 8 failed · 9 timeout |
| `len` | uint | image size announced at `begin` |
| `rx` | uint | bytes relayed **and acknowledged by the C6** |
| `err` | uint | why the session ended badly — table below; 0 none |
| `c6_err` | uint | the C6's own status code for its last refusal (HealthyBridge OTA namespace, e.g. 6 = image invalid, 8 = running image still unconfirmed) |
| `c6_st` | uint | C6 receiver: 0 idle · 1 receiving · 2 verifying · 3 committed · 4 error · 255 unknown |
| `img` | uint | C6 running image: 0 valid · 1 pending verify · 2 new · 3 invalid · 4 aborted · 5 undefined · 255 unknown |
| `other` | uint | the other slot, same values; 3 or 4 after a rollback |
| `slot` | uint | C6 running slot, 0 or 1; 255 unknown |
| `ver` | tstr | C6 running firmware version; `""` if unknown |
| `valid` | bool | `img` = 0 |
| `pend` | bool | `img` = 1 — a reset now would roll it back |
| `busy` | bool | a session is between `begin` and a final state, or the C6's image is pending: **do not reset the device or call `conn_disable`** |

Answered from a cache; it never touches the C6 and is safe to poll. The C6
fields are refreshed during a session, and otherwise once each time the C6
boots (every 2 s while its image is pending), so they read 255 while the C6 is
powered off. What each phase shows: idle — `st` 0, C6 fields from the last boot
or 255; preparing — `st` 1, `rx` 0; receiving — `st` 2, `rx` climbing, `c6_st` 1,
`busy`; committing/rebooting — `st` 3/4, `c6_st` 2→3 then the C6 goes quiet;
verifying — `st` 5, `slot` changed, `img` 1, `ver` the new version; done —
`st` 6, `img` 0.

| `err` | Name | Meaning |
|---|---|---|
| 1 | `SIG_INVALID` | signature does not verify; nothing was sent |
| 2 | `LINK` | the C6 could not be powered or never answered |
| 3 | `NO_OTA` | the C6 answers but not the update protocol: its firmware predates it (update it over its own USB port once) |
| 4 | `C6_REFUSED` | the C6 refused; see `c6_err` |
| 5 | `NO_REPLY` | the C6 stopped answering mid-session |
| 6 | `PROTOCOL` | a C6 reply disagreed with the transfer |
| 7 | `DIGEST` | the relayed bytes do not hash to `sha` |
| 8 | `INCOMPLETE` | commit before every byte was relayed |
| 9 | `ABORTED` | `espfw_abort` |
| 10 | `HOST_IDLE` | no chunk for 30 s |
| 11 | `CRYPTO` | the device's crypto library failed |
| 12 | `NOT_BACK` | the C6 did not come back after COMMIT within 10 s |
| 13 | `NOT_VALID` | it came back but did not report VALID within 75 s |
| 14 | `ROLLED_BACK` | the new image rolled itself back |
| 15 | `NOT_SWITCHED` | the C6 restarted on its old image without recording a rollback |

#### Updating all three processors

The C6 keeps a new image only if, after its restart, it hears from the M7 within
60 s; **any reset of the M7 or the C6 before `st` reaches a final state with
`busy` false rolls it back.** So:

1. Update the **M4** (`m4fw_*`) and the **M7** (stock `img` upload) first, then do
   the one reset (and reconnect) that both need.
2. Update the **C6 last**, on the new M7 firmware — it is the one that must keep
   running undisturbed afterwards, and doing it last means no reset follows it.
   This also covers a device whose current M7 firmware predates these commands
   (`espfw_status` answers `ENOTSUP`).
3. Do not send `os reset`, `enter_recovery` or `conn_disable` until
   `espfw_status.busy` is false. The device refuses the first two and
   `conn_disable` with **274** while it is true; `os reset {"force": true}` is
   not refused, and nothing guards a reset from the device's own power menu, the
   development shell, a watchdog or a power loss.

`healthypi fw update` (protocentral-healthypi 0.2.0 and later) follows this
sequence and this order, takes the 5 s timeout from its command catalog, and
does not send `os reset`, `enter_recovery` or `conn_disable` while `busy` is
true; `healthypi fw esp32c6-status` prints `espfw_status`. Like the device side,
it **has not yet run on hardware**.

---

## 6. Error codes

Standard MCUmgr codes apply: `0` OK, `2` unknown, `3` invalid argument,
`4` message too large, `8` not supported, `11` access denied (locked).

HealthyPi adds codes from 256 upward, returned in the reply's error map as
`{"group": 64, "rc": <code>}` — **never** as a top-level `rc`, which is the
protocol-wide namespace where those numbers do not exist. Read the two shapes
differently: a top-level `{"rc": n}` is a standard MCUmgr code, and `n` in an
error map means whatever the named group says it means. A client that reads a
group code against the standard table reports a confident lie.

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
| 272 | `ESP_NO_REPLY` | The ESP32-C6 did not answer in time; resend the same request |
| 273 | `ESP_REFUSED` | The ESP32-C6 refused; the update session is over — see `espfw_status` |
| 274 | `ESP_UPDATE_ACTIVE` | Refused: an ESP32-C6 update is in flight or its new image is still pending verification; a reset now would roll it back |
| 275 | `IMAGE_INCOMPLETE` | Commit before every byte of the image was sent |

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
probe or claim · `4` quarantined after a fault · `5` module present, powered
off on request

### 7.6. Module IDs

From the ID EEPROM's `module_id` field, and from `module_list`'s `id`:

| ID | Module |
|----|--------|
| `0x0001` | EEG-8CH (ADS1299) |
| `0x0002` | EMG-4CH |
| `0x0003` | TRIGGER-IO |
| `0x0004` | CAN-INTERFACE |
| `0x0005` | HealthyLink Compute (STM32N657 NPU) |
| `0x0006` | HIGH-RES-ADC |
| `0x0007` | STIM-OUTPUT |
| `0x0008` | SYNC-MASTER |
| `0x0009` | GSR-RESPIRATION |
| `0x000A` | GPIO — passive breakout: headers for every interface, no claim |

`0x000B`–`0x00FF` are reserved for ProtoCentral; `0x0100`–`0xFFFE` are free for
community and third-party modules. A module whose ID has no provider in the
running firmware is reported in state `1` (unsupported) and left unpowered.

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

`device_info.gv` reports the group schema version, currently `0x0003` (`0x0003`
added the ESP32-C6 update commands, §5.12). Within a
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
