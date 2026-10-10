#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 OpenIRD contributors
# アドオン機構（host/ird-mkaddon と機器側の ird-addon）の自己完結テスト。docs/addon.md の
# 決まりを、作った .ird と偽の機器ツリー（rom/ ＝ base、その外が union の見え方）で確かめる。
#   使い方: tests/addon/run.sh
#   前提:   Linux/WSL（一時ディレクトリが ext4。モードと symlink を見る）、python3。
#   任意:   BUSYBOX=<busybox のパス>（無ければ PATH の busybox）があれば、ird-addon を
#           BusyBox の ash とアプレットだけで走らせる＝機器と同じ道具立てになる。
#           無ければホストの sh と coreutils で走らせる。
set -u

usage() { sed -n '4,10p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }
case "${1:-}" in -h|--help) usage 0 ;; esac

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
MKADDON="$ROOT/host/ird-mkaddon/ird_mkaddon.py"
IRD_ADDON="$ROOT/br-ext-common/package/ird-addon/files/usr/sbin/ird-addon"
PYTHON="${PYTHON:-python3}"
command -v "$PYTHON" >/dev/null 2>&1 || { echo "✗ python3 が見つかりません"; exit 1; }

T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
R="$T/dev"                 # 偽の機器ツリー（IRD_ADDON_ROOT）
S="$T/stage"               # staging を作る場所
REPO="$T/repo"             # アドオンリポジトリ（file://）
ST="$R/var/lib/ird-addon"  # 受領記録
export SOURCE_DATE_EPOCH=1760000000

BB="${BUSYBOX:-$(command -v busybox 2>/dev/null || true)}"
if [ -n "$BB" ]; then
	[ -x "$BB" ] || { echo "✗ BUSYBOX=$BB が実行できません"; exit 1; }
	mkdir "$T/bb"
	for a in $("$BB" --list); do ln -s "$BB" "$T/bb/$a"; done
	addon() { env -i PATH="$T/bb" IRD_ADDON_ROOT="$R" "$T/bb/sh" "$IRD_ADDON" "$@"; }
	echo "ird-addon を BusyBox で走らせる: $("$BB" 2>&1 | head -n 1)"
else
	addon() { IRD_ADDON_ROOT="$R" sh "$IRD_ADDON" "$@"; }
	echo "busybox が無い → ird-addon をホストの sh と coreutils で走らせる"
fi
mk() { "$PYTHON" "$MKADDON" "$@"; }
sha() { sha256sum "$1" | cut -d' ' -f1; }

pass=0
fail=0
ok() { pass=$((pass + 1)); echo "  ✓ $1"; }
ng() { fail=$((fail + 1)); echo "  ✗ $1${2:+  — $2}"; }
# check <名前> <コマンド…>: 成功すること
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
# says <名前> <出力に含まれるべき文字列> <コマンド…>: 成功して、その文字列を出すこと
says() {
	local n="$1" pat="$2" out rc; shift 2
	out="$("$@" 2>&1)"; rc=$?
	if [ $rc -eq 0 ] && printf '%s' "$out" | grep -qF -- "$pat"; then ok "$n"
	else ng "$n" "rc=$rc: $out"; fi
}

# --- 偽の機器ツリー ---------------------------------------------------------------
# rom/ が base（squashfs）。union なので base の中身は / にも見える。
mkroot() {
	rm -rf "$R"
	mkdir -p "$R/rom/etc/openird" "$R/rom/etc/init.d" "$R/rom/usr/bin" "$R/rom/var/lib" \
		"$R/rom/opt" "$R/rom/run"
	printf 'OVERLAY_DEV=/dev/mtdblock9\nADDON_ARCH=test-arch\n' > "$R/rom/etc/openird/board.conf"
	printf '# no repositories\n' > "$R/rom/etc/openird/addons.conf"
	echo base > "$R/rom/usr/bin/basetool"
	ln -s ../run "$R/rom/var/run"
	cp -a "$R/rom/." "$R/"
	mkdir -p "$R/tmp"
	tree > "$T/pristine"
}
# 機器ツリーの見え方（種別・モード・名前・link 先）。揮発する /tmp とインストーラの記録は除く
tree() {
	( cd "$R" && find . \( -path ./tmp -o -path ./var/lib/ird-addon \) -prune -o \
		-printf '%y %m %p %l\n' | LC_ALL=C sort )
}
pristine() { tree | cmp -s - "$T/pristine"; }
marks() { cat "$R/tmp/marks/$1" 2>/dev/null | tr '\n' ' '; }

