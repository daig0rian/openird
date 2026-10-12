#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 OpenIRD contributors
# 更新（sysupgrade）の自己完結テスト。docs/sysupgrade.md の決まりを、作った更新イメージと
# 偽の機器ツリーで確かめる: イメージの検査・RAM への用意・書いて照合して駄目なら戻す手順・
# overlay の初期化。init から PID 1 を引き継ぐところは実機でしか確かめられない。
#   使い方: tests/sysupgrade/run.sh
#   前提:   Linux/WSL、python3。
#   任意:   BUSYBOX=<busybox のパス>（無ければ PATH の busybox）があれば、機器側のスクリプトを
#           BusyBox の ash とアプレットだけで走らせる＝機器と同じ道具立てになる。
set -u

usage() { sed -n '4,10p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }
case "${1:-}" in -h|--help) usage 0 ;; esac

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
MKIMAGE="$ROOT/host/openird-mkimage/openird_mkimage.py"
BASE="$ROOT/br-ext-common/package/ird-base-files/files"
PLATFORM="$ROOT/br-ext-chip-renesas/board/emma3sep/rootfs_overlay/lib/upgrade/platform.sh"
BOARD=emma3sep_softbank_poe-br100-yb1
PYTHON="${PYTHON:-python3}"
command -v "$PYTHON" >/dev/null 2>&1 || { echo "✗ python3 が見つかりません"; exit 1; }

T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
R="$T/dev-root"            # 偽の機器ツリー（SYSUPGRADE_ROOT）
W="$T/work"                # 作業ディレクトリ（SYSUPGRADE_WORK）
I="$T/img"                 # 作ったイメージ

# 機器の mtd の代わり: 普通のファイルに書く・読み比べる。FAKE_MTD_BAD に名前のある区画は、
# 新しいイメージ（old.* 以外）を書くたびに壊す。FAKE_MTD_BAD_ONCE は最初の 1 回だけ壊す。
mkdir -p "$T/fakebin"
cat > "$T/fakebin/mtd" <<'EOF'
#!/bin/sh
while [ "${1#-}" != "$1" ]; do shift; done
cmd="$1"; img="$2"; dev="$3"; name="${dev##*/}"
case "$cmd" in
	write)
		dd if="$img" of="$dev" conv=notrunc 2>/dev/null || exit 1
		case "${img##*/}" in old.*) exit 0 ;; esac
		bad=0
		case " ${FAKE_MTD_BAD:-} " in *" $name "*) bad=1 ;; esac
		if [ -n "${FAKE_MTD_BAD_ONCE:-}" ] && [ ! -e "$FAKE_MTD_BAD_ONCE.$name" ]; then
			case " ${FAKE_MTD_BAD_ONCE_PARTS:-} " in *" $name "*) bad=1; : > "$FAKE_MTD_BAD_ONCE.$name" ;; esac
		fi
		[ "$bad" = 0 ] || printf 'X' | dd of="$dev" bs=1 seek=7 conv=notrunc 2>/dev/null
		;;
	verify) cmp -s -n "$(( $(wc -c < "$img") ))" "$img" "$dev" ;;
	*) exit 2 ;;
esac
EOF
chmod +x "$T/fakebin/mtd"

BB="${BUSYBOX:-$(command -v busybox 2>/dev/null || true)}"
if [ -n "$BB" ]; then
	[ -x "$BB" ] || { echo "✗ BUSYBOX=$BB が実行できません"; exit 1; }
	mkdir "$T/bb"
	for a in $("$BB" --list); do ln -s "$BB" "$T/bb/$a"; done
	dev_sh() { env -i PATH="$T/fakebin:$T/bb" SYSUPGRADE_ROOT="$R" SYSUPGRADE_WORK="$W" \
		FAKE_MTD_BAD="${FAKE_MTD_BAD:-}" FAKE_MTD_BAD_ONCE="${FAKE_MTD_BAD_ONCE:-}" \
		FAKE_MTD_BAD_ONCE_PARTS="${FAKE_MTD_BAD_ONCE_PARTS:-}" "$T/bb/sh" "$@"; }
	echo "機器側のスクリプトを BusyBox で走らせる: $("$BB" 2>&1 | head -n 1)"
