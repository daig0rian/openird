#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# 段階 B の観測 rig のイメージを、所有者のダンプから再現ビルドする。
#
#   scripts/build-vendor-rig.sh [--rootfs IMG] [--slot0 IMG] [--out DIR]
#                               [--work DIR] [--verify]
#
# 作るもの（docs/specs/sb-tv02-wfpl-vendor-boot.md §6）:
#   rig-rootfs.sqfs  mtd6 へ。ベンダ rootfs の /etc/init.d/rcS だけ差し替え
#                    （slot_selector を通さない＝firstaid を守る）
#   rig-slot0.sqfs   mtd7 へ。slot_entry.sh の末尾からベンダアプリ起動を除去し、
#                    rig-daemon.sh と fapex_trace.so を同梱
#
# 方針:
#   - ベンダバイナリは 1 バイトも改変しない。触るのはシェルスクリプト 2 本だけ。
#   - root 権限を要求しない。ベンダ rootfs の squashfs は静的デバイスノードを 451 個
#     持つので、非 root の unsquashfs で落ちた分を mksquashfs の pseudo-file で戻す。
#   - 非 root 展開で umask に削られたモードも、マニフェストから chmod で戻す。
#   - 2.6.31 の squashfs4 が読める設定に固定（gzip / 128KB / xattr 無し）。
#
# ダンプは git に入らない（所有者の解析対象）。--rootfs/--slot0 で場所を渡す。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RIG="$ROOT/tools/vendor-rig"
ROOTFS_IMG="$ROOT/temp/parts/mtd6_rootfs.img"
SLOT0_IMG="$ROOT/temp/parts/mtd7_slot0.img"
OUT="$ROOT/temp/rig"
WORK="${HOME}/openird-rig"
VERIFY=0

while [ $# -gt 0 ]; do
  case "$1" in
    --rootfs) ROOTFS_IMG="$2"; shift 2 ;;
    --slot0)  SLOT0_IMG="$2";  shift 2 ;;
    --out)    OUT="$2";        shift 2 ;;
    --work)   WORK="$2";       shift 2 ;;
    --verify) VERIFY=1;        shift ;;
    -h|--help) sed -n '2,25p' "$0"; exit 0 ;;
    *) echo "✗ 不明な引数: $1"; exit 1 ;;
  esac
done

for t in unsquashfs mksquashfs; do
  command -v "$t" >/dev/null || { echo "✗ $t が無い（apt install squashfs-tools）"; exit 1; }
done
for f in "$ROOTFS_IMG" "$SLOT0_IMG"; do
  [ -f "$f" ] || { echo "✗ ダンプが無い: $f（--rootfs/--slot0 で指定する）"; exit 1; }
