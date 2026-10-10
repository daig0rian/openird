#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Build tools/fapex-gpio/fapex-gpio with the Buildroot target (musl) toolchain.
#   scripts/build-fapex-gpio.sh [BOARD]   (default: mb86h61_softbank_sb-tv02-wfpl)
# The tool is a plain target userland program (not LD_PRELOAD'd), so it links
# against the Buildroot libc.  Output: tools/fapex-gpio/fapex-gpio (static ARM).
set -eu

BOARD="${1:-mb86h61_softbank_sb-tv02-wfpl}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

CROSS=""
for d in "$ROOT/output/$BOARD/host/bin" "$HOME/openird-fa/output/$BOARD/host/bin"; do
	if [ -x "$d/arm-linux-gcc" ]; then CROSS="$d/arm-linux-"; break; fi
done
[ -n "$CROSS" ] || { echo "✗ Buildroot toolchain が無い。先に make BOARD=$BOARD を実行する"; exit 1; }

make -C "$ROOT/tools/fapex-gpio" clean
make -C "$ROOT/tools/fapex-gpio" CROSS="$CROSS"

bin="$ROOT/tools/fapex-gpio/fapex-gpio"
echo
echo "✓ $bin"
"${CROSS}readelf" -h "$bin" | grep -E 'Machine|Class|Type' || true
