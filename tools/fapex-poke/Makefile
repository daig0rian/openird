# SPDX-License-Identifier: GPL-2.0-only
# fapex-poke: issue an arbitrary FAPex ioctl to any FAPex character device.
#
# Plain (musl) target program. Build with the Buildroot toolchain:
#   make CROSS=/path/to/arm-buildroot-linux-musleabi-
# scripts/build-fapex-poke.sh finds CROSS from the Buildroot output tree.

CROSS ?= arm-buildroot-linux-musleabi-
CC     = $(CROSS)gcc

CFLAGS  = -Os -static -Wall -Wextra -fno-strict-aliasing
LDFLAGS =

all: fapex-poke

fapex-poke: fapex-poke.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	$(CROSS)strip --strip-unneeded $@

clean:
	rm -f fapex-poke

.PHONY: all clean
