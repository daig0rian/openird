# SPDX-License-Identifier: GPL-2.0-only
# fapex-gpio: stage-B bring-up helper for the FAPex GPIO character device.
#
# A plain (musl) userland program for the target -- unlike fapex-trace it is not
# LD_PRELOAD'd, so it links normally against the Buildroot target libc.  Build
# it with the same toolchain that builds the rootfs:
#
#   make CROSS=/path/to/arm-buildroot-linux-musleabi-
# scripts/build-fapex-gpio.sh finds CROSS from the Buildroot output tree.

CROSS ?= arm-buildroot-linux-musleabi-
CC     = $(CROSS)gcc

CFLAGS  = -Os -static -Wall -Wextra -fno-strict-aliasing
LDFLAGS =

fapex-gpio: fapex-gpio.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	$(CROSS)strip --strip-unneeded $@

clean:
	rm -f fapex-gpio

.PHONY: clean
