# HealthyPi 6 release public keys

`healthypi fw update`, `fw recover` and `fw info` check a bundle's manifest
signature against the `*.pub.pem` files in this directory when no `--pubkey`
is given. Two files mean primary + backup; a bundle signed by either verifies.

**Public keys only.** A private key never belongs here or anywhere in this
repository -- CI fails the build if a tracked file contains one. The private
release key is kept offline; see `keys/README.md`.

To add the release key, on the machine that holds it:

```bash
openssl ec -in /secure/hp6_release_ec256.pem -pubout \
    -out tools/healthypi/src/healthypi/fw/release_keys/hp6_release_ec256.pub.pem
healthypi fw keys        # prints the fingerprint to publish
```
