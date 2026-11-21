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
| the release manifest | host tools (`healthypi fw update/recover/info`) | **shipped in the package**, `tools/healthypi/src/healthypi/fw/release_keys/*.pub.pem`; `--pubkey` overrides it |

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

### The public half is published

Add it once, on the machine that holds the private key, and commit **only** the
`.pub.pem`:

```bash
openssl ec -in /secure/hp6_release_ec256.pem -pubout \
    -out tools/healthypi/src/healthypi/fw/release_keys/hp6_release_ec256.pub.pem
healthypi fw keys      # prints its fingerprint -- publish it in SECURITY.md and release notes
```

From then on the `healthypi` tools check every bundle against it without
`--pubkey`, so a repackaged bundle is refused. `release.sh` checks that each
release verifies against it before declaring the release ready. CI allows a
tracked `*.pub.pem`, and fails any tracked file containing a PEM private key,
whatever its name.

### The private half is not

It is never on a developer machine, in this directory, in a repository
secret, or in a cloud drive. It exists in two kinds of place, and nowhere
else:

- **The release workflow's signing job.** `.github/workflows/release.yml`
  splits a release into three jobs:
  - **`build`**: no secrets. It embeds only the public key, so the M7 comes
    out unsigned (`HP6_SIGNING_KEY=<.pub.pem> scripts/build.sh signed`).
  - **`sign`**: reads the key from the `HP6_RELEASE_KEY` secret of the
    `firmware-release` **environment**. That environment requires the release
    owner's approval and accepts only `v*` tags, which a tag ruleset protects.
    The job installs only hash-pinned tools
    (`tools/ci/release-sign-requirements.txt`) and never builds. It refuses
    unless the key matches the public key the build embedded
    (`tools/ci/release_sign.sh`).
  - **`release`**: creates a **draft** GitHub release, which is published by
    hand after the hardware gate.
- **The master copies**, offline on encrypted removable media. GitHub cannot
  give a secret back, so these copies are the key; the environment holds a
  copy of it.

The same scripts sign by hand from an offline copy, if the workflow is
unavailable:

```bash
HP6_SIGNING_KEY=<release .pub.pem> scripts/build.sh signed prod && scripts/build.sh m4
tools/ci/release_stage.sh /tmp/unsigned ../healthybridge-esp32
tools/ci/release_sign.sh  /tmp/unsigned /secure/media/hp6_release_ec256.pem /tmp/signed
```

Key paths must be absolute. Kconfig resolves a relative key path against the
west topdir, not this repo, so a relative path silently signs with something
else or fails obscurely.

### Custody

1. Generated once, on an offline machine, with `imgtool keygen -t ecdsa-p256`.
2. Stored on encrypted removable media, with **two** copies in separate physical
   locations. There is no recovery from losing it (see below).
3. Copied into the `firmware-release` environment secret only, never into a
   repository or organisation secret, which every workflow can read.
4. Never emailed, committed, or pasted into a chat or issue.

### What losing it means

Every unit already in the field has that key's public half embedded in its
bootloader. Losing the private half means **no shipped unit can ever receive a
firmware update over any non-SWD path again** — recovery mode included, since the
bootloader still verifies signatures there. The only remedy is recalling units
and reflashing over SWD with a new bootloader.

This is why the copies are physical and duplicated, and why the release procedure
does not depend on any one person's laptop.

### Rotation, and a backup key

Rotating the release key requires shipping a new bootloader. MCUboot cannot be
updated in the field on this board, so rotation is a full
`scripts/flash.sh factory` cycle, not an OTA.

That is why a **second (backup) public key** should be in the bootloader before
the first unit ships. Its private half is held by a different person in a
different place. If the primary is lost or leaks, releases switch to the backup
with an ordinary update instead of a recall.

- **MCUboot supports it.** Its verifier (`bootutil_find_key()`) searches a
  table of keys.
- **But the Zephyr port doesn't, yet.** `boot/zephyr/keys.c` in MCUboot v2.4.0
  builds a **one-entry** table from `CONFIG_BOOT_SIGNATURE_KEY_FILE`, so adding
  the backup key needs a small patch there.
- **Status: not done.** It needs a decision and a hardware test.
- **The M4 and manifest keys are easier.** They live in updatable code.
  `m4fw_pubkey.c` (generated from one key today) can become a list, and the
  `release_keys/` directory already accepts several keys.

### Owners may re-key their unit

The release key protects official updates. It does not lock the hardware:
anyone can program a bootloader built with their own key over SWD and run their
own firmware ("Your device, your key" in [`../.github/SECURITY.md`](../.github/SECURITY.md)).
Never publish a shared private key to make that easier, since every unit would
then accept anything signed with it.

## Anti-rollback

Software version-based, and only on the normal update path
(`CONFIG_MCUBOOT_DOWNGRADE_PREVENTION`): MCUboot refuses an M7 image older than
the installed one when it installs from the secondary slot. **Serial recovery
writes the primary slot directly and does not compare versions**, so an older
image signed with the release key can be installed that way, and the M4 path
compares no versions at all. It is a guard against accidents, not a security
boundary (see `docs/DEVICE_LOCK.md`). A hardware
OTP-backed security counter is a later hardening item — plain STM32H7 provides no
counter backend, and it belongs with the Step-9 OTP work.