# stage <dir> <name> <version> [arch]: 小さなアドオンの staging を作る
stage() {
	local d="$1" n="$2" v="$3"
	rm -rf "$d"
	mkdir -p "$d/root/usr/bin" "$d/root/usr/share/$n" "$d/root/etc/init.d"
	printf 'Package: %s\nVersion: %s\nArch: %s\nDescription: test add-on %s\n' \
		"$n" "$v" "${4:-test-arch}" "$n" > "$d/manifest"
	printf '#!/bin/sh\necho %s %s\n' "$n" "$v" > "$d/root/usr/bin/$n"
	chmod 775 "$d/root/usr/bin/$n"
	echo data > "$d/root/usr/share/$n/data.txt"
	chmod 600 "$d/root/usr/share/$n/data.txt"
	ln -s "$n" "$d/root/usr/bin/$n-link"
	printf '#!/bin/sh\n' > "$d/root/etc/init.d/S80$n"
	chmod 755 "$d/root/etc/init.d/S80$n"
	printf 'mkdir -p "$IRD_ADDON_ROOT/tmp/marks"\necho post-%s >> "$IRD_ADDON_ROOT/tmp/marks/%s"\n' \
		"$v" "$n" > "$d/postinstall"
	printf 'echo pre-%s >> "$IRD_ADDON_ROOT/tmp/marks/%s"\n' "$v" "$n" > "$d/preremove"
}

# rawird <out> <spec…>: ird-mkaddon を通さずに .ird を組む（壊れた・悪意のある入力）
#   spec = <メンバ名>=<種別>[:<内容>]   種別 f=ファイル d=ディレクトリ l=symlink(:先) p=FIFO
rawird() {
	"$PYTHON" - "$@" <<'EOF'
import gzip, io, sys, tarfile
out, specs = sys.argv[1], sys.argv[2:]
buf = io.BytesIO()
with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as tf:
    for s in specs:
        name, _, rest = s.partition("=")
        kind, _, arg = rest.partition(":")
        ti = tarfile.TarInfo(name)
        ti.mode = 0o755
        data = None
        if kind == "f":
            data = arg.replace("\\n", "\n").encode()
            ti.size = len(data)
        elif kind == "d":
            ti.type = tarfile.DIRTYPE
        elif kind == "l":
            ti.type = tarfile.SYMTYPE
            ti.linkname = arg
        elif kind == "p":
            ti.type = tarfile.FIFOTYPE
        tf.addfile(ti, io.BytesIO(data) if data is not None else None)
with open(out, "wb") as f:
    f.write(gzip.compress(buf.getvalue(), mtime=0))
EOF
}
MF='manifest=f:Package: raw\nVersion: 1\nArch: test-arch\n'
# raw <名前> <出力に含まれるべき文字列> <spec…>: その .ird を入れようとして拒まれ、何も残らないこと
raw() {
	local n="$1" pat="$2"; shift 2
	rawird "$T/raw.ird" "$@"
	refuses "$n" "$pat" addon install "$T/raw.ird" "$(sha "$T/raw.ird")"
	check "$n: 何も残らない" pristine
}

# ================================ ird-mkaddon =================================
echo "[mkaddon] build"
stage "$S/hello" hello 1.0
mkdir -p "$REPO"
out="$(mk build "$S/hello" -o "$REPO" 2>&1)"
H="$REPO/hello_1.0_test-arch.ird"
check "名前は <Package>_<Version>_<Arch>.ird" test -f "$H"
check "SHA256 を出す" grep -qF "$(sha "$H")" <<<"$out"
cp "$H" "$T/first.ird"
touch "$S/hello/root/usr/bin/hello"
mk build "$S/hello" -o "$REPO" >/dev/null
check "同じ入力から同じバイト列（時刻に依らない）" cmp -s "$H" "$T/first.ird"

