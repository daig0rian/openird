# /lib/upgrade/platform.sh - Renesas EMMA3SEP boards with the 128MB NAND layout
# (POE-BR100-YB1, POE-BR310L-SB1)
#
# The kernel is a uImage on mtd2, the root filesystem a squashfs on mtd3.
# The boot loader only loads what is there, so an upgrade is two partitions
# written from Linux; the limits are the boot loader's and the vendor
# layout's (docs/specs/poe-br100-yb1-nand-update.md):
#
#   kernel  0x280000   beyond this the boot loader unpacks the uImage over
#                      what it has not read yet, and the device does not boot
#   root    0x680000   what the partition layout reserves for the image
#
# The two go together (the kernel carries the tuner and board drivers the
# start-up scripts expect), so both are always written: root first, then the
# kernel, and if either cannot be made to verify, both go back to what they
# were (upgrade_parts).

RAMFS_COPY_BIN="mtd"

EMMA3SEP_KERNEL_MAX=2621440
EMMA3SEP_ROOT_MAX=6815744

platform_check_image() {
	local d="$1"

	[ -f "$d/kernel" ] && [ -f "$d/root" ] ||
		{ v "the image has no kernel and root"; return 1; }
	# the 128MB layout; the 64MB boards have other offsets and sizes
	[ "$(mtd_size mtd2)" = $((0x480000)) ] && [ "$(mtd_size mtd3)" = $((0x900000)) ] ||
		{ v "this flash layout is not the one the image is for"; return 1; }
	[ "$(file_magic "$d/kernel" 4)" = 27051956 ] ||
		{ v "kernel is not a uImage"; return 1; }
	[ "$(file_size "$d/kernel")" -le "$EMMA3SEP_KERNEL_MAX" ] ||
		{ v "kernel is larger than the boot loader can unpack"; return 1; }
	# "sqsh": squashfs as this big-endian kernel writes it
	[ "$(file_magic "$d/root" 4)" = 73717368 ] ||
		{ v "root is not a squashfs this kernel reads"; return 1; }
	[ "$(file_size "$d/root")" -le "$EMMA3SEP_ROOT_MAX" ] ||
		{ v "root is larger than its partition allows"; return 1; }
	return 0
}

platform_do_upgrade() {
	upgrade_parts "$1/root" mtd3 "$1/kernel" mtd2
}
