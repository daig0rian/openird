#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 OpenIRD contributors
#
# Runs on a device: the write / verify / roll-back logic of /lib/upgrade/common.sh
# and the overlay wipe, under the device's own BusyBox, against plain files in
# /tmp.  No flash is touched.  tests/sysupgrade/run.sh checks the same things on
# the build host, but with the host's shell and tools unless it is given a
# BusyBox; this is the check that counts when porting to a new board.
#
#   sh selftest-device.sh <common.sh> [<file A> <file B>]
#
# <file A> and <file B> stand in for the two images (any two files of some
# size; made up when not given).  Messages are ASCII only.

COMMON="${1:?usage: selftest-device.sh <common.sh> [<file A> <file B>]}"
T=/tmp/sysupgrade-selftest
rm -rf "$T"
mkdir -p "$T/root/dev" "$T/root/proc" "$T/work" "$T/bin" || exit 1
A="${2:-$T/a.img}"
B="${3:-$T/b.img}"
[ -f "$A" ] || dd if=/dev/urandom of="$A" bs=1024 count=300 2>/dev/null
[ -f "$B" ] || dd if=/dev/urandom of="$B" bs=1024 count=200 2>/dev/null

# stands in for the flash tool: plain files, and it can be told to spoil
# what it writes (FAKE_BAD: always, FAKE_ONCE: the first time) - but never
# the old contents being put back
cat > "$T/bin/mtd" <<'EOF'
#!/bin/sh
while [ "${1#-}" != "$1" ]; do shift; done
cmd="$1"; img="$2"; dev="$3"; name="${dev##*/}"
case "$cmd" in
	write)
		dd if="$img" of="$dev" conv=notrunc 2>/dev/null || exit 1
		case "${img##*/}" in old.*) exit 0 ;; esac
		bad=0
		case " ${FAKE_BAD:-} " in *" $name "*) bad=1 ;; esac
		case " ${FAKE_ONCE:-} " in
			*" $name "*) [ -e "/tmp/sysupgrade-selftest/once.$name" ] ||
				{ bad=1; : > "/tmp/sysupgrade-selftest/once.$name"; } ;;
		esac
		[ "$bad" = 0 ] || printf 'X' | dd of="$dev" bs=1 seek=7 conv=notrunc 2>/dev/null
		;;
	verify) cmp -s -n "$(( $(wc -c < "$img") ))" "$img" "$dev" ;;
	*) exit 2 ;;
esac
EOF
chmod +x "$T/bin/mtd"
PATH="$T/bin:$PATH"
export PATH

pass=0
fail=0
ok() { pass=$((pass + 1)); echo "  ok    $1"; }
ng() { fail=$((fail + 1)); echo "  FAIL  $1"; }

setup() {
	rm -f "$T"/once.* "$T"/work/old.*
	printf 'dev:    size   erasesize  name\nmtd2: 00480000 00020000 "k"\nmtd3: 00900000 00020000 "r"\n' > "$T/root/proc/mtd"
	dd if=/dev/urandom of="$T/root/dev/mtd2" bs=65536 count=16 2>/dev/null
	dd if=/dev/urandom of="$T/root/dev/mtd3" bs=65536 count=16 2>/dev/null
	cp "$T/root/dev/mtd2" "$T/mtd2.before"
	cp "$T/root/dev/mtd3" "$T/mtd3.before"
}
# run [VAR=value ...]: upgrade_parts B -> mtd3, A -> mtd2
run() {
	env "$@" SYSUPGRADE_ROOT="$T/root" sh -c ". '$COMMON'; W='$T/work'; DRYRUN=\${DRYRUN:-0}
		upgrade_parts '$B' mtd3 '$A' mtd2" > "$T/log" 2>&1
}
starts() { cmp -s -n "$(( $(wc -c < "$1") ))" "$1" "$2"; }
same() { cmp -s "$T/root/dev/mtd2" "$T/mtd2.before" && cmp -s "$T/root/dev/mtd3" "$T/mtd3.before"; }

setup
if run X=1 && starts "$B" "$T/root/dev/mtd3" && starts "$A" "$T/root/dev/mtd2"
then ok "both partitions are written and verify"; else ng "both partitions are written and verify"; cat "$T/log"; fi
if starts "$T/work/old.mtd3" "$T/mtd3.before"; then ok "the old contents were kept"; else ng "the old contents were kept"; fi

setup
if run DRYRUN=1 && same; then ok "a dry run writes nothing"; else ng "a dry run writes nothing"; cat "$T/log"; fi

setup
if run FAKE_ONCE=mtd3 && starts "$B" "$T/root/dev/mtd3" && starts "$A" "$T/root/dev/mtd2"
then ok "a write that does not verify is tried again"; else ng "a write that does not verify is tried again"; cat "$T/log"; fi

setup
if ! run FAKE_BAD=mtd3 && same; then ok "first partition bad: it is put back, the second is not touched"
else ng "first partition bad: it is put back, the second is not touched"; cat "$T/log"; fi

setup
if ! run FAKE_BAD=mtd2 && same; then ok "second partition bad: both are put back"
else ng "second partition bad: both are put back"; cat "$T/log"; fi

# the overlay wipe
wipe() { SYSUPGRADE_ROOT="$T/root" sh -c ". '$COMMON'; wipe_overlay '$1'" > "$T/log" 2>&1; }
mkdir -p "$T/root/overlay/a/upper/etc" "$T/root/overlay/a/other" "$T/root/etc"
: > "$T/root/overlay/a/upper/etc/x"; : > "$T/root/overlay/a/upper/.hidden"
: > "$T/root/overlay/a/other/keep"; : > "$T/root/etc/keep"
wipe /overlay/a/upper
if [ -d "$T/root/overlay/a/upper" ] && [ -z "$(ls -A "$T/root/overlay/a/upper")" ] && [ -f "$T/root/overlay/a/other/keep" ]
then ok "the overlay is emptied, dot files too, and nothing beside it"; else ng "the overlay is emptied, dot files too, and nothing beside it"; fi
if ! wipe /etc && ! wipe "" && ! wipe /overlay/../etc && [ -f "$T/root/etc/keep" ]
then ok "anything not below /overlay is refused"; else ng "anything not below /overlay is refused"; fi

rm -rf "$T"
echo "== $pass passed, $fail failed"
[ "$fail" -eq 0 ]