else
	dev_sh() { PATH="$T/fakebin:$PATH" SYSUPGRADE_ROOT="$R" SYSUPGRADE_WORK="$W" sh "$@"; }
	echo "busybox が無い → 機器側のスクリプトをホストの sh と coreutils で走らせる"
fi
sysupgrade() { dev_sh "$BASE/sbin/sysupgrade" "$@"; }
mkimage() { "$PYTHON" "$MKIMAGE" "$@"; }

pass=0
fail=0
ok() { pass=$((pass + 1)); echo "  ✓ $1"; }
ng() { fail=$((fail + 1)); echo "  ✗ $1${2:+  — $2}"; }
check() {
	local n="$1"; shift
	if "$@" >/dev/null 2>&1; then ok "$n"; else ng "$n"; fi
}
# refuses <名前> <出力に含まれるべき文字列> <コマンド…>: 失敗して、理由を言うこと
refuses() {
	local n="$1" pat="$2" out rc; shift 2
	out="$("$@" 2>&1)"; rc=$?
	if [ $rc -ne 0 ] && printf '%s' "$out" | grep -qF -- "$pat"; then ok "$n"
	else ng "$n" "rc=$rc: $out"; fi
}

# 偽の機器ツリーを作り直す
fresh_root() {
	rm -rf "$R" "$W"
	mkdir -p "$R/etc/openird" "$R/lib/upgrade" "$R/proc" "$R/dev" "$R/bin" "$R/overlay/openird/overlay"
	printf 'OPENIRD_VARIANT=base\nOPENIRD_BOARD=%s\nOPENIRD_BUILD=v0-old\n' "$BOARD" > "$R/etc/openird_release"
	cp "$BASE/lib/upgrade/common.sh" "$BASE/lib/upgrade/stage2" "$PLATFORM" "$R/lib/upgrade/"
	cp "$BASE/etc/inittab" "$R/etc/inittab"
	printf 'OVERLAY_SUBDIR=openird/overlay\n' > "$R/etc/openird/board.conf"
	cat > "$R/proc/mtd" <<-'EOF'
	dev:    size   erasesize  name
	mtd2: 00480000 00020000 "[ 2]Kernel(vmlinux)"
	mtd3: 00900000 00020000 "[ 3]FileSystem(SQUASHFS)"
	EOF
	if [ -n "$BB" ]; then cp "$BB" "$R/bin/busybox"; else : > "$R/bin/busybox"; fi
}

