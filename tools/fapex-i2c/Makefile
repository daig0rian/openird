# SPDX-License-Identifier: GPL-2.0-only
# fapex-i2c: stage-B bring-up helper for the FAPex I2C character device.
# fapex-i2cd: cross-process I2C helper (serves fapex-trace's delegation, §5.5/§5.6).
#
# Plain (musl) target programs. Build with the Buildroot toolchain:
#   make CROSS=/path/to/arm-buildroot-linux-musleabi-
# scripts/build-fapex-i2c.sh finds CROSS from the Buildroot output tree.

CROSS ?= arm-buildroot-linux-musleabi-
CC     = $(CROSS)gcc

CFLAGS  = -Os -static -Wall -Wextra -fno-strict-aliasing
LDFLAGS =

all: fapex-i2c fapex-i2cd

fapex-i2c: fapex-i2c.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	$(CROSS)strip --strip-unneeded $@

fapex-i2cd: fapex-i2cd.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	$(CROSS)strip --strip-unneeded $@

clean:
	rm -f fapex-i2c fapex-i2cd

.PHONY: all clean
