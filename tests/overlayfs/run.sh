#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Daigorian
# overlayfs カーネルパッチの QEMU 機能テスト（copy-up / whiteout / 権限 / 永続性 など約 80 項目）。
#   使い方: tests/overlayfs/run.sh [-n 回数] [--debug] [--skip-patch GLOB]... [--no-build] <BOARD>
#   前提:   先に make kernel BOARD=<BOARD> を一度通す（dl/ に toolchain と GPL tarball が揃う）。
#   出力:   output/test-overlayfs/<BOARD>/（kernel/ images/ run-N.log diag-N.txt）
# 機種の board/<family>/kernel/{build.env,patches} をそのまま使い、同じカーネルツリーを
# QEMU で動く汎用ボード（targets/<ARCH>.env）向けにビルドして試す。詳細は README.md。
set -eu

usage() { sed -n '4,9p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-1}"; }

REPEAT=1; DEBUG=0; BUILD=1; SKIP=(); BOARD=
while [ $# -gt 0 ]; do
  case "$1" in
    -n) REPEAT=$2; shift 2 ;;
    --debug) DEBUG=1; shift ;;
    --skip-patch) SKIP+=("$2"); shift 2 ;;
    --no-build) BUILD=0; shift ;;
    -h|--help) usage 0 ;;
    -*) echo "✗ 不明なオプション: $1"; usage ;;
    *) BOARD=$1; shift ;;
  esac
done
[ -n "$BOARD" ] || usage

T="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$T/../.." && pwd)"
STALL_SEC=${STALL_SEC:-60}
RUN_TIMEOUT=${RUN_TIMEOUT:-900}

die() { echo "✗ $*"; exit 1; }

matches=( "$ROOT"/br-ext-chip-*/configs/"$BOARD"_defconfig )
[ -e "${matches[0]}" ] || die "未知の BOARD: $BOARD"
VT="$(dirname "$(dirname "${matches[0]}")")"
KDIRSRC="$VT/board/${BOARD%%_*}/kernel"
[ -f "$KDIRSRC/build.env" ] || die "$KDIRSRC/build.env が無い（先にカーネル再現ビルドを整備する）"
# shellcheck disable=SC1090,SC1091
. "$KDIRSRC/build.env"
[ -f "$T/targets/$ARCH.env" ] || die "QEMU ターゲット定義 targets/$ARCH.env が無い（README の「別の機種・カーネル」参照）"
# shellcheck disable=SC1090
. "$T/targets/$ARCH.env"

W="$ROOT/output/test-overlayfs/$BOARD"; K="$W/kernel"; I="$W/images"
mkdir -p "$W"

for c in "$QEMU_SYSTEM" mksquashfs unsquashfs mke2fs python3 curl sha256sum; do
  command -v "$c" >/dev/null || die "$c が無い（README の前提を参照）"
done
CROSS="$ROOT/dl/toolchain/$CROSS_COMPILE_SUBPATH"
[ -x "${CROSS}gcc" ] || die "toolchain が無い。先に make kernel BOARD=$BOARD を実行する"
export ARCH CROSS_COMPILE="$CROSS"
export LD_LIBRARY_PATH="$ROOT/dl/toolchain/hostlibs:${LD_LIBRARY_PATH:-}"

