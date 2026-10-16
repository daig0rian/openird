#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# OpenIRD emma3sep: post-build ($1 = TARGET_DIR, $2 = the board name, from
# BR2_ROOTFS_POST_SCRIPT_ARGS in the defconfig)
# Bakes in the /dev symlinks that are normally made at boot (the repository
# carries no symlinks - Windows checkouts), the mount points the boot scripts
# use, and the release information.
set -eu
TARGET_DIR="$1"
BOARD="${2:?post-build: no board name (BR2_ROOTFS_POST_SCRIPT_ARGS in the defconfig)}"

ln -sfn /proc/self/fd   "$TARGET_DIR/dev/fd"
ln -sfn /proc/self/fd/0 "$TARGET_DIR/dev/stdin"
ln -sfn /proc/self/fd/1 "$TARGET_DIR/dev/stdout"
ln -sfn /proc/self/fd/2 "$TARGET_DIR/dev/stderr"

# The owner's own partitions, mounted read-only at run time (never shipped)
mkdir -p "$TARGET_DIR/vendor/app" "$TARGET_DIR/vendor/fw"

# telnetd is in the BusyBox binary for the rescue initramfs; the normal
# system must not start it
rm -f "$TARGET_DIR/etc/init.d/S50telnet"

rev="$(git -C "$BR2_EXTERNAL_IRD_RENESAS_PATH" describe --always --dirty 2>/dev/null || echo unknown)"
cat > "$TARGET_DIR/etc/openird_release" <<REL
OPENIRD_VARIANT=base
OPENIRD_BOARD=$BOARD
OPENIRD_BUILD=$rev
REL
