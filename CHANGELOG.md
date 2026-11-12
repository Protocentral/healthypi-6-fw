# Changelog

All notable changes to the HealthyPi 6 firmware are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Versions here are the **firmware** version reported by the device
(`app_m7/VERSION`, `app_m4/VERSION`) and carried in the release bundle.

## [Unreleased]

### Added
- **The ESP32-C6 firmware version is reported.** `device_info` and
  `fw_versions` now fill `espfw` (it was always empty). The connectivity
  service asks the C6 over HealthyBridge (new control command `0x31`
  GET_VERSION) once the link comes up and caches the answer, so a group-64 read
  never waits on the co-processor. The cache is dropped when the C6 is powered
  down, its link faults, or it restarts (including an in-place reboot after its
  own OTA). `espfw` is `""` while the C6 is in reset -- the boot default, until
  `conn_enable` -- before it has answered, and on C6 firmware older than
  HealthyBridge 1.1.0.
- **The ESP32-C6 can be updated over USB, through the M7 -- not yet run on
  hardware.** Group-64 `espfw_begin` / `espfw_chunk` / `espfw_commit` /
  `espfw_status` / `espfw_abort` (`0x00A6`-`0x00AA`, signed builds,
  `CONFIG_HPI_ESP32_UPDATE`; schema version `0x0003`). The M7 relays each chunk
  over HealthyBridge (new frame types `0x80`/`0x81`) into the C6's inactive OTA
  slot; nothing is staged on the M7. The image must carry an ECDSA-P256
  signature with the release key -- checked before anything is sent, with the
  same verifier as the M4 image -- and the C6 is sent COMMIT only once every
  byte has been relayed and their SHA-256 matches the signed digest. The M7 then
  watches the C6 restart and keeps it talking until the new image reports
  VALID (or reports the rollback). `espfw_chunk` blocks for up to 1.6 s, so
  hosts must use a 5 s per-request timeout for `espfw_*`. New group-64 errors
  272 `ESP_NO_REPLY`, 273 `ESP_REFUSED`, 274 `ESP_UPDATE_ACTIVE`, 275
  `IMAGE_INCOMPLETE`. Needs healthybridge-esp32 with the OTA receiver
  (`hp6/c6-ota-receiver`). `healthypi fw update` uses it (next entry).
- **`healthypi fw update` updates the ESP32-C6 -- not yet run on hardware.**
  The host half of the path above (protocentral-healthypi 0.2.0): status
  first, `begin`, poll to receiving, chunks in order (272: resend the same
  chunk; 260: resume at `rx`; 273: start again), `commit` exactly once (never
  resent, even when its reply is lost), then poll to done / rolled back /
  failed, allowing 45 s. Every `espfw_*` request uses the 5 s timeout from the
  command catalog, applied by the transport for any command that states one. The
  C6's version is read while it is powered; on a unit that holds it in reset
  (every boot) the tool powers it with no radio to ask, and puts it back. An
  identical version is skipped and an older one refused unless `--force` (the
  C6 has no downgrade protection). New exit codes: 5 rolled back, 6 outcome not
  confirmed (C6 left powered), 7 C6 firmware predates the update path. New
  verb `healthypi fw esp32c6-status`; new acceptance cases `espfw_status` and
  `espfw rejects a bad signature` (23 cases).
- **Resets that would roll a C6 update back are refused.** While a C6 update is
  in flight or its new image is pending verification, `os reset` (unless
  `force`), `enter_recovery` (arm) and `conn_disable` answer 274, and a
  power-down requested from the UI or the shell waits.
- **Keepalive on the C6 link.** When the link is up and the M7 has sent nothing
  for 5 s it sends one real status request, so a pending C6 image always hears
  from the host.

### Fixed
- **The C6's radios come back after it restarts on its own.** A C6 restart the
  M7 did not cause (panic, watchdog, the restart after a firmware update) left
  both radios off while the M7 believed them on, so Wi-Fi/BLE streaming stopped
  until toggled by hand. The connectivity service now re-sends the requested
  radio state when it detects the restart.
