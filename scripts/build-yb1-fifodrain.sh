#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Build tools/yb1-fifodrain (fifodrain.ko, sramspeed.ko) against the prepared 2.6.27.39 tree
# (the one scripts/build-yb1-kmod.sh sets up from the device's own .config, so
# vermagic and structure layout match the vendor kernel).
set -eu
K="${YB1_KERNEL:-$HOME/yb1-kernel/linux-2.6.27.39}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DL="$ROOT/dl"
CROSS="$DL/toolchain/gcc-4.9.4-nolibc/mips-linux/bin/mips-linux-"
export LD_LIBRARY_PATH="$DL/toolchain/hostlibs:${LD_LIBRARY_PATH:-}"
[ -d "$K" ] || { echo "✗ kernel tree not found: $K"; exit 1; }
make -C "$K" ARCH=mips CROSS_COMPILE="$CROSS" M="$ROOT/tools/yb1-fifodrain" modules
for ko in "$ROOT"/tools/yb1-fifodrain/*.ko; do
ls -l "$ko"
strings -a "$ko" | grep -m1 '^vermagic'
# The tree's Module.symvers lists module exports only, so the kernel's own
# exports cannot be checked here -- insmod on the device does that.  What CAN
# go wrong at build time is gcc reaching for a libgcc helper (64-bit divide
# and the like), which no 2.6.27 kernel exports: refuse those.
helpers=$("${CROSS}nm" -u "$ko" | awk '{print $2}' | grep -E '^__(u?(div|mod|mul)di3|ashldi3|ashrdi3|lshrdi3)$' || true)
[ -z "$helpers" ] || { echo "✗ libgcc helpers needed:" $helpers; exit 1; }
echo "  ✓ no libgcc helpers; undefined: $("${CROSS}nm" -u "$ko" | wc -l) kernel symbols (resolved at insmod)"
done
