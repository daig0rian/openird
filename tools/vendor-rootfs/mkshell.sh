#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 OpenIRD contributors
# ベンダ rootfs 像に「root シェルへの入口」だけを足して、同じ形式で梱包し直す。
#
#   使い方: tools/vendor-rootfs/mkshell.sh <in.img> <out.img> [--tty "ttyS0 ttyS1"] [--no-telnetd]
#   前提:   Linux/WSL・fakeroot・scripts/fetch-squashfs3.sh でビルドした mksquashfs
#
# 何のためか: 段階 B の観察環境を、ベンダ純正との差が最小の状態で用意する。差が
# 大きいほどベンダアプリが落ちる要因が増えて観察にならない（SB-TV02 での教訓）。
#
# **/etc/inittab を編集しても効かない。** ベンダの /etc/init.d/rcS は末尾が
#   while : ; do setsid cttyhack /bin/sh ; done
# の無限ループで、busybox init は sysinit アクションの完了を waitfor() で待つ。
# ∴ rcS は永久に戻らず、inittab の respawn 行（ベンダ自身の getty や syslogd も
# 含めて）は一つも実行されない。
#
# **さらに rcS は自分の末尾にも到達しない。** その手前の
#   setsid cttyhack /app/bin/setup.sh
# に & が無く、setup.sh は最後に ./bin/blur（メインアプリ）をフォアグラウンドで
# 起動して戻らないため。∴ ベンダ自身の while ループも死んだコードで、これも
# 実際には動いていない。**介入点は setup.sh を呼ぶ行の直前**＝rcS がまだ生きている
# 最後の地点。
#
# シリアルについて: 製品用の bootargs には console= が無く（u-boot 既定環境の `rom`）、
# **カーネルの出力は元から捨てられている**。実ポートは Linux から見て ttyS1 の
# はずだが（開発用 `rom_dev` が console=ttyS1,115200 を渡す）、確証を得るため
# 既定では **ttyS0 と ttyS1 の両方**に生存確認を出して getty を立てる。
# getty はカーネルコンソールとは無関係にポートを開くので console= が無くても入れる。
#
# 注意: 出力はベンダ由来の解析素材で、配布物ではない。commit しないこと
#       （CONTRIBUTING.md §0）。所有者が自分の機器のダンプから自分で作る。
set -eu

