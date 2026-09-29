#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Build tools/h57cmd/h57cmd with the Buildroot target (musl) toolchain.
#   scripts/build-h57cmd.sh [BOARD]   (default: mb86h61_softbank_sb-tv02-wfpl)
# Output: tools/h57cmd/h57cmd (static ARM).
set -eu

BOARD="${1:-mb86h61_softbank_sb-tv02-wfpl}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

CROSS=""
for d in "$ROOT/output/$BOARD/host/bin" "$HOME/openird-fa/output/$BOARD/host/bin"; do
	if [ -x "$d/arm-linux-gcc" ]; then CROSS="$d/arm-linux-"; break; fi
done
[ -n "$CROSS" ] || { echo "x Buildroot toolchain missing; run make BOARD=$BOARD first"; exit 1; }

make -C "$ROOT/tools/h57cmd" clean
make -C "$ROOT/tools/h57cmd" CROSS="$CROSS"

bin="$ROOT/tools/h57cmd/h57cmd"
echo
echo "OK $bin"
"${CROSS}readelf" -h "$bin" | grep -E 'Machine|Class|Type' || true
