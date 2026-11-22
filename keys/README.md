# HealthyPi 6 image-signing keys

**No private key is ever committed to this repository.** The `.gitignore` here
ignores everything except itself and this README, and CI fails the build if a
`*.pem` is ever tracked.

## One key, three uses (plus a backup)

A single ECDSA-P256 keypair signs everything on the device:

| What it signs | Who verifies it | How the public half gets there |
|---|---|---|
| the M7 image | **MCUboot**, before boot | embedded into the bootloader at build time |
| the M4 and ESP32-C6 images | **the M7 application**, at group-64 commit / `espfw_begin` | `tools/build/gen_m4_pubkey.py` extracts it from the same PEM into `m4fw_pubkey.c` |
| the release manifest | host tools (`healthypi fw update/recover/info`) | **shipped in the package**, `tools/healthypi/src/healthypi/fw/release_keys/*.pub.pem`; `--pubkey` overrides it |

Deliberately not one key per processor. One key to hold, one to rotate, one to
lose — and no way for the M7 and M4 anchors to drift apart, because both are
generated from the same file in the same build.

Every place above also trusts a **backup key**, which signs nothing until the
primary is lost or revoked ([below](#backup-key)).

## Dev key — `hp6_dev_ec256.pem`

Generated automatically the first time you run `scripts/build.sh signed`, or by
hand:

```bash
imgtool keygen -k keys/hp6_dev_ec256.pem -t ecdsa-p256
```

Its backup, `hp6_dev_backup_ec256.pem`, is generated next to it the same way,
so a dev bootloader is built exactly as a release one is: with two keys.

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

Add both public halves once, on the machine that holds the private keys, and
commit **only** the `.pub.pem` files:

```bash
R=tools/healthypi/src/healthypi/fw/release_keys
openssl ec -in /secure/hp6_release_ec256.pem        -pubout -out $R/hp6_release_ec256.pub.pem
openssl ec -in /secure/hp6_release_backup_ec256.pem -pubout -out $R/hp6_release_backup_ec256.pub.pem
healthypi fw keys      # prints the fingerprints -- publish them in SECURITY.md and release notes
```

The release workflow embeds both, by those names.

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
    unless the key matches a public key the build embedded, the primary or
    the backup (`tools/ci/release_sign.sh`).
  - **`release`**: creates a **draft** GitHub release, which is published by
    hand after the hardware gate.
- **The master copies**, offline on encrypted removable media. GitHub cannot
  give a secret back, so these copies are the key; the environment holds a
  copy of it.

The same scripts sign by hand from an offline copy, if the workflow is
unavailable:

```bash
HP6_SIGNING_KEY=<release .pub.pem> HP6_BACKUP_KEY=<backup .pub.pem> \
    scripts/build.sh signed prod && scripts/build.sh m4
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
bootloader. Losing the private half of **both** the primary and the backup
means **no shipped unit can ever receive a firmware update over any non-SWD
path again** — recovery mode included, since the bootloader still verifies
signatures there. The only remedy is recalling units and reflashing over SWD
with a new bootloader. Losing one of the two is recoverable
([backup key](#backup-key)).

This is why the copies are physical and duplicated, and why the release procedure
does not depend on any one person's laptop.

### Rotation, and a backup key

Rotating the release key requires shipping a new bootloader. MCUboot cannot be
updated in the field on this board, so rotation is a full
`scripts/flash.sh factory` cycle, not an OTA.

That is why the bootloader holds a **second (backup) public key** from the first
unit on. Its private half is held by a different person in a different place.

<a id="backup-key"></a>
**How it works.**

- **MCUboot.** Its verifier (`bootutil_find_key()`) searches a table of keys,
  picking the one whose hash matches the image's KEYHASH TLV. MCUboot's Zephyr
  port builds a one-entry table (`boot/zephyr/keys.c`), so this project
  carries a two-line patch, in `zephyr/patches.yml`, that lets the build
  supply the table. This repo's module `CMakeLists.txt` generates it from
  `CONFIG_BOOT_SIGNATURE_KEY_FILE` and `CONFIG_HPI_BACKUP_SIGNING_KEY`.
  `scripts/build.sh` applies the patch when `west update` has dropped it. If
  it is ever missing anyway, the link fails (`bootutil_keys` defined twice);
  a one-key bootloader is never built by accident.
- **The M7 application.** `gen_m4_pubkey.py` emits both keys, and
  `fw_release_sig.c` accepts a signature from either, for the M4 and the C6.
- **Host tools.** `release_keys/` holds both `.pub.pem` files; any match
  passes.
- **Building.** `HP6_BACKUP_KEY` names the backup, an absolute path to a
  private key or a `.pub.pem` (only the public half is embedded). It
  defaults to the dev backup only when `HP6_SIGNING_KEY` is the dev key; any
  other signing key requires it. `HP6_BACKUP_KEY=none` builds a one-key
  bootloader, which `check_prod_surface.sh --release` refuses.
- **Signing.** Releases are signed with the primary. `release_sign.sh`
  accepts either key and says which it used.

| Event | What happens |
|---|---|
| Primary **lost** | Sign from now on with the backup: put it in the `firmware-release` environment in place of the primary. No unit changes. Generate a new backup at the next factory batch. |
| Primary **leaks** | Sign with the backup. Ship an M7 update that drops the leaked key from the M4/C6 list, remove it from `release_keys/`, and announce its fingerprint as revoked. MCUboot on units already shipped keeps trusting it; that cannot be fixed in the field. New units get a bootloader without it. |
| Backup lost or leaks | The same, the other way round. |

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
