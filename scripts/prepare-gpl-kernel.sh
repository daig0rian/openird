#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 OpenIRD contributors
#
# GPL カーネルソースの「完全な参照ツリー」を ext4 上に展開する（読む・grep する用）。
#   使い方: scripts/prepare-gpl-kernel.sh [BOARD]   （Linux/WSL で実行）
#     BOARD 既定: emma3sep_softbank_poe-br100-yb1（EMMA3SEP 2.6.27.39・WRIP と同 SoC）
#   出力: 参照ツリーの絶対パスを最後に 1 行で表示（他スクリプト・人が使う）
#
# なぜ専用スクリプトか（＝土台の前提。コンテクストで失われやすいので明記）:
#   1. **DrvFs(/mnt/c=リポジトリの置き場)への tar 展開はこのカーネル tar で不完全になる**
#      （権限/utime で途中失敗。実例: 過去に temp/gpl/BMMPX1301/full が 22/26583 ファイルで
#      停止していた）。∴ 参照ツリーは **WSL の ext4（$HOME 配下）**に展開する。
#   2. ビルド用の展開は build-kernel.sh / build-yb1-kmod.sh が output/ に別途行う（パッチ適用）。
#      こちらは **vanilla（未パッチ）**の読み取り専用参照＝mainline の i2c-gpio.c 等をそのまま見る用。
#   3. tarball は build.env の KERNEL_SHA256 で pin。無ければ Pixela の GPL 公開から取得を試みる
#      （EMMA3SEP は /gpl/bmmpx1301/data/…）。ベンダ由来ソースなので repo には展開ツリーを置かない。
set -eu

BOARD="${1:-emma3sep_softbank_poe-br100-yb1}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

matches=( "$ROOT"/br-ext-chip-*/configs/"$BOARD"_defconfig )
[ -e "${matches[0]}" ] || { echo "✗ 未知の BOARD: $BOARD（configs/${BOARD}_defconfig が無い）" >&2; exit 1; }
VT="$(dirname "$(dirname "${matches[0]}")")"
FAMILY="${BOARD%%_*}"
KDIRSRC="$VT/board/$FAMILY/kernel"
[ -f "$KDIRSRC/build.env" ] || { echo "✗ $KDIRSRC/build.env が無い" >&2; exit 1; }
# shellcheck disable=SC1090
. "$KDIRSRC/build.env"

DL="$ROOT/dl"; mkdir -p "$DL"
KT="$DL/$KERNEL_TARBALL"
verify() { echo "$1  $2" | sha256sum -c - >/dev/null 2>&1; }

# 既知の公開 URL（EMMA3SEP = Pixela /gpl/bmmpx1301/）。他 family は各自 dl/ に置く。
KURL="${KERNEL_URL:-https://www.pixela.co.jp/gpl/bmmpx1301/data/$KERNEL_TARBALL}"

if [ ! -f "$KT" ] || ! verify "$KERNEL_SHA256" "$KT"; then
  echo "  downloading $KERNEL_TARBALL ..." >&2
  wget -q -O "$KT" "$KURL" || curl -sL -o "$KT" "$KURL" || true
  verify "$KERNEL_SHA256" "$KT" || {
    echo "✗ GPL カーネルソースが用意できない: $KT" >&2
    echo "  sha256=$KERNEL_SHA256" >&2
    echo "  ${KERNEL_SRC_NOTE:-各自 Pixela の GPL 公開から dl/ へ}" >&2
    exit 1; }
fi

# --- 展開先は ext4（$HOME 配下）。DrvFs には展開しない ---
REFROOT="${OPENIRD_GPL_REF:-$HOME/.openird/gpl/$FAMILY}"
K="$REFROOT/$KERNEL_TOP"
STAMP="$REFROOT/.openird-kernel-sha256"

case "$(realpath "$REFROOT" 2>/dev/null || echo "$REFROOT")" in
  /mnt/*) echo "⚠ 参照先が DrvFs($REFROOT)＝展開が不完全になる恐れ。OPENIRD_GPL_REF に ext4 を指定してください。" >&2 ;;
esac

if [ -d "$K" ] && [ -f "$STAMP" ] && [ "$(cat "$STAMP" 2>/dev/null)" = "$KERNEL_SHA256" ]; then
  n=$(find "$K" -type f | wc -l)
  echo "  cached: $K（$n ファイル）" >&2
else
  echo "  展開 $KERNEL_TARBALL → $REFROOT（ext4）..." >&2
  rm -rf "$REFROOT"; mkdir -p "$REFROOT"
  tar xjf "$KT" -C "$REFROOT"
  n=$(find "$K" -type f | wc -l)
  # 完全性の目安: r557 は 26,583 ファイル。1 万未満なら展開失敗を疑う。
  [ "$n" -ge 10000 ] || { echo "✗ 展開が不完全（$n ファイル）。ext4 で実行しているか確認。" >&2; exit 1; }
  echo "$KERNEL_SHA256" > "$STAMP"
  echo "  展開完了: $n ファイル" >&2
fi

# 参照ツリーの絶対パスを stdout に（他スクリプト・人が食う唯一の出力）
echo "$K"
