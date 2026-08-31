# HealthyPi 6 image-signing keys

**No private key is ever committed to this repository.** The `.gitignore` here
ignores everything except itself and this README, and CI fails the build if a
`*.pem` is ever tracked.

## One key, three uses

A single ECDSA-P256 keypair covers the whole device:

| What it signs | Who verifies it | How the public half gets there |
|---|---|---|
| the M7 image | **MCUboot**, before boot | embedded into the bootloader at build time |
| the M4 image | **the M7 application**, at group-64 commit | `tools/build/gen_m4_pubkey.py` extracts it from the same PEM into `m4fw_pubkey.c` |
| the release manifest | host tools (`healthypi fw info --pubkey`) | passed on the command line |

Deliberately not one key per processor. One key to hold, one to rotate, one to
lose — and no way for the M7 and M4 anchors to drift apart, because both are
generated from the same file in the same build.

## Dev key — `hp6_dev_ec256.pem`

Generated automatically the first time you run `scripts/build.sh signed`, or by
hand:

```bash
imgtool keygen -k keys/hp6_dev_ec256.pem -t ecdsa-p256
```

**Key divergence is the trap.** MCUboot embeds the *public* half of whatever key
it was built with. A board flashed with a bootloader built against dev key A will
**reject** an image signed with dev key B — the device keeps running its old
firmware and the update simply does not take. That is the mechanism working
correctly, and it looks exactly like a broken update.

If more than one machine builds signed images for the same board, either share
one dev key out of band (password manager, secure drop), or re-flash the complete
signed build (`scripts/build.sh signed && scripts/flash.sh signed`) whenever the
key changes — that replaces the bootloader too, so the anchor moves with it.

## Release key

**Air-gapped.** It never lives on a developer machine, in this directory, in CI,
or in a cloud drive. Release builds pass it explicitly, as an absolute path:

```bash
HP6_SIGNING_KEY=/secure/media/hp6_release_ec256.pem scripts/release.sh
```

A relative path is a known trap — Kconfig key paths resolve against the west
topdir, not this repo, so a relative path silently signs with something else or
fails obscurely.

### Custody

1. Generated once, on an offline machine, with `imgtool keygen -t ecdsa-p256`.
2. Stored on encrypted removable media, with **two** copies in separate physical
   locations. There is no recovery from losing it (see below).
3. Present only for the duration of a release build, then removed.
4. Never emailed, committed, or pasted into a chat or issue.

### What losing it means

Every unit already in the field has that key's public half embedded in its
bootloader. Losing the private half means **no shipped unit can ever receive a
firmware update over any non-SWD path again** — recovery mode included, since the
bootloader still verifies signatures there. The only remedy is recalling units
and reflashing over SWD with a new bootloader.

This is why the copies are physical and duplicated, and why the release procedure
does not depend on any one person's laptop.

### Rotation

Rotating the release key requires shipping a new bootloader, so it is a full
`scripts/flash.sh factory` cycle, not an OTA. Plan it with a hardware revision,
not on its own.

## Anti-rollback

Currently software version-based (`CONFIG_MCUBOOT_DOWNGRADE_PREVENTION`): MCUboot
refuses an image whose version is lower than what is installed. A hardware
OTP-backed security counter is a later hardening item — plain STM32H7 provides no
counter backend, and it belongs with the Step-9 OTP work.
