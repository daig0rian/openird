#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 OpenIRD contributors
# pin した squashfs-tools 3.x を dl/ に取得・sha256 検証し、ホスト向けにビルドする。
#   使い方: scripts/fetch-squashfs3.sh        （Linux/WSL で実行）
#   出力:   output/host/squashfs3/{mksquashfs,unsquashfs}
#   要るもの: gcc / make / zlib のヘッダ（Debian 系なら zlib1g-dev）
# 既にビルド済みで pin と一致していれば何もしない。
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck disable=SC1091
. "$ROOT/scripts/squashfs3.env"
DL="$ROOT/dl"; mkdir -p "$DL"
OUT="$ROOT/output/host/squashfs3"
STAMP="$OUT/.openird-squashfs3-version"

if [ -f "$STAMP" ] && [ "$(cat "$STAMP")" = "$SQUASHFS3_VERSION" ] && [ -x "$OUT/mksquashfs" ]; then
  exit 0
fi

verify() { echo "$1  $2" | sha256sum -c - >/dev/null 2>&1; }
T="$DL/$SQUASHFS3_TARBALL"
if ! { [ -f "$T" ] && verify "$SQUASHFS3_SHA256" "$T"; }; then
  echo "  downloading $SQUASHFS3_TARBALL ..."
  wget -q -O "$T" "$SQUASHFS3_URL" || curl -sL -o "$T" "$SQUASHFS3_URL"
  verify "$SQUASHFS3_SHA256" "$T" || { echo "✗ sha256 不一致: $T"; exit 1; }
fi

echo "  building squashfs-tools $SQUASHFS3_VERSION (host)"
B="$ROOT/output/host/.squashfs3-build"
rm -rf "$B"; mkdir -p "$B"
tar xzf "$T" -C "$B" --strip-components=1

# 2008 年のコードを現代の gcc で通すための互換フラグ。上流ソースは書き換えない。
#   -std=gnu17            gcc 14+ の既定 C23 では K&R の空括弧 () が (void) になり
#                         signal() のハンドラ型と衝突する
#   -fcommon              gcc 10+ の既定 -fno-common だと共通シンボル(swap)が二重定義になる
#   -fgnu89-inline        C99 の inline 意味論だと inline 関数の実体が出ず未定義参照になる
#   -include sys/sysmacros.h  glibc 2.28 で major()/minor() が sys/types.h から移動した
#   -include sys/sysinfo.h    get_nprocs() の宣言（unsquashfs.c）
COMPAT="-std=gnu17 -fcommon -fgnu89-inline -include sys/sysmacros.h -include sys/sysinfo.h -w"
CF="-I. -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -D_GNU_SOURCE -O2 $COMPAT"

if ! make -C "$B/squashfs-tools" CFLAGS="$CF" >"$B/build.log" 2>&1; then
  echo "✗ ビルド失敗。末尾:"; tail -20 "$B/build.log"
  echo "  zlib のヘッダが無い場合は Debian 系なら: sudo apt install -y zlib1g-dev"
  exit 1
fi

mkdir -p "$OUT"
cp "$B/squashfs-tools/mksquashfs" "$B/squashfs-tools/unsquashfs" "$OUT/"
rm -rf "$B"
"$OUT/mksquashfs" 2>&1 | grep -q -- "-be" || { echo "✗ -be が無い＝想定外の版"; exit 1; }
echo "$SQUASHFS3_VERSION" > "$STAMP"
echo "  → $OUT/mksquashfs (3.1 BE が書ける)"
