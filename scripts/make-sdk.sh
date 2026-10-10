#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 OpenIRD contributors
# アドオン作者向けの SDK を固める（docs/addon.md §6）。`make sdk BOARD=<board>` が呼ぶ。
#
#   scripts/make-sdk.sh <BOARD>
#
# 前提: `make BOARD=<BOARD>` と Buildroot の `prepare-sdk` が済んでいること
#       （output/<BOARD>/host に relocate-sdk.sh がある。Makefile がこの順で呼ぶ）。
# 出力: output/<BOARD>/images/openird-sdk-<BOARD>-<ver>.tar.gz
#
# 中身は Buildroot の host ツリー（再配置できる cross toolchain と target の sysroot）に
#   sdk.env          IRD_ARCH / CROSS_COMPILE / CC / SYSROOT
#   bin/ird-mkaddon  staging ディレクトリ → .ird、リポジトリの索引
#   README.md
# を足したもの。host ツリーは汚さない（足す分は別の場所で作り、tar で重ねる）。
set -eu

BOARD="${1:?usage: make-sdk.sh <BOARD>}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
O="$ROOT/output/$BOARD"
HOST="$O/host"

[ -x "$HOST/relocate-sdk.sh" ] || {
	echo "✗ $HOST/relocate-sdk.sh が無い（make sdk BOARD=$BOARD から呼ぶこと）"; exit 1; }

# cross toolchain の tuple ＝ sysroot を持つディレクトリの名前（Buildroot の配置）
TUPLE=""
for d in "$HOST"/*/sysroot; do
	[ -d "$d" ] || continue
	[ -z "$TUPLE" ] || { echo "✗ sysroot が 2 つある: $HOST/$TUPLE と $d"; exit 1; }
	TUPLE="$(basename "$(dirname "$d")")"
done
[ -n "$TUPLE" ] && [ -x "$HOST/bin/$TUPLE-gcc" ] || {
	echo "✗ cross toolchain が見つからない（$HOST/<tuple>/sysroot と bin/<tuple>-gcc）"; exit 1; }

# Arch id は機器が名乗るのと同じ 1 か所から取る（像の board.conf）
CONF="$O/target/etc/openird/board.conf"
IRD_ARCH="$(sed -n 's/^ADDON_ARCH=//p' "$CONF" 2>/dev/null | head -n 1)"
[ -n "$IRD_ARCH" ] || {
	echo "✗ $CONF に ADDON_ARCH= が無い（この機種はアドオンの Arch id を決めていない）"; exit 1; }

VER="$(git -C "$ROOT" describe --always --dirty 2>/dev/null || echo unknown)"
NAME="openird-sdk-$BOARD-$VER"
OUT="$O/images/$NAME.tar.gz"

EXTRA="$(mktemp -d)"
trap 'rm -rf "$EXTRA"' EXIT
mkdir -p "$EXTRA/bin"
install -m 0755 "$ROOT/host/ird-mkaddon/ird_mkaddon.py" "$EXTRA/bin/ird-mkaddon"

cat > "$EXTRA/sdk.env" <<EOF
# OpenIRD SDK ($BOARD, $VER) - source this file:
#     . <SDK directory>/sdk.env
# bash and zsh find the SDK directory themselves; in another shell set
# OPENIRD_SDK=<SDK directory> first.  Run ./relocate-sdk.sh once after
# unpacking (or moving) the SDK.
if [ -z "\${OPENIRD_SDK:-}" ]; then
	if [ -n "\${BASH_SOURCE:-}" ]; then
		OPENIRD_SDK="\$(cd "\$(dirname "\$BASH_SOURCE")" && pwd)"
	elif [ -n "\${ZSH_VERSION:-}" ]; then
		eval 'OPENIRD_SDK="\${\${(%):-%x}:A:h}"'
	else
		echo "sdk.env: set OPENIRD_SDK=<SDK directory> before sourcing this file" >&2
		return 1
	fi
fi
export OPENIRD_SDK
# the "Arch" of an add-on built with this SDK; ird-mkaddon picks it up
export IRD_ARCH=$IRD_ARCH
export CROSS_COMPILE="\$OPENIRD_SDK/bin/$TUPLE-"
export CC="\${CROSS_COMPILE}gcc"
export SYSROOT="\$OPENIRD_SDK/$TUPLE/sysroot"
export PATH="\$OPENIRD_SDK/bin:\$PATH"
EOF

cat > "$EXTRA/README.md" <<EOF
# OpenIRD SDK — $BOARD ($VER)

OpenIRD の**アドオン**（実行中の機器に後から載せるソフト。\`.ird\`）を、OpenIRD や
Buildroot のツリー無しで作るための土台。形式と決まりは OpenIRD の \`docs/addon.md\`。

| | |
|---|---|
| 機種 (BOARD) | \`$BOARD\` |
| Arch id（manifest の \`Arch\`） | \`$IRD_ARCH\` |
| cross toolchain | \`bin/$TUPLE-gcc\` ほか（musl） |
| sysroot（target の headers / libs） | \`$TUPLE/sysroot\` |

## 使い方

\`\`\`
tar xzf $NAME.tar.gz
cd $NAME
./relocate-sdk.sh          # 展開した場所に合わせる（場所を移したら、もう一度）
. ./sdk.env                # IRD_ARCH / CROSS_COMPILE / CC / SYSROOT と PATH
\`\`\`

ビルドする（アドオンは**自己完結**にする＝静的リンクか、必要な lib を同梱する）:

\`\`\`
\$CC -O2 -static -o hello hello.c
\`\`\`

staging ディレクトリを組んで \`.ird\` にする:

\`\`\`
hello/
  manifest                 Package: hello / Version: 1.0 / Description: ...
  root/usr/bin/hello       / に重ねるツリー（base に在るパスは上書きできない）
  postinstall              任意（展開後に機器の sh が走らせる）
  preremove                任意（削除の前に走らせる）

ird-mkaddon build hello -o repo     # → repo/hello_1.0_$IRD_ARCH.ird と、その SHA256
ird-mkaddon index repo              # → repo/Packages（索引）
\`\`\`

\`manifest\` に \`Arch\` を書かなければ、\`sdk.env\` の \`IRD_ARCH\` が入る。

\`repo/\` を HTTPS で配れば、それがアドオンリポジトリになる。機器の
\`/etc/openird/addons.conf\` にその URL を足して \`ird-addon update\` →
\`ird-addon install hello\`。1 本だけ入れるなら \`ird-addon install <url|file> <sha256>\`。

この SDK の中身（GCC・binutils・musl・Linux のヘッダほか）は、それぞれの上流のライセンスに
従う。この SDK で作ったアドオンのライセンスは、そのアドオンの中身が決める。
EOF

mkdir -p "$O/images"
echo "→ $NAME.tar.gz（host $(du -sh "$HOST" | cut -f1)）"
# 名前の先頭の "." を SDK のディレクトリ名に替える。S = symlink の指す先には適用しない
tar -czf "$OUT.tmp" --owner=0 --group=0 --numeric-owner \
	--transform="s#^\.#$NAME#S" \
	-C "$HOST" . -C "$EXTRA" .
mv "$OUT.tmp" "$OUT"

echo "✓ $OUT ($(stat -c%s "$OUT") B)"
echo "  Arch id: $IRD_ARCH   toolchain: $TUPLE"
echo "  sha256:  $(sha256sum "$OUT" | cut -d' ' -f1)"
