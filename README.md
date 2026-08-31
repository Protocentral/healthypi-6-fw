<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/images/hpi6-logo-dark.png">
  <img src="docs/images/hpi6-logo-light.png" alt="HealthyPi 6" width="380">
</picture>

# HealthyPi 6 — Firmware

![HealthyPi 6](docs/images/healthypi6%20v5.jpeg)

[![Firmware: MIT](https://img.shields.io/badge/Firmware-MIT-blue.svg)](LICENSE.md)
[![Docs: CC BY-SA 4.0](https://img.shields.io/badge/Docs-CC%20BY--SA%204.0-lightgrey.svg)](LICENSE.md)
[![Zephyr RTOS](https://img.shields.io/badge/Zephyr-4.4-7929d2.svg)](https://www.zephyrproject.org/)
[![Board](https://img.shields.io/badge/board-healthypi6__v5-informational.svg)](boards/protocentral/healthypi6_v5)

Firmware for the HealthyPi 6 open-source vital signs monitor.

The device acquires ECG, thoracic respiration and PPG on dedicated analog front
ends. Heart rate, HRV and SpO₂ are derived from those on a second core. Data is
shown on a 4" touch screen, recorded to SD, streamed over USB, and re-streamed
over Wi-Fi and BLE by a network co-processor.

This repository builds the images that run on the STM32H757 and drives the
ESP32-C6.

> **HealthyPi 6 is a research and education instrument, not a medical device.**
> Read [Important notice](#important-notice) before connecting it to a person.
> It also says which derived values are not yet trustworthy, and why.

---

## Design

A few structural decisions explain most of the codebase.

- Acquisition uses dedicated analog front ends: a TI ADS1294R for ECG and
  thoracic respiration, a TI AFE4400 for PPG.
- The two cores are split by job. The Cortex-M7 samples and never stops; the
  Cortex-M4 does calculation only — QRS detection, heart rate and HRV on
  CMSIS-DSP — and owns no hardware.
- Acquisition publishes to a sample bus rather than calling consumers. Each
  consumer holds its own ring and drops on its own terms, keeping a count of
  what it dropped, so a slow consumer cannot back-pressure the ADC and losses
  are visible rather than silent.
- One USB-C cable presents two independent serial ports: a live data stream and
  a control channel that stay out of each other's way.
- Images are signed and verified by the bootloader before boot, and a serial
  recovery path exists for a device left unbootable by a failed update.
- The HealthyLink expansion port adds modules without changes to the
  acquisition path.

---

## Signals and front ends

| Signal | Front end | Resolution / rate | Notes |
|---|---|---|---|
| **ECG** | TI ADS1294R | 24-bit, 500 Hz | Leads I, II and V1 from a 5-electrode cable; other limb leads derived in software |
| **Respiration** | TI ADS1294R (CH1) | 24-bit, 500 Hz | Thoracic impedance — shares the ECG front end |
| **PPG** | TI AFE4400 | 22-bit, 250 Hz | Red + IR, at full rate to the bus, the stream and the recording |
| **SpO₂** | derived from PPG | — | Computed on the M4; the R→SpO₂ curve is not yet calibrated on this hardware |
| **Temperature** | ams AS6221 | 16-bit | Declared in the devicetree and backed by a driver, but **the firmware never reads it**; the field always reads 0 |
| **Motion** | Bosch BMI323 | 6-axis | Accelerometer + gyroscope |
| **Battery** | Maxim MAX17048 · TI BQ24074 | — | Fuel gauge and linear charger |

Sample rates have a single source of truth in the firmware:
`CONFIG_AFE4400_PRF_HZ` programs the AFE4400 *and* derives the rate advertised on
the wire, so the hardware and the metadata cannot drift apart.

---

## Hardware

- **STM32H757BI** (LQFP208) — Cortex-M7 @ 400 MHz (application) + Cortex-M4 @ 200 MHz
  (algorithms), communicating over OpenAMP/RPMSG
- **ESP32-C6** network co-processor for Wi-Fi and BLE, reached over a framed UART
  link at 2 Mbaud with hardware flow control
- **4" 480×800 touch display** (GC9503V, MIPI-DSI) driven by LVGL
- **32 MB SDRAM**, **128 MB QSPI NOR**, **microSD**
- **USB-C** composite device — two CDC ACM ports, plus mass storage on demand
- **HealthyLink expansion port** — M.2 connector carrying SPI, UART, I²C, CAN-FD
  and ADC for add-on modules

Current board revision: **`healthypi6_v5`**. Earlier revisions (v2–v4) remain in
the tree and are electrically similar; v5 is the one actively built and tested.

---

## Getting started

Install the [Zephyr toolchain](https://docs.zephyrproject.org/latest/develop/getting_started/)
first — Zephyr SDK, `west`, and a Python virtual environment.

```bash
# 1. Create the workspace
west init -m https://github.com/protocentral/healthypi-6-fw --mr main hpi6-workspace
cd hpi6-workspace && west update

# 2. Set up the environment (venv, board, paths)
cd healthypi-6-fw
source scripts/env.sh

# 3. Build and flash both cores
scripts/build.sh m7
scripts/build.sh m4
scripts/flash.sh all
```

Plug in USB: the device enumerates two serial ports and starts streaming.

---

## Building

Use the scripts rather than a bare `west build`: they select the board revision,
the configuration fragments and the devicetree overlays, which a direct
invocation silently omits.

```bash
scripts/build.sh m7 [dev|prod]      # application + Material 3 UI   -> build/m7
scripts/build.sh m4                 # algorithm core                -> build/m4
scripts/build.sh signed [dev|prod]  # MCUboot + signed application  -> build/m7s
scripts/build.sh esp32              # network co-processor (external repo)
scripts/build.sh all                # m7 + m4
```

`dev` builds are verbose and carry debug surfaces; `prod` builds trim them.

### What ships

```bash
scripts/release.sh        # -> build/release/hpi6-<version>.hpifw
```

`release.sh` is the only supported production path. It builds the signed prod
image, packages it with the M4 image into a `.hpifw` bundle, and refuses to emit
one that fails the shippability check. A plain `scripts/build.sh m7` image has no
bootloader, no update path and no recovery entry — a unit flashed with it can
only ever be updated over SWD, with the case open.

---

## Flashing

```bash
scripts/flash.sh all        # M7 + M4 over SWD
scripts/flash.sh signed     # bootloader + signed application
scripts/flash.sh factory    # full production programming sequence
```

---

## Updating a device

Each processor updates differently, because the hardware differs:

| Processor | Path | Verified by |
|---|---|---|
| **M7** | MCUboot image slot, MCUmgr over USB | MCUboot — ECDSA-P256, before boot |
| **M4** | staged to QSPI, written to bank 2 by the M7 | the M7 — SHA-256 + ECDSA-P256 + vector-table sanity check |
| **ESP32-C6** | its own OTA over Wi-Fi | ESP-IDF app descriptor + rollback |

One release key signs all of it, and a release is a single bundle:

```bash
healthypi fw update --port <control-port> --bundle build/release/hpi6-1.0.0.hpifw
```

If an update ever leaves a device unbootable, MCUboot's **serial recovery** takes
over automatically: the device enumerates as a single CDC port named
"HealthyPi 6 Recovery", and the same tool reflashes it with `--recover`.

Full detail: **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#9-firmware-update-and-recovery)**.

---

## Host interface

One USB-C cable presents two independent serial ports:

| Port | Purpose |
|---|---|
| **CDC 0** | Live sample stream in the `.HP6` format — byte-identical to what the SD recording contains, so one parser reads both |
| **CDC 1** | MCUmgr/SMP control channel: device info, streaming, recording, Wi-Fi, firmware update |

**Transfer Mode** additionally exposes the microSD card as USB mass storage, so
recordings can be copied without removing it. The mass-storage function
enumerates only while armed.

---

## Software and documentation

| Repository | What it is |
|---|---|
| [`healthypi-6-fw`](https://github.com/protocentral/healthypi-6-fw) | This repository — STM32H757 firmware |
| [`healthybridge-esp32`](https://github.com/protocentral/healthybridge-esp32) | ESP32 network co-processor firmware, shared with HealthyPi 5 |

> The two update together. The link between them carries a versioned wire
> contract, and mismatched firmware on either side is not a graceful degradation.

Design documentation is in [`docs/`](docs/):

| | |
|---|---|
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | the three cores, the data path, the cross-core contracts |
| [`docs/HP6_DATA_FORMAT.md`](docs/HP6_DATA_FORMAT.md) | **the `.HP6` frame, stream and file format** — write your own parser from this |
| [`docs/MCUMGR_COMMANDS.md`](docs/MCUMGR_COMMANDS.md) | **the group-64 command reference** — for standard MCUmgr clients |
| [`docs/HOST_INTERFACE.md`](docs/HOST_INTERFACE.md) | the wider host-interface specification (§ 7 is superseded; the two rows above are canonical) |
| [`docs/ARCHITECTURE.md` § 9](docs/ARCHITECTURE.md#9-firmware-update-and-recovery) | updating and recovering a device |
| [`docs/HEALTHYLINK.md`](docs/HEALTHYLINK.md) | the expansion port: connector, pinout, module detection |
| [`app_m7/ARCHITECTURE.md`](app_m7/ARCHITECTURE.md) | the M7's internal layering, and the one rule |

Full index: [`docs/README.md`](docs/README.md).

---

## What's in this repository

| Path | Contents |
|---|---|
| `app_m7/` | Application core: acquisition, sample bus, services, transport, UI |
| `app_m4/` | Algorithm core: QRS/HR/HRV, SpO₂, CMSIS-DSP |
| `drivers/` | Out-of-tree Zephyr drivers — ADS1294R, AFE4400, display, HealthyLink, host link |
| `boards/` `dts/bindings/` | Board definitions, devicetree and bindings |
| `scripts/` | Build, flash and release — the supported entry points |
| `tools/` | Host tooling: the `healthypi` CLI and library (updater, `.HP6`, group-64, acceptance suite) and the generic `smpgroup` machinery |
| `docs/` | Architecture, host interface, update system, UI design |

---

## Support

Please open an issue in the repository that matches the problem:

- Firmware, build or update issues → this repository
- Wi-Fi, BLE, MQTT or the dashboard → [`healthybridge-esp32`](https://github.com/protocentral/healthybridge-esp32)

For a firmware issue, include the board revision, the exact `scripts/build.sh`
command you used, and the device's reported firmware versions.

---

## Contributing

See [`CONTRIBUTING.md`](CONTRIBUTING.md). In short: build both cores *and* the
signed flavor before opening a PR, keep the layer rule, put new capabilities in
`services/` with thin adapters, and add new sources to `app_m7/CMakeLists.txt`
explicitly.

---

## Important notice

**HealthyPi 6 is not a medical device.** It is intended for education, research
and development only. It is not certified, calibrated or validated for diagnosis,
treatment, or any clinical decision.

**Do not connect a person to a device powered from a mains-derived USB supply.**
Use battery power, or a properly isolated supply, whenever electrodes are
attached to a body.

**Some values are not yet trustworthy, and the firmware cannot tell you which.**
As of firmware 1.0.0:

- **SpO₂** is computed, but the R → SpO₂ curve has never been calibrated against
  a reference oximeter on this hardware. Treat the number as uncalibrated.
- **HRV** (SDNN, RMSSD, LF/HF) is computed, but has never been validated against
  a known RR series.
- **Lead-off** detects the RA, LA and LL electrodes. **V1 is not detected**, so
  absence of a V1 lead-off warning does not mean the V1 electrode is attached.
- **Temperature and respiration rate are never produced.** Both fields exist in
  the data format and both always read 0. Zero means "not available" throughout,
  never "measured zero".

Heart rate from the ECG is the one derived value that has been exercised
end to end.

---

## License

| Component | Licence |
|---|---|
| **Firmware written for this project** | [MIT](LICENSE.md) |
| **Files derived from or co-copyright with upstream** (Zephyr, Linaro, ST, Nordic, NXP, Espressif) | Apache-2.0 — retained, and identified per file |
| **Bundled fonts** | SIL Open Font License 1.1 |
| **Documentation** | CC BY-SA 4.0 |

Every source file written for this project carries an `SPDX-License-Identifier`,
and **that identifier is authoritative for that file**. Third-party components,
their copyright holders and the verbatim licence texts are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Zephyr, its modules and MCUboot are fetched by `west update` and licensed in their
own repositories, not here.

This firmware is distributed in the hope that it will be useful, but **without any
warranty**; without even the implied warranty of merchantability or fitness for a
particular purpose.