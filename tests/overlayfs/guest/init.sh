#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Daigorian
# QEMU ゲストの /init として走る overlayfs 機能テスト（busybox ash）。
# 出力: PASS:/FAIL:/NOTE:/DIAG: 行と最後の SUMMARY: 行。run.sh が集計する。
# mount 構文は v4 系（-t overlayfs, workdir 無し）。後の版に移すなら setup 節を合わせる。
export PATH=/bin:/sbin
/bin/busybox --install -s /bin
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mkdir -p /sys/kernel/debug; mount -t debugfs none /sys/kernel/debug 2>/dev/null
mknod /dev/loop0 b 7 0; mknod /dev/loop1 b 7 1
mknod /dev/mtd0 c 90 0; mknod /dev/mtdblock0 b 31 0

PASS=0; FAIL=0; NOTE=0
ok()   { PASS=$((PASS+1)); echo "PASS: $*"; }
ng()   { FAIL=$((FAIL+1)); echo "FAIL: $*"; }
note() { NOTE=$((NOTE+1)); echo "NOTE: $*"; }
# t NAME CMD... : 成功を期待
t()  { n=$1; shift; if sh -c "$*" >/tmp/o 2>&1; then ok "$n"; else ng "$n :: $* :: $(head -c 160 /tmp/o | tr '\n' ' ')"; fi; }
# tf NAME CMD... : 失敗を期待（エラーメッセージを記録）
tf() { n=$1; shift; if sh -c "$*" >/tmp/o 2>&1; then ng "$n (unexpectedly succeeded) :: $*"; else ok "$n [$(head -c 100 /tmp/o | tr '\n' ' ')]"; fi; }
# U: 一般ユーザ(uid 1000)で実行（t の sh -c 内からも呼べるようスクリプトにする）
printf '#!/bin/sh\nexec setuidgid user sh -c "$*"\n' > /bin/U; chmod 755 /bin/U

L=/mnt/lower; UPM=/mnt/upper; UP=$UPM/u; M=/mnt/merged
mkdir -p $L $UPM $M /mnt/tup /mnt/tm /mnt/eup /mnt/em

echo "===== setup ====="
uname -a
losetup /dev/loop0 /lower.sqfs && mount -t squashfs -o ro /dev/loop0 $L || ng "mount squashfs"
cat /proc/mtd
mount -t jffs2 /dev/mtdblock0 $UPM || ng "mount jffs2"
mkdir -p $UP
t  "S01 mount overlayfs (lower=squashfs, upper=jffs2)" "mount -t overlayfs overlayfs -o lowerdir=$L,upperdir=$UP $M"
t  "S02 appears in /proc/mounts" "grep -q ' $M overlayfs ' /proc/mounts"
ls -ln $M

echo "===== basic read / copy-up ====="
t  "B01 read lower file" "[ \"\$(cat $M/etc/config/network)\" = lower-network ]"
t  "B02 no copy-up on read" "[ ! -e $UP/etc/config/network ]"
t  "B03 owner/mode pass through (secret 0600 root)" "[ \"\$(stat -c %a:%u $M/etc/secret)\" = 600:0 ]"
t  "B04 append -> copy-up" "echo appended >> $M/etc/config/network"
t  "B05 merged content after append" "[ \"\$(cat $M/etc/config/network)\" = \"\$(printf 'lower-network\nappended')\" ]"
t  "B06 upper has copied file" "[ -f $UP/etc/config/network ]"
t  "B07 lower untouched" "[ \"\$(cat $L/etc/config/network)\" = lower-network ]"
t  "B08 O_TRUNC open -> copy-up+truncate" "echo new > $M/etc/trunc && [ \"\$(cat $M/etc/trunc)\" = new ]"
echo "DIAG lower big.bin: size=$(stat -c %s $L/big.bin) md5=$(md5sum $L/big.bin | cut -d' ' -f1) expected_lower=$(cat /expected_lower.md5)"
t  "B09a lower big.bin matches host (image consistency)" "[ \"\$(md5sum $L/big.bin | cut -d' ' -f1)\" = \"\$(cat /expected_lower.md5)\" ]"
t  "B09 2.5MB file copy-up integrity (>1 chunk)" "echo X >> $M/big.bin && [ \"\$(md5sum $M/big.bin | cut -d' ' -f1)\" = \"\$(cat /expected_big.md5)\" ]"
echo "DIAG merged big.bin: size=$(stat -c %s $M/big.bin) upper size=$(stat -c %s $UP/big.bin 2>&1)"
echo "DIAG head2500000 md5=$(head -c 2500000 $M/big.bin | md5sum | cut -d' ' -f1) tail=$(tail -c 2 $M/big.bin | od -An -c | tr -s ' ')"
echo "DIAG first diffs lower vs merged: $(cmp -l $L/big.bin $M/big.bin 2>&1 | head -3 | tr '\n' ';') count=$(cmp -l $L/big.bin $M/big.bin 2>/dev/null | wc -l)"
echo "DIAG date=$(date -u +%Y-%m-%dT%H:%M:%S) secret_mtime=$(stat -c %Y $L/etc/secret)"
t  "B10 create file" "echo n > $M/newfile && [ -f $UP/newfile ]"
t  "B11 mkdir" "mkdir $M/newdir && [ -d $UP/newdir ]"
t  "B12 chmod lower file -> copy-up" "chmod 600 $M/etc/public && [ \"\$(stat -c %a $UP/etc/public)\" = 600 ]"
t  "B13 chown lower file -> copy-up" "chown 1000:1000 $M/etc/public2 && [ \"\$(stat -c %u $M/etc/public2)\" = 1000 ]"
t  "B14 touch sets mtime to now (guest clock starts at 1970)" "a=\$(stat -c %Y $M/etc/secret); touch $M/etc/secret; b=\$(stat -c %Y $M/etc/secret); now=\$(date +%s); [ \$b -ne \$a ] && [ \$b -ge \$((now-5)) ] && [ \$b -le \$now ]"
t  "B15 symlink readlink" "[ \"\$(readlink $M/link_to_network)\" = etc/config/network ]"
t  "B16 follow symlink" "grep -q appended $M/link_to_network"
t  "B17 fifo type" "[ -p $M/fifo ]"
t  "B18 char device open/write (lower dev node)" "[ -c $M/nulldev ] && echo x > $M/nulldev"
t  "B19 statfs" "stat -f $M >/dev/null"