- **QSPI flash is 64 MiB on HealthyPi 6 v5 and later.** The board definition
  declared the 1 Gbit (128 MiB) W25Q01JV and placed the `/lfs` LittleFS at
  `0x4000000`; the fitted part is 512 Mbit (64 MiB), which ignores the higher
  address bit, so LittleFS aliased the start of the chip -- the M7 update slot.
  An in-app M7 update could hang the device until the watchdog reset it, and
  each M7 update could silently erase stored settings. The QSPI is now declared
  as 64 MiB and every partition sits inside it (`xip` 48 MB, `tflite_model` at
  `0x3000000`, `/lfs` 8 MB at `0x3800000`); the M7 update slot and M4 staging
  are unchanged. **Settings start empty on the first boot after updating**, as
  LittleFS moves. The release check now fails a build whose QSPI partitions do
  not fit the declared part, or a v5+ board that declares anything but 64 MiB.

### Changed
- **Update order is M4 → M7 → reset → ESP32-C6** (was ESP32-C6 → M4 → M7).
  Any M7 reset holds the C6 in reset, and a C6 reset before its new image has
  proved itself rolls it back, so the C6 goes after every reset -- which also
  lets one run update a unit whose old M7 predates the C6 commands (the M7 goes
  first, the C6 step runs on the new one). `healthypi device reset`, `conn
  disable` and `fw enter-recovery` check `espfw_status.busy` first and explain,
  instead of surfacing the device's 274.
- **The bundle's ESP32-C6 entry is signed and applied.** Its transport is now
  `hpi-g64-esp` with a `sig` like the M4's (ECDSA-P256 over the image's
  SHA-256, release key), and its version is read from the image's app
  descriptor; `fw info` and the update check every image signature against the
  key. `format` stays 1: an older bundle's unsigned `esp-ota-http` C6 entry is
  reported and skipped (its M4/M7 still apply), and older tools skipped the C6
  anyway. `scripts/release.sh` now **requires** the C6 app image from
  healthybridge-esp32's `build.hp6` -- checking target, HP6 profile, OTA receiver
  and staleness -- and no longer builds it; `--no-esp32` makes a bundle without
  it and says that bundle cannot update the C6.
- **HealthyBridge vitals carry the full canonical record (wire change; flash
  the M7 together with healthybridge-esp32 1.1.0).** The vitals frame (`0x40`)
  sent to the ESP32-C6 is now the 16-byte `hp6_vitals` record as-is: heart rate,
  SpO₂ ×10, respiration rate, temperature ×100, SDNN, RMSSD, LF/HF ×10 and the
  `HP6_VIT_*` flags. It used to be a 12-byte form that clamped every rate to a
  byte, rounded SpO₂ to whole percent and temperature to tenths, dropped HRV,
  and sent the flags as 0 -- so the co-processor could not tell an ECG heart
  rate from a PPG pulse rate, nor show lead-off, weak PPG or motion. A C6 on
  1.1.0 rejects the old payload and an older C6 misreads the new one.
- The release bundle is now named `hpi6-firmware-<version>.zip` instead of
  `hpi6-<version>.hpifw`. The format and signing are unchanged: it was always a
  plain zip. Tools read it by content, so existing `.hpifw` files still work.

## [1.0.0] — unreleased

The first public release. Nothing had shipped before it, so both cores start at
1.0.0 regardless of the internal numbering used during development.

> **Note on MCUboot downgrade prevention.** Development units running a 2.0.x
> image cannot be updated *into* 1.0.0 over OTA — MCUboot refuses an image whose
> version is lower than what is installed (`E: Insufficient version in secondary
> slot`). Reflash those over SWD with `scripts/flash.sh factory`.

### Acquisition
- ECG and thoracic respiration on the TI ADS1294R, 24-bit at 500 Hz. Leads I, II
  and V1 from a 5-electrode cable; other limb leads derived in software.
- PPG on the TI AFE4400, 22-bit at 250 Hz. The rate has a single source of truth
  in `CONFIG_AFE4400_PRF_HZ`, which programs the AFE *and* derives the rate
  advertised on the wire, so hardware and metadata cannot drift apart.
- ECG lead-off detection: comparators configured, electrode mask debounced and
  published, and an ECG-derived heart rate suppressed while any electrode is off.
- Temperature (ams AS6221), motion (BMI323), fuel gauge (MAX17048) and a
  TI BQ24074 linear charger.

### Architecture
- Layered publish/subscribe application on the Cortex-M7: a sample bus with
  per-consumer lock-free rings and drop-on-full, so a slow consumer can never
  back-pressure acquisition.
