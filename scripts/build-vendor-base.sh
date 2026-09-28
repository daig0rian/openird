#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# SB-TV02-WFPL を「ベンダ純正＋telnet」の観察土台に戻すための像を、所有者の全ダンプから作る。
#
#   scripts/build-vendor-base.sh [--dump FILE] [--out DIR] [--work DIR]
#
# 作るもの（使い方は tools/vendor-base/README.md）:
#   base-rootfs.sqfs      ベンダ rootfs。/etc/init.d/rcS の「/slot/slot_entry.sh」の直前に
#                         tools/vendor-base/rcS-insert.sh を差し込んだだけ（ほかは無改変）
#   vendor-kernel.bin     ベンダのカーネル区画（mtd5・1.5MB）。ダンプのまま
#   xfer-rootfs.bin       移行用。OpenIRD 区画表の mtd6（5MB）へ書く分
#   xfer-rootfs_data.bin  移行用。OpenIRD 区画表の mtd7（7MB）へ書く分
#                         （OpenIRD の mtd8 = firstaid の 1MB は、ベンダ配置では slot1 の
#                          末尾の未使用域＝0xFF。erase するだけでよいので像は作らない）
#   expected.md5          書いた後に実機の md5sum と突き合わせる値（区画表 2 通り）
#
# 方針:
#   - ベンダの像はカーネル・slot0・slot1 ともダンプのバイトのまま。変えるのは rcS への
#     差し込みと、それに伴う rootfs の再梱包だけ。
#   - slot info（mtd2/mtd9）には slot0/slot1 の 6MB 全体の MD5 が入っている（下で検査）。
#     ∴ slot は 1 バイトも変えない。変えると slot_selector の整合検査に掛かる。
#   - root 権限は要らない（scripts/lib/vendor-sqfs4.sh）。ext4 上で実行すること。
#
# ダンプは git に入らない（所有者の解析対象）。出力も同じ扱いで commit しない。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BASE="$ROOT/tools/vendor-base"
DUMP="$ROOT/temp/sbtv02_fw.bin"
OUT="$ROOT/temp/vendor-base"
WORK="${HOME}/openird-vendor-base"

while [ $# -gt 0 ]; do
  case "$1" in
    --dump) DUMP="$2"; shift 2 ;;
    --out)  OUT="$2";  shift 2 ;;
    --work) WORK="$2"; shift 2 ;;
    -h|--help) sed -n '2,24p' "$0"; exit 0 ;;
    *) echo "✗ 不明な引数: $1"; exit 1 ;;
  esac
done

for t in unsquashfs mksquashfs md5sum dd; do
  command -v "$t" >/dev/null || { echo "✗ $t が無い"; exit 1; }
