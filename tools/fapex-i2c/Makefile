# SPDX-License-Identifier: GPL-2.0-only
# fapex-i2c: stage-B bring-up helper for the FAPex I2C character device.
#
# A plain (musl) userland program for the target -- like fapex-gpio it links
# normally against the Buildroot target libc.  Build it with the same toolchain
# that builds the rootfs:
#
#   make CROSS=/path/to/arm-buildroot-linux-musleabi-
# scripts/build-fapex-i2c.sh finds CROSS from the Buildroot output tree.

CROSS ?= arm-buildroot-linux-musleabi-
CC     = $(CROSS)gcc

CFLAGS  = -Os -static -Wall -Wextra -fno-strict-aliasing
LDFLAGS =

fapex-i2c: fapex-i2c.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	$(CROSS)strip --strip-unneeded $@

clean:
	rm -f fapex-i2c

.PHONY: clean
