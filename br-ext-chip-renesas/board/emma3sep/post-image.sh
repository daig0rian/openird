#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# OpenIRD emma3sep: post-image ($1 = BINARIES_DIR)
# Checks rootfs.img against what the board can take (squashfs 3.1 BE, and the
# fixed length the vendor updater writes to mtd3 - anything longer is silently
# cut off there) and fails the build otherwise.
set -eu
BINARIES_DIR="$1"
ROOT="$(cd "$BR2_EXTERNAL_IRD_RENESAS_PATH/.." && pwd)"

python3 "$ROOT/host/openird-mkimage/openird_mkimage.py" pack-rootfs poe-br100-yb1 --mtd 3 \
	"$BINARIES_DIR/rootfs.img" "$BINARIES_DIR/openird-poe-br100-yb1-rootfs.img"
( cd "$BINARIES_DIR" && sha256sum openird-poe-br100-yb1-rootfs.img rescue.cpio > SHA256SUMS && cat SHA256SUMS )
