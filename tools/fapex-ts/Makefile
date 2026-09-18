# SPDX-License-Identifier: GPL-2.0-only
# fapex-ts: stage-B / Milestone-1 FAPex TSD (transport-stream) reader.
#
# Plain (musl) target userland program, like fapex-i2c. Build with the same
# Buildroot toolchain that builds the rootfs:
#   make CROSS=/path/to/arm-buildroot-linux-musleabi-
# scripts/build-fapex-ts.sh finds CROSS from the Buildroot output tree.

CROSS ?= arm-buildroot-linux-musleabi-
CC     = $(CROSS)gcc

CFLAGS  = -Os -static -Wall -Wextra -fno-strict-aliasing
LDFLAGS =

fapex-ts: fapex-ts.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	$(CROSS)strip --strip-unneeded $@

clean:
	rm -f fapex-ts

.PHONY: clean