if [ "$BUILD" = 1 ]; then
  BB="$ROOT/dl/$BUSYBOX_FILE"
  if ! { [ -f "$BB" ] && echo "$BUSYBOX_SHA256  $BB" | sha256sum -c - >/dev/null 2>&1; }; then
    echo "== busybox 取得（テスト専用。配布物には入らない）=="
    curl -sfL -o "$BB" "$BUSYBOX_URL" || die "busybox の取得に失敗: $BUSYBOX_URL"
    echo "$BUSYBOX_SHA256  $BB" | sha256sum -c - >/dev/null 2>&1 || die "busybox の sha256 不一致"
  fi
  echo "$KERNEL_SHA256  $ROOT/dl/$KERNEL_TARBALL" | sha256sum -c - >/dev/null 2>&1 \
    || die "GPL tarball が無いか sha256 不一致: dl/$KERNEL_TARBALL"

  echo "== [1/4] カーネルソース展開 + 機種パッチ =="
  rm -rf "$K" "$W/x"; mkdir -p "$W/x"
  ex=(); for e in $KERNEL_EXTRACT_EXCLUDE; do ex+=("--exclude=$e"); done
  tar xjf "$ROOT/dl/$KERNEL_TARBALL" -C "$W/x" "${ex[@]}" || true
  [ -f "$W/x/$KERNEL_TOP/Makefile" ] || die "カーネル展開失敗"
  mv "$W/x/$KERNEL_TOP" "$K"; rm -rf "$W/x"
  for p in "$KDIRSRC"/patches/*.patch; do
    [ -e "$p" ] || continue
    b=$(basename "$p"); skip=0
    for g in ${SKIP[@]+"${SKIP[@]}"}; do case "$b" in $g) skip=1 ;; esac; done
    if [ "$skip" = 1 ]; then echo "  skip  $b"; continue; fi
    patch -s -d "$K" -p1 < "$p" || die "パッチ適用失敗: $b"
  done

  echo "== [2/4] テストイメージ（lower squashfs / ext2 / initramfs）=="
  "$T/mkimages.sh" "$I" "$BB" "$T/guest/init.sh"

  echo "== [3/4] QEMU 用カーネル設定（$KDEFCONFIG + kconfig/*.fragment）=="
  make -s -C "$K" "$KDEFCONFIG" >/dev/null 2>&1 || die "make $KDEFCONFIG 失敗"
  {
    sed "s|@INITRAMFS_LIST@|$I/initramfs.list|" "$T/kconfig/common.fragment"
    cat "$T/kconfig/$KCONFIG_TARGET_FRAGMENT"
    [ "$DEBUG" = 0 ] || cat "$T/kconfig/debug-locking.fragment"
  } >> "$K/.config"
  yes "" | make -C "$K" oldconfig >/dev/null 2>&1 || die "oldconfig 失敗"
  for s in OVERLAYFS_FS SQUASHFS BLK_DEV_LOOP MTD_MTDRAM MTD_BLOCK JFFS2_FS_XATTR EXT2_FS_XATTR TMPFS BLK_DEV_INITRD MAGIC_SYSRQ; do
    grep -q "^CONFIG_$s=y" "$K/.config" || die "CONFIG_$s が有効にならない（overlayfs パッチ未適用か依存不足）"
  done

  echo "== [4/4] ビルド（$KIMAGE）=="
  make -C "$K" -j"$(nproc)" "$KIMAGE" > "$W/kbuild.log" 2>&1 \
    || { tail -20 "$W/kbuild.log"; die "カーネルビルド失敗（$W/kbuild.log）"; }
  grep -E 'fs/overlayfs/.*(warning|error)' "$W/kbuild.log" || true
fi

KIMG="$K/$KIMAGE_PATH"
[ -f "$KIMG" ] || die "$KIMG が無い（--no-build の前に一度ビルドする）"

fail=0
for i in $(seq 1 "$REPEAT"); do
  raw="$W/run-$i.raw"; log="$W/run-$i.log"; sock="$W/qmp-$i.sock"
  rm -f "$raw" "$sock" "$W/diag-$i.txt"
  # shellcheck disable=SC2086
  "$QEMU_SYSTEM" -M "$QEMU_MACHINE" -m "$QEMU_MEM" $QEMU_EXTRA \
    -kernel "$KIMG" -append "console=$CONSOLE rdinit=/init" \
    -nographic -no-reboot -qmp "unix:$sock,server,nowait" < /dev/null > "$raw" 2>&1 &
  qp=$!
  last=-1; idle=0; start=$(date +%s); stalled=0
  while kill -0 "$qp" 2>/dev/null; do
    sleep 2
    sz=$(stat -c %s "$raw" 2>/dev/null || echo 0)
    if [ "$sz" != "$last" ]; then last=$sz; idle=0; else idle=$((idle + 2)); fi
    if [ "$idle" -ge "$STALL_SEC" ] || [ $(( $(date +%s) - start )) -ge "$RUN_TIMEOUT" ]; then
      stalled=1; break
    fi
  done
  if [ "$stalled" = 1 ]; then
    "$T/diag.sh" "$sock" "$K/System.map" "$K/.config" "$T/targets/$ARCH.env" > "$W/diag-$i.txt" 2>&1 || true
    kill "$qp" 2>/dev/null || true
  fi
  wait "$qp" 2>/dev/null || true
  tr -d '\r' < "$raw" > "$log"; rm -f "$raw" "$sock"

  summary=$(grep -a '^SUMMARY:' "$log" || true)
  splats=$(grep -aE 'WARNING: at|kernel BUG at|BUG: |Oops|INFO: task .* blocked|Kernel panic|possible recursive locking|possible circular locking|inconsistent lock state|sleeping function called|scheduling while atomic' "$log" \
           | grep -avE '^(PASS|FAIL|NOTE|DIAG):' || true)
  if [ "$stalled" = 1 ] || [ -z "$summary" ]; then
    echo "run $i: ✗ 完走せず（${STALL_SEC}s 出力なし / panic / ハング）。最後のテスト出力:"
    grep -aE '^(PASS|FAIL|=====)' "$log" | tail -3 | sed 's/^/    /'
    grep -a '^PC=' "$W/diag-$i.txt" 2>/dev/null | sed 's/^/  /'
    if grep -aq 'Show Blocked State' "$log"; then
      echo "  SysRq-w（ブロック中のタスク）:"
      sed -n '/Show Blocked State/,/Sched Debug Version/p' "$log" | grep -v 'Sched Debug Version' | head -60 | sed 's/^/    /'
    else
      echo "  __log_buf（コンソール初期化前の panic などはここに出る）:"
      sed -n '/^== __log_buf/,$p' "$W/diag-$i.txt" 2>/dev/null | awk '
        { l[NR] = $0 }
        !s && /kernel BUG at|BUG: |Oops|Unable to handle|Kernel panic|WARNING: at/ { s = NR }
        END { from = s ? s : (NR > 40 ? NR - 39 : 1); for (i = from; i <= NR && i < from + 60; i++) print l[i] }' \
        | sed 's/^/    /'
    fi
    echo "  詳細: $W/diag-$i.txt, $log"
    fail=1; continue
  fi
  echo "run $i: $summary"
  grep -a '^FAIL:' "$log" | sed 's/^/    /' || true
  if [ -n "$splats" ]; then echo "  カーネルの異常ログ:"; echo "$splats" | head -20 | sed 's/^/    /'; fail=1; fi
  case "$summary" in *" FAIL=0 "*) : ;; *) fail=1 ;; esac
done

if [ "$fail" = 0 ]; then echo "✓ 全 $REPEAT 回 PASS（$W）"; else echo "✗ 失敗あり（$W）"; exit 1; fi