echo "===== device nodes (usable by mount through the overlay, as /dev is on a root overlay) ====="
mkdir -p /mnt/d
t  "D01 lower block node has block type" "[ -b $M/blkdev ]"
t  "D02 mount via lower block node (lookup_bdev)" "losetup $M/blkdev /lower.sqfs && mount -t squashfs -o ro $M/blkdev /mnt/d && [ \"\$(cat /mnt/d/etc/config/network)\" = lower-network ]"
umount /mnt/d 2>/dev/null; losetup -d $M/blkdev 2>/dev/null
tf "D03 user cannot open 0600 root block node" "U 'head -c 1 $M/blkdev'"
t  "D04 mknod block node on overlay" "mknod $M/blkdev_up b 7 3 && [ -b $UP/blkdev_up ] && [ \"\$(stat -c %t:%T $M/blkdev_up)\" = 7:3 ]"
t  "D05 mount via block node created on overlay" "losetup $M/blkdev_up /lower.sqfs && mount -t squashfs -o ro $M/blkdev_up /mnt/d && [ -f /mnt/d/big.bin ]"
umount /mnt/d 2>/dev/null; losetup -d $M/blkdev_up 2>/dev/null
t  "D06 copied-up block node (chmod) still mounts" "chmod 640 $M/blkdev && [ -b $UP/blkdev ] && losetup $M/blkdev /lower.sqfs && mount -t squashfs -o ro $M/blkdev /mnt/d && [ -f /mnt/d/big.bin ]"
umount /mnt/d 2>/dev/null; losetup -d $M/blkdev 2>/dev/null
t  "D07 char node created on overlay reads/writes" "mknod $M/null_up c 1 3 && [ -c $M/null_up ] && echo x > $M/null_up && [ \"\$(head -c 1 $M/null_up | wc -c)\" -eq 0 ]"

echo "===== whiteout / opaque / readdir ====="
t  "W01 unlink lower file" "rm $M/etc/todelete"
t  "W02 hidden in merged" "[ ! -e $M/etc/todelete ] && ! ls $M/etc | grep -qx todelete"
t  "W03 whiteout symlink in upper" "[ -L $UP/etc/todelete ] && [ \"\$(readlink $UP/etc/todelete)\" = '(overlay-whiteout)' ]"
t  "W04 lower still has it" "[ -f $L/etc/todelete ]"
t  "W05 recreate over whiteout" "echo re > $M/etc/todelete && [ \"\$(cat $M/etc/todelete)\" = re ]"
t  "W06 unlink second file (kept deleted)" "rm $M/etc/todelete2 && [ ! -e $M/etc/todelete2 ]"
tf "W07 rmdir non-empty lower dir fails" "rmdir $M/dir_to_remove"
t  "W08 rm -rf lower tree" "rm -rf $M/dir_to_remove && [ ! -e $M/dir_to_remove ]"
t  "W09 mkdir over removed dir is opaque/empty" "mkdir $M/dir_to_remove && [ -z \"\$(ls -A $M/dir_to_remove)\" ]"
t  "W10 rmdir empty lower dir" "rmdir $M/empty_dir && [ ! -e $M/empty_dir ]"
t  "R01 readdir merge (no dups)" "echo b > $M/dir_merge/b_upper && echo c >> $M/dir_merge/common && [ \"\$(ls -A $M/dir_merge | tr '\n' ' ')\" = 'a_lower b_upper common ' ]"
t  "R02 merged dir entry count via find" "[ \$(find $M/dir_merge -type f | wc -l) -eq 3 ]"
t  "R03 fsync on merged dir" "fsync $M/dir_merge"

