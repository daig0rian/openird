#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 OpenIRD contributors
# factory イメージを作る（docs/release.md）。`make factory BOARD=<board>` が呼ぶ。
#
#   scripts/make-factory.sh <BOARD> [--kernel <uImage>] [--rootfs <squashfs>]
#                           [--outdir <dir>] [--version <版>] [--payload-only]
#
# ビルド済みのカーネル（make kernel）と rootfs（make BOARD=…）を、純正の更新機構が受け取る
# コンテナに包む。中身の検査（uImage の形・squashfs の版・書き込み固定長・ブートローダの
# 展開余裕）と write.sh の生成は openird-mkimage pack-factory が行う。
#
#   出力: <outdir>/openird-<版>-<family>-<device>-factory.bin
#         --payload-only のときは …-factory-payload.zip（暗号層なし・鍵不要）
#
# 鍵と ZIP パスワードは openird-mkimage の決まりどおり、**ファイルのパス**で渡す
# （既定 ~/.openird/v1b/、または OPENIRD_V1B_KEY_FILE / OPENIRD_V1B_ZIPPW_FILE）。
# 値はこのスクリプトにも出力にも現れない。openird-mkimage が出す指紋の行（fp=…）も、
# 公開されるログに残さないために落とす。
#
# Python: 既定は uv（openird-mkimage の依存を解決する）、無ければ python3。
# PYTHON=<python> を渡すとそれを使う（依存は PYTHONPATH などで用意しておくこと。
# CI はハッシュで固定した pycryptodome をそうやって渡す）。
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
die() { echo "✗ $*" >&2; exit 1; }
usage() { sed -n '4,8p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

BOARD=""; KERNEL=""; ROOTFS=""; OUTDIR=""; VER=""; PAYLOAD=""
while [ $# -gt 0 ]; do
	case "$1" in
	--kernel) KERNEL="${2:?}"; shift 2 ;;
	--rootfs) ROOTFS="${2:?}"; shift 2 ;;
	--outdir) OUTDIR="${2:?}"; shift 2 ;;
	--version) VER="${2:?}"; shift 2 ;;
	--payload-only) PAYLOAD=1; shift ;;
	-h|--help) usage 0 ;;
	-*) die "知らないオプション: $1" ;;
	*) [ -z "$BOARD" ] || die "BOARD は 1 つだけ: $1"; BOARD="$1"; shift ;;
	esac
done
[ -n "$BOARD" ] || usage 1

matches=( "$ROOT"/br-ext-chip-*/configs/"$BOARD"_defconfig )
[ -e "${matches[0]}" ] || die "未知の BOARD: $BOARD（configs/${BOARD}_defconfig が無い）"
VT="$(dirname "$(dirname "${matches[0]}")")"
FAMILY="${BOARD%%_*}"
ENVF="$VT/board/$FAMILY/kernel/build.env"
[ -f "$ENVF" ] || die "$ENVF が無い"
# build.env は ARCH なども設定するので、要る値だけを取り出す
DEVICE="$( . "$ENVF"; echo "${MKIMAGE_BOARD:-}" )"
[ -n "$DEVICE" ] || die "$BOARD は openird-mkimage の board 名（build.env の MKIMAGE_BOARD=）を決めていない"

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
# コンテナのヘッダに入れる版数は 0〜255 の三つ組。版（YY.MM…）の先頭 2 つを使う。
# 機器は版数の大小を見ない（docs/specs/update-v1b-container.md）。
TRIPLE="$(printf '%s' "$VER" | sed -n 's/^\([0-9]\{1,3\}\)\.\([0-9]\{1,3\}\).*/\1.\2.0/p')"
[ -n "$TRIPLE" ] || TRIPLE=0.0.0

MKIMAGE="$ROOT/host/openird-mkimage/openird_mkimage.py"
if [ -n "${PYTHON:-}" ]; then
	RUN=( "$PYTHON" "$MKIMAGE" )
elif command -v uv >/dev/null 2>&1; then
	RUN=( uv run -q "$MKIMAGE" )
else
	RUN=( python3 "$MKIMAGE" )
fi

mkdir -p "$OUTDIR"
NAME="openird-$VER-$FAMILY-$DEVICE-factory"
if [ -n "$PAYLOAD" ]; then OUT="$OUTDIR/$NAME-payload.zip"; else OUT="$OUTDIR/$NAME.bin"; fi
rm -f "$OUT"

echo "→ $BOARD  版 $VER（ヘッダの版数 $TRIPLE）"
LOG="$(mktemp)"
trap 'rm -f "$LOG"' EXIT
rc=0
"${RUN[@]}" pack-factory "$DEVICE" "$OUT" --kernel "$KERNEL" --rootfs "$ROOTFS" \
	--fw-version "$TRIPLE" ${PAYLOAD:+--payload-only} >"$LOG" 2>&1 || rc=$?
grep -v 'fp=' "$LOG" || true
[ "$rc" -eq 0 ] && [ -s "$OUT" ] || die "pack-factory が失敗した（終了コード $rc）"

if [ -z "$PAYLOAD" ]; then
	# 鍵なしで読める範囲（ヘッダと本体の SHA-1）を、出したファイルで確かめる
	"${RUN[@]}" probe-factory "$OUT" >"$LOG" 2>&1 || { cat "$LOG"; die "できたコンテナのヘッダが仕様どおりでない"; }
	grep -E '族タグ|version|^  [✓✗]' "$LOG" || true
fi
echo "✓ $OUT"
echo "  $(wc -c < "$OUT") B  sha256 $(sha256sum < "$OUT" | cut -d' ' -f1)"
