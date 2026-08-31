# Device lock / unlock

> This document describes a **firmware feature** — the runtime access-control
> gate on the control interface. To **report a security vulnerability**, see
> [SECURITY.md](../.github/SECURITY.md) at the repository root instead.

The runtime access-control gate on the group-64 control interface: the device
boots locked, and privileged commands are refused until a host proves it holds
the device secret. Specified by the access-control section of
[`HOST_INTERFACE.md`](HOST_INTERFACE.md).

**Gated behind `CONFIG_HPI_SECURITY` (default n)** — with it off,
`hpi_security_require_unlocked()` is a no-op and every command is allowed.

---

## Shipping posture: units ship unlocked

`CONFIG_HPI_SECURITY` stays **off** in the production image
(pinned explicitly in `app_m7/prj.prod.conf`, asserted by
`tools/ci/check_prod_surface.sh`). This is a decision, not a default that was
never revisited. The reasoning, so it can be re-litigated on evidence rather
than from scratch:

1. **It is not what protects firmware authenticity.** Every image a unit accepts
   is ECDSA-P256 verified — the M7 by MCUboot before boot, the M4 by the M7
   application at commit. That holds whether the unlock gate is on or off. The
   gate protects against a *local USB actor issuing privileged commands*, which
   is a different and much smaller threat.
2. **The only honest implementation is not available yet.** A real gate needs a
   per-device secret in OTP. `HPI_UNLOCK_SECRET_SOURCE_OTP` is unimplemented and
   fails closed (the device cannot be unlocked at all).
3. **The available alternative is worse than off.** Enabling it with
   `HPI_UNLOCK_SECRET_SOURCE_KCONFIG` gives every unit ever built the *same*
   secret — protection that looks real, isn't, and would be discovered by the
   first person to read this repo. The firmware logs a DO-NOT-SHIP warning for
   exactly this case, and `check_prod_surface.sh` fails the combination outright.
4. **The device is a USB-tethered research instrument.** Physical access to the
   port is already physical access to the electrodes and the SD card.

**What would change this:** OTP provisioning landing (then the OTP secret source
below becomes real), or the WiFi OTA path shipping — at that point the attack
surface stops being local, and the gate must be on before it does. The code stays
in the tree, compiled out, so that turning it on is a Kconfig change plus
provisioning, not a rewrite.

---

## Model

Boot **LOCKED**. Read-only / non-PHI commands work locked; destructive / PHI /
privileged commands return `MGMT_ERR_EACCES` until unlocked:

```
host                                            device (LOCKED)
 ── hpi/unlock_challenge ──────────────────────► nonce[16] = CSPRNG, TTL 60 s
 ◄─ { nonce, ttl_ms:60000 } ───────────────────
 tag = HMAC-SHA256(secret, nonce)
 ── hpi/unlock_response { tag:bstr[32] } ───────► verify; if OK → UNLOCKED
 ◄─ { state:1, ttl_ms:<grant> } ──────────────── grant TTL = CONFIG_HPI_UNLOCK_GRANT_S (def 600 s)
```

Relocks on: grant TTL elapse, `hpi/lock` (0x0012), or reboot. (CDC1 DTR-down
relock is a TODO hook.) Nonce is single-use and 60 s; a new challenge invalidates
the old one.

## Commands (group 64)

| id | cmd | dir | payload |
|----|-----|-----|---------|
| 0x0010 | unlock_challenge | read | → `{ nonce: bstr[16], ttl_ms }` |
| 0x0011 | unlock_response | write | `{ tag: bstr[32] }` → `{ state, ttl_ms }` |
| 0x0012 | lock | write | → `{ state:0 }` |
| 0x0013 | lock_state | read | → `{ state }` |

Errors: `264` challenge-missing, `265` HMAC-invalid, `266` grant/nonce expired
(group-64 error codes, already in `hpi_mgmt_group.h`).

## Gated commands (wired so far)

`hpi/stream_start`, `hpi/sd_record_start`, `hpi/module_power`, and
`hpi/transfer_mode` (MSC read-write) call `hpi_security_require_unlocked()`. The
helper is available for the rest of the privileged list (`os reset`, `os/datetime_set`,
`config_write` sensitive keys, `sd_delete`/`sd_format`, `wifi_set`, OTA
`img upload`) as those handlers land.

## Files

- `control/security/hpi_security.{c,h}` — state machine + nonce + HMAC verify
  (PSA Crypto: `psa_generate_random`, `psa_import_key` HMAC, `psa_mac_verify`).
- `control/mcumgr_hpi/hpi_security_cmd.c` — the four group-64 handlers.
- `main.c` calls `hpi_security_init()` at boot.

## Secret source (`CONFIG_HPI_UNLOCK_SECRET_SOURCE_*`)

- **KCONFIG (dev):** `CONFIG_HPI_UNLOCK_SECRET_HEX` (64 hex chars). Logs a
  `DO NOT SHIP` warning at boot. Use for bench testing.
- **OTP (production):** device-unique 256-bit secret in STM32H7 OTP, provisioned
  at manufacture, QR-stickered inside the enclosure for recovery. **OTP read is not
  implemented** — it currently fails closed (unlock refused).

## Enabling (when ready)

1. `CONFIG_HPI_SECURITY=y` (pulls in mbedTLS PSA: HMAC + SHA-256 + CSPRNG).
2. Validate the PSA crypto + entropy config on target (a CSPRNG/entropy source
   must be available for `psa_generate_random`).
3. Dev: set `CONFIG_HPI_UNLOCK_SECRET_HEX`; host computes
   `HMAC-SHA256(secret, nonce)` and sends it to `unlock_response`.
4. Production: implement the OTP secret read, provision per-device secrets, and
   enable in `prj.prod.conf`.

> Note: `prj.prod.conf` and `tools/ci/check_prod_surface.sh` keep the prod image's
> debug surface closed regardless; this adds the *runtime* access-control gate.
