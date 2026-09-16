#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Daigorian
# テスト用イメージを作る: lower.sqfs（所有者・モードを振り分けた lower 層）、ext2.img、
# 期待値 md5、initramfs リスト（カーネルの gen_init_cpio 形式。cpio コマンド不要）。
#   使い方: mkimages.sh <出力ディレクトリ> <静的 busybox> <guest/init.sh>
set -eu
[ $# -eq 3 ] || { sed -n '6p' "$0" | sed 's/^# //'; exit 1; }
O=$1; BB=$(readlink -f "$2"); INIT=$(readlink -f "$3")
rm -rf "$O"; mkdir -p "$O"; O=$(readlink -f "$O")
L="$O/lowertree"; mkdir -p "$L"; cd "$L"

mkdir -p etc/config dir_merge dir_to_remove/sub empty_dir private userdir bin stress cold_dir
printf 'lower-network\n' > etc/config/network
printf 'secret\n'        > etc/secret
printf 'public\n'        > etc/public
printf 'public2\n'       > etc/public2
printf 'trunc-me\n'      > etc/trunc
printf 'delete-me\n'     > etc/todelete
printf 'delete-me2\n'    > etc/todelete2
printf 'rename-me\n'     > etc/renameme
printf 'r-src\n'         > etc/r_src
printf 'r-dst\n'         > etc/r_dst
printf 'a\n'             > dir_merge/a_lower
printf 'common-lower\n'  > dir_merge/common
printf 'x\n' > dir_to_remove/x; printf 'y\n' > dir_to_remove/sub/y
printf 'p\n' > private/file
printf 'u\n' > userdir/userfile
printf 'd\n' > userdir/deleteme
printf 'c1\n' > cold_dir/c1; printf 'c2\n' > cold_dir/c2
printf 'hl\n' > hl_src
printf '#!/bin/sh\necho hello\n'  > bin/hello.sh
printf '#!/bin/sh\necho noexec\n' > bin/noexec.sh
for i in $(seq 1 100); do printf 's%s\n' "$i" > "stress/f$i"; done
ln -s etc/config/network link_to_network
# copy-up のチャンク(1MB)を跨ぐ大きさ
head -c 2500000 /dev/urandom > big.bin
cd "$O"
{ cat "$L/big.bin"; printf 'X\n'; } | md5sum | cut -d' ' -f1 > expected_big.md5
md5sum "$L/big.bin" | cut -d' ' -f1 > expected_lower.md5

# 全エントリの mode/uid/gid を pseudo 定義で明示する（-all-root は pseudo の uid を潰す）
( cd "$L" && find . -mindepth 1 | sed 's|^\./||' | sort ) | while read -r p; do
  case "$p" in
    etc/secret)        m=0600; o="0 0" ;;
    private)           m=0700; o="0 0" ;;
    bin/hello.sh)      m=0755; o="0 0" ;;
    bin/noexec.sh)     m=0644; o="0 0" ;;
    userdir|userdir/*) m=$(stat -c %a "$L/$p"); o="1000 1000" ;;
    *)                 m=$(stat -c %a "$L/$p"); o="0 0" ;;
  esac
  echo "$p m $m $o"
done > pseudo.txt
printf 'fifo i 0644 0 0 f\nnulldev c 0666 0 0 1 3\nblkdev b 0600 0 0 7 2\n' >> pseudo.txt
# 2.6.31 以前の squashfs 4.0 は xattr を読めない
mksquashfs "$L" lower.sqfs -comp gzip -no-xattrs -quiet -pf pseudo.txt \
  -root-uid 0 -root-gid 0 -root-mode 0755 >/dev/null
unsquashfs -lln lower.sqfs | grep -qE '1000/1000 .*userdir/userfile' || { echo "✗ lower.sqfs の所有者設定が効いていない"; exit 1; }

# 128B inode は「2038 年以降の日付を扱えない」警告が出るだけで、テストには影響しない
mke2fs -q -t ext2 -b 1024 -I 128 -O ^dir_index ext2.img 8192 >/dev/null 2>&1 \
  || { echo "✗ mke2fs 失敗"; exit 1; }

printf 'root:x:0:0:root:/root:/bin/sh\nuser:x:1000:1000:user:/:/bin/sh\n' > passwd
printf 'root:x:0:\nuser:x:1000:\n' > group

cat > initramfs.list <<EOF
dir /dev 0755 0 0
nod /dev/console 0600 0 0 c 5 1
nod /dev/null 0666 0 0 c 1 3
dir /bin 0755 0 0
dir /sbin 0755 0 0
dir /proc 0755 0 0
dir /sys 0755 0 0
dir /tmp 1777 0 0
dir /root 0700 0 0
dir /etc 0755 0 0
dir /mnt 0755 0 0
file /bin/busybox $BB 0755 0 0
slink /bin/sh busybox 0777 0 0
file /init $INIT 0755 0 0
file /etc/passwd $O/passwd 0644 0 0
file /etc/group $O/group 0644 0 0
file /lower.sqfs $O/lower.sqfs 0644 0 0
file /ext2.img $O/ext2.img 0644 0 0
file /expected_big.md5 $O/expected_big.md5 0644 0 0
file /expected_lower.md5 $O/expected_lower.md5 0644 0 0
EOF
rm -rf "$L"
