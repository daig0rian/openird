#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# pin した Buildroot を dl/ に取得・sha256 検証し、buildroot/ に展開する。
#   使い方: scripts/fetch-buildroot.sh    （Linux/WSL で実行。make が自動で呼ぶ）
# 展開済みの版が pin と一致していれば何もしない。版が違えば展開し直す。
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck disable=SC1091
. "$ROOT/scripts/buildroot.env"
DL="$ROOT/dl"; mkdir -p "$DL"
BR="$ROOT/buildroot"
STAMP="$BR/.openird-buildroot-version"

if [ -f "$STAMP" ] && [ "$(cat "$STAMP")" = "$BUILDROOT_VERSION" ] && [ -f "$BR/Makefile" ]; then
  exit 0
fi

verify() { echo "$1  $2" | sha256sum -c - >/dev/null 2>&1; }
T="$DL/$BUILDROOT_TARBALL"
if ! { [ -f "$T" ] && verify "$BUILDROOT_SHA256" "$T"; }; then
  echo "  downloading $BUILDROOT_TARBALL ..."
  wget -q -O "$T" "$BUILDROOT_URL"
  verify "$BUILDROOT_SHA256" "$T" || { echo "✗ sha256 不一致: $T"; exit 1; }
fi

echo "  extracting Buildroot $BUILDROOT_VERSION → buildroot/"
rm -rf "$BR"; mkdir -p "$BR"
tar xJf "$T" -C "$BR" --strip-components=1
[ -f "$BR/Makefile" ] || { echo "✗ Buildroot 展開失敗"; exit 1; }
echo "$BUILDROOT_VERSION" > "$STAMP"
