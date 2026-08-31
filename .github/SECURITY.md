# Security policy

## Reporting a vulnerability

**Please do not open a public GitHub issue for a security vulnerability.**

Report it privately, either way:

- **GitHub Security Advisories** — use the *Report a vulnerability* button under
  this repository's **Security** tab. This is preferred; it keeps the report,
  the discussion and the eventual advisory in one place.
- **Email** — `support@protocentral.com`, with `SECURITY` in the subject line.

Please include, as far as you can:

- what the issue is and what an attacker gains from it;
- the board revision and the firmware versions the device reports
  (`healthypi device info --port <control-port>`);
- how to reproduce it, and whether it needs physical access, USB access, or
  network access via the Wi-Fi co-processor;
- anything you have already tried that mitigates it.

We will acknowledge your report within **5 working days** and tell you whether we
can reproduce it, whether we consider it a vulnerability, and what we intend to
do. We will keep you updated while we work on a fix, and we will credit you in
the advisory unless you would rather we did not.

## Scope

| In scope | Out of scope |
|---|---|
| The M7 and M4 firmware in this repository | Zephyr, MCUboot and their modules — report those upstream |
| The host tooling under [`tools/`](../tools/) | The ESP32-C6 firmware — report in [`healthybridge-esp32`](https://github.com/Protocentral/healthybridge-esp32) |
| The `.HP6` format and the MCUmgr group-64 surface | The HealthyLink Compute (STM32N657) module firmware, which lives in the separate `Protocentral/healthylink-compute-fw` repository — report it there |
| The firmware update, signing and recovery paths | Physical attacks that require disassembling the unit (see below) |

## Known and accepted limitations

These are documented decisions, not undiscovered problems. Reporting them is
welcome if you can show the reasoning is wrong, but they are not news:

- **Units ship with the runtime access-control gate off.** `CONFIG_HPI_SECURITY`
  is `n` in the production image. The reasoning, and what would change it, is in
  [`docs/DEVICE_LOCK.md`](../docs/DEVICE_LOCK.md). Firmware *authenticity* does
  not depend on that gate — every image a unit accepts is ECDSA-P256 verified.
- **Physical access is game over.** The device is a USB-tethered research
  instrument. There is no RDP level 2, no JTAG lockout and no tamper detection.
  Anyone who can open the case can read the flash.
- **Anti-rollback is version-based only.** MCUboot refuses a lower version; there
  is no hardware security counter.

## What this product is

**HealthyPi 6 is not a medical device.** It is for education, research and
development, and it is not certified or validated for diagnosis, treatment or any
clinical decision. Please weigh vulnerability severity accordingly — but do
report anything that could mislead a user about the data they are looking at, or
that could let an attacker alter recorded or streamed signals.