usage() { sed -n '4,9p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-1}"; }
case "${1:-}" in -h|--help) usage 0 ;; "") usage ;; esac
[ $# -ge 2 ] || usage

IN=$(readlink -f "$1"); OUT=$(readlink -f "$2"); shift 2
TELNETD=1                       # 既定で有効（シリアルが駄目でも入れる道を残す）
TTYS="ttyS0 ttyS1"              # 両方に出して、どちらが実ポートか実機で確かめる
while [ $# -gt 0 ]; do
	case "$1" in
		--no-telnetd) TELNETD=0 ;;
		--tty) shift; TTYS=${1:-}; [ -n "$TTYS" ] || { echo "✗ --tty に名前がありません"; exit 1; } ;;
		*) echo "✗ 未知の引数: $1"; exit 1 ;;
	esac
	shift
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
	W="$1"; IN="$2"; OUT="$3"; MKSQ="$4"; BLK="$5"; TELNETD="$6"; TTYS="$7"

	unsquashfs -d "$W/root" "$IN" >"$W/unsq.log" 2>&1
	grep -E "created .* (devices|symlinks)" "$W/unsq.log" | sed "s/^/  /"

	RCS="$W/root/etc/init.d/rcS"
	[ -f "$RCS" ] || { echo "✗ /etc/init.d/rcS がありません"; exit 1; }
	cp "$RCS" "$W/rcS.orig"
	grep -qE "cttyhack[[:space:]]+/app/bin/setup\.sh" "$RCS" || {
		echo "✗ rcS に setup.sh を呼ぶ行がありません（形が想定と違う）"; exit 1; }
	grep -q "OPENIRD" "$RCS" && { echo "✗ すでに改変済みです"; exit 1; }

	# 生存確認と入口を、rcS が setup.sh で止まる直前に挿し込む。
	# 出力は ASCII のみ（シリアルに Unicode を出さない）。
	{
		echo "# --- OpenIRD observation entry points (inserted) ---"
		echo "# Must come BEFORE setup.sh: it runs blur in the foreground and"
		echo "# never returns, so nothing after it in rcS ever executes."
		echo "# /etc/inittab is not usable either: busybox init waits for rcS."
		for t in $TTYS; do
			echo "stty -F /dev/$t 115200 2>/dev/null"
			echo "echo \"OPENIRD alive on $t\" > /dev/$t 2>/dev/null"
		done
		if [ "$TELNETD" = "1" ]; then echo "/usr/sbin/telnetd"; fi
		for t in $TTYS; do
			echo "while : ; do /sbin/getty -L 115200 $t ; sleep 2 ; done &"
		done
		echo "# --- end OpenIRD ---"
	} > "$W/block"

	# setup.sh を呼ぶ行の直前に挿し込む（head/tail で分割。awk は外側の
	# シングルクォートと噛み合わないのでここでは使わない）
	N=$(grep -n -m1 -E "cttyhack[[:space:]]+/app/bin/setup\.sh" "$W/rcS.orig" | cut -d: -f1)
	head -n $((N - 1)) "$W/rcS.orig" > "$RCS"
	cat "$W/block" >> "$RCS"
	tail -n +"$N" "$W/rcS.orig" >> "$RCS"
	chmod 755 "$RCS"

	for t in $TTYS; do
		[ -e "$W/root/dev/$t" ] || { echo "✗ /dev/$t がありません"; exit 1; }
		grep -qx "$t" "$W/root/etc/securetty" 2>/dev/null \
			|| echo "  ⚠ $t が /etc/securetty に無い＝root で login できない可能性"
	done
	[ "$TELNETD" = "0" ] || [ -e "$W/root/usr/sbin/telnetd" ] \
		|| { echo "✗ telnetd がありません"; exit 1; }
	grep -q "OPENIRD" "$RCS" || { echo "✗ 挿し込みに失敗しました"; exit 1; }

	echo "--- /etc/init.d/rcS の差分 ---"
	diff -u "$W/rcS.orig" "$RCS" | tail -n +3 | sed "s/^/  /" || true

	rm -f "$OUT"
	"$MKSQ" "$W/root" "$OUT" -be -b "$BLK" -noappend >"$W/mk.log" 2>&1 || { tail -20 "$W/mk.log"; exit 1; }
	grep -E "^Creating|^Filesystem size" "$W/mk.log" | sed "s/^/  /"
' _ "$W" "$IN" "$OUT" "$MKSQ" "$BLK" "$TELNETD" "$TTYS"

# 生成物の検証: メタデータが元と一致し、違うのは rcS だけであること
echo "--- 検証 ---"
meta() { unsquashfs -ll "$1" 2>/dev/null | awk '/^[-dlcb]/{print $1,$2,$3,$NF}' | sed 's#squashfs-root##' | sort; }
count() { unsquashfs -ll "$1" 2>/dev/null | grep -c "^$2"; }
for t in "c:キャラクタデバイス" "b:ブロックデバイス" "l:シンボリックリンク"; do
	k=${t%%:*}; n=${t#*:}
	a=$(count "$IN" "$k"); b=$(count "$OUT" "$k")
	[ "$a" = "$b" ] && echo "  ✓ $n $a 個が保たれている" || { echo "  ✗ $n が $a → $b"; exit 1; }
done
if diff <(meta "$IN") <(meta "$OUT") | grep -vE "rcS" | grep -q "^[<>]"; then
	echo "  ✗ rcS 以外のメタデータに差があります:"
	diff <(meta "$IN") <(meta "$OUT") | grep -vE "rcS" | head -10
	exit 1
fi
echo "  ✓ rcS 以外は mode/owner/size/name まで一致"
echo "✓ $OUT ($(stat -c%s "$OUT") B)"