listing="$("$PYTHON" - "$H" <<'EOF'
import sys, tarfile
for ti in tarfile.open(sys.argv[1], "r:gz"):
    print(ti.name, "%o" % ti.mode, ti.uid, ti.gid, ti.mtime, ti.linkname)
EOF
)"
check "manifest が先頭" test "$(head -n 1 <<<"$listing" | cut -d' ' -f1)" = manifest
check "所有者は 0:0" test -z "$(awk '$3 != 0 || $4 != 0' <<<"$listing")"
check "時刻は SOURCE_DATE_EPOCH" test -z "$(awk -v t="$SOURCE_DATE_EPOCH" '$5 != t' <<<"$listing")"
check "実行ファイルは 755 に正規化" grep -qx "root/usr/bin/hello 755 0 0 $SOURCE_DATE_EPOCH " <<<"$listing"
check "それ以外は 644 に正規化" grep -qx "root/usr/share/hello/data.txt 644 0 0 $SOURCE_DATE_EPOCH " <<<"$listing"
check "symlink はそのまま入る" grep -qx "root/usr/bin/hello-link 777 0 0 $SOURCE_DATE_EPOCH hello" <<<"$listing"
check "スクリプトは 755" grep -qx "postinstall 755 0 0 $SOURCE_DATE_EPOCH " <<<"$listing"

echo "[mkaddon] 拒むもの"
bad() {   # bad <名前> <文字列> <staging を壊すコマンド…>
	local n="$1" pat="$2"; shift 2
	stage "$S/bad" bad 1.0
	( cd "$S/bad" && "$@" )
	refuses "$n" "$pat" mk build "$S/bad" -o "$T/badout"
}
bad "Package に _ や大文字" "Package=" sed -i 's/^Package: .*/Package: Bad_Name/' manifest
bad "Version が無い" "Version が無い" sed -i '/^Version:/d' manifest
bad "知らないキー" "知らないキー" sh -c 'echo "Descripton: typo" >> manifest'
bad "予約キー Depends" "予約キー" sh -c 'echo "Depends: libfoo" >> manifest'
bad "予約キー Requires-OpenIRD" "予約キー" sh -c 'echo "Requires-OpenIRD: 26.10" >> manifest'
bad "staging に余計なファイル" "余計なもの" touch build.log
bad "/tmp の下に置く" "/tmp の下" sh -c 'mkdir root/tmp && touch root/tmp/x'
bad "インストーラの記録の下に置く" "/var/lib/ird-addon の下" \
	sh -c 'mkdir -p root/var/lib/ird-addon && touch root/var/lib/ird-addon/x.files'
bad "スクリプトが CRLF" "CRLF" sh -c 'printf "echo x\r\n" > postinstall'
bad "FIFO" "以外は入れられない" mkfifo root/usr/bin/pipe
bad "root/ が空" "ファイルが 1 つも無い" sh -c 'rm -rf root && mkdir root'

echo "[mkaddon] Arch の与え方"
stage "$S/noarch" noarch 1.0
sed -i '/^Arch:/d' "$S/noarch/manifest"
refuses "Arch がどこにも無ければ拒む" "Arch が無い" env -u IRD_ARCH "$PYTHON" "$MKADDON" build "$S/noarch" -o "$T/o1"
check "--arch で与える" mk build "$S/noarch" -o "$T/o1" --arch other-arch
check "…ファイル名に入る" test -f "$T/o1/noarch_1.0_other-arch.ird"
check "\$IRD_ARCH（sdk.env）で与える" env IRD_ARCH=env-arch "$PYTHON" "$MKADDON" build "$S/noarch" -o "$T/o1"
check "…ファイル名に入る" test -f "$T/o1/noarch_1.0_env-arch.ird"
refuses "manifest と --arch が食い違えば拒む" "--arch" mk build "$S/hello" -o "$T/o1" --arch other-arch

