# /lib/upgrade/common.sh - what sysupgrade and the boards' platform.sh share.
#
# Sourced twice: by /sbin/sysupgrade in the running system, and by stage2
# once it runs from RAM as PID 1.  Nothing here may assume which.
#
# A board supplies /lib/upgrade/platform.sh with two functions:
#
#   platform_check_image <dir>   is what is in <dir> right for this board?
#   platform_do_upgrade <dir>    write it (stage2: only RAM is in use)
#
# and lists in RAMFS_COPY_BIN the programs its platform_do_upgrade needs
# besides BusyBox.  (docs/sysupgrade.md)

# The generation of what a system keeps across an upgrade (the overlay).  An
# image carries the value of the tree it was built from; sysupgrade asks for
# -n when it differs from the running system's.  Raise it only when a newer
# base can no longer use an older overlay.
OPENIRD_COMPAT=1

# test hook: a directory that stands in for / (tests/sysupgrade)
SR="${SYSUPGRADE_ROOT:-}"

v() {
	echo "sysupgrade: $*"
}

# file_size <file>  (BusyBox stat has no -c unless it is built with it)
file_size() {
	echo $(( $(wc -c < "$1") ))
}

# file_magic <file> <n>: the first n bytes, as hex digits
file_magic() {
	dd if="$1" bs=1 count="$2" 2>/dev/null | od -An -tx1 | tr -d ' \n'
}

# mtd_size <mtdN> / mtd_erasesize <mtdN>: from /proc/mtd, in bytes (empty if
# there is no such partition)
mtd_field() {
	awk -v d="$1:" -v f="$2" '$1 == d { print $f }' "$SR/proc/mtd" 2>/dev/null
}
mtd_size() {
	local x
	x="$(mtd_field "$1" 2)"
	[ -n "$x" ] && echo $((0x$x))
}
mtd_erasesize() {
	local x
	x="$(mtd_field "$1" 3)"
	[ -n "$x" ] && echo $((0x$x))
}

# control_get <dir> <key>: one value of the image's CONTROL file
control_get() {
	sed -n "s/^$2=//p" "$1/CONTROL" | head -n 1
}

# indicate_upgrade: both LEDs orange, where the board has them
indicate_upgrade() {
	[ -f "$W/lib/ird-led" ] || return 0
	sh "$W/lib/ird-led" power orange
	sh "$W/lib/ird-led" status orange
}

# part_write <file> <mtdN>: write and read back.  Nothing is written in a
# dry run.
part_write() {
	if [ "$DRYRUN" = 1 ]; then
		v "dry run: not writing $(file_size "$1") bytes to $2"
		return 0
	fi
	mtd -q write "$1" "$SR/dev/$2" || return 1
	mtd -q verify "$1" "$SR/dev/$2" >/dev/null 2>&1
}

# upgrade_parts <file> <mtdN> [<file> <mtdN> ...]
#
# Writes each file to its partition, in the order given, and reads it back.
# What the partitions held is first copied to RAM (as many erase blocks as
# the new image covers).  If a partition cannot be made to verify, the ones
# written so far get their old contents back, so that what boots next is one
# version and not a mixture.
upgrade_parts() {
	local f m es n done_parts w

	# keep the old contents
	set -- "$@" --
	while [ "$1" != -- ]; do
		f="$1"; m="$2"; shift 2
		es="$(mtd_erasesize "$m")"
		[ -n "$es" ] && [ -f "$f" ] || { v "cannot write $f to $m"; return 1; }
		n=$(( ($(file_size "$f") + es - 1) / es ))
		v "keeping what $m holds ($n blocks)"
		dd if="$SR/dev/$m" of="$W/old.$m" bs="$es" count="$n" 2>/dev/null ||
			{ v "cannot keep a copy of what $m holds"; return 1; }
		set -- "$@" "$f" "$m"
	done
	shift

	done_parts=
	while [ $# -gt 0 ]; do
		f="$1"; m="$2"; shift 2
		v "writing $m"
		done_parts="$m $done_parts"
		if part_write "$f" "$m" || part_write "$f" "$m"; then
			continue
		fi
		v "$m does not verify - putting the previous version back"
		for w in $done_parts; do
			if mtd -q write "$W/old.$w" "$SR/dev/$w" &&
			   mtd -q verify "$W/old.$w" "$SR/dev/$w" >/dev/null 2>&1; then
				v "$w is back as it was"
			else
				v "COULD NOT RESTORE $w"
			fi
		done
		return 1
	done
	return 0
}

# wipe_overlay <upper>: empty the writable layer, so that the new version
# starts from its own defaults (sysupgrade -n).  Only ever below /overlay.
wipe_overlay() {
	case "$1" in
		/overlay|/overlay/*) ;;
		*) v "not wiping '$1': not below /overlay"; return 1 ;;
	esac
	case "$1" in
		*/..|*/../*|*/.|*/./*) v "not wiping '$1'"; return 1 ;;
	esac
	[ -d "$SR$1" ] || return 0
	mount -o remount,rw "$SR/overlay" 2>/dev/null
	# everything in it, the dot files too, but not the directory itself
	find "$SR$1" -mindepth 1 -maxdepth 1 -exec rm -rf {} \;
}
