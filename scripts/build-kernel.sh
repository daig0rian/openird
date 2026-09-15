#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# OpenIRD 再現ビルド: 検証済みのカーネル(mtd5 像)を clone から再現する。
#   使い方: scripts/build-kernel.sh <BOARD>    （Linux/WSL で実行）
#   例:      scripts/build-kernel.sh mb86h61_softbank_sb-tv02-wfpl
#
# pin は各機種の board/<family>/kernel/build.env。toolchain と host lib は公開URLから
# sha256 検証つきで自動取得。GPL カーネルソースは各自 dl/ に置く（sha256 で検証）。
# 出力: output/<BOARD>/mtd5.img（= openird-mkimage pack-kernel の RDHB 像）。
set -eu

BOARD="${1:-}"
[ -n "$BOARD" ] || { echo "usage: $0 <BOARD>  (例: mb86h61_softbank_sb-tv02-wfpl)"; exit 1; }

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DL="$ROOT/dl"; mkdir -p "$DL"
OUT="$ROOT/output/$BOARD"; mkdir -p "$OUT"

# --- BOARD → ベンダツリー / 機種ディレクトリ ---
matches=( "$ROOT"/br-ext-chip-*/configs/"$BOARD"_defconfig )
[ -e "${matches[0]}" ] || { echo "✗ 未知の BOARD: $BOARD（configs/${BOARD}_defconfig が無い）"; exit 1; }
VT="$(dirname "$(dirname "${matches[0]}")")"
FAMILY="${BOARD%%_*}"
KDIRSRC="$VT/board/$FAMILY/kernel"
[ -f "$KDIRSRC/build.env" ] || { echo "✗ $KDIRSRC/build.env が無い"; exit 1; }
# shellcheck disable=SC1090
. "$KDIRSRC/build.env"

verify() { echo "$1  $2" | sha256sum -c - >/dev/null 2>&1; }
fetch() {  # url sha dest
  if [ -f "$3" ] && verify "$2" "$3"; then echo "  cached: $(basename "$3")"; return 0; fi
  echo "  downloading $(basename "$3") ..."
  wget -q -O "$3" "$1"
  verify "$2" "$3" || { echo "✗ sha256 不一致: $3"; exit 1; }
}

echo "== [1/6] toolchain =="
fetch "$TOOLCHAIN_URL" "$TOOLCHAIN_SHA256" "$DL/$TOOLCHAIN_TARBALL"
TC="$DL/toolchain"; mkdir -p "$TC"
CROSS="$TC/$CROSS_COMPILE_SUBPATH"
# /mnt/c 等 timestamp 設定不可の FS でも tar の utime 警告で失敗しない（出力の有無で判定）
if [ ! -x "${CROSS}gcc" ]; then tar xf "$DL/$TOOLCHAIN_TARBALL" -C "$TC" || true; fi
[ -x "${CROSS}gcc" ] || { echo "✗ toolchain 展開失敗"; exit 1; }
export ARCH CROSS_COMPILE="$CROSS"
echo "  CC: $("${CROSS}gcc" --version | head -1)"

echo "== [2/6] host lib (libmpfr.so.4) =="
fetch "$HOSTLIB_URL" "$HOSTLIB_SHA256" "$DL/$HOSTLIB_DEB"
HL="$TC/hostlibs"; mkdir -p "$HL"
if ! ls "$HL"/libmpfr.so.4* >/dev/null 2>&1; then
  t="$(mktemp -d)"; ( cd "$t" && ar x "$DL/$HOSTLIB_DEB" && tar xf data.tar.* )
  cp -P "$t"/$HOSTLIB_GLOB "$HL"/; rm -rf "$t"
fi
export LD_LIBRARY_PATH="$HL:${LD_LIBRARY_PATH:-}"

echo "== [3/6] GPL kernel source =="
if [ ! -f "$DL/$KERNEL_TARBALL" ] || ! verify "$KERNEL_SHA256" "$DL/$KERNEL_TARBALL"; then
  echo "✗ GPL カーネルソースが必要: $KERNEL_TARBALL"
  echo "  sha256=$KERNEL_SHA256"
  echo "  $KERNEL_SRC_NOTE"
  echo "  → $DL/ に置いて再実行してください。"; exit 2
fi
echo "  ok: $KERNEL_TARBALL"

echo "== [4/6] extract (+ FAPex 除外) =="
KSRC="$OUT/$KERNEL_TOP"; rm -rf "$KSRC"
ex=""; for e in $KERNEL_EXTRACT_EXCLUDE; do ex="$ex --exclude=$e"; done
# shellcheck disable=SC2086
tar xjf "$DL/$KERNEL_TARBALL" -C "$OUT" $ex || true
[ -f "$KSRC/Makefile" ] || { echo "✗ kernel 展開失敗（$KSRC/Makefile 無し）"; exit 1; }

echo "== [5/6] patch + config + build ($KERNEL_IMAGE_TARGET) =="
for p in "$KDIRSRC"/patches/*.patch; do [ -e "$p" ] || continue; echo "  apply $(basename "$p")"; patch -s -d "$KSRC" -p1 < "$p"; done
cp "$KDIRSRC/linux.config" "$KSRC/.config"
yes "" | make -C "$KSRC" ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE" oldconfig >/dev/null
# 再現性のためビルド identity を固定（2.6.31 が尊重すれば banner も固定）
export KBUILD_BUILD_USER=openird KBUILD_BUILD_HOST=openird
export KBUILD_BUILD_TIMESTAMP="Thu Jan  1 00:00:00 UTC 1970"
make -C "$KSRC" ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE" -j"$(nproc)" "$KERNEL_IMAGE_TARGET"

echo "== [6/6] pack RDHB =="
python3 "$ROOT/host/openird-mkimage/openird_mkimage.py" pack-kernel "${MKIMAGE_BOARD:-$BOARD}" \
  "$KSRC/arch/$ARCH/boot/$KERNEL_IMAGE_TARGET" "$OUT/mtd5.img"
echo
echo "✓ DONE: $OUT/mtd5.img"
