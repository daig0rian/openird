#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# OpenIRD mb86h61: post-image（$1 = BINARIES_DIR）
# rootfs.squashfs を openird-mkimage で検査（squashfs 4.0・gzip・xattr 無し・mtd6 容量）し、
# 実機へ転送する名前で出力する。容量超過や非互換形式ならビルドを失敗させる。
set -eu
BINARIES_DIR="$1"
ROOT="$(cd "$BR2_EXTERNAL_IRD_FUJITSU_PATH/.." && pwd)"
OUT="$BINARIES_DIR/openird-sb-tv02-wfpl-rootfs.squashfs"

python3 "$ROOT/host/openird-mkimage/openird_mkimage.py" pack-rootfs sb-tv02-wfpl --mtd 6 \
	"$BINARIES_DIR/rootfs.squashfs" "$OUT"
( cd "$BINARIES_DIR" && sha256sum "$(basename "$OUT")" > "$(basename "$OUT").sha256" )
cat "$OUT.sha256"