echo "[mkaddon] index"
check "索引を作る" mk index "$REPO"
check "スタンザが spec §3 の形" cmp -s "$REPO/Packages" - <<EOF
Package: hello
Version: 1.0
Arch: test-arch
Filename: hello_1.0_test-arch.ird
SHA256: $(sha "$H")
Size: $(stat -c%s "$H")
Description: test add-on hello
EOF
mkdir "$T/dup"
cp "$H" "$T/dup/"
stage "$S/hello2" hello 2.0
mk build "$S/hello2" -o "$T/dup" >/dev/null
refuses "同じ Package/Arch の 2 版は拒む" "1 版だけ" mk index "$T/dup"
mkdir "$T/ren"
cp "$H" "$T/ren/renamed.ird"
refuses "ファイル名が manifest と合わなければ拒む" "合わない" mk index "$T/ren"
mkdir "$T/evilrepo"
rawird "$T/evilrepo/raw_1_test-arch.ird" "$MF" 'root/usr/bin/x=f:x' 'stray=f:x'
refuses "余計なメンバを持つ .ird は索引に載せない" "余計なメンバ" mk index "$T/evilrepo"

# ================================= ird-addon ==================================
mkroot
echo "[addon] リポジトリから入れる"
says "リポジトリが空なら何もしない" "no add-on repositories" addon update
refuses "索引が無ければ名前では入らない" "not in the repository indexes" addon install hello
echo "file://$REPO/" > "$R/etc/openird/addons.conf"
tree > "$T/pristine"       # addons.conf は所有者の設定＝以後の「元どおり」の基準に含める
says "update が索引を取る" "1 add-ons" addon update
says "available が名前を出す" "hello 1.0 - test add-on hello" addon available
says "install <name>" "installed hello 1.0" addon install hello
check "ファイルが入る（実行ビットつき）" test -x "$R/usr/bin/hello"
check "symlink が symlink のまま入る" test "$(readlink "$R/usr/bin/hello-link")" = hello
check "base に在るディレクトリへも足せる" test -f "$R/etc/init.d/S80hello"
check "base は変わらない" test -z "$(find "$R/rom" -newer "$T/pristine" -type f)"
check "postinstall が走る" test "$(marks hello)" = "post-1.0 "
check "受領記録（manifest・files・preremove）" test -f "$ST/hello.manifest" -a -f "$ST/hello.files" -a -f "$ST/hello.preremove"
says "list" "hello 1.0" addon list
says "info が manifest を出す" "Description: test add-on hello" addon info hello
says "info がファイルを出す" "  /usr/bin/hello-link" addon info hello
says "同じ版なら取りに行かない" "already installed" addon install hello

echo "[addon] 消す"
says "remove" "removed hello" addon remove hello
check "preremove が走る" test "$(marks hello)" = "post-1.0 pre-1.0 "
check "元どおり（足したディレクトリも消え、base のディレクトリは残る）" pristine
check "受領記録が消える" test -z "$(ls "$ST" | grep -v '^lists$')"
refuses "入っていないものは消せない" "not installed" addon remove hello

echo "[addon] 1 本を直接入れる"
says "install <file> <sha256>" "installed hello 1.0" addon install "$H" "$(sha "$H")"
says "大文字の sha256 と file:// も通る" "installed hello 1.0" \
	addon install "file://$H" "$(sha "$H" | tr a-f A-F)"
addon remove hello >/dev/null
refuses "sha256 が違えば入れない" "SHA256 mismatch" addon install "$H" "$(printf '%064d' 0)"
check "…何も残らない" pristine
refuses "sha256 無しのファイルは入れない" "needs its sha256" addon install "$H"
refuses "sha256 の形でなければ拒む" "not a sha256" addon install "$H" abc123
refuses "http は拒む" "plain http is refused" addon install "http://example.org/x.ird" "$(sha "$H")"
refuses "他の scheme は拒む" "unsupported URL" addon install "ftp://example.org/x.ird" "$(sha "$H")"

