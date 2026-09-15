#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# OpenIRD mb86h61 first-aid: post-build（$1 = TARGET_DIR）
# / は squashfs（RO）なので、BusyBox 既定 inittab が起動時に張る symlink を
# ここでイメージに焼き込む（リポジトリには symlink を置かない＝Windows checkout 対策）。
set -eu
TARGET_DIR="$1"

ln -sfn /proc/self/fd   "$TARGET_DIR/dev/fd"
ln -sfn /proc/self/fd/0 "$TARGET_DIR/dev/stdin"
ln -sfn /proc/self/fd/1 "$TARGET_DIR/dev/stdout"
ln -sfn /proc/self/fd/2 "$TARGET_DIR/dev/stderr"

# どのビルドが焼かれているかを実機で確かめられるようにする
rev="$(git -C "$BR2_EXTERNAL_IRD_FUJITSU_PATH" describe --always --dirty 2>/dev/null || echo unknown)"
cat > "$TARGET_DIR/etc/openird_release" <<EOF
OPENIRD_VARIANT=firstaid
OPENIRD_BOARD=mb86h61_softbank_sb-tv02-wfpl
OPENIRD_BUILD=$rev
EOF
