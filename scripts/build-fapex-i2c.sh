#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Build tools/fapex-i2c/fapex-i2c with the Buildroot target (musl) toolchain.
#   scripts/build-fapex-i2c.sh [BOARD]   (default: mb86h61_softbank_sb-tv02-wfpl)
# Output: tools/fapex-i2c/fapex-i2c (static ARM).
set -eu

BOARD="${1:-mb86h61_softbank_sb-tv02-wfpl}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

CROSS=""
for d in "$ROOT/output/$BOARD/host/bin" "$HOME/openird-fa/output/$BOARD/host/bin"; do
	if [ -x "$d/arm-linux-gcc" ]; then CROSS="$d/arm-linux-"; break; fi
done
[ -n "$CROSS" ] || { echo "x Buildroot toolchain missing; run make BOARD=$BOARD first"; exit 1; }

make -C "$ROOT/tools/fapex-i2c" clean
make -C "$ROOT/tools/fapex-i2c" CROSS="$CROSS" all

bin="$ROOT/tools/fapex-i2c/fapex-i2c"
echo
echo "OK $bin"
"${CROSS}readelf" -h "$bin" | grep -E 'Machine|Class|Type' || true