echo "[addon] 拒むもの"
stage "$S/alien" alien 1.0 other-arch
mk build "$S/alien" -o "$T" >/dev/null
refuses "Arch が違う" "is built for 'other-arch', this device is 'test-arch'" \
	addon install "$T/alien_1.0_other-arch.ird" "$(sha "$T/alien_1.0_other-arch.ird")"
check "…何も残らない" pristine
stage "$S/clobber" clobber 1.0
echo evil > "$S/clobber/root/usr/bin/basetool"
mk build "$S/clobber" -o "$T" >/dev/null
refuses "base のファイルを上書きする" "would replace a file of the base system: /usr/bin/basetool" \
	addon install "$T/clobber_1.0_test-arch.ird" "$(sha "$T/clobber_1.0_test-arch.ird")"
check "…何も残らない" pristine
check "…base のファイルは元のまま" test "$(cat "$R/usr/bin/basetool")" = base
stage "$S/viarun" viarun 1.0
mkdir -p "$S/viarun/root/var/run"
touch "$S/viarun/root/var/run/x.pid"
mk build "$S/viarun" -o "$T" >/dev/null
refuses "base の symlink をディレクトリとして使う" "is not a directory in the base system" \
	addon install "$T/viarun_1.0_test-arch.ird" "$(sha "$T/viarun_1.0_test-arch.ird")"
check "…何も残らない" pristine

raw "manifest が無い" "has no manifest" 'root/usr/bin/x=f:x'
raw "root/ が無い" "has no root/ tree" "$MF"
raw "余計なメンバ" "unexpected member in the add-on" "$MF" 'root/usr/bin/x=f:x' 'stray=f:x'
raw "絶対パス" "in the add-on" "$MF" 'root/usr/bin/x=f:x' '/etc/evil=f:x'
raw "上へ出るパス" "in the add-on" "$MF" 'root/usr/bin/x=f:x' '../evil=f:x'
raw "root/ から上へ出るパス" "in the add-on" "$MF" 'root/usr/bin/x=f:x' 'root/../../evil=f:x'
raw "root が symlink" "has no root/ tree" "$MF" 'root=l:/'
raw "FIFO" "not a file, a link or a directory" "$MF" 'root/usr/bin/x=f:x' 'root/usr/bin/pipe=p'
raw "/tmp の下に置く" "installs into /tmp" "$MF" 'root/tmp/x=f:x'
raw "インストーラの記録の下に置く" "installs into /var/lib/ird-addon" "$MF" 'root/var/lib/ird-addon/x.files=f:x'
raw "ファイルが 1 つも無い" "installs no files" "$MF" 'root/usr/share/empty=d'
raw "予約キー Depends" "uses Depends" \
	'manifest=f:Package: raw\nVersion: 1\nArch: test-arch\nDepends: libfoo\n' 'root/usr/bin/x=f:x'
raw "予約キー Requires-OpenIRD" "uses Requires-OpenIRD" \
	'manifest=f:Package: raw\nVersion: 1\nArch: test-arch\nRequires-OpenIRD: 26.10\n' 'root/usr/bin/x=f:x'
raw "Package 名が不正" "not an add-on name" \
	'manifest=f:Package: ../x\nVersion: 1\nArch: test-arch\n' 'root/usr/bin/x=f:x'
echo "not a tarball" > "$T/junk.ird"
refuses "tar でないもの" "not an add-on" addon install "$T/junk.ird" "$(sha "$T/junk.ird")"

echo "[addon] ほかのアドオンとの関係"
addon install hello >/dev/null
stage "$S/thief" thief 1.0
echo mine > "$S/thief/root/usr/bin/hello"
mk build "$S/thief" -o "$T" >/dev/null
refuses "他のアドオンのファイルは取れない" "/usr/bin/hello exists already (it belongs to the add-on hello)" \
	addon install "$T/thief_1.0_test-arch.ird" "$(sha "$T/thief_1.0_test-arch.ird")"
