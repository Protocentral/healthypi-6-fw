# Changelog

All notable changes to the HealthyPi 6 firmware are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Versions here are the **firmware** version reported by the device
(`app_m7/VERSION`, `app_m4/VERSION`) and carried in the `.hpifw` release bundle.

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
