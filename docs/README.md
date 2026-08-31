# HealthyPi 6 documentation

One directory. Start with whichever row describes what you are trying to do.

| I want to… | Read |
|---|---|
| Understand how the system fits together | [ARCHITECTURE.md](ARCHITECTURE.md) |
| **Read HealthyPi data in my own code** | [HP6_DATA_FORMAT.md](HP6_DATA_FORMAT.md) |
| **Control a device with an MCUmgr client** | [MCUMGR_COMMANDS.md](MCUMGR_COMMANDS.md) |
| Talk to a device from a host application | [HOST_INTERFACE.md](HOST_INTERFACE.md) |
| Update or rescue a unit | [ARCHITECTURE.md § 9](ARCHITECTURE.md#9-firmware-update-and-recovery) |
| Understand the release bundle | [ARCHITECTURE.md § 10](ARCHITECTURE.md#10-the-release-bundle) |
| Understand the Wi-Fi / BLE co-processor link | [ARCHITECTURE.md § 7](ARCHITECTURE.md#7-the-esp32-c6-link) |
| Know what the access-control gate does | [DEVICE_LOCK.md](DEVICE_LOCK.md) |
| **Report a security vulnerability** | [../.github/SECURITY.md](../.github/SECURITY.md) |
| Build an expansion module | [HEALTHYLINK.md](HEALTHYLINK.md) |
| Work with the NPU compute module | [HEALTHYLINK.md § 12](HEALTHYLINK.md#12-npu-compute-module) |
| Work with the EEG module | [HEALTHYLINK.md § 13](HEALTHYLINK.md#13-eeg-module) |

Building and flashing are covered in the root [README](../README.md) and
[CONTRIBUTING](../CONTRIBUTING.md). The M7 application's internal layering is
documented beside the code in [`app_m7/ARCHITECTURE.md`](../app_m7/ARCHITECTURE.md).

## What is not here

- **ESP32-C6 firmware** — a separate, dual-target project shared with
  HealthyPi 5: [`protocentral/healthybridge-esp32`](https://github.com/protocentral/healthybridge-esp32).
- **Vendor datasheets** — linked from the vendor rather than redistributed.
- **Internal working notes** — plans, handoffs, phase trackers and bring-up logs
  are not published. They go stale within a week of any publish, so publishing
  them would mislead more than it helps.

## A note on scope

Several subsystems here are real code that is **not finished**, and the documents
say so where it matters rather than implying otherwise. In particular: the NPU
and EEG data links are hardware-gated and default to off; Wi-Fi OTA is not
enabled; the M4 currently computes the ECG → heart-rate path only; and the
`rr_bpm` / `temp_c_x100` fields in the sample format are present but read zero.
