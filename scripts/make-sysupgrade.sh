#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 OpenIRD contributors
# 更新イメージ（sysupgrade）を作る（docs/sysupgrade.md）。`make sysupgrade BOARD=<board>` が呼ぶ。
#
#   scripts/make-sysupgrade.sh <BOARD> [--kernel <uImage>] [--rootfs <squashfs>]
#                              [--outdir <dir>] [--version <版>]
#
# ビルド済みのカーネル（make kernel）と rootfs（make BOARD=…）を、動いている OpenIRD の
# `sysupgrade` が受け取る形（tar）にまとめる。中身の検査は factory と同じもの
# （openird-mkimage pack-sysupgrade）。鍵は要らない。
#
#   出力: <outdir>/openird-<版>-<family>-<device>-sysupgrade.bin
#
# Python: 既定は uv、無ければ python3。PYTHON=<python> を渡すとそれを使う。
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
die() { echo "✗ $*" >&2; exit 1; }
usage() { sed -n '4,7p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

BOARD=""; KERNEL=""; ROOTFS=""; OUTDIR=""; VER=""
while [ $# -gt 0 ]; do
	case "$1" in
	-h|--help) usage 0 ;;
	--kernel) KERNEL="${2:?}"; shift 2 ;;
	--rootfs) ROOTFS="${2:?}"; shift 2 ;;
	--outdir) OUTDIR="${2:?}"; shift 2 ;;
	--version) VER="${2:?}"; shift 2 ;;
	-*) die "知らないオプション: $1" ;;
	*) [ -z "$BOARD" ] || die "BOARD は 1 つだけ: $1"; BOARD="$1"; shift ;;
	esac
done
[ -n "$BOARD" ] || usage 1

matches=( "$ROOT"/br-ext-chip-*/configs/"$BOARD"_defconfig )
[ -e "${matches[0]}" ] || die "未知の BOARD: $BOARD（configs/${BOARD}_defconfig が無い）"
FAMILY="${BOARD%%_*}"
# openird-mkimage の board 名＝機種名から「<ファミリー>_<ベンダ>_」を除いたもの
# （board/<ファミリー>/post-image.sh と同じ決め方）。ファミリーの build.env の
# MKIMAGE_BOARD はカーネルの梱包（ファミリー共通の uImage の契約）にだけ使う
DEVICE="${BOARD#*_}"
DEVICE="${DEVICE#*_}"
[ -f "$ROOT/host/openird-mkimage/boards/$DEVICE.json" ] ||
	die "$BOARD の board 定義が無い: host/openird-mkimage/boards/$DEVICE.json"

O="$ROOT/output/$BOARD"
KERNEL="${KERNEL:-$O/vmlinux.img}"
ROOTFS="${ROOTFS:-$O/images/rootfs.img}"
OUTDIR="${OUTDIR:-$O/images}"
[ -f "$KERNEL" ] || die "カーネルが無い: $KERNEL（make kernel BOARD=$BOARD）"
[ -f "$ROOTFS" ] || die "rootfs が無い: $ROOTFS（make BOARD=$BOARD）"

if [ -z "$VER" ]; then
	VER="$(git -C "$ROOT" describe --tags --match 'v[0-9]*' 2>/dev/null || true)"
	VER="${VER#v}"
	[ -n "$VER" ] || VER=unknown
fi
# 設定を引き継げる世代は、機器に入る /lib/upgrade/common.sh と同じ値にする（出どころは 1 か所）
COMMON="$ROOT/br-ext-common/package/ird-base-files/files/lib/upgrade/common.sh"
COMPAT="$(sed -n 's/^OPENIRD_COMPAT=\([0-9][0-9]*\)$/\1/p' "$COMMON")"
[ -n "$COMPAT" ] || die "$COMMON に OPENIRD_COMPAT= が無い"

MKIMAGE="$ROOT/host/openird-mkimage/openird_mkimage.py"
if [ -n "${PYTHON:-}" ]; then
	RUN=( "$PYTHON" "$MKIMAGE" )
elif command -v uv >/dev/null 2>&1; then
	RUN=( uv run -q "$MKIMAGE" )
else
	RUN=( python3 "$MKIMAGE" )
fi

mkdir -p "$OUTDIR"
OUT="$OUTDIR/openird-$VER-$FAMILY-$DEVICE-sysupgrade.bin"
rm -f "$OUT"

echo "→ $BOARD  版 $VER（compat $COMPAT）"
"${RUN[@]}" pack-sysupgrade "$DEVICE" "$OUT" --kernel "$KERNEL" --rootfs "$ROOTFS" \
	--version "$VER" --compat "$COMPAT" || die "pack-sysupgrade が失敗した"
[ -s "$OUT" ] || die "出力が無い: $OUT"
"${RUN[@]}" probe-sysupgrade "$OUT" || die "できたイメージを読み直せない"
echo "✓ $OUT"
echo "  $(wc -c < "$OUT") B  sha256 $(sha256sum < "$OUT" | cut -d' ' -f1)"
