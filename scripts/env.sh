#!/usr/bin/env bash
# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT
# Shared environment for every HealthyPi 6 build/flash script.  SOURCE, don't run:
#
#     source scripts/env.sh
#
# Also sourced by scripts/build.sh, flash.sh and release.sh, so an interactive
# shell and a script always agree on the toolchain, the repo root and where the
# external HealthyBridge (ESP32) repo lives.

# --- repo root -------------------------------------------------------------
# Not $0: this file is sourced, so $0 is the caller. Not BASH_SOURCE alone
# either — that array does not exist in zsh, where it expands to empty, making
# dirname "" = "." and HPI_ROOT the PARENT of the repo (the west workspace). The
# docs tell people to `source scripts/env.sh` interactively and macOS ships zsh,
# so that path is taken routinely; it silently pointed HP6_SIGNING_KEY at a
# nonexistent keys/ dir, and `build.sh signed` would then GENERATE A NEW DEV KEY
# there — after which the board rejects every update signed with the real one
# (found on hardware). eval keeps zsh-only syntax away from bash's
# parser, which would otherwise choke on it while reading the branch.
if [ -n "${ZSH_VERSION:-}" ]; then
    eval '_hpi_self="${(%):-%x}"'
else
    _hpi_self="${BASH_SOURCE[0]:-$0}"
fi
HPI_ROOT="$(cd "$(dirname "$_hpi_self")/.." && pwd)"
unset _hpi_self

# Belt and braces: whatever the shell, refuse to export a root that is not this
# repo rather than let every derived path be quietly wrong.
if [ ! -f "$HPI_ROOT/app_m7/VERSION" ]; then
    echo "env.sh: HPI_ROOT=$HPI_ROOT is not the healthypi-6-fw repo" >&2
    echo "  (no app_m7/VERSION there). Source it by path from the repo:" >&2
    echo "    source scripts/env.sh" >&2
    return 1 2>/dev/null || exit 1
fi
export HPI_ROOT

# --- Zephyr venv -----------------------------------------------------------
# Everything (west, imgtool, the bundle tools) lives in it. Activating here
# means "forgetting the venv" stops being a failure mode.
if [ -z "${VIRTUAL_ENV:-}" ]; then
    for _venv in "${ZEPHYR_VENV:-}" "$HOME/zephyrproject/.venv" "$HPI_ROOT/../.venv"; do
        if [ -n "$_venv" ] && [ -f "$_venv/bin/activate" ]; then
            # shellcheck disable=SC1091
            . "$_venv/bin/activate"
            break
        fi
    done
    unset _venv
fi

if ! command -v west >/dev/null 2>&1; then
    echo "⚠️  west not found. Activate the Zephyr venv, or set ZEPHYR_VENV=/path/to/.venv" >&2
fi

# --- host tool packages ----------------------------------------------------
# tools/healthypi (device library + CLI) and tools/smpgroup (its MCUmgr group
# machinery) are imported by the updater, the MCUmgr suite and the M7 build
# itself, so a fresh clone has to have them importable. Editable, so edits under
# tools/ take effect without reinstalling.
#
# "Importable" is not enough: protocentral-healthypi is also on PyPI, and a venv
# holding that release (or an editable install of another checkout/worktree)
# imports fine, so release.sh would run a CLI that is not this tree's. So check
# WHERE each package resolves, and reinstall editable from here when it is not
# under $HPI_ROOT/tools/<pkg>/src. -I so a PYTHONPATH workaround cannot mask a
# stale venv. find_spec locates the package without
# executing it, which keeps the no-op path to one short python3 start-up.
if command -v python3 >/dev/null 2>&1 && [ -n "${VIRTUAL_ENV:-}" ]; then
    _hpi_stale="$(python3 -I - "$HPI_ROOT" <<'PY' 2>/dev/null
import importlib.util, os, sys
root = os.path.realpath(sys.argv[1])
bad = []
for pkg in ("smpgroup", "healthypi"):
    want = os.path.join(root, "tools", pkg, "src", pkg) + os.sep
    try:
        spec = importlib.util.find_spec(pkg)
    except Exception:
        spec = None
    origin = spec.origin if spec and spec.origin else ""
    if not origin or not os.path.realpath(origin).startswith(want):
        bad.append(f"{pkg} -> {origin or 'missing'}")
print("; ".join(bad))
PY
)" || _hpi_stale="python3 failed"
    if [ -n "$_hpi_stale" ]; then
        echo "env.sh: host tools not from this tree ($_hpi_stale); reinstalling editable from $HPI_ROOT/tools" >&2
        python3 -m pip install -q -e "$HPI_ROOT/tools/smpgroup" \
                                  -e "$HPI_ROOT/tools/healthypi" \
            || echo "⚠️  could not install tools/healthypi -- the updater and MCUmgr suite will not run" >&2
    fi
    unset _hpi_stale
fi

# --- build acceleration ----------------------------------------------------
export CCACHE_MAXSIZE=2G
if command -v ccache >/dev/null 2>&1; then
    export CMAKE_C_COMPILER_LAUNCHER=ccache
    export CMAKE_CXX_COMPILER_LAUNCHER=ccache
fi

# --- the one board this tree targets ---------------------------------------
# v5 is LDO-only (`power-supply = "ldo"`). A value that does not match the
# populated regulator path deadlocks the part at boot with no way back in, so
# the board is pinned here rather than passed per-invocation.
export HPI_BOARD_M7="healthypi6_v5/stm32h757xx/m7"
export HPI_BOARD_M4="healthypi6_v5/stm32h757xx/m4"
export HPI_BOARD_DIR="$HPI_ROOT/boards/protocentral/healthypi6_v5"

# --- signing key -----------------------------------------------------------
# Release builds set HP6_SIGNING_KEY to an air-gapped key. Absolute paths only:
# a relative Kconfig key path resolves against the west topdir, not this repo.
export HP6_SIGNING_KEY="${HP6_SIGNING_KEY:-$HPI_ROOT/keys/hp6_dev_ec256.pem}"

# --- external HealthyBridge (ESP32-C6) repo --------------------------------
# The C6 firmware is NOT in this repo. It lives in the dual-target HealthyBridge
# project, which serves HealthyPi 5 and 6 from one codebase. The former in-tree
# in-tree ESP32 app is superseded — its frame CRC and command-ID table both
# disagree with what the M7 driver now speaks, so flashing it looks like a
# hardware fault rather than a wrong image.
hpi_find_healthybridge() {
    local d
    for d in \
        "${HEALTHYBRIDGE_DIR:-}" \
        "$HPI_ROOT/../healthybridge-esp32" \
        "$HPI_ROOT/../../healthybridge-esp32" \
        "$HOME/Documents/GitHub/healthybridge-esp32" \
        "$HOME/GitHub/healthybridge-esp32" \
        "$HOME/healthybridge-esp32"; do
        if [ -n "$d" ] && [ -f "$d/hp6.sh" ]; then
            (cd "$d" && pwd)
            return 0
        fi
    done

    cat >&2 <<'EOF'
❌ HealthyBridge firmware repo not found.

   The ESP32-C6 firmware moved out of this repo into the dual-target
   HealthyBridge project. Clone it:

     git clone https://github.com/Protocentral/healthybridge-esp32

   Then place it beside this repo, or point at it:

     export HEALTHYBRIDGE_DIR=/path/to/healthybridge-esp32
EOF
    return 1
}
