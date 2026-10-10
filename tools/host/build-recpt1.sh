#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# recpt1（stz2012 版）を機種の Buildroot musl toolchain で MIPS BE 静的に再現ビルドする。
#
#   tools/host/build-recpt1.sh [BOARD]
#       BOARD default: emma3sep_softbank_poe-br100-yb1
#
# 用途は「段階 C の受け入れ検証」だけ。OpenIRD は px4_drv 互換の chardev を契約と
# するので（CLAUDE.md §4.1）、未改造の本物 recpt1 がその chardev から録れることが
# 段階 C 完了の判定になる。recpt1 は上流の第三者 GPL で、OpenIRD 配布物には一切
# 含めない（この成果物は所有者が自機で検証に使う道具）。
#
# 前提:
#   - 先に rootfs をビルドして Buildroot の musl toolchain を用意しておくこと
#     （`make BOARD=<BOARD>`。output/<BOARD>/host/bin/*-linux-musl-gcc が要る）。
#   - clone した ext4 上で実行すること（/mnt/c=DrvFs は不可。docs/BUILDING.md）。
#   - git でネットに出られること（recpt1 を pin した commit で取得する）。
#
# recpt1 の ABI は driver/pt1_ioctl.h＝ドライバ（ird-tuner-core）の px4_drv 互換 ioctl
# と同一（SET_CHANNEL _IOW(0x8D,…)/START_REC/STOP_REC/GET_SIGNAL_STRENGTH）。
# libarib25 は使わない（スクランブル解除はスコープ外・CLAUDE.md §1-2）＝config.h で
# HAVE_LIBARIB25 を未定義にし、b25 を含まない 5 本だけを静的リンクする。
#
# 出力: dl/recpt1-build/recpt1（git 管理外）。実機へは nc/tftp 等で /tmp に置く:
#   実機  : nc -l -p 9000 > /tmp/recpt1
#   ホスト: cat recpt1 | nc <ip> 9000      （実機を listener にすると FW 不要）
#   実機  : chmod +x /tmp/recpt1
# 使い方: recpt1 --device /dev/irdvideo0 <ch> <sec> <file>
#         recpt1 --http 8888 --device /dev/irdvideo0   （YB1 を TS サーバにする）
set -eu

BOARD="${1:-emma3sep_softbank_poe-br100-yb1}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

# recpt1（stz2012）の pin。版を上げるときはここだけ変える。
RECPT1_REPO="https://github.com/stz2012/recpt1"
RECPT1_COMMIT="77c7bce2527e1df45cc8667ede753d09de7b3e6c"   # 2025-07-29

# --- Buildroot の musl cross-gcc を探す（rootfs を先にビルド済みのこと）---------
CC=""
for d in "$ROOT/output/$BOARD/host/bin" \
         "$HOME/openird-yb1"/*/output/"$BOARD"/host/bin \
         "$HOME/openird-$BOARD"/output/"$BOARD"/host/bin; do
	g=$(ls "$d"/*-linux-musl-gcc 2>/dev/null | head -1 || true)
	[ -n "$g" ] && { CC="$g"; break; }
done
[ -n "$CC" ] || {
	echo "✗ Buildroot の musl toolchain が無い（*-linux-musl-gcc）"
	echo "  先に rootfs をビルドすること: make BOARD=$BOARD"
	exit 1
}
PFX="${CC%gcc}"
echo "toolchain: $CC"
"$CC" -dumpmachine

# --- recpt1 を pin した commit で取得 ------------------------------------------
WORK="$ROOT/dl/recpt1-build"
SRC="$WORK/recpt1-src"
mkdir -p "$WORK"
if [ ! -d "$SRC/.git" ]; then
	rm -rf "$SRC"
	git clone "$RECPT1_REPO" "$SRC"
fi
git -C "$SRC" fetch --all --tags -q 2>/dev/null || true
git -C "$SRC" checkout -q "$RECPT1_COMMIT" || {
	echo "✗ pin した commit を checkout できない: $RECPT1_COMMIT"; exit 1; }
echo "recpt1 @ $(git -C "$SRC" log -1 --format='%h %ci')"

R="$SRC/recpt1"
D="$SRC/driver"
[ -f "$D/pt1_ioctl.h" ] || { echo "✗ driver/pt1_ioctl.h が無い"; exit 1; }

# ABI がドライバと一致しているかを機械で確かめる（ずれたら気付けるように）
echo "== ABI (driver/pt1_ioctl.h) =="
grep -E 'SET_CHANNEL|START_REC|STOP_REC|GET_SIGNAL_STRENGTH' "$D/pt1_ioctl.h" \
	| grep -q '0x8D' || { echo "✗ pt1_ioctl.h の magic が 0x8D でない"; exit 1; }
grep -E 'SET_CHANNEL|START_REC|STOP_REC|GET_SIGNAL_STRENGTH' "$D/pt1_ioctl.h"

# configure を回さずに済むよう、autoconf が作る 2 つだけ手で用意する
#   version.h : const char *version（recpt1core.c が include）
#   config.h  : HAVE_LIBARIB25 を未定義（b25 を外す）
echo "const char *version = \"$RECPT1_COMMIT (openird accept-test build)\";" > "$R/version.h"
printf '/* openird: cross build without libarib25 */\n#undef HAVE_LIBARIB25\n' > "$R/config.h"

# --- b25 を含まない 5 本を静的リンク（recpt1 の Makefile.in の OBJS と同じ）------
mkdir -p "$WORK"
echo "== compile (static, musl, MIPS BE) =="
"$CC" -O2 -Wall -D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64 \
	-I"$R" -I"$D" -static \
	-o "$WORK/recpt1" \
	"$R/recpt1.c" "$R/recpt1core.c" "$R/decoder.c" "$R/mkpath.c" "$R/tssplitter_lite.c" \
	-lpthread -lm

bin="$WORK/recpt1"
echo
echo "✓ $bin ($(stat -c%s "$bin") B)"
"${PFX}readelf" -h "$bin" | grep -E 'Class|Data|Machine|Type'

# ベンダ rootfs でも OpenIRD rootfs でもそのまま動くよう、静的リンクを機械で確かめる
"${PFX}readelf" -l "$bin" | grep -q 'interpreter' \
	&& { echo "✗ 動的リンクになっている（interpreter がある）"; exit 1; }
"${PFX}readelf" -h "$bin" | grep -q 'big endian' \
	|| { echo "✗ big-endian でない"; exit 1; }
echo "  ✓ 静的・big-endian MIPS"