done
[ -f "$DUMP" ] || { echo "✗ ダンプが無い: $DUMP（--dump で指定する）"; exit 1; }
[ "$(stat -c%s "$DUMP")" -eq $((16 * 1024 * 1024)) ] || { echo "✗ ダンプが 16MB ではない: $DUMP"; exit 1; }
case "$WORK" in /mnt/*) echo "⚠ 作業領域が DrvFs。--work で ext4 上を指定することを推奨" ;; esac
rm -rf "$WORK"; mkdir -p "$WORK" "$OUT"

# shellcheck source=lib/vendor-sqfs4.sh
. "$ROOT/scripts/lib/vendor-sqfs4.sh"

# ベンダの区画表（GPL mb86h61_devkit.c）。OpenIRD の区画表は kernel/patches/0015。
MB=$((1024 * 1024))
K_OFF=$((0x140000)); K_SIZE=$((0x180000))    # kernel bslot
R_OFF=$((0x2C0000)); R_SIZE=$MB              # rootfs
S0_OFF=$((0x3C0000)); S1_OFF=$((0x9C0000)); S_SIZE=$((6 * MB))
SI0_OFF=$((0x110000)); SI1_OFF=$((0xFC0000)); SI_SIZE=$((0x10000))

slice() { dd if="$DUMP" bs=64K iflag=skip_bytes,count_bytes skip="$1" count="$2" status=none; }
ff() { head -c "$1" /dev/zero | tr '\0' '\377'; }
md5() { md5sum | cut -d' ' -f1; }
hex16() { od -An -tx1 -v | tr -d ' \n'; }

echo "=== ダンプの整合（slot info が slot の MD5 を持つこと）==="
slice $K_OFF $K_SIZE > "$WORK/vendor-kernel.bin"
slice $R_OFF $R_SIZE > "$WORK/rootfs.img"
slice $S0_OFF $S_SIZE > "$WORK/slot0.img"
slice $S1_OFF $S_SIZE > "$WORK/slot1.img"
[ "$(head -c 4 "$WORK/vendor-kernel.bin")" = "RDHB" ] || { echo "✗ mtd5 が RDHB で始まらない"; exit 1; }
m0=$(md5 < "$WORK/slot0.img"); m1=$(md5 < "$WORK/slot1.img")
for si in $SI0_OFF $SI1_OFF; do
  slice $si $SI_SIZE > "$WORK/si.img"
  [ "$(head -c 4 "$WORK/si.img")" = "TOLS" ] || { echo "✗ slot info が TOLS で始まらない"; exit 1; }
  f0=$(dd if="$WORK/si.img" bs=1 skip=8 count=16 status=none | hex16)
  f1=$(dd if="$WORK/si.img" bs=1 skip=28 count=16 status=none | hex16)
  [ "$f0" = "$m0" ] && [ "$f1" = "$m1" ] \
    || { echo "✗ slot info@$(printf %#x $si) の MD5 欄が slot と合わない（$f0/$f1 vs $m0/$m1）"; exit 1; }
done
echo "  ✓ slot info0/1 の MD5 欄 = md5(slot0) $m0 / md5(slot1) $m1"
# OpenIRD の firstaid（0xEC0000〜1MB）はベンダ配置では slot1 の末尾。そこが 0xFF なら
# 移行時は erase だけで slot1 がダンプと一致する
[ "$(slice $((0xEC0000)) $MB | md5)" = "$(ff $MB | md5)" ] \
  || { echo "✗ 0xEC0000〜 が 0xFF ではない（erase だけでは戻らない）"; exit 1; }
echo "  ✓ 0xEC0000〜0xFBFFFF（OpenIRD の firstaid）はダンプでは 0xFF"

echo "=== rootfs の展開と往復テスト ==="
prepare rootfs "$WORK/rootfs.img"
pack "$WORK/rootfs.src" "$WORK/rootfs.rt.sqfs" "$WORK/rootfs.pseudo"
unsquashfs -lls "$WORK/rootfs.rt.sqfs" > "$WORK/rootfs.rt.lls" 2>/dev/null
norm "$WORK/rootfs.lls" > "$WORK/a"; norm "$WORK/rootfs.rt.lls" > "$WORK/b"
if diff -q "$WORK/a" "$WORK/b" >/dev/null; then echo "  ✓ 無改変で再梱包するとマニフェストが完全一致"
else echo "  ✗ 往復で差分"; diff "$WORK/a" "$WORK/b" | head -10; exit 1; fi

echo "=== rcS に差し込む ==="
RCS="$WORK/rootfs.src/etc/init.d/rcS"
cp "$RCS" "$WORK/rcS.orig"
n=$(grep -c -x '/slot/slot_entry.sh' "$RCS" || true)
[ "$n" -eq 1 ] || { echo "✗ rcS に「/slot/slot_entry.sh」の行が 1 つではない（$n）"; exit 1; }
grep -q OpenIRD "$RCS" && { echo "✗ rcS はすでに改変済み"; exit 1; }
L=$(grep -n -x '/slot/slot_entry.sh' "$RCS" | cut -d: -f1)
m=$(stat -c%a "$RCS")
{ head -n $((L - 1)) "$WORK/rcS.orig"; cat "$BASE/rcS-insert.sh"; tail -n +"$L" "$WORK/rcS.orig"; } > "$RCS"
chmod "$m" "$RCS"
LC_ALL=C grep -q '[^[:print:][:space:]]' "$RCS" && { echo "✗ rcS に ASCII 以外が入った"; exit 1; }
diff -u "$WORK/rcS.orig" "$RCS" | tail -n +3 | sed 's/^/  /' || true

pack "$WORK/rootfs.src" "$OUT/base-rootfs.sqfs" "$WORK/rootfs.pseudo"
unsquashfs -lls "$OUT/base-rootfs.sqfs" > "$WORK/base.lls" 2>/dev/null
norm "$WORK/base.lls" > "$WORK/c"
# 違うのは rcS の行（サイズ）だけであること
d=$( { diff "$WORK/a" "$WORK/c" || true; } | grep '^[<>]' || true)
bad=$(printf '%s\n' "$d" | grep -v -e '^$' -e ' /etc/init.d/rcS$' || true)
[ -z "$bad" ] || { echo "✗ rcS 以外に差分がある:"; printf '%s\n' "$bad"; exit 1; }
[ "$(printf '%s\n' "$d" | grep -c 'rcS$')" -eq 2 ] || { echo "✗ rcS の差分が想定と違う"; exit 1; }
echo "  ✓ 元のマニフェストとの差は /etc/init.d/rcS だけ"
s=$(stat -c%s "$OUT/base-rootfs.sqfs")
[ "$s" -le $R_SIZE ] || { echo "✗ base-rootfs.sqfs $s > ベンダ rootfs 区画 $R_SIZE"; exit 1; }
echo "  ✓ base-rootfs.sqfs $s bytes（ベンダ rootfs 区画 1MB・余裕 $((R_SIZE - s))）"

echo "=== 書き込み用の像 ==="
cp "$WORK/vendor-kernel.bin" "$OUT/vendor-kernel.bin"
# ベンダ配置の 0x2C0000〜0xFBFFFF（rootfs 1MB + slot0 6MB + slot1 6MB）
{ cat "$OUT/base-rootfs.sqfs"; ff $((R_SIZE - s)); cat "$WORK/slot0.img" "$WORK/slot1.img"; } > "$WORK/region.bin"
[ "$(stat -c%s "$WORK/region.bin")" -eq $((13 * MB)) ] || { echo "✗ 領域の大きさが違う"; exit 1; }
head -c $((5 * MB)) "$WORK/region.bin" > "$OUT/xfer-rootfs.bin"
dd if="$WORK/region.bin" bs=64K iflag=skip_bytes,count_bytes skip=$((5 * MB)) count=$((7 * MB)) \
  status=none > "$OUT/xfer-rootfs_data.bin"
[ "$(tail -c $MB "$WORK/region.bin" | md5)" = "$(ff $MB | md5)" ] || { echo "✗ 末尾 1MB が 0xFF ではない"; exit 1; }

FF1M=$(ff $MB | md5)
{
  echo "# 移行中（OpenIRD カーネル＝OpenIRD 区画表。firstaid から書いた直後）"
  echo "openird mtd5 $(md5 < "$OUT/vendor-kernel.bin")"
  echo "openird mtd6 $(md5 < "$OUT/xfer-rootfs.bin")"
  echo "openird mtd7 $(md5 < "$OUT/xfer-rootfs_data.bin")"
  echo "openird mtd8 $FF1M"
  echo "# 移行後（ベンダカーネル＝ベンダ区画表）。mtd1 は cmdline なので対象外、"
  echo "# mtd3/mtd4（nvram）はベンダアプリが書き換えうるので参考値"
  echo "vendor mtd0 $(slice 0 $MB | md5)"
  echo "vendor mtd2 $(slice $SI0_OFF $SI_SIZE | md5)"
  echo "vendor mtd3 $(slice $((0x120000)) $SI_SIZE | md5)"
  echo "vendor mtd4 $(slice $((0x130000)) $SI_SIZE | md5)"
  echo "vendor mtd5 $(md5 < "$OUT/vendor-kernel.bin")"
  echo "vendor mtd6 $(head -c $MB "$WORK/region.bin" | md5)"
  echo "vendor mtd7 $m0"
  echo "vendor mtd8 $m1"
  echo "vendor mtd9 $(slice $SI1_OFF $SI_SIZE | md5)"
  echo "vendor mtd10 $(slice $((0xFD0000)) $((0x30000)) | md5)"
} > "$OUT/expected.md5"
( cd "$OUT" && md5sum vendor-kernel.bin xfer-rootfs.bin xfer-rootfs_data.bin base-rootfs.sqfs > files.md5 )
sed 's/^/  /' "$OUT/files.md5"
echo "  expected.md5:"; sed 's/^/    /' "$OUT/expected.md5"
echo
echo "出力: $OUT（手順は tools/vendor-base/README.md）"
