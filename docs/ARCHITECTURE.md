# HealthyPi 6 system architecture

How the three processors divide the work, how a sample gets from an electrode to
a host, and the contracts between the parts. Read this before changing anything
that crosses a core or a layer boundary.

The M7 application's internal layering is documented next to the code, in
[`../../app_m7/ARCHITECTURE.md`](../app_m7/ARCHITECTURE.md).

---

## 1. The three processors

| Processor | Role | Owns |
|---|---|---|
| **STM32H757 Cortex-M7** @ 400 MHz | Application core | Sensors, the sample bus, storage, USB, the display, expansion modules |
| **STM32H757 Cortex-M4** @ 200 MHz | Algorithm core | Nothing. It is a calculator: raw signal in, vitals out |
| **ESP32-C6** (RISC-V) | Network co-processor | Wi-Fi and BLE, reached over UART4 |

The M4 owning no hardware is deliberate. It has no sensor drivers, no bus and no
storage, so an algorithm fault cannot stall acquisition — the M7 simply stops
receiving vitals and keeps streaming and recording. The same property lets the M4
be updated independently of the M7.

The ESP32-C6 firmware is **not in this repository**. It is a separate,
dual-target project shared with HealthyPi 5:
[`protocentral/healthybridge-esp32`](https://github.com/protocentral/healthybridge-esp32).
`scripts/build.sh esp32` and `scripts/flash.sh esp32` delegate to it.

## 2. Data flow

The sample bus is the spine. Producers publish to it and never learn who
consumes.

```
  ADS1294R (ECG/resp)  ─┐
                        ├─► core/acquisition.c ─► ┌─────────────┐
  AFE4400  (PPG)       ─┘   DATA_READY-triggered, │ sample bus  │
                            converts to canonical │ (per-       │
                            units, batches 16     │  consumer   │
                            samples per frame     │  rings)     │
                                                  └──────┬──────┘
  Cortex-M4 ◄── raw ECG/PPG ── platform/ipc.c ───────────┤
            ──► HR/HRV/SpO2 ──────────────────────────►  │
                                                         │
        ┌────────────────┬───────────────┬───────────────┴──────────┐
        ▼                ▼               ▼                          ▼
   stream service   recording       connectivity                   UI
   → USB CDC0       → SD card       → ESP32 UART4              live preview
     (.HP6)           (.HP6)
```

**Rates.** ECG 500 Hz, PPG 250 Hz. The PPG rate *is* the AFE4400 pulse-repetition
frequency, set by `CONFIG_AFE4400_PRF_HZ` — the driver programs
`PRPCOUNT = 4000000/PRF - 1` from that same symbol and `HPI_PPG_RATE_HZ` derives
from it, so the hardware rate and the advertised rate cannot drift apart.
Acquisition batches 16 samples per frame (~31 frames/s ECG, ~16 frames/s PPG).
Only the M4 feed is decimated, 2:1 to 125 Hz; the bus, the stream and the
recording all carry full-rate PPG.

**Back-pressure.** Each consumer owns its own lock-free ring and drops on full,
incrementing a per-consumer dropped counter. A slow consumer degrades itself and
nothing else — it can never stall acquisition.

## 3. Canonical sample formats

One set of payload structs is used on the bus, in the `.HP6` live stream, and in
the recorded file, byte for byte. A host therefore needs exactly one parser, and
a recording and a live capture are the same bytes. They are defined in
[`../../app_m7/src/core/sample_formats.h`](../app_m7/src/core/sample_formats.h):

| Struct | Contents |
|---|---|
| `hp6_ecg_sample` | `resp`, `lead_i`, `lead_ii`, `v1` (µV, int32), `lead_off` bitmap |
| `hp6_ppg_sample` | `red`, `ir` (raw counts, int32), `lead_off` |
| `hp6_vitals` | `hr_bpm`, `spo2_x10`, `rr_bpm`, `temp_c_x100`, `hrv_sdnn_ms`, `hrv_rmssd_ms`, `hrv_lf_hf_x10` |
| `hp6_eeg_sample` | `ch[8]` (µV, int32), `lead_off` |

`rr_bpm` and `temp_c_x100` are present in the struct but read 0 — the
respiration-rate and temperature paths are not wired yet. The fields exist so the
format does not change when they are.

The `.HP6` container itself (magic `HPI6`, version `0x0300`) is described in
[`HOST_INTERFACE.md`](HOST_INTERFACE.md).

## 4. M7 ↔ M4 IPC

OpenAMP/RPMSG over a static-vring node in **SRAM4** (64 KB at `0x38000000`,
non-cached), endpoint `hpi_ipc`. The M7 is HOST, the M4 is REMOTE.

Every message is a 4-byte header plus payload, capped at the 512-byte RPMSG
buffer — which is why sensor data is batched rather than sent per sample:

```c
struct hpi_ipc_msg {
    uint8_t  type;
    uint8_t  reserved;
    uint16_t length;
    uint8_t  data[];
} __packed;
```

| Type | Direction | Payload |
|---|---|---|
| `0x05` `VERSION` | M4 → M7 | M4 firmware version string |
| `0x10` `PPG_RAW` | M7 → M4 | Raw PPG batch |
| `0x11` `PPG_VITALS` | M4 → M7 | SpO2 + HR |
| `0x20` `ECG_RAW` | M7 → M4 | Raw ECG batch |
| `0x21` `ECG_VITALS` | M4 → M7 | HR + HRV + QRS |

The contract lives in
[`../../app_m7/src/platform/m4_ipc_protocol.h`](../app_m7/src/platform/m4_ipc_protocol.h),
mirrored by the M4's `app_m4/src/ipc_module.h`. **The two must be kept in sync** —
nothing enforces it at build time.

Two constraints that are easy to break:

- **Boot order.** The M7 boots first and initialises IPC as HOST; the M4 waits
  ~7 s before calling `hpi_ipc_m4_init()` as REMOTE. The M4's bind is one-shot, so
  a delay that lets the M7 finish first is required. If the M4 never binds, the M7
  logs a bind timeout and acquisition continues without vitals.
- **Callbacks must not block.** IPC callbacks run in RPMSG context: queue the work
  (`k_msgq_put` + `k_work_submit`) and return.

## 5. What the M4 actually computes today

Intended scope is QRS/HR/HRV, SpO2, and EEG band power via CMSIS-DSP. What is
live today is **the ECG → HR path only** — SpO2 is compiled out, EEG is a stub,
and HRV is implemented but off by default. There is no TFLite or ML on the M4;
inference belongs to the HealthyLink NPU module.

## 6. Host interface

One USB cable, two CDC functions:

| Port | Carries |
|---|---|
| **CDC0** | The `.HP6` live stream |
| **CDC1** | MCUmgr SMP — the control interface, and what HealthyPi Studio uses |

A mass-storage LUN over the SD card appears only while Transfer Mode is armed.
Full details, including the group-64 command set, are in
[`HOST_INTERFACE.md`](HOST_INTERFACE.md).

## 7. The ESP32-C6 link

The network co-processor hangs off **UART4**: 2 Mbaud, 8N1, with **hardware
RTS/CTS**. Pins on v5 are TX `PA0`, RX `PI9`, RTS `PA15`, CTS `PB0`.

The baud is an exact divisor at both ends — STM32 PCLK1 100 MHz / 50 and
ESP32-C6 PLL_F80M 80 MHz / 40 — so there is no baud error to chase. It runs the
link at roughly 9.5% utilisation, and that margin is what absorbs a co-processor
stall before CTS back-pressure reaches the transmitter.

> An earlier revision used SPI2. That transport was **deleted on 2026-07-27**;
> `&spi2` is `disabled` on v5 and the pins, though still routed, are driven by
> nothing. The wire contract outlived it — it was never SPI-specific — and now
> lives transport-independently in
> `drivers/misc/healthybridge_esp32/healthybridge_esp32_protocol.h` as `HPI_HB_*`.

### Data path

Connectivity is an ordinary bus consumer, with the same isolation as every other
service: its own ring, drop-on-full, so a slow or absent co-processor can never
back-pressure acquisition.

```
sample bus ── ECG/PPG/VITALS ──► connectivity service (thread)
                                   │  map hp6_* -> HealthyBridge frames
                                   ▼
                         HealthyBridge UART driver (UART4, RTS/CTS)
                                   ▼
                            ESP32-C6  ──► Wi-Fi TCP / BLE GATT / SMP gateway
```

The service reaches the wire through a vtable (`healthybridge_esp32_link.h`), so
it never knew which physical layer was underneath — which is why swapping SPI for
UART touched no service code.

| canonical (`hp6_*`) | HealthyBridge frame |
|---|---|
| `hp6_ecg_sample` resp/lead_i/lead_ii/v1 | `hpi_ecg_sample_multi` ch0/ch1/ch2/ch3 |
| `hp6_ppg_sample` red/ir | `hpi_ppg_sample` red/ir |
| `hp6_vitals` hr/spo2_x10/rr/temp_c_x100 | vitals payload hr/spo2/resp/temp_x10 |

### Control

`CONFIG_HPI_CONNECTIVITY=y` in `app_m7/prj.conf`, so connectivity is part of a
normal `scripts/build.sh m7` build. With the co-processor absent the driver
reports "not ready" and the service stays idle. Build with
`CONFIG_HPI_CONNECTIVITY=n` to compile it out — the `wifi_*` commands then return
`ENOTSUP`.

| id | command | dir | action |
|---|---|---|---|
| `0x0070` | `wifi_status` | read | `{state, rssi, ssid, ip}` from the ESP32 |
| `0x0071` | `wifi_scan` | read | `ENOTSUP` — provisioning goes through the SoftAP captive portal |
| `0x0072` | `wifi_set` | write | `{ssid, pw}` → connect (unlock-gated) |
| `0x0073` | `wifi_forget` | write | disconnect (unlock-gated) |
| `0x0074` | `wifi_softap` | write | bring the provisioning access point up or down |
| `0x0075` | `conn_enable` | write | `{radios}` — power the C6 and start radios (bit0 WiFi, bit1 BLE) |
| `0x0076` | `conn_disable` | write | radios down, C6 back into reset |
| `0x0077` | `conn_status` | read | `{link, radios, state, rssi, ssid, ip, ble_adv, ble_conn}` |

### Radios are off at boot

**The device boots with the co-processor in reset and neither radio running, and
nothing restores a previous "on" — every boot starts quiet.** This is a power
decision. The C6 used to bring both radios up on its own ~220 ms into M7
bring-up: NimBLE advertising, plus either an STA connect or — on any unit with no
stored credentials, which includes every factory-fresh one — a *sticky* SoftAP
captive portal that never timed out. That lands concurrently with SDRAM, the
backlight, sensor init and USB enumeration, and a unit running on USB alone
browns out and resets. The BQ24074's input ceiling is strapped by EN1/EN2 and the
part has no register interface, so firmware cannot ask for more current; drawing
less is the only lever.

It is enforced on both sides, because neither alone is sufficient:

- **M7** holds the C6's EN line (PA11) low from `PRE_KERNEL_1`
  (`CONFIG_HEALTHYBRIDGE_ESP32_HOLD_RESET_AT_BOOT`, default y). Costs microamps
  rather than the tens of milliamps a booted-but-idle C6 draws.
- **ESP32-C6** initialises both stacks but starts neither radio
  (`CONFIG_HB_RADIOS_OFF_AT_BOOT`, default y on the HP6 profile). `esp_wifi_init()`
  runs; `esp_wifi_start()` — which powers the PHY and runs calibration, the actual
  spike — does not. This is what covers the windows the M7 cannot: MCUboot
  disables the HealthyBridge node, so nothing drives PA11 during the bootloader,
  and a device sitting in MCUboot **serial recovery** would otherwise run the C6
  unmanaged indefinitely.

Turning it on is always explicit, from three places: the Connectivity screen
(**More → Link**), group-64 `0x0075`, and `hpi wifi|ble|link` on the dev shell.
`conn_enable` with `radios = 0` powers the C6 without starting a radio — needed
because a C6 held in reset cannot be flashed over its own USB port.

Because the co-processor also runs an MCUmgr SMP gateway — forwarding SMP to the
M7 over the same UART — a host that speaks the control protocol over USB speaks
it over Wi-Fi unchanged. Only the connection transport differs.

### Firmware

The ESP32-C6 firmware is **not in this repository**. It lives in
[`protocentral/healthybridge-esp32`](https://github.com/protocentral/healthybridge-esp32),
a dual-target project shared with HealthyPi 5. `scripts/build.sh esp32` and
`scripts/flash.sh esp32` delegate to it.

**The baud must match at both ends** — M7 `&uart4 current-speed` and the
co-processor's `CONFIG_HB_UART_BAUD_HP6`. A mismatch does not fail gracefully; it
produces framing errors that read like bad wiring.

## 8. Expansion

The HealthyLink port takes plugin modules — an 8-channel EEG front end
(ADS1299) and an NPU compute module (STM32N657). Modules register through an
iterable section, an arbiter claims the interface pins they need, and a
supervisor can power down and quarantine a faulting module without disturbing
onboard acquisition. See [`HEALTHYLINK.md`](HEALTHYLINK.md).

Only the **host** half of each module lives here — the provider, the driver and
the slot overlays. The NPU module runs its own firmware on its own silicon, and
that firmware is a separate repository,
[`Protocentral/healthylink-compute-fw`](https://github.com/Protocentral/healthylink-compute-fw)
(it vendors STMicroelectronics NPU runtime under a proprietary licence that
cannot be redistributed inside an MIT repository). The SPI contract between the
two halves is [`HEALTHYLINK.md` §12](HEALTHYLINK.md#12-npu-compute-module).

## 9. Firmware update and recovery

How a unit gets new firmware, and how it is rescued when that goes wrong.
The single document for *how a HealthyPi 6 gets new firmware, and how it gets
rescued when that goes wrong*. Everyone touching a shipped unit — release
engineer, factory operator, support, or a user with a USB cable — should be able
to work from this page alone.

The bundle wire format is in
[the bundle format](#10-the-release-bundle).
This page is the operational view.

---

### What is updateable

A HealthyPi 6 is three processors. All three can be updated in the field, by
three different mechanisms, because their hardware genuinely differs.

| Processor | Role | How it is updated | Verified by |
|---|---|---|---|
| **STM32H757 M7** | application | MCUboot image 0, stock MCUmgr **img group** over USB CDC1 | **MCUboot**, ECDSA-P256, before boot |
| **STM32H757 M4** | algorithm core | group-64 `0x00A0-0x00A4` → QSPI staging → M7 writes bank 2 | **the M7 app**: SHA-256 + ECDSA-P256 + vector-table sanity |
| **ESP32-C6** | network coprocessor | its own `esp_ota` over WiFi (`POST /api/ota/upload`) | ESP-IDF app descriptor + rollback |

Every image is signed with **one** ECDSA-P256 release key. There is not a
separate key per processor — see [Keys](#keys).

> **The M4 has no downgrade protection, and the M7 does.** MCUboot enforces
> `MCUBOOT_DOWNGRADE_PREVENTION` on the M7 and will refuse an older image
> (`E: Insufficient version in secondary slot`, observed during F8). The M4 path
> verifies *authenticity* — SHA-256, ECDSA-P256 and a vector-table check — but
> nothing compares versions, so an **older M4 image installs happily**; 1.0.1 →
> 1.0.0 was accepted on hardware. The asymmetry is a consequence of the M4 having
> no bootloader and no image header to carry a version, not a decision anyone
> took. It is recorded rather than fixed because the threat it would address —
> an attacker who can already reach group-64 over USB choosing to install an
> older *validly signed* M4 algorithm image — is not one the v1 posture defends
> against (the device ships unlocked; see [`DEVICE_LOCK.md`](DEVICE_LOCK.md)). If that changes,
> the fix is a monotonic counter in the M4 metadata checked by
> `m4_update_service.c`, not an MCUboot slot.

> **Why the M4 is not an MCUboot image.** Its reset vector comes from the
> `BOOT_CM4_ADD0` option byte, which encodes `address >> 16` — 64 KB
> granularity. Bank 2 erases in 128 KB sectors. Between them, any MCUboot header
> would have to be a whole multiple of 64 KB, but `image_header.ih_hdr_size` is
> `uint16_t` and caps at 65535. One byte short, with no slot base that fixes it.
> This was attempted and abandoned; it is not an oversight. Full derivation in
> the M4 update service.

### The one thing that decides whether a unit is updateable at all

**A unit must ship with the signed build.** The development build
(`scripts/build.sh m7` → direct flash, no bootloader) has no MCUboot, no img
group, no M4-update path and no recovery entry. A board flashed that way can
only ever be updated over SWD, with the case open.

There is no runtime way to tell the difference from the outside, so it is
enforced at build time instead:

```bash
tools/ci/check_prod_surface.sh --release build/release/m7s
```

fails unless the image carries `BOOTLOADER_MCUBOOT`, `MCUMGR_GRP_IMG`,
`IMG_MANAGER`, `HPI_M4_UPDATE`, `HPI_RECOVERY_MODE` and `RETENTION_BOOT_MODE`,
the bootloader carries serial recovery + signing + downgrade prevention, and the
USB VID is no longer the Zephyr development pair. `scripts/release.sh` runs it
and refuses to produce a bundle if it fails.

### Normal update — what a user does

One file, one command, all three processors:

```bash
healthypi fw update --port <CDC1-port> --bundle hpi6-2.0.1.hpifw
```

The tool reads the device's current versions (group-64 `hpi/fw_versions`), skips
what is already current, and applies the rest in the order **ESP32-C6 → M4 →
M7**. The M7 is last on purpose: it is the processor running the update logic
for the other two, so replacing it first would mean applying the rest with
firmware that is about to be overwritten.

`--dry-run` prints the plan without writing anything. `--force` reapplies images
whose version already matches.

**Ports:** CDC1 is the control/MCUmgr port; CDC0 is the `.HP6` sample stream and
is never used for updates. The tool stops streaming before uploading — a live
stream and a firmware upload competing for the same USB bandwidth is the usual
cause of a slow update.

#### What happens on the device

1. **M7:** the image is streamed into the QSPI secondary slot (`slot1`), the
   device reboots, MCUboot verifies the signature and version, then
   **overwrite-installs** it into internal flash and boots it. Roughly 40 s to
   upload at ~17 KB/s, plus ~5 s to install.
2. **M4:** the image is streamed into the QSPI staging region, the M7 checks its
   digest, then its signature, then that its first two vectors look like an M4
   image, and only then erases and rewrites bank 2. A reset follows; the M4
   rebinds IPC ~7-10 s later.
3. **ESP32-C6:** uploaded to the C6's own HTTP endpoint; it writes the inactive
   OTA slot and rolls back by itself if the new image fails its self-test.

**Bank 2 and slot0 are never touched until the whole image has arrived and
verified.** An interrupted or corrupt upload leaves the running firmware alone.
This was demonstrated rather than assumed: three consecutive commit failures
during bring-up left bank 2 untouched and the M4 running throughout.

### Recovery — when an update goes wrong

MCUboot runs in **overwrite-only** mode (the secondary slot is on a different
flash device than the primary, so swap-with-revert is not possible). There is
therefore **no automatic revert**. Recovery is a ladder; use the first rung that
applies.

| Rung | Situation | What to do | Case open? |
|---|---|---|---|
| **1. Serial recovery, auto** | slot0 is empty or corrupt — MCUboot finds no bootable image | Plug in USB. The device enumerates as **“HealthyPi 6 Recovery”**, a single CDC port speaking the MCUmgr img group. `healthypi fw recover --port <port> --bundle …` | no |
| **2. Serial recovery, requested** | device boots but needs reflashing | `healthypi fw enter-recovery --port <CDC1>` (group-64 `0x00A5`), or a 5 s hold of the user button → **Power menu → Recovery mode**. Sets a retained flag in backup SRAM and reboots into rung 1's state | no |
| **3. System bootloader** | MCUboot itself is damaged | hold **BOOT0** high at power-on, flash over the ST-ROM USB DFU | yes |
| **4. SWD** | anything else | `scripts/flash.sh signed` / `scripts/flash.sh factory` with an ST-Link | yes |

**The gap, stated plainly:** an image that is validly signed, installs, boots,
and *then* hangs before it can service a recovery request is not covered by
rungs 1 or 2 — MCUboot sees a bootable image and jumps to it. That case needs
rung 3 or 4. Closing it would mean a `BOOT_SERIAL_WAIT_FOR_DFU` window on every
boot (extra delay + a second USB enumeration each time), which was judged the
worse trade. Mitigation is on the release side: the acceptance run in §8 exists
so a hanging image never reaches a bundle.

#### Recovery-mode details

- The recovery port is **one** CDC ACM, not the two-port composite the
  application enumerates. A host tool must re-scan; the app's CDC1 port name
  does not come back.
- **Same USB VID/PID as the application** (`0x1209:0xFF90`). pid.codes allocates
  one PID per entry, and nothing needs them to differ: a human reads the product
  string, and a tool that must be certain asks the protocol — the application
  answers group 64, the bootloader does not. `healthypi fw recover` probes
  exactly that and reports a wrong port before writing anything, which is more
  robust than matching descriptors and does not consume a second allocation.
- The retained flag lives in **backup SRAM** (VBAT-backed, D3 domain) with a
  `"HP6B"` prefix and a checksum, so an uninitialised area on a first power-on
  cannot read as “enter recovery”. Both MCUboot and the application take its
  address from one shared devicetree file
  (`boards/protocentral/healthypi6_v5/healthypi6_v5_bootmode.dtsi`) — they must
  agree byte-for-byte, so it is declared exactly once.
- MCUboot clears the flag once it has acted on it; recovery is not sticky.
- **`fw recover` writes the M7 only.** In recovery nothing but MCUboot is running,
  so the group-64 M4 path does not exist — a recovered unit keeps whatever M4
  image it had. That is usually right (the M4 is rarely the reason you are in
  recovery), but it means **recovery does not restore a bundle in full**: follow it
  with a normal `healthypi fw update --port <CDC1> --bundle …` against the
  recovered application to bring the M4 into line. The tool says so on completion.
  Confirmed on hardware during F8 — a unit recovered to M7 1.0.0 still reported
  `m4fw=1.0.1`.
- **Updating both cores in one bundle needs two resets, and the updater does the
  second one for you.** MCUboot copies ~646 KB from the secondary slot before the
  M7 application starts, which delays the M7 past the M4's single RPMSG bind
  attempt, so the first boot after a combined update comes up with **no vitals**
  (`W: M4 IPC bind timeout`). A second reset boots the cores together and they
  pair normally. There is no live re-bind to fall back on: RPMSG static vrings
  cannot bind against an already-running host (`app_m7/src/platform/ipc.c`). If
  you ever apply an update by hand, reset twice and check `hpi/fw_versions`
  reports a non-empty `m4`.

### Release bundles

A release is a single `.hpifw` file — a zip containing `manifest.json`, a
signature over it, and one image per processor. Handing out three loose `.bin`
files plus an ordering rule is how a device ends up with an M7 that no longer
understands its M4.

```bash
scripts/release.sh                       # build prod + package + verify
healthypi fw info --bundle build/release/hpi6-2.0.1.hpifw
```

The manifest carries, per processor: version, sha256, size, transport, optional
`min_version`, and (for the M4) the device-verifiable signature. Format detail:
[the bundle format](#10-the-release-bundle).

### Transports

| Transport | Status | Notes |
|---|---|---|
| **USB CDC1 (SMP)** | **primary, validated** | works before WiFi is provisioned, needs no infrastructure, best throughput |
| **WiFi TCP:9000 → UART4** | secondary, not yet exercised | raw byte relay on the ESP32-C6 (`HB_ENABLE_SMP_GATEWAY`); the ESP does no SMP parsing |
| **BLE** | **excluded** | SMP-over-BLE is far too slow for ~1 MB images on typical phones; BLE stays a data-only GATT surface |
| **SWD** | factory / recovery | |

### Keys

One ECDSA-P256 keypair signs everything: the M7 image (enforced by MCUboot), the
M4 image (enforced by the M7 application, whose compiled-in copy of the public
half is generated from the same PEM at build time), and the release manifest
(checked by host tools).

- **Dev key** `keys/hp6_dev_ec256.pem` — generated locally on first signed build,
  never committed. A device flashed with one dev key will refuse an image signed
  with another; that is the mechanism working, not a fault.
- **Release key** — air-gapped, passed as `HP6_SIGNING_KEY=/abs/path`. Never
  reaches the repo, CI, or a developer machine.
- Losing the release key means shipped units can no longer receive updates over
  any non-SWD path. Custody procedure: [`../keys/README.md`](../keys/README.md).

### Acceptance run before any release

Run on a `healthypi6_v5` board with an ST-Link attached:

```bash
source ~/zephyrproject/.venv/bin/activate
scripts/release.sh                                   # must refuse on a dev VID
scripts/flash.sh signed                              # MCUboot + signed app
healthypi device info --port <CDC1>                  # group-64 reachable

# full update cycle from the bundle
healthypi fw update --port <CDC1> --bundle build/release/hpi6-<ver>.hpifw

# negatives — each must be REFUSED, and leave the running firmware intact
#  a) image signed with a different key      -> MCUboot keeps the old image
#  b) lower version than installed           -> downgrade prevention
#  c) M4 upload with a corrupted digest      -> bank 2 untouched (verify over SWD)
#  d) M7 image uploaded as the M4 image      -> "not an M4 image" (vector check)

# recovery — both entries
#  1) erase slot0 over SWD  -> device must appear as "HealthyPi 6 Recovery"
#     and accept a full image via  healthypi fw recover --port <port> --bundle …
#  2) healthypi fw enter-recovery --port <CDC1> -> same, from a healthy device
```

> `flash_write()` returning 0 does **not** prove a bank-2 write. Verify over SWD:
> `STM32_Programmer_CLI -c port=SWD mode=UR -u 0x08100000 <len> /tmp/rb.bin` and
> compare digests. This is what actually established that `flash0 + 0x100000`
> resolves to bank 2.

### Current state

| Item | State |
|---|---|
| M7 signed OTA over CDC1 | ✅ HW-validated (2026-07-23) — full cycle, downgrade + wrong-key rejected |
| M4 update via the M7 | ✅ HW-validated (2026-07-24) — bank 2 readback matched over SWD |
| M4 signature + vector check | 🟡 implemented, build-verified; HW run pending |
| Serial recovery over USB CDC | 🟡 implemented, builds (MCUboot 61.3 KB / 128 KB); HW run pending |
| Release bundle + unified updater | 🟡 implemented; HW run pending |
| Registered USB VID/PID | 🔴 external — pid.codes allocation not yet made; blocks a real release |
| ESP32-C6 self-OTA | 🔴 not implemented (healthybridge-esp32 Phase D) |
| WiFi OTA end-to-end | 🔴 not implemented (healthybridge-esp32 Phase C + UART4 wire test) |
| `espfw` version reporting | 🟡 unblocked (SPI return path fixed); needs an ESP-side `GET_VERSION` handler — see below |

**`espfw` is empty because no one reports it yet.** It was previously blocked on
the dead ESP→M7 SPI return path; that turned out to be a fault on one board and
the path works (2026-07-25, no firmware change). What is still missing is a
`GET_VERSION` handler in the external HealthyBridge firmware's `control.c` —
`healthybridge_spi_get_version()` returns `-ENOTSUP` without transmitting.

Whether the C6 version arrives over SPI or over UART4 (the SMP-gateway line) is
now an open choice rather than a forced one. Either way the update system is
deliberately built so **nothing in §3 or §4 depends on it**.

## 10. The release bundle

`.hpifw` — what a release actually is.
The wire/on-disk format for a HealthyPi 6 firmware release. Produced by
`scripts/release.sh`, consumed by `healthypi fw update` and (later)
by HealthyPi Studio's "check for updates".


---

### Why a bundle exists

A HealthyPi 6 is three independently-updateable processors whose firmware
versions have to move together — the M7↔M4 IPC contract in particular is a
matched pair (`app_m7/src/platform/m4_ipc_protocol.h` mirrors
`app_m4/src/ipc_module.h`). Shipping three loose `.bin` files plus a written
ordering rule is how a unit ends up with an M7 that no longer understands its M4,
which presents as "the vitals stopped working" rather than "the update was
applied wrong".

One file also gives the update tool something to reason about: which images are
actually newer than what is installed, what order to apply them in, and whether
the whole set is intact before the first byte of flash is written.

### Container

A plain **zip**, so it can be inspected with `unzip -l` by anyone debugging a
release without this repo checked out.

```
hpi6-2.0.1.hpifw
├── manifest.json     what is inside, per processor
├── manifest.sig      ECDSA-P256 over sha256(manifest.json), raw r||s (64 B)
├── m7.bin            MCUboot-signed M7 image
├── m4.bin            raw M4 image (no container of its own)
└── esp32c6.bin       optional ESP32-C6 image
```

File names inside the archive come from the manifest's `file` field; the names
above are what `scripts/release.sh` writes.

### `manifest.json`

```json
{
  "format": 1,
  "product": "healthypi6",
  "release": "2.0.1-dev",
  "hw_rev": ["v5"],
  "created": "2026-07-25T10:12:33+05:30",
  "images": {
    "m7": {
      "file": "m7.bin",
      "version": "2.0.1-dev",
      "sha256": "…",
      "size": 663264,
      "transport": "mcumgr-img"
    },
    "m4": {
      "file": "m4.bin",
      "version": "1.0.0-dev",
      "sha256": "…",
      "size": 159952,
      "transport": "hpi-g64",
      "sig": "…128 hex chars…"
    }
  }
}
```

| Field | Meaning |
|---|---|
| `format` | bumped on any breaking change; a tool refuses a format it does not know |
| `release` | the release's name, conventionally the M7 version |
| `hw_rev` | board revisions this bundle is valid for |
| `created` | the release commit's timestamp, not the clock — a release build is reproducible |
| `images.<proc>.transport` | `mcumgr-img` (stock SMP img group) · `hpi-g64` (group-64 0x00A0-0x00A4) · `esp-ota-http` (the C6's own endpoint) |
| `images.<proc>.min_version` | optional: refuse to apply over anything older |
| `images.m4.sig` | ECDSA-P256 over that image's SHA-256, raw r||s, hex |

`sort_keys=True` and two-space indent when written, so the byte stream that gets
signed is deterministic for a given content.

### Signatures — three of them, doing different jobs

This is the part worth being precise about, because "is it signed?" has three
different answers depending on what you are protecting against.

| Signature | Covers | Enforced by | Protects against |
|---|---|---|---|
| MCUboot header on `m7.bin` | the M7 image | **the bootloader**, before it boots | a forged M7 image, at the only point that can actually stop it |
| `images.m4.sig` in the manifest | the M4 image's digest | **the M7 application**, at group-64 commit | a forged M4 image — the M4 has no bootloader of its own |
| `manifest.sig` | the manifest, hence every digest in it | **the host tool** (`--pubkey`) | a tampered *bundle*, before anything is written |

All three use the **same** ECDSA-P256 key. There is deliberately not a key per
processor: one key to hold, one to rotate, one to lose. The public half reaches
the M7 application through `tools/build/gen_m4_pubkey.py`, which extracts it from
the same PEM the MCUboot signing uses at build time — so the two cannot drift.

Note what the manifest signature is **not**: it is a host-side convenience, not a
device-enforced control. A host that skips `--pubkey` still gets digest checking
(cheap, catches the ordinary truncated-download failure), and the device still
enforces its own two signatures regardless of what the host did.

#### Raw r||s, not DER

Device-side verification uses `psa_verify_hash()`, which takes a raw 64-byte
r||s pair. Signing tools produce DER by default, so `hpifw.sign_digest_raw()`
converts once on the host. The alternative — an ASN.1 parser inside the firmware
update path — buys nothing.

### Applying a bundle

Order is fixed at `esp32c6 → m4 → m7` (`hpifw.APPLY_ORDER`). The M7 goes **last**
because it is the processor that runs the update logic for the other two:
replacing it first would mean applying the rest with firmware that is about to be
overwritten, across a reboot.

```bash
healthypi fw update --port <CDC1> --bundle hpi6-2.0.1.hpifw
healthypi fw info --bundle hpi6-2.0.1.hpifw                  # no device
```

The tool reads `hpi/fw_versions` first and skips processors already at the
bundle's version (`--force` overrides). Version comparison ignores `-dev`-style
suffixes on purpose: treating `2.0.1-dev` as older than `2.0.1` would make the
updater reflash on every run during development, which trains people to ignore
it.

### Compatibility rules

- A tool **must** refuse a `format` it does not recognise rather than
  best-effort a partial read — a bundle it half-understands is worse than none.
- Unknown keys inside an image entry are ignored, so additive fields are safe.
- Adding a processor is additive; a tool that does not know the name skips it and
  says so.
- `hw_rev` is advisory today (the device reports its revision as `br` in
  `hpi/device_info`); enforcing it is the natural place to stop a v5 image
  reaching a future v6.

### Not in scope for v1

- **Delta/patch images.** Full images only. At ~650 KB over a 17 KB/s link the
  saving does not pay for the failure modes.
- **Encryption.** The images are open-source firmware; signing establishes
  authenticity, which is the property that matters here.
- **A remote manifest / auto-update server** — not built yet. When it
  lands it will serve exactly this manifest shape with a URL per image, so the
  format does not change — only where the bytes come from.

## 11. Where to go next

| Topic | Document |
|---|---|
| Talking to the device from a host | [`HOST_INTERFACE.md`](HOST_INTERFACE.md) |
| The M7 layering and the one rule | [`../../app_m7/ARCHITECTURE.md`](../app_m7/ARCHITECTURE.md) |
| Access control | [`DEVICE_LOCK.md`](DEVICE_LOCK.md) |
