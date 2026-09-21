#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# POE-BR100-YB1 のベンダカーネル向けにカーネルモジュールを作る。
#
#   scripts/build-yb1-kmod.sh <実機の .config> [BOARD]
#     <実機の .config> … 実機で `gzip -dc /proc/config.gz` した出力
#     BOARD 既定: emma3sep_softbank_poe-br100-yb1
#
# 出力: output/yb1-kmod/*.ko（i2c-algo-emma.ko ほか）
#
# なぜ実機から .config を取るのか: GPL ドロップに emma3sep の defconfig が無く
# （同梱は別チップの emma2rh_defconfig だけ）、vermagic と構造体配置を合わせるには
# 実機の設定そのものが要るため。ベンダ由来の内容なので repo には置かない
# （所有者が自分の機器から取る）。
#
# 前提: Linux/WSL の **ext4 上**で動かすこと（DrvFs では tar/patch/make が権限で失敗）。
#       GPL カーネル tarball を dl/ に置くこと（各自 Pixela の GPL 提供で入手）。
set -eu

CFG="${1:-}"
BOARD="${2:-emma3sep_softbank_poe-br100-yb1}"
[ -n "$CFG" ] && [ -f "$CFG" ] || { sed -n '3,9p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }
CFG=$(readlink -f "$CFG")

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
	tar xJf "$T" -C "$DL/toolchain"
fi
export LD_LIBRARY_PATH="$DL/toolchain/hostlibs:${LD_LIBRARY_PATH:-}"

KT="$DL/$KERNEL_TARBALL"
[ -f "$KT" ] || { echo "✗ $KT がありません。"; echo "  $KERNEL_SRC_NOTE"; exit 1; }
verify "$KERNEL_SHA256" "$KT" || { echo "✗ sha256 不一致: $KT"; exit 1; }

W="$ROOT/output/yb1-kmod"
K="$W/$KERNEL_TOP"
if [ ! -d "$K" ]; then
	echo "  展開 $KERNEL_TARBALL ..."
	mkdir -p "$W"; tar xjf "$KT" -C "$W"
	for p in "$KDIRSRC"/patches/*.patch; do
		echo "  適用 $(basename "$p")"
		( cd "$K" && patch -p1 -s --no-backup-if-mismatch < "$p" )
	done
fi

cp "$CFG" "$K/.config"
cd "$K"
echo "  oldconfig"
yes '' | make ARCH="$ARCH" CROSS_COMPILE="$CROSS" oldconfig > "$W/oldconfig.log" 2>&1

# gcc 4.9 は 2.6.27 の MIPS do_div（HI/LO 制約）を通せない。受信に無関係な
# モジュールだけが引っかかるので、それらを外して modules を通す。
# 外したものは output/yb1-kmod/disabled.txt に残す。
: > "$W/disabled.txt"
for _ in $(seq 1 15); do
	make -j"$(nproc)" ARCH="$ARCH" CROSS_COMPILE="$CROSS" modules > "$W/modules.log" 2>&1 && break
	objs=$(grep -oE '\[scripts/Makefile\.build:[0-9]+: [a-zA-Z0-9_./-]+\.o\]' "$W/modules.log" \
	       | sed -E 's/.*: ([^]]+)\]/\1/' | sort -u)
	[ -n "$objs" ] || { echo "✗ 失敗:"; grep -E 'error:' "$W/modules.log" | head -10; exit 1; }
	case "$objs" in *drivers/i2c*)
		echo "✗ i2c が落ちている（目的のもの）:"; grep -E 'error:' "$W/modules.log" | head -10; exit 1 ;;
	esac
	n=0
	for o in $objs; do
		d=$(dirname "$o"); f=$(basename "$o")
		[ -f "$d/Makefile" ] || continue
		c=$(grep -E "obj-\\\$\(CONFIG_[A-Z0-9_]+\).*[ /]$f" "$d/Makefile" \
		    | grep -oE 'CONFIG_[A-Z0-9_]+' | head -1)
		[ -n "$c" ] && grep -q "^$c=m$" .config || continue
		sed -i "s/^$c=m\$/# $c is not set/" .config
		echo "$c  $o" >> "$W/disabled.txt"; n=$((n+1))
	done
	[ "$n" -gt 0 ] || { echo "✗ 外せるものが無い"; grep -E 'error:' "$W/modules.log" | head -10; exit 1; }
done

mkdir -p "$W/out"
find drivers/i2c -name '*.ko' -exec cp {} "$W/out/" \;
echo
echo "✓ $W/out"
ls -l "$W/out"
echo "  受信に無関係で外したモジュール: $(wc -l < "$W/disabled.txt") 件（$W/disabled.txt）"
echo "  vermagic:"
for m in "$W/out"/*.ko; do printf '    %-20s ' "$(basename "$m")"; strings -a "$m" | grep -m1 '^vermagic'; done
