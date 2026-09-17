#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Build tools/fapex-trace/fapex_trace.so with the pinned kernel cross-gcc.
#   scripts/build-fapex-trace.sh [BOARD]   (BOARD default: mb86h61_softbank_sb-tv02-wfpl)
# Reuses the toolchain that scripts/build-kernel.sh fetched into dl/.  The .so
# links against no libc (see tools/fapex-trace/Makefile), so nothing else is
# needed.  Output: tools/fapex-trace/fapex_trace.so.
set -eu

BOARD="${1:-mb86h61_softbank_sb-tv02-wfpl}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

matches=( "$ROOT"/br-ext-chip-*/configs/"$BOARD"_defconfig )
[ -e "${matches[0]}" ] || { echo "✗ 未知の BOARD: $BOARD"; exit 1; }
VT="$(dirname "$(dirname "${matches[0]}")")"
KDIRSRC="$VT/board/${BOARD%%_*}/kernel"
# shellcheck disable=SC1090,SC1091
. "$KDIRSRC/build.env"

CROSS="$ROOT/dl/toolchain/$CROSS_COMPILE_SUBPATH"
[ -x "${CROSS}gcc" ] || { echo "✗ toolchain が無い。先に make kernel BOARD=$BOARD を実行する"; exit 1; }
export LD_LIBRARY_PATH="$ROOT/dl/toolchain/hostlibs:${LD_LIBRARY_PATH:-}"

make -C "$ROOT/tools/fapex-trace" clean
make -C "$ROOT/tools/fapex-trace" CROSS="$CROSS"

so="$ROOT/tools/fapex-trace/fapex_trace.so"
echo
echo "✓ $so"
"${CROSS}readelf" -dh "$so" | grep -E 'Machine|NEEDED|SONAME' || true
echo "undefined symbols (must be only __errno_location):"
"${CROSS}nm" -D --undefined-only "$so" || true