- Cortex-M4 as a calculations-only core over OpenAMP/RPMSG. QRS detection and
  heart rate are live; SpO₂ and EEG band power are present but not enabled.
- HealthyLink expansion framework: provider registry, interface arbiter and a
  supervisor that quarantines a faulting module without disturbing acquisition.

### Host interface
- USB composite device: CDC 0 carries the live `.HP6` sample stream, CDC 1 the
  MCUmgr/SMP control channel, on one cable.
- `.HP6` container (magic `HPI6`, version `0x0200`) — byte-identical between the
  live stream and the SD recording, so one parser reads both.
- MCUmgr group 64 for device info, telemetry, diagnostics, recording, streaming,
  configuration, Wi-Fi and firmware transfer.
- Transfer Mode: the microSD card exposed as USB mass storage, enumerated only
  while armed.
- Wi-Fi and BLE via the ESP32-C6 co-processor over a framed UART link at 2 Mbaud
  with hardware flow control.

### Firmware update and recovery
- MCUboot with ECDSA-P256 verification of the M7 image before boot.
- M4 images staged to QSPI and written to bank 2 by the M7, verified there with
  SHA-256, ECDSA-P256 and a vector-table sanity check.
- One `.hpifw` release bundle covering both cores, applied with
  `healthypi fw update`.
- MCUboot serial recovery over USB CDC, entered automatically when no bootable
  image is present or on request over group 64.
- `scripts/release.sh` as the only supported production path, gated on a
  shippability check.

### Display
- 4" 480×800 touch UI on LVGL with a Material 3 design system.

### Host tooling
- The `healthypi` Python package and CLI: `.HP6` decode/verify/export, the
  group-64 catalog and generated wire classes, the firmware updater and bundle
  format, HealthyLink EEPROM images, and a device acceptance suite.
- `smpgroup`, the generic MCUmgr custom-group machinery, separable and
  Apache-2.0.
- `healthypi/smp/catalog.py` is the single source of truth for the group-64
  surface; CI re-parses the firmware sources and fails on any disagreement.

### Moved out of this repository
- **The HealthyLink Compute (STM32N657 NPU) module firmware** now lives in
  **[`Protocentral/healthylink-compute-fw`](https://github.com/Protocentral/healthylink-compute-fw)**.
  `app_healthylink_compute/`, `boards/protocentral/healthylink_compute/` and
  `scripts/npu/` left this tree on 2026-08-31, and `scripts/build.sh npu` /
  `scripts/flash.sh npu` went with them.

  The reason is licensing. That firmware vendors STMicroelectronics' ATON NPU
  runtime under **ST SLA0104**, whose clause 5 forbids redistribution "in any
  manner that would subject the SOFTWARE PACKAGE to any Open Source Terms" —
  and the agreement names MIT explicitly. This repository declares MIT. The two
  could not coexist, so the code moved rather than the licence.

  **The host half stays here**: `app_m7/src/healthylink/mod_npu.c`,
  `drivers/misc/healthylink/`, `include/healthylink/` and the per-board
  `healthylink-compute.overlay` files. The contract binding the two halves is
  documented in `docs/HEALTHYLINK.md` §12 here and, in full, in
  [`docs/SPI_PROTOCOL.md`](https://github.com/Protocentral/healthylink-compute-fw/blob/main/docs/SPI_PROTOCOL.md)
  there — it did not exist in writing before the split.

### Known limitations
- **SpO₂** is computed, but the R→SpO₂ curve has never been calibrated against a
  reference oximeter on this hardware. Do not treat the value as clinical.
- **HRV** (SDNN/RMSSD, LF/HF) is computed, but has not been validated against a
  known RR series.
- **Lead-off** detects RA, LA and LL. **V1 is not detected** — a board-level
  fault, not a firmware one. The comparator threshold has not been checked on a
  body.
- **EEG and NPU expansion modules** are experimental. The NPU link works; the
  inference path is a stub on the module side, and the module's firmware lives
  in the separate `Protocentral/healthylink-compute-fw` repository.

Camera capture is **not** a limitation — it is outside the product definition.
The board has a CSI connector; no camera support is planned.

---

Development history before this release is not published. Pre-1.0 version
numbers that may appear in older bundles or device reports do not correspond to
any entry above.
