#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Build tools/fapex-poke/fapex-poke with the Buildroot target (musl) toolchain.
#   scripts/build-fapex-poke.sh [BOARD]   (default: mb86h61_softbank_sb-tv02-wfpl)
# Output: tools/fapex-poke/fapex-poke (static ARM).
set -eu

BOARD="${1:-mb86h61_softbank_sb-tv02-wfpl}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

CROSS=""
for d in "$ROOT/output/$BOARD/host/bin" "$HOME/openird-fa/output/$BOARD/host/bin"; do
	if [ -x "$d/arm-linux-gcc" ]; then CROSS="$d/arm-linux-"; break; fi
done
[ -n "$CROSS" ] || { echo "x Buildroot toolchain missing; run make BOARD=$BOARD first"; exit 1; }

make -C "$ROOT/tools/fapex-poke" clean
make -C "$ROOT/tools/fapex-poke" CROSS="$CROSS" all

bin="$ROOT/tools/fapex-poke/fapex-poke"
echo
echo "OK $bin"
"${CROSS}readelf" -h "$bin" | grep -E 'Machine|Class|Type' || true
