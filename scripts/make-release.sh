#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 OpenIRD contributors
# リリース物を揃える（docs/release.md）。`make release BOARD=<board>` が呼ぶ。
#
#   scripts/make-release.sh --check    リリースを作ってよいツリーかだけを確かめる
#   scripts/make-release.sh <BOARD>    output/release/<版>/ に揃える
#
# リリース物は、版のタグ（vYY.MM…）を打った commit の、変更の無い git checkout からだけ作る。
# 版の名前が中身を指すこと（git でないツリーは "unknown" になる）と、git に入っているとおりの
# ファイルでビルドできること（Windows から写したツリーは実行ビットの付け忘れを隠す）を、
# リリースのたびに確かめるため。
#
# 揃えるもの（Arch id ごと）:
#   openird-sdk-<版>-<arch-id>.<host>.tar.xz   アドオン作者向け SDK（make sdk の成果物）
#   openird-<版>-<arch-id>-sources.tar         対応するソース一式（Buildroot の legal-info と、
#                                              ビルド元の記録 openird-build.txt）
#   SHA256SUMS                                 このディレクトリの全ファイル
# GitHub への公開（gh release create）は人が行う。
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
die() { echo "✗ $*" >&2; exit 1; }

git -C "$ROOT" rev-parse --git-dir >/dev/null 2>&1 ||
	die "git の checkout でない（リリース物は git clone から作る）: $ROOT"
TAG="$(git -C "$ROOT" describe --tags --exact-match --match 'v[0-9]*' 2>/dev/null)" ||
	die "HEAD に版のタグ（vYY.MM…）が無い: $(git -C "$ROOT" describe --tags --always)"
[ -z "$(git -C "$ROOT" status --porcelain --untracked-files=no)" ] ||
	die "追跡しているファイルに変更がある（commit して、タグを打ち直すこと）"
VER="${TAG#v}"
COMMIT="$(git -C "$ROOT" rev-parse HEAD)"

if [ "${1:-}" = --check ]; then
	echo "✓ リリース $VER を作れるツリー（$TAG = ${COMMIT:0:7}・変更なし）"
	exit 0
fi

BOARD="${1:?usage: make-release.sh --check | <BOARD>}"
O="$ROOT/output/$BOARD"
IRD_ARCH="$(sed -n 's/^ADDON_ARCH=//p' "$O/target/etc/openird/board.conf" 2>/dev/null | head -n 1)"
[ -n "$IRD_ARCH" ] || die "$BOARD は Arch id（board.conf の ADDON_ARCH=）を決めていない"
HOSTOS="$(uname -s)-$(uname -m)"
SDK="$O/images/openird-sdk-$VER-$IRD_ARCH.$HOSTOS.tar.xz"
[ -f "$SDK" ] || die "SDK が無い: $SDK（make release BOARD=$BOARD から呼ぶこと）"
[ -f "$O/legal-info/manifest.csv" ] || die "legal-info が無い: $O/legal-info"

R="$ROOT/output/release/$VER"
mkdir -p "$R"
cp "$SDK" "$R/"

# ソース一式: Buildroot が保存しないもの（Buildroot 自身と OpenIRD）は、どこの何かを書き添える
SRC="openird-$VER-$IRD_ARCH-sources"
EXTRA="$(mktemp -d)"
trap 'rm -rf "$EXTRA"' EXIT
. "$ROOT/scripts/buildroot.env"
cat > "$EXTRA/openird-build.txt" <<EOF
OpenIRD $VER  (tag $TAG, commit $COMMIT)
board:      $BOARD
arch id:    $IRD_ARCH
build host: $HOSTOS

This archive is the output of Buildroot's "make legal-info" for the build
above: the sources (with the patches that were applied) and the licence texts
of every package that went into the root filesystem image and the SDK -
sources/ for the target, host-sources/ for the build host (the toolchain).

Not in this archive, because Buildroot does not save them:
  Buildroot    $BUILDROOT_VERSION
               $BUILDROOT_URL
               sha256 $BUILDROOT_SHA256
  OpenIRD      the repository at tag $TAG: the external trees, the board
               configuration and the scripts that drive the build.
EOF
# S = symlink の指す先には適用しない
tar -cf "$R/$SRC.tar" --sort=name --owner=0 --group=0 --numeric-owner \
	--transform="s#^legal-info#$SRC#S;s#^\./openird-build\.txt#$SRC/openird-build.txt#S" \
	-C "$O" legal-info -C "$EXTRA" ./openird-build.txt

( cd "$R" && rm -f SHA256SUMS && sha256sum -- * > "$EXTRA/SHA256SUMS" && mv "$EXTRA/SHA256SUMS" . )

echo "✓ $R"
( cd "$R" && ls -l | awk 'NR > 1 {printf "  %12d  %s\n", $5, $9}' && echo && cat SHA256SUMS )
echo
echo "公開は人が行う（docs/release.md）: git push origin main $TAG → gh release create $TAG …"