# --- 更新イメージを作る（ホスト側）---------------------------------------------
echo "[host] pack-sysupgrade"
mkdir -p "$I"
"$PYTHON" - "$ROOT" "$I" <<'EOF' || { echo "✗ テスト用のイメージを作れません"; exit 1; }
import os, struct, sys, tarfile, io
root, out = sys.argv[1], sys.argv[2]
sys.path.insert(0, os.path.join(root, "host", "openird-mkimage"))
from ird import images, sysupgrade
from ird.board import load_board, h
b = load_board("poe-br100-yb1")
spec = b["uimage"]
def uimage(n):
    return images.build_uimage(images.gzip_deterministic(os.urandom(64) * (n // 64)), spec, h(spec["load"]), h(spec["entry"]))
def squash(n):
    d = bytearray(os.urandom(n)); d[0:4] = b"sqsh"; struct.pack_into(">HH", d, 28, 3, 1); return bytes(d)
k, r = uimage(300000), squash(260000)
open(os.path.join(out, "kernel.img"), "wb").write(k)
open(os.path.join(out, "rootfs.img"), "wb").write(r)
board = b["defconfig"]
def save(name, data): open(os.path.join(out, name), "wb").write(data)
files = [("kernel", k), ("root", r)]
save("compat2.bin", sysupgrade.build(board, "99.02-test", 2, files))
save("otherboard.bin", sysupgrade.build("some_other_board", "99.02-test", 1, files))
save("notuimage.bin", sysupgrade.build(board, "99.02-test", 1, [("kernel", b"\0" * 4096), ("root", r)]))
save("bigroot.bin", sysupgrade.build(board, "99.02-test", 1, [("kernel", k), ("root", squash(0x680000 + 4096))]))
save("noroot.bin", sysupgrade.build(board, "99.02-test", 1, [("kernel", k)]))
# 形の崩れたもの: 余計なメンバ・ディレクトリ 2 つ・symlink・検査値の違うもの
def tar(members):
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as t:
        for name, kind, data in members:
            ti = tarfile.TarInfo(name)
            if kind == "dir": ti.type, ti.mode = tarfile.DIRTYPE, 0o755; t.addfile(ti)
            elif kind == "link": ti.type, ti.linkname = tarfile.SYMTYPE, data; t.addfile(ti)
            else: ti.size, ti.mode = len(data), 0o644; t.addfile(ti, io.BytesIO(data))
    return buf.getvalue()
top = "sysupgrade-" + board
ctl = sysupgrade.control(board, "99.02-test", 1, files)
good = [(top, "dir", None), (top + "/CONTROL", "file", ctl), (top + "/kernel", "file", k), (top + "/root", "file", r)]
save("extra.bin", tar(good + [(top + "/sub/x", "file", b"x")]))
save("twodirs.bin", tar(good + [("sysupgrade-other", "dir", None)]))
save("symlink.bin", tar(good[:2] + [(top + "/kernel", "link", "/etc/passwd"), good[3]]))
bad = bytearray(sysupgrade.build(board, "99.02-test", 1, files)); bad[-20000] ^= 0xff
save("damaged.bin", bytes(bad))
EOF
IMG="$I/openird-99.01-test-sysupgrade.bin"
check "kernel と rootfs から更新イメージを作る" mkimage pack-sysupgrade poe-br100-yb1 "$IMG" \
	--kernel "$I/kernel.img" --rootfs "$I/rootfs.img" --version 99.01-test
check "作ったイメージを読み直せる（probe-sysupgrade）" mkimage probe-sysupgrade "$IMG"
mkimage pack-sysupgrade poe-br100-yb1 "$I/again.bin" --kernel "$I/kernel.img" --rootfs "$I/rootfs.img" \
	--version 99.01-test >/dev/null 2>&1
check "同じ入力からは同じバイト列になる" cmp -s "$IMG" "$I/again.bin"
refuses "壊れたイメージは probe で分かる" "SHA-256" mkimage probe-sysupgrade "$I/damaged.bin"
head -c 100000 /dev/urandom > "$I/notkernel.img"
refuses "uImage でないカーネルは作る段で弾く" "uImage" mkimage pack-sysupgrade poe-br100-yb1 "$I/x.bin" \
	--kernel "$I/notkernel.img" --rootfs "$I/rootfs.img" --version 99.01-test

# --- 機器側: イメージの検査（-T）---------------------------------------------
echo "[device] sysupgrade -T"
fresh_root
out="$(sysupgrade -T "$IMG" 2>&1)"; rc=$?
if [ $rc -eq 0 ] && printf '%s' "$out" | grep -q "the image is good"; then ok "正しいイメージを受け取る"
else ng "正しいイメージを受け取る" "rc=$rc: $out"; fi
check "-T は作業ディレクトリを残さない" test ! -e "$W"
refuses "tar でないものは受け取らない" "not a tar archive" sysupgrade -T "$I/kernel.img"
refuses "余計なメンバのあるものは受け取らない" "unexpected members" sysupgrade -T "$I/extra.bin"
refuses "sysupgrade-* が 2 つあるものは受け取らない" "no single" sysupgrade -T "$I/twodirs.bin"
refuses "symlink のメンバは受け取らない" "the image" sysupgrade -T "$I/symlink.bin"
refuses "中身の壊れたものは受け取らない" "checksum mismatch" sysupgrade -T "$I/damaged.bin"
refuses "別の機種のイメージは受け取らない" "this image is for some_other_board" sysupgrade -T "$I/otherboard.bin"
refuses "機種の検査: uImage でないカーネル" "not a uImage" sysupgrade -T "$I/notuimage.bin"
refuses "機種の検査: 大きすぎる rootfs" "larger than its partition" sysupgrade -T "$I/bigroot.bin"
refuses "機種の検査: root の無いイメージ" "no kernel and root" sysupgrade -T "$I/noroot.bin"
refuses "設定の世代が違えば -n を求める" "use -n" sysupgrade -T "$I/compat2.bin"
check "-n を付ければ世代が違っても通す" sysupgrade -n -T "$I/compat2.bin"
sed -i 's/^mtd3: 00900000/mtd3: 007c0000/' "$R/proc/mtd"
refuses "区画の並びが違う機器では受け取らない" "flash layout" sysupgrade -T "$IMG"
fresh_root
rm "$R/lib/upgrade/platform.sh"
refuses "機種の部品が無ければ「未実装」と言う" "not implemented for this board" sysupgrade -T "$IMG"
fresh_root
mkdir -p "$W"; cp "$IMG" "$W/in.bin"
refuses "作業ディレクトリの中のイメージは受け取らない" "work directory" sysupgrade -T "$W/in.bin"

# --- 機器側: RAM への用意 ------------------------------------------------------
echo "[device] RAM への用意"
fresh_root
if [ -n "$BB" ]; then
	out="$(sysupgrade "$IMG" 2>&1)"; rc=$?
	if [ $rc -eq 0 ] && [ -f "$W/armed" ]; then ok "用意して、引き継ぎの印を付ける"
	else ng "用意して、引き継ぎの印を付ける" "rc=$rc: $out"; fi
	check "BusyBox と機種の道具（mtd）を写す" test -x "$W/bin/busybox" -a -x "$W/bin/mtd" -a -L "$W/bin/sh"
	check "更新のスクリプトを写す" test -f "$W/lib/common.sh" -a -f "$W/lib/platform.sh" -a -f "$W/lib/stage2"
	check "env に、消さない設定と overlay の場所を書く" sh -c "grep -q -x KEEP=1 '$W/env' && grep -q -x \"UPPER='/overlay/openird/overlay'\" '$W/env'"
	sysupgrade -n --dry-run "$IMG" >/dev/null 2>&1
	check "-n と --dry-run が env に伝わる" sh -c "grep -q -x KEEP=0 '$W/env' && grep -q -x DRYRUN=1 '$W/env'"
	sed -i '/^::restart:/d' "$R/etc/inittab"
	refuses "init が引き継げない系では始めない" "no restart action" sysupgrade "$IMG"
	check "始めなかったときは、引き継ぎの印を付けない" test ! -e "$W/armed"
else
	echo "  - busybox が無いので省く"
fi

# --- 機器側: 書いて、照合して、駄目なら戻す（upgrade_parts）-------------------
echo "[device] upgrade_parts"
# 区画の代わりのファイル（古い中身で埋める）と、書くイメージ
parts_setup() {
	fresh_root; mkdir -p "$W"
	head -c $((0x480000)) /dev/urandom > "$R/dev/mtd2"
	head -c $((0x900000)) /dev/urandom > "$R/dev/mtd3"
	cp "$R/dev/mtd2" "$T/mtd2.before"; cp "$R/dev/mtd3" "$T/mtd3.before"
}
run_parts() {	# [DRYRUN=1]
	dev_sh -c ". '$R/lib/upgrade/common.sh'; W='$W'; DRYRUN=${1:-0}; upgrade_parts '$I/rootfs.img' mtd3 '$I/kernel.img' mtd2"
}
starts_with() { cmp -s -n "$(stat -c %s "$1")" "$1" "$2"; }

parts_setup
check "2 つの区画に書く" run_parts
check "書いた先頭がイメージと一致する" sh -c "cmp -s -n $(stat -c %s "$I/rootfs.img") '$I/rootfs.img' '$R/dev/mtd3' && cmp -s -n $(stat -c %s "$I/kernel.img") '$I/kernel.img' '$R/dev/mtd2'"
check "書く前の中身を RAM に取ってある" starts_with "$W/old.mtd3" "$T/mtd3.before"

parts_setup
check "--dry-run は成功を返す" run_parts 1
check "--dry-run は何も書かない" sh -c "cmp -s '$R/dev/mtd2' '$T/mtd2.before' && cmp -s '$R/dev/mtd3' '$T/mtd3.before'"

parts_setup
FAKE_MTD_BAD_ONCE="$T/once" FAKE_MTD_BAD_ONCE_PARTS="mtd3" run_parts >/dev/null 2>&1; rc=$?
if [ $rc -eq 0 ] && starts_with "$I/rootfs.img" "$R/dev/mtd3" && starts_with "$I/kernel.img" "$R/dev/mtd2"
then ok "1 回目の照合が合わなければ書き直す"; else ng "1 回目の照合が合わなければ書き直す" "rc=$rc"; fi

parts_setup
out="$(FAKE_MTD_BAD="mtd3" run_parts 2>&1)"; rc=$?
if [ $rc -ne 0 ] && cmp -s "$R/dev/mtd3" "$T/mtd3.before" && cmp -s "$R/dev/mtd2" "$T/mtd2.before"
then ok "1 つ目が書けなければ元に戻し、2 つ目には触れない"
else ng "1 つ目が書けなければ元に戻し、2 つ目には触れない" "rc=$rc: $out"; fi

parts_setup
out="$(FAKE_MTD_BAD="mtd2" run_parts 2>&1)"; rc=$?
if [ $rc -ne 0 ] && cmp -s "$R/dev/mtd3" "$T/mtd3.before" && cmp -s "$R/dev/mtd2" "$T/mtd2.before"
then ok "2 つ目が書けなければ、書けていた 1 つ目も元に戻す"
else ng "2 つ目が書けなければ、書けていた 1 つ目も元に戻す" "rc=$rc: $out"; fi

# --- 機器側: overlay の初期化（wipe_overlay）----------------------------------
echo "[device] wipe_overlay"
wipe() { dev_sh -c ". '$R/lib/upgrade/common.sh'; wipe_overlay '$1'"; }
fresh_root
mkdir -p "$R/overlay/openird/overlay/etc/dropbear" "$R/overlay/openird/other"
: > "$R/overlay/openird/overlay/etc/dropbear/key"; : > "$R/overlay/openird/overlay/.hidden"
: > "$R/overlay/openird/other/keep"; : > "$R/etc/keep"
wipe /overlay/openird/overlay >/dev/null 2>&1
check "overlay の中身を、ドットファイルも含めて消す" sh -c "[ -d '$R/overlay/openird/overlay' ] && [ -z \"\$(ls -A '$R/overlay/openird/overlay')\" ]"
check "同じ区画の、overlay の外のものには触れない" test -f "$R/overlay/openird/other/keep"
refuses "/overlay の外は消さない" "not below /overlay" wipe /etc
refuses "空の指定では消さない" "not below /overlay" wipe ""
refuses ".. を含む指定では消さない" "not wiping" wipe /overlay/../etc
check "断ったときは何も消えていない" test -f "$R/etc/keep"

echo
echo "== $pass passed, $fail failed"
[ "$fail" -eq 0 ]
