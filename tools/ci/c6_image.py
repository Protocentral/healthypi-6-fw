#!/usr/bin/env python3
# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT
"""Print the path of the HealthyPi 6 C6 app image in a healthybridge-esp32
checkout, or exit non-zero saying why it is unusable.

Shared by scripts/release.sh and the CI release workflow, so the two cannot
disagree about which C6 image is fit to ship. Everything is read from
ESP-IDF's own build metadata (project_description.json, sdkconfig.json):
the .bin name is per-target, and a guessed name once left the image silently
out of every bundle.

    tools/ci/c6_image.py <healthybridge-esp32 dir> [<build dir name>]
"""
import json, pathlib, sys
repo = pathlib.Path(sys.argv[1])
build = sys.argv[2] if len(sys.argv) > 2 else "build.hp6"
fix = f"Build it there:  (cd {repo} && ./hp6.sh build)"
desc = repo / build / "project_description.json"
if not desc.is_file():
    sys.exit(f"no HealthyPi 6 build of the C6 firmware in {repo}/{build}. {fix}")
d = json.loads(desc.read_text())
if d.get("target") != "esp32c6":
    sys.exit(f"{repo}/{build} was built for {d.get('target')!r}, not esp32c6 -- "
             f"wrong target. Rebuild:  (cd {repo} && ./hp6.sh clean && ./hp6.sh build)")
# build_dir is recorded as an absolute path at build time. A build made in a
# container (CI's esp-idf-ci-action mounts the checkout elsewhere) records a
# path that does not exist here, so fall back to the directory we were given.
bdir = pathlib.Path(d["build_dir"])
if not bdir.is_dir():
    bdir = repo / build
cfg = bdir / "config" / "sdkconfig.json"
k = json.loads(cfg.read_text()) if cfg.is_file() else {}
if not k.get("HB_PROFILE_HP6"):
    sys.exit(f"{repo}/{build} is not the HealthyPi 6 profile (CONFIG_HB_PROFILE_HP6 "
             f"unset in {cfg}) -- wrong profile. Rebuild:  (cd {repo} && ./hp6.sh "
             "clean && ./hp6.sh build)")
if not k.get("HB_OTA_RECEIVER"):
    sys.exit(f"{repo}/{build} has no firmware-update receiver (CONFIG_HB_OTA_RECEIVER "
             "unset): a unit given this image could never be updated over USB again. "
             "Use a healthybridge-esp32 with the HP6 OTA receiver.")
img = bdir / d["app_bin"]
if not img.is_file():
    sys.exit(f"{img} is missing. {fix}")
want = (repo / "version.txt").read_text().strip() if (repo / "version.txt").is_file() else ""
built = d.get("project_version", "")
if want and built != want:
    sys.exit(f"{img} is stale: built as {built}, but {repo}/version.txt says {want}. {fix}")
print(img)
