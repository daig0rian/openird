#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# OpenIRD emma3sep: post-image ($1 = BINARIES_DIR, $2 = the board name, from
# BR2_ROOTFS_POST_SCRIPT_ARGS in the defconfig)
# Checks rootfs.img against what the board can take (squashfs 3.1 BE, and the
# fixed length the vendor updater writes to mtd3 - anything longer is silently
# cut off there) and fails the build otherwise.
set -eu
BINARIES_DIR="$1"
BOARD="${2:?post-image: no board name (BR2_ROOTFS_POST_SCRIPT_ARGS in the defconfig)}"
ROOT="$(cd "$BR2_EXTERNAL_IRD_RENESAS_PATH/.." && pwd)"
# the openird-mkimage board: the board name without "<family>_<vendor>_"
DEVICE="${BOARD#*_}"
DEVICE="${DEVICE#*_}"

python3 "$ROOT/host/openird-mkimage/openird_mkimage.py" pack-rootfs "$DEVICE" --mtd 3 \
	"$BINARIES_DIR/rootfs.img" "$BINARIES_DIR/openird-$DEVICE-rootfs.img"
( cd "$BINARIES_DIR" && sha256sum "openird-$DEVICE-rootfs.img" rescue.cpio > SHA256SUMS && cat SHA256SUMS )
