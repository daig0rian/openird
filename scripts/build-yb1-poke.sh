#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Build tools/yb1-poke/yb1-poke with the pinned MIPS cross-gcc.
#   scripts/build-yb1-poke.sh [BOARD]   (BOARD default: emma3sep_softbank_poe-br100-yb1)
# Fetches the toolchain pinned in the board's kernel build.env into dl/ if it is
# not there yet.  The binary is static and links against no libc (see
# tools/yb1-poke/Makefile), so it runs on the vendor rootfs as-is and nothing
# else is needed.  Output: tools/yb1-poke/yb1-poke.
set -eu

BOARD="${1:-emma3sep_softbank_poe-br100-yb1}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

matches=( "$ROOT"/br-ext-chip-*/configs/"$BOARD"_defconfig )
[ -e "${matches[0]}" ] || { echo "✗ 未知の BOARD: $BOARD"; exit 1; }
VT="$(dirname "$(dirname "${matches[0]}")")"
KDIRSRC="$VT/board/${BOARD%%_*}/kernel"
# shellcheck disable=SC1090,SC1091
. "$KDIRSRC/build.env"

DL="$ROOT/dl"; mkdir -p "$DL/toolchain"
CROSS="$DL/toolchain/$CROSS_COMPILE_SUBPATH"

verify() { echo "$1  $2" | sha256sum -c - >/dev/null 2>&1; }

if [ ! -x "${CROSS}gcc" ]; then
	T="$DL/$TOOLCHAIN_TARBALL"
	if ! { [ -f "$T" ] && verify "$TOOLCHAIN_SHA256" "$T"; }; then
		echo "  downloading $TOOLCHAIN_TARBALL ..."
		wget -q -O "$T" "$TOOLCHAIN_URL" || curl -sL -o "$T" "$TOOLCHAIN_URL"
		verify "$TOOLCHAIN_SHA256" "$T" || { echo "✗ sha256 不一致: $T"; exit 1; }
	fi
	# tarball の最上位は gcc-<ver>-nolibc/<target>/… なので strip せずに展開する
	tar xJf "$T" -C "$DL/toolchain"
	[ -x "${CROSS}gcc" ] || { echo "✗ 展開しても ${CROSS}gcc が無い"; exit 1; }
fi

# 4.9.4 の cross-gcc は libmpfr.so.4 (MPFR 3.x) を要する。build-kernel.sh と同じ置き場。
export LD_LIBRARY_PATH="$DL/toolchain/hostlibs:${LD_LIBRARY_PATH:-}"
"${CROSS}gcc" --version >/dev/null 2>&1 || {
	echo "✗ ${CROSS}gcc が動かない（libmpfr.so.4 が dl/toolchain/hostlibs に要る）"; exit 1; }

make -C "$ROOT/tools/yb1-poke" clean
make -C "$ROOT/tools/yb1-poke" CROSS="$CROSS"

bin="$ROOT/tools/yb1-poke/yb1-poke"
echo
echo "✓ $bin ($(stat -c%s "$bin") B)"
"${CROSS}readelf" -h "$bin" | grep -E 'Machine|Data|Flags|Type'

# 静的・libc 非依存であることを機械で確かめる（ベンダ rootfs にそのまま置くため）
"${CROSS}readelf" -l "$bin" | grep -q 'interpreter' \
	&& { echo "✗ 動的リンクになっている（interpreter がある）"; exit 1; }
und=$("${CROSS}nm" --undefined-only "$bin" 2>/dev/null | awk '{print $NF}' | grep -v '^$' || true)
[ -z "$und" ] || { echo "✗ 未定義シンボルがある:"; echo "$und" | sed 's/^/  /'; exit 1; }
echo "  ✓ 静的・未定義シンボル無し"