done
# DrvFs（/mnt/c）では chmod と特殊ファイルが期待どおりに効かない。ext4 で作業する。
case "$WORK" in /mnt/*) echo "⚠ 作業領域が DrvFs。--work で ext4 上を指定することを推奨" ;; esac
mkdir -p "$WORK" "$OUT"

# shellcheck source=lib/vendor-sqfs4.sh
. "$ROOT/scripts/lib/vendor-sqfs4.sh"

echo "=== 展開とマニフェスト ==="
prepare rootfs "$ROOTFS_IMG"
prepare slot0  "$SLOT0_IMG"

if [ "$VERIFY" -eq 1 ]; then
  echo "=== 往復テスト（無改変で再梱包して元と一致するか）==="
  for n in rootfs slot0; do
    pack "$WORK/$n.src" "$WORK/$n.rt.sqfs" "$WORK/$n.pseudo"
    unsquashfs -lls "$WORK/$n.rt.sqfs" > "$WORK/$n.rt.lls" 2>/dev/null
    norm "$WORK/$n.lls" > "$WORK/$n.a"; norm "$WORK/$n.rt.lls" > "$WORK/$n.b"
    d=$(diff "$WORK/$n.a" "$WORK/$n.b" | grep -c '^[<>]' || true)
    if [ "$d" -eq 0 ]; then echo "  ✓ $n 完全一致"; else echo "  ✗ $n 差分 $d 行"; diff "$WORK/$n.a" "$WORK/$n.b" | head -10; exit 1; fi
  done
fi

echo "=== rootfs (mtd6): rcS を差し替え ==="
rm -rf "$WORK/rootfs.rig"; cp -a "$WORK/rootfs.src" "$WORK/rootfs.rig"
m=$(stat -c%a "$WORK/rootfs.rig/etc/init.d/rcS")
cp "$RIG/rcS" "$WORK/rootfs.rig/etc/init.d/rcS"
chmod "$m" "$WORK/rootfs.rig/etc/init.d/rcS"   # 元と同じモードを保つ
pack "$WORK/rootfs.rig" "$OUT/rig-rootfs.sqfs" "$WORK/rootfs.pseudo"

echo "=== slot0 (mtd7): アプリ起動を除去し、rig のツールを同梱 ==="
rm -rf "$WORK/slot0.rig"; cp -a "$WORK/slot0.src" "$WORK/slot0.rig"
orig="$WORK/slot0.src/slot_entry.sh"; new="$WORK/slot0.rig/slot_entry.sh"
m=$(stat -c%a "$orig")
awk '/^insmod \/slot\/drv\/h57fmb\.ko$/ {found=1; exit} {print}
     END {if (!found) exit 1}' "$orig" > "$new" \
  || { echo "✗ slot_entry.sh に切断点（行頭 insmod h57fmb.ko）が無い"; exit 1; }
cat "$RIG/slot-entry-tail.sh" >> "$new"
chmod "$m" "$new"
cp "$RIG/rig-daemon.sh" "$WORK/slot0.rig/rig-daemon.sh"; chmod 755 "$WORK/slot0.rig/rig-daemon.sh"
# 観察ツールを同梱する。tmpfs に毎回 wget で入れ直す手間が消え、ネットワークが
# デーモンに奪われている状態でも使える。
for t in fapex-trace/fapex_trace.so fapex-ts/fapex-ts fapex-i2c/fapex-i2c \
         fapex-gpio/fapex-gpio fapex-poke/fapex-poke; do
  src="$ROOT/tools/$t"
  if [ -f "$src" ]; then
    cp "$src" "$WORK/slot0.rig/$(basename "$t")"
    chmod 755 "$WORK/slot0.rig/$(basename "$t")"
    echo "   同梱: $(basename "$t") ($(stat -c%s "$src") bytes)"
  else
    echo "  ⚠ $t が無い。同梱せずに続行（scripts/build-$(basename "$(dirname "$t")").sh）"
  fi
done
pack "$WORK/slot0.rig" "$OUT/rig-slot0.sqfs" "$WORK/slot0.pseudo"

echo "=== 元イメージとの差分（意図した変更だけであること）==="
for n in rootfs slot0; do
  unsquashfs -lls "$OUT/rig-$n.sqfs" > "$WORK/rig-$n.lls" 2>/dev/null
  norm "$WORK/$n.lls" > "$WORK/$n.a"; norm "$WORK/rig-$n.lls" > "$WORK/$n.c"
  echo "--- $n ---"
  d=$( { diff "$WORK/$n.a" "$WORK/$n.c" || true; } | { grep '^[<>]' || true; } )
  if [ -n "$d" ]; then printf '%s\n' "$d"; else echo "  (差分なし)"; fi
done

echo "=== 区画に収まるか ==="
check() { local f="$1" lim="$2" nm="$3"; local s; s=$(stat -c%s "$f")
  if [ "$s" -gt "$lim" ]; then echo "  ✗ $nm: $s > $lim"; exit 1; fi
  echo "  ✓ $nm: $s bytes（上限 $lim・余裕 $((lim-s))）"; }
check "$OUT/rig-rootfs.sqfs" 5242880 "rig-rootfs.sqfs -> mtd6"
check "$OUT/rig-slot0.sqfs"  7340032 "rig-slot0.sqfs  -> mtd7"

echo "=== 成果物 ==="
( cd "$OUT" && sha256sum rig-rootfs.sqfs rig-slot0.sqfs )
cat <<'EOF'

書き込み手順（docs/specs/sb-tv02-wfpl-vendor-boot.md §6）:
  1. libooter で cmdline の root= だけを /dev/mtdblock8 にして saveparam → 電源再投入
     （firstaid が起動する。root= 以外は 1 文字も変えない＝mac_adrs= を守る）
  2. firstaid で:  mtd write rig-rootfs.sqfs rootfs
                   mtd verify rig-rootfs.sqfs rootfs
                   mtd write rig-slot0.sqfs  rootfs_data
                   mtd verify rig-slot0.sqfs rootfs_data
  3. cmdline の root= を /dev/mtdblock6 に戻して saveparam → 電源再投入
  mtd5（自前カーネル）と mtd8（firstaid）は書かない。
EOF
