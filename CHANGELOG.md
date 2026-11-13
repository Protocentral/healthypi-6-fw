# Changelog

All notable changes to the HealthyPi 6 firmware are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Versions here are the **firmware** version reported by the device
(`app_m7/VERSION`, `app_m4/VERSION`) and carried in the release bundle.

## [Unreleased]

### Fixed
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
- `healthypi test run` against a port that does not answer exited 1 with a
  traceback, the same code as a test FAIL. It now exits 3 with a message.
- **Heart rate, SpO2 and HRV no longer stay on screen after the M4 stops
  sending them.** Their 5 s freshness limit was only checked when a new vitals
  message arrived, so if the M4 crashed, hung, was being updated or simply went
  quiet, the last values stayed on the display, in the live stream, on the
  ESP32 link and in the lead-off diagnostic reply as if they were current.
  The M7 now publishes an updated vitals sample at the moment each value
  expires, with expired values reading 0 ("not available"); values return with
  the next message from the M4. SpO2 and the weak-perfusion flag now expire
  with the PPG data they came from -- before, SpO2 had no freshness limit at
  all.
- **HRV frequency domain (M4).** Three defects found by the new host test of the
  HRV maths (`app_m4/tests/hrv/`) are fixed. VLF/LF/HF band powers were exactly
  1/64 of the true value (the frequency step was applied twice). The spectrum
  was taken over the *oldest* 64 s of the up-to-4-minute RR history rather than
  the newest, so LF/HF could describe data about three minutes old. With 32–64 s
  of data the record was tapered with a truncated window and normalised as if it
  were whole, reading 20–50 % low and weighting the record unevenly in time.
  Band powers and LF/HF now agree with an independent reference; only LF/HF is
  carried in `hp6_vitals`, so the scale fix changes nothing a host sees; the
  other two do change the LF/HF it receives. Not yet checked on hardware.
- **ECG heart rate follows a change in seconds, not minutes (M4).** It was the
  mean of the whole 256-beat RR history, so a step from 60 to 100 BPM took 236
  beats (~2.4 minutes) to be reported within 5 BPM. It is now the median of the
  newest 8 RR intervals: the same step, either direction, settles in 5 beats,
  and a single premature, missed or extra beat no longer moves the reading (a
  plain mean of 8 would have swung it by up to 8 BPM). A steady rhythm reads
  exactly as before, and the first rate still appears after 4 beats. HRV is
  unchanged and still uses the full history. Checked on the host
  (`app_m4/tests/hrv/`); not yet checked against real QRS detection on hardware.
- **A stale ECG heart rate is withdrawn after 5 s (M4).** With HRV on (the
  default) the M4 re-sends its ECG vitals every 5 s whether or not a beat was
  detected, still carrying the last rate, and the M7 counts an ECG rate as
  fresh for 5 s from each message's arrival. So if QRS detection stopped while
  the electrodes stayed on -- asystole, a flat or very low-amplitude trace, a
  detector that lost the signal -- the last heart rate could stay on the
  display, in the stream and in recordings indefinitely, as a live ECG rate.
  The M4 now reports no rate (0, confidence 0) once no RR interval has been
  accepted for 5 s of signal, and sends that at once; the M7 then falls back
  to a fresh PPG rate (flagged as such) or shows none. A rhythm below 30 BPM,
  which the M4 cannot measure, now also clears the rate instead of keeping the
  last one. After a pause the rate returns after 4 beats and is built only from
  intervals after the pause. A steady 30 BPM and a single missed beat at
  normal rates do not trigger it; below about 36 BPM one missed beat does.
  Checked on the host; not yet checked on hardware.

### Added
- **`scripts/release.sh` gates a bundle on hardware.** `--port <CDC1>` (or
  `HP6_RELEASE_PORT`) runs the group-64 acceptance suite (`healthypi test run`)
  on the attached unit first and refuses to build on any FAIL or if the suite
  cannot run. SKIPs are listed and allowed, and the report is kept as
  `build/release/hw-gate.json`. Without a port it refuses unless `--no-hw-gate`
  is given, which prints a warning that the bundle was not hardware-gated.
- `CONFIG_HPI_RESP_MIN_AMP_UV` (default 100, unchanged): the respiration-rate
  detector's amplitude floor, now configurable. With `resp_debug.conf` the
  detector logs the filtered amplitude it sees and the rate it produced every
  5 s, so the floor can be set from bench data.

### Changed
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
- ECG lead-off detection for RA, LA and LL: comparators configured, electrode
  mask debounced and published, and an ECG-derived heart rate suppressed while
  any electrode is off. V1 is wired into the mask but never asserts on v5 (see
  Known limitations).
- Temperature (ams AS6221), motion (BMI323), fuel gauge (MAX17048) and a
  TI BQ24074 linear charger.

### Architecture
- Layered publish/subscribe application on the Cortex-M7: a sample bus with
  per-consumer lock-free rings and drop-on-full, so a slow consumer can never
  back-pressure acquisition.
- Cortex-M4 as a calculations-only core over OpenAMP/RPMSG. QRS detection and
  ECG heart rate are live. SpO₂ and a PPG pulse rate are computed and published,
  but the R→SpO₂ curve is inherited and uncalibrated on this hardware. HRV
  (SDNN/RMSSD, LF/HF) is computed by default but not yet validated against a
  reference. EEG band power is a stub: EEG batches are received and counted,
  not analysed.
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
- **Lead-off** detects RA, LA and LL. **V1 is not detected on v5** — its bit
  in the lead-off mask never asserts, so the absence of a V1 lead-off warning
  does not mean the V1 electrode is attached. This is a board-level limitation,
  not a firmware one, and 1.0.0 ships with it. The comparator threshold has not been
  checked on a body.
- **EEG and NPU expansion modules** are experimental. The NPU link works; the
  inference path is a stub on the module side, and the module's firmware lives
  in the separate `Protocentral/healthylink-compute-fw` repository.

Camera capture is **not** a limitation — it is outside the product definition.
The board has a CSI connector; no camera support is planned.

---

Development history before this release is not published. Pre-1.0 version
numbers that may appear in older bundles or device reports do not correspond to
any entry above.