echo "===== rename / link ====="
t  "N01 rename lower file" "mv $M/etc/renameme $M/etc/renamed && [ ! -e $M/etc/renameme ] && [ \"\$(cat $M/etc/renamed)\" = rename-me ]"
t  "N02 rename over existing lower file" "mv $M/etc/r_src $M/etc/r_dst && [ ! -e $M/etc/r_src ] && [ \"\$(cat $M/etc/r_dst)\" = r-src ]"
t  "N03 rename merged dir (kernel or mv fallback)" "mv $M/dir_merge $M/dir_merge2 && [ \"\$(ls -A $M/dir_merge2 | tr '\n' ' ')\" = 'a_lower b_upper common ' ] && [ ! -e $M/dir_merge ]"
t  "N04 hardlink lower file" "ln $M/hl_src $M/hl_dst && [ \"\$(cat $M/hl_dst)\" = hl ]"
t  "N05 hardlink shares data" "echo more >> $M/hl_dst && grep -q more $M/hl_src"
t  "N06 unlink original, link survives" "rm $M/hl_src && [ \"\$(cat $M/hl_dst | tr '\n' ' ')\" = 'hl more ' ]"
t  "N07 user can read surviving link" "U 'cat $M/hl_dst' >/dev/null"

echo "===== permissions as uid 1000 (ovl_permission port) ====="
t  "P01 user reads 0644 root lower" "U 'cat $M/etc/config/network' >/dev/null"
tf "P02 user reads 0600 root lower -> EACCES" "U 'cat $M/etc/secret'"
tf "P03 user appends 0644 root lower -> EACCES" "U 'echo x >> $M/etc/r_dst'"
t  "P04 ...and no copy-up for denied write" "[ ! -e $UP/etc/r_dst ] || [ \"\$(cat $M/etc/r_dst)\" = r-src ]"
tf "P05 user traverses 0700 root dir -> EACCES" "U 'cat $M/private/file'"
t  "P06 user exec 0755 lower script" "[ \"\$(U $M/bin/hello.sh)\" = hello ]"
tf "P07 user exec 0644 lower script -> EACCES" "U $M/bin/noexec.sh"
t  "P08 user appends own lower file (copy-up as non-root)" "U 'echo mine >> $M/userdir/userfile' && [ \"\$(stat -c %u $UP/userdir/userfile)\" = 1000 ]"
t  "P09 user creates in own lower dir" "U 'echo z > $M/userdir/created'"
tf "P10 user creates in root 0755 dir -> EACCES" "U 'echo z > $M/etc/usercreated'"
t  "P11 user unlinks in own lower dir (whiteout as non-root)" "U 'rm $M/userdir/deleteme' && [ ! -e $M/userdir/deleteme ]"
tf "P12 user reads file chmod'ed 600 via copy-up -> EACCES" "U 'cat $M/etc/public'"
t  "P13 user reads file chown'ed to user" "U 'cat $M/etc/public2' >/dev/null"
t  "P14 remount ro" "mount -o remount,ro $M"
tf "P15 write on ro overlay fails" "touch $M/ro_test"
t  "P16 remount rw" "mount -o remount,rw $M && touch $M/rw_test"

echo "===== stress: copy-up (dentry unhash) racing with non-root permission checks ====="
dmesg -c >/dev/null
( i=1; while [ $i -le 100 ]; do echo w >> $M/stress/f$i; i=$((i+1)); done ) &
bg=$!
errs=0; r=0
while [ $r -lt 20 ]; do
  i=1; while [ $i -le 100 ]; do U "cat $M/stress/f$i" >/dev/null 2>&1 || errs=$((errs+1)); i=$((i+10)); done
  r=$((r+1))
done
wait $bg
t  "X01 all stress files copied up" "[ \$(ls $UP/stress | wc -l) -eq 100 ]"
t  "X02 no permission errors during race (errs=$errs)" "[ $errs -eq 0 ]"
t  "X03 no WARN/BUG during race" "! dmesg | grep -qE 'WARNING|BUG|Oops|ovl_'"
dmesg | tail -5

