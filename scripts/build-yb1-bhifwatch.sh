#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Build tools/yb1-bhifwatch against the prepared 2.6.27.39 tree.
set -eu
K="${YB1_KERNEL:-$HOME/yb1-kernel/linux-2.6.27.39}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DL="$ROOT/dl"
CROSS="$DL/toolchain/gcc-4.9.4-nolibc/mips-linux/bin/mips-linux-"
export LD_LIBRARY_PATH="$DL/toolchain/hostlibs:${LD_LIBRARY_PATH:-}"
[ -d "$K" ] || { echo "✗ kernel tree not found: $K"; exit 1; }
make -C "$K" ARCH=mips CROSS_COMPILE="$CROSS" M="$ROOT/tools/yb1-bhifwatch" modules
ls -l "$ROOT/tools/yb1-bhifwatch/bhifwatch.ko"
strings -a "$ROOT/tools/yb1-bhifwatch/bhifwatch.ko" | grep -m1 '^vermagic'