check "…相手は無傷" test -x "$R/usr/bin/hello" -a ! -e "$ST/thief.files"
echo user > "$R/usr/bin/mine"
stage "$S/overuser" overuser 1.0
echo x > "$S/overuser/root/usr/bin/mine"
mk build "$S/overuser" -o "$T" >/dev/null
refuses "所有者が置いたファイルも取れない" "/usr/bin/mine exists already (it does not belong to this add-on)" \
	addon install "$T/overuser_1.0_test-arch.ird" "$(sha "$T/overuser_1.0_test-arch.ird")"
rm "$R/usr/bin/mine"
# 2 つのアドオンが同じ新しいディレクトリ（base に無い）を使う
for n in share-a share-b; do
	stage "$S/$n" "$n" 1.0
	mkdir -p "$S/$n/root/opt/common"
	echo "$n" > "$S/$n/root/opt/common/$n.txt"
	mk build "$S/$n" -o "$T" >/dev/null
	addon install "$T/${n}_1.0_test-arch.ird" "$(sha "$T/${n}_1.0_test-arch.ird")" >/dev/null
done
addon remove share-a >/dev/null
check "共有ディレクトリは、使う者が残るあいだ残る" test -f "$R/opt/common/share-b.txt"
addon remove share-b >/dev/null
check "…最後の 1 つが消えると消える（base の /opt は残る）" test ! -e "$R/opt/common" -a -d "$R/opt"
addon remove hello >/dev/null
check "全部消すと元どおり" pristine

echo "[addon] 入れ替え（版が変わる）"
addon install hello >/dev/null
rm -f "$R/tmp/marks/hello"
stage "$S/hello2" hello 2.0
rm "$S/hello2/root/usr/bin/hello-link"
echo new > "$S/hello2/root/usr/share/hello/new.txt"
rm "$REPO"/*.ird
mk build "$S/hello2" -o "$REPO" >/dev/null
mk index "$REPO" >/dev/null
addon update >/dev/null
says "install が新しい版に入れ替える" "installed hello 2.0" addon install hello
check "新しい版の中身" test "$(sh "$R/usr/bin/hello")" = "hello 2.0"
check "新しい版が足したファイルが入る" test -f "$R/usr/share/hello/new.txt"
check "新しい版に無いファイルは消える" test ! -e "$R/usr/bin/hello-link" -a ! -L "$R/usr/bin/hello-link"
check "古い preremove → 新しい postinstall の順" test "$(marks hello)" = "pre-1.0 post-2.0 "
says "list は新しい版" "hello 2.0" addon list
addon remove hello >/dev/null
check "消すと元どおり" pristine

echo "[addon] 索引の値を信じすぎない"
H2="$REPO/hello_2.0_test-arch.ird"
idx() {   # idx <sed 式>: 索引を書き換えて取り直す
	mk index "$REPO" >/dev/null
	sed -i "$1" "$REPO/Packages"
	addon update >/dev/null
}
idx 's/^Size: .*/Size: 1/'
refuses "Size が違えば入れない" "size mismatch" addon install hello
idx 's/^SHA256: .*/SHA256: nothex/'
refuses "SHA256 が無効なら入れない" "no usable SHA256" addon install hello
idx 's/^SHA256: \(.\).*/SHA256: \1/'
refuses "SHA256 が短ければ入れない" "no usable SHA256" addon install hello
idx 's|^Filename: .*|Filename: ../repo/hello_2.0_test-arch.ird|'
refuses "Filename が上へ出れば入れない" "bad Filename" addon install hello
idx 's|^Filename: .*|Filename: https://example.org/x.ird|'
refuses "Filename が URL なら入れない" "bad Filename" addon install hello
idx 's/^Arch: .*/Arch: other-arch/'
refuses "Arch が違うスタンザは見えない" "not in the repository indexes for test-arch" addon install hello
check "…何も残らない" pristine
# 索引は hello と言うが、その Filename の中身（sha256 は合っている）は別のアドオン
stage "$S/liar" liar 2.0
mk build "$S/liar" -o "$T" >/dev/null
cp "$H2" "$T/hello2.keep"
cp "$T/liar_2.0_test-arch.ird" "$H2"
cat > "$REPO/Packages" <<EOF
Package: hello
Version: 2.0
Arch: test-arch
Filename: hello_2.0_test-arch.ird
SHA256: $(sha "$H2")
Size: $(stat -c%s "$H2")
EOF
addon update >/dev/null
refuses "索引の名前と中身の Package が違えば入れない" "the index says hello, but the add-on it points to is liar" \
	addon install hello
