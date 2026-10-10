#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# OpenIRD tuner-core の PID 列挙（ird_psi.c）のホスト単体テスト。
# カーネルヘッダをシム（tests/tuner/shim）で置き換え、合成した PSI を食わせて
# 列挙結果を検証する。実機もベンダ資材も要らない（ホストの cc だけ）。
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
SRC="$ROOT/br-ext-common/package/ird-tuner-core/src"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT
CC="${CC:-cc}"
"$CC" -std=gnu11 -Wall -Wextra -Wno-unused-parameter -O2 \
	-I "$HERE/shim" -I "$SRC" \
	-o "$OUT/test_psi" "$HERE/test_psi.c" "$SRC/ird_psi.c"
"$OUT/test_psi"
