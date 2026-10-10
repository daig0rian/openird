#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# OpenIRD emma3sep: post-fakeroot ($1 = TARGET_DIR, inside Buildroot's fakeroot,
# after the device table has been applied - so the device nodes exist here).
#
# Makes the two images Buildroot has no generator for:
#   rescue.cpio  the initramfs that goes into the kernel image: BusyBox, mtd,
#                and an /init that mounts the real root or stays as a rescue
#                system (board/emma3sep/rescue/init)
#   rootfs.img   the root filesystem as squashfs 3.1 big-endian, the only
#                squashfs this board's 2.6.27 kernel reads
set -eu
TARGET_DIR="$1"
ROOT="$(cd "$BR2_EXTERNAL_IRD_RENESAS_PATH/.." && pwd)"
BOARD_DIR="$BR2_EXTERNAL_IRD_RENESAS_PATH/board/emma3sep"
MKSQ="$ROOT/output/host/squashfs3/mksquashfs"
mkdir -p "$BINARIES_DIR"

# --- rescue initramfs ---------------------------------------------------------
R="$BUILD_DIR/openird-rescue"
rm -rf "$R"
mkdir -p "$R/bin" "$R/sbin" "$R/usr/bin" "$R/usr/sbin" "$R/dev" "$R/proc" "$R/sys" \
	"$R/tmp" "$R/etc" "$R/flash" "$R/newroot" "$R/usr/share/udhcpc"
cp -a "$TARGET_DIR/dev/." "$R/dev/"
install -m 0755 "$TARGET_DIR/bin/busybox" "$R/bin/busybox"
install -m 0755 "$TARGET_DIR/sbin/mtd" "$R/sbin/mtd"
ln -s busybox "$R/bin/sh"
install -m 0755 "$BOARD_DIR/rescue/init" "$R/init"
install -m 0755 "$TARGET_DIR/usr/share/udhcpc/default.script" "$R/usr/share/udhcpc/default.script"
cp "$TARGET_DIR/etc/openird_release" "$R/etc/openird_release"
printf 'root:x:0:0:root:/:/bin/sh\n' > "$R/etc/passwd"
printf 'root:x:0:\n' > "$R/etc/group"
# fixed times and order: the archive ends up inside the kernel image
find "$R" -exec touch -h -d @0 {} +
( cd "$R" && find . | LC_ALL=C sort | cpio -o -H newc --quiet ) > "$BINARIES_DIR/rescue.cpio"
echo "  rescue.cpio: $(wc -c < "$BINARIES_DIR/rescue.cpio") bytes"

# --- root filesystem (squashfs 3.1 BE) -----------------------------------------
[ -x "$MKSQ" ] || "$ROOT/scripts/fetch-squashfs3.sh"
rm -f "$BINARIES_DIR/rootfs.img"
"$MKSQ" "$TARGET_DIR" "$BINARIES_DIR/rootfs.img" -be -noappend > "$BUILD_DIR/openird-mksquashfs.log" 2>&1 ||
	{ tail -20 "$BUILD_DIR/openird-mksquashfs.log"; exit 1; }
chmod 0644 "$BINARIES_DIR/rootfs.img"
