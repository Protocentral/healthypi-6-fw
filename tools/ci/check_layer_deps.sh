#!/usr/bin/env bash
# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT
#
# check_layer_deps.sh — enforce "the one rule" of the M7 layered architecture.
#
#   core/ and healthylink/ (L2/L3 producers) may include ONLY bus/, core/ and
#   HAL headers — NEVER services/, control/, transport/ or ui/. Producers publish
#   to the sample bus; they must not depend on their consumers. Route data through
#   the bus or a service API instead of adding an upward include.
#
# Fails (exit 1) and prints each offending include on any violation. Run before
# committing changes under app_m7/src/core or app_m7/src/healthylink, and in CI.
#
# Usage:  tools/ci/check_layer_deps.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC="$ROOT/app_m7/src"

GUARDED=(core healthylink)
FORBIDDEN='services|control|transport|ui'

rc=0
for dir in "${GUARDED[@]}"; do
  d="$SRC/$dir"
  [ -d "$d" ] || continue
  # Match an #include whose path's first or any segment is a forbidden layer,
  # e.g. "services/x.h", "../control/y.h", <transport/z.h>. Precise: the forbidden
  # name must be a full path segment (after the quote/bracket or a slash).
  hits="$(grep -rnE \
      '#[[:space:]]*include[[:space:]]*["<]([^">]*/)?('"$FORBIDDEN"')/' \
      "$d" --include='*.c' --include='*.h' 2>/dev/null || true)"
  if [ -n "$hits" ]; then
    echo "LAYER VIOLATION — $dir/ must not include ($FORBIDDEN):" >&2
    echo "$hits" | sed 's/^/  /' >&2
    rc=1
  fi
done

if [ "$rc" -eq 0 ]; then
  echo "layer check: OK — core/ and healthylink/ include no services/control/transport/ui."
fi
exit "$rc"
