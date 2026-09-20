#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 OpenIRD contributors
# ベンダ rootfs 像に「root シェルへの入口」だけを足して、同じ形式で梱包し直す。
#
#   使い方: tools/vendor-rootfs/mkshell.sh <in.img> <out.img> [--telnetd]
#   前提:   Linux/WSL・fakeroot・scripts/fetch-squashfs3.sh でビルドした mksquashfs
#
# 何のためか: 段階 B の観察環境を、ベンダ純正との差が最小の状態で用意する。差が
# 大きいほどベンダアプリが落ちる要因が増えて観察にならない（SB-TV02 での教訓）。
# 既定の改変は **/etc/inittab のシリアル getty 行のコメントを外すだけ**。ベンダの
# rootfs には getty 行が最初からコメントアウトで入っており、root はパスワード空。
#
# 注意: 出力はベンダ由来の解析素材で、配布物ではない。commit しないこと
#       （CONTRIBUTING.md §0）。所有者が自分の機器のダンプから自分で作る。
set -eu

usage() { sed -n '4,9p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-1}"; }
case "${1:-}" in -h|--help) usage 0 ;; "") usage ;; esac
[ $# -ge 2 ] || usage

IN=$(readlink -f "$1"); OUT=$(readlink -f "$2"); shift 2
TELNETD=0
for a in "$@"; do
	case "$a" in
		--telnetd) TELNETD=1 ;;
		*) echo "✗ 未知の引数: $a"; exit 1 ;;
	esac
done

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
MKSQ="$ROOT/output/host/squashfs3/mksquashfs"
[ -x "$MKSQ" ] || { echo "✗ $MKSQ がありません。先に scripts/fetch-squashfs3.sh を実行してください。"; exit 1; }
command -v fakeroot >/dev/null || { echo "✗ fakeroot がありません（apt install fakeroot）"; exit 1; }
command -v unsquashfs >/dev/null || { echo "✗ unsquashfs がありません（apt install squashfs-tools）"; exit 1; }
[ -f "$IN" ] || { echo "✗ 入力がありません: $IN"; exit 1; }

# 元イメージの形式を読み取って、同じ形で梱包し直せるようにする
INFO=$(unsquashfs -s "$IN" 2>&1)
echo "$INFO" | grep -q "big endian" || { echo "✗ BE の squashfs ではありません（この機種の像ではない？）"; exit 1; }
BLK=$(echo "$INFO" | sed -n 's/^Block size \([0-9]*\)$/\1/p')
[ -n "$BLK" ] || { echo "✗ ブロックサイズを読めません"; exit 1; }
echo "元イメージ: BE squashfs 3.x / block $BLK / $(stat -c%s "$IN") B"

W=$(mktemp -d); trap 'rm -rf "$W"' EXIT

fakeroot -- bash -euc '
	W="$1"; IN="$2"; OUT="$3"; MKSQ="$4"; BLK="$5"; TELNETD="$6"

	unsquashfs -d "$W/root" "$IN" >"$W/unsq.log" 2>&1
	grep -E "created .* (devices|symlinks)" "$W/unsq.log" | sed "s/^/  /"

	IT="$W/root/etc/inittab"
	[ -f "$IT" ] || { echo "✗ /etc/inittab がありません"; exit 1; }
	cp "$IT" "$W/inittab.orig"

	# シリアル getty のコメントを外す（行の先頭の # だけを取る）
	sed -i "s|^#\(ttyS[0-9]*::respawn:/sbin/getty\)|\1|" "$IT"
	if ! grep -qE "^ttyS[0-9]*::respawn:/sbin/getty" "$IT"; then
		echo "✗ シリアル getty のコメント行が見つかりません（inittab の形が想定と違う）"; exit 1
	fi
	if [ "$TELNETD" = "1" ]; then
		grep -q "telnetd" "$IT" || printf "\n# OpenIRD: observation entry point\nnull::respawn:/usr/sbin/telnetd -F\n" >> "$IT"
	fi

	echo "--- /etc/inittab の差分 ---"
	diff -u "$W/inittab.orig" "$IT" | tail -n +3 | sed "s/^/  /" || true

	rm -f "$OUT"
	"$MKSQ" "$W/root" "$OUT" -be -b "$BLK" -noappend >"$W/mk.log" 2>&1 || { tail -20 "$W/mk.log"; exit 1; }
	grep -E "^Creating|^Filesystem size" "$W/mk.log" | sed "s/^/  /"
' _ "$W" "$IN" "$OUT" "$MKSQ" "$BLK" "$TELNETD"

# 生成物の検証: メタデータが元と一致し、違うのは inittab だけであること
echo "--- 検証 ---"
meta() { unsquashfs -ll "$1" 2>/dev/null | awk '/^[-dlcb]/{print $1,$2,$3,$NF}' | sed 's#squashfs-root##' | sort; }
count() { unsquashfs -ll "$1" 2>/dev/null | grep -c "^$2"; }
for t in "c:キャラクタデバイス" "b:ブロックデバイス" "l:シンボリックリンク"; do
	k=${t%%:*}; n=${t#*:}
	a=$(count "$IN" "$k"); b=$(count "$OUT" "$k")
	[ "$a" = "$b" ] && echo "  ✓ $n $a 個が保たれている" || { echo "  ✗ $n が $a → $b"; exit 1; }
done
if diff <(meta "$IN") <(meta "$OUT") | grep -vE "inittab" | grep -q "^[<>]"; then
	echo "  ✗ inittab 以外のメタデータに差があります:"
	diff <(meta "$IN") <(meta "$OUT") | grep -vE "inittab" | head -10
	exit 1
fi
echo "  ✓ inittab 以外は mode/owner/size/name まで一致"
echo "✓ $OUT ($(stat -c%s "$OUT") B)"