cp "$T/hello2.keep" "$H2"
check "…何も残らない" pristine
: > "$R/etc/openird/addons.conf"
addon update >/dev/null
check "外したリポジトリの索引は捨てる" test -z "$(ls "$ST/lists")"
echo "file://$REPO/" > "$R/etc/openird/addons.conf"

echo "[addon] 途中で止まったとき"
mk index "$REPO" >/dev/null
addon update >/dev/null
addon install hello >/dev/null
rm "$ST/hello.manifest"          # 完了の印が無い＝コピーの途中で電源が落ちた状態
says "list が未完了と言う" "hello (incomplete install" addon list
rm -f "$R/tmp/marks/hello"
says "remove で片づく" "removed hello" addon remove hello
check "…未完了のものの preremove は走らせない" test -z "$(marks hello)"
check "…元どおり" pristine
addon install hello >/dev/null
rm "$ST/hello.manifest"
says "入れ直しても片づく" "clearing an incomplete install of hello" addon install hello
addon remove hello >/dev/null
mkdir "$R/tmp/ird-addon.lock"
refuses "同時には走らない" "another ird-addon is running" addon install hello
rmdir "$R/tmp/ird-addon.lock"
# base が後から同じパスを持った（像の更新）: 消すと whiteout が base を隠すので残す
addon install hello >/dev/null
echo base > "$R/rom/usr/bin/hello"
says "base が持つようになったパスは消さない" "kept /usr/bin/hello" addon remove hello
check "…そのファイルは残る" test -e "$R/usr/bin/hello"
rm "$R/rom/usr/bin/hello" "$R/usr/bin/hello"
check "…ほかは元どおり" pristine
# コピーの途中で書けなくなる（overlay が満杯、の代わりに書けないディレクトリ）
if [ "$(id -u)" -ne 0 ]; then
	chmod 555 "$R/usr/bin"
	refuses "コピーに失敗したら入れた分を取り消す" "hello was not installed" addon install hello
	chmod 755 "$R/usr/bin"
	check "…何も残らない" pristine
	check "…受領記録も残らない" test ! -e "$ST/hello.files"
else
	echo "  - root で走っているので「コピーに失敗」は試せない（skip）"
fi

echo "[addon] スクリプトが失敗したとき"
stage "$S/grumpy" grumpy 1.0
echo 'exit 3' > "$S/grumpy/postinstall"
echo 'exit 4' > "$S/grumpy/preremove"
mk build "$S/grumpy" -o "$T" >/dev/null
refuses "postinstall の失敗は非 0 で知らせる" "postinstall script of grumpy failed" \
	addon install "$T/grumpy_1.0_test-arch.ird" "$(sha "$T/grumpy_1.0_test-arch.ird")"
says "…ファイルは入ったまま" "grumpy 1.0" addon list
says "preremove が失敗しても消す" "removed grumpy" addon remove grumpy
check "…元どおり" pristine

echo "[addon] overlay が無いとき"
mv "$R/rom" "$T/rom.away"
refuses "base が見えなければ入れない" "not running on an overlay" addon install hello
mv "$T/rom.away" "$R/rom"
sed -i '/^ADDON_ARCH=/d' "$R/etc/openird/board.conf"
refuses "Arch id の無い像では入れない" "no ADDON_ARCH" addon install hello

echo
echo "== $pass passed, $fail failed"
[ "$fail" -eq 0 ]
