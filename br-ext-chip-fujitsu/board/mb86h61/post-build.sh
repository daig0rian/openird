#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# OpenIRD mb86h61: post-build（$1 = TARGET_DIR）
# 起動時に張るはずの /dev の symlink をイメージに焼き込む（リポジトリには
# symlink を置かない＝Windows checkout 対策）。合わせて版情報を書く。
set -eu
TARGET_DIR="$1"

ln -sfn /proc/self/fd   "$TARGET_DIR/dev/fd"
ln -sfn /proc/self/fd/0 "$TARGET_DIR/dev/stdin"
ln -sfn /proc/self/fd/1 "$TARGET_DIR/dev/stdout"
ln -sfn /proc/self/fd/2 "$TARGET_DIR/dev/stderr"

rev="$(git -C "$BR2_EXTERNAL_IRD_FUJITSU_PATH" describe --always --dirty 2>/dev/null || echo unknown)"
cat > "$TARGET_DIR/etc/openird_release" <<EOF
OPENIRD_VARIANT=base
OPENIRD_BOARD=mb86h61_softbank_sb-tv02-wfpl
OPENIRD_BUILD=$rev
EOF