echo "===== persistence: remount jffs2 + overlay ====="
t  "Z00 whiteout in cold_dir before remount" "rm $M/cold_dir/c1"
sync
t  "Z01 umount overlay" "umount $M"
t  "Z02 umount jffs2" "umount $UPM"
t  "Z03 mount jffs2 again" "mount -t jffs2 /dev/mtdblock0 $UPM"
t  "Z04 mount overlay again" "mount -t overlayfs overlayfs -o lowerdir=$L,upperdir=$UP $M"
t  "Z05 copied-up content persists" "grep -q appended $M/etc/config/network"
t  "Z06 recreated file persists" "[ \"\$(cat $M/etc/todelete)\" = re ]"
t  "Z07 whiteout persists" "[ ! -e $M/etc/todelete2 ]"
t  "Z08 opaque dir persists" "[ -z \"\$(ls -A $M/dir_to_remove)\" ]"
t  "Z09 xattr-based opaque survives remount (dir lower had x,sub)" "[ ! -e $M/dir_to_remove/x ]"
t  "Z10 big file persists" "[ \"\$(md5sum $M/big.bin | cut -d' ' -f1)\" = \"\$(cat /expected_big.md5)\" ]"
t  "Z11 user-owned copy-up persists" "grep -q mine $M/userdir/userfile"
echo "(cold dcache: readdir/rmdir of merged dir holding whiteouts — v4 deadlocked here on jffs2)"
t  "Z12 cold readdir hides whiteout" "[ \"\$(ls -A $M/cold_dir | tr '\n' ' ')\" = 'c2 ' ]"
t  "Z13 cold rm -rf merged dir with whiteouts" "rm -rf $M/cold_dir && [ ! -e $M/cold_dir ]"
t  "Z14 cold readdir of parent after rmdir" "! ls -A $M | grep -qx cold_dir"
for k in 1 2 3 4 5; do umount $M; mount -t overlayfs overlayfs -o lowerdir=$L,upperdir=$UP $M; done
umount $M

echo "===== comparison: ext2 upper (xattr) ====="
losetup /dev/loop1 /ext2.img && mount -t ext2 /dev/loop1 /mnt/eup && mkdir -p /mnt/eup/u
t  "E01 mount overlay on ext2 upper" "mount -t overlayfs overlayfs -o lowerdir=$L,upperdir=/mnt/eup/u /mnt/em"
t  "E02 whiteout on ext2" "rm /mnt/em/etc/todelete && [ ! -e /mnt/em/etc/todelete ]"
t  "E03 opaque on ext2" "rm -rf /mnt/em/dir_to_remove && mkdir /mnt/em/dir_to_remove && [ -z \"\$(ls -A /mnt/em/dir_to_remove)\" ]"
umount /mnt/em; umount /mnt/eup

echo "===== comparison: tmpfs upper (2.6.31 tmpfs has no trusted.* xattr) ====="
mount -t tmpfs tmpfs /mnt/tup && mkdir -p /mnt/tup/u
t  "T01 mount overlay on tmpfs upper" "mount -t overlayfs overlayfs -o lowerdir=$L,upperdir=/mnt/tup/u /mnt/tm"
t  "T02 copy-up works on tmpfs" "echo t >> /mnt/tm/etc/public && grep -q t /mnt/tm/etc/public"
if rm /mnt/tm/etc/todelete >/tmp/o 2>&1 && [ ! -e /mnt/tm/etc/todelete ]; then
  note "T03 whiteout on tmpfs upper SUCCEEDED (unexpected)"
else
  note "T03 whiteout on tmpfs upper fails as predicted: $(tr '\n' ' ' </tmp/o)"
fi
umount /mnt/tm; umount /mnt/tup

echo "===== kernel log checks ====="
dmesg | grep -nE 'WARNING|BUG|Oops|INFO: |lockdep|possible (recursive|circular|irq)|inconsistent|scheduling while atomic|sleeping function|Poison|corrupt|kmemleak' | head -40
t  "K01 no WARNING/BUG/Oops/lockdep/sleep-in-atomic splats" "! dmesg | grep -qE 'WARNING|BUG:|Oops|INFO: (possible|inconsistent|trying)|scheduling while atomic|sleeping function called|Slab corruption|Poison overwritten'"
if [ -e /sys/kernel/debug/kmemleak ]; then
  echo scan > /sys/kernel/debug/kmemleak; sleep 6; echo scan > /sys/kernel/debug/kmemleak; sleep 2
  leaks=$(grep -c '^unreferenced object' /sys/kernel/debug/kmemleak)
  note "kmemleak unreferenced objects: $leaks"
  grep -A12 '^unreferenced object' /sys/kernel/debug/kmemleak | grep -E 'unreferenced|ovl_|backtrace|match_strdup|kstrdup|\[<' | head -30
else
  note "kmemleak not available"
fi
dmesg | grep -iE 'lock|debug' | head -5
echo "SUMMARY: PASS=$PASS FAIL=$FAIL NOTE=$NOTE"
sync
reboot -f
