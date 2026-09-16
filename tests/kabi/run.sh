#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Daigorian
# ベンダ製 .ko とのカーネル内部 ABI（構造体・共用体のレイアウト）互換テスト。
#   使い方: tests/kabi/run.sh <BOARD>
#   前提:   先に make kernel BOARD=<BOARD> を一度通す（dl/ に toolchain と GPL tarball が揃う）。
#   出力:   output/test-kabi/<BOARD>/（ref.layout new.layout report.txt）
# 「ベンダ出荷相当のツリー」（GPL tarball ＋ KABI_REF_PATCHES ＋ tarball 同梱の .config）と
# 「OpenIRD のツリー」（全パッチ ＋ linux.config）で同じ probe をデバッグ情報付きでコンパイルし、
# メンバの位置を比べる。既存メンバが動いたら失敗。詳細は README.md。
set -eu

usage() { sed -n '4,10p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-1}"; }
case "${1:-}" in -h|--help) usage 0 ;; "") usage ;; esac
BOARD=$1

T="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$T/../.." && pwd)"

die() { echo "✗ $*"; exit 1; }

matches=( "$ROOT"/br-ext-chip-*/configs/"$BOARD"_defconfig )
[ -e "${matches[0]}" ] || die "未知の BOARD: $BOARD"
VT="$(dirname "$(dirname "${matches[0]}")")"
KDIRSRC="$VT/board/${BOARD%%_*}/kernel"
[ -f "$KDIRSRC/build.env" ] || die "$KDIRSRC/build.env が無い"
# shellcheck disable=SC1090,SC1091
. "$KDIRSRC/build.env"
[ -n "${KABI_REF_CONFIG:-}" ] || die "build.env に KABI_REF_CONFIG / KABI_REF_PATCHES が無い（README 参照）"

command -v python3 >/dev/null || die "python3 が無い"
CROSS="$ROOT/dl/toolchain/$CROSS_COMPILE_SUBPATH"
[ -x "${CROSS}gcc" ] || die "toolchain が無い。先に make kernel BOARD=$BOARD を実行する"
export ARCH CROSS_COMPILE="$CROSS"
export LD_LIBRARY_PATH="$ROOT/dl/toolchain/hostlibs:${LD_LIBRARY_PATH:-}"
TB="$ROOT/dl/$KERNEL_TARBALL"
echo "$KERNEL_SHA256  $TB" | sha256sum -c - >/dev/null 2>&1 || die "GPL tarball が無いか sha256 不一致: $TB"

W="$ROOT/output/test-kabi/$BOARD"
rm -rf "$W"; mkdir -p "$W"
ex=""; for e in $KERNEL_EXTRACT_EXCLUDE; do ex="$ex --exclude=$e"; done

# build <name> <config（KERNEL_TOP からの相対 or 絶対）> <patch>...
build() {
  local name=$1 cfg=$2; shift 2
  local d="$W/$name" k
  echo "== $name =="
  mkdir -p "$d"
  # shellcheck disable=SC2086
  tar xjf "$TB" -C "$d" $ex || true
  k="$d/$KERNEL_TOP"
  [ -f "$k/Makefile" ] || die "展開失敗: $k"
  for p in "$@"; do echo "  apply $(basename "$p")"; patch -s -d "$k" -p1 < "$p"; done
  case "$cfg" in /*) cp "$cfg" "$k/.config" ;; *) [ "$cfg" = .config ] || cp "$k/$cfg" "$k/.config" ;; esac
  { yes "" | make -s -C "$k" oldconfig && make -s -C "$k" -j"$(nproc)" modules_prepare; } \
    >"$d/prepare.log" 2>&1 || { tail -30 "$d/prepare.log"; die "modules_prepare に失敗（$name）"; }
  cp -r "$T/probe" "$d/probe"
  # 必要なのはオブジェクトのデバッグ情報だけ。modpost の警告（Module.symvers 無し）は無視する
  make -s -C "$k" M="$d/probe" modules >"$d/probe.log" 2>&1 || true
  [ -f "$d/probe/kabi_probe.o" ] || { cat "$d/probe.log"; die "probe のコンパイルに失敗（$name）"; }
  "${CROSS}readelf" --debug-dump=info "$d/probe/kabi_probe.o" > "$d/probe.dwarf"
  python3 "$T/layout.py" dump "$d/probe.dwarf" > "$W/$name.layout"
  echo "  $(grep -c '^S' "$W/$name.layout") types"
}

ref_patches=(); for p in $KABI_REF_PATCHES; do ref_patches+=( "$KDIRSRC/patches/$p" ); done
build ref "$KABI_REF_CONFIG" "${ref_patches[@]}"
build new "$KDIRSRC/linux.config" "$KDIRSRC"/patches/*.patch

echo "== compare =="
rc=0
python3 "$T/layout.py" compare "$W/ref.layout" "$W/new.layout" "$T/allowed-additions.txt" >"$W/report.txt" || rc=$?
cat "$W/report.txt"
[ "$rc" = 0 ] || die "FAIL: 既存メンバが動いた／許可リストに無いメンバが増えた（$W/report.txt）"
echo "✓ PASS: 既存メンバの位置はベンダ出荷相当のツリーと一致（$W/report.txt）"
