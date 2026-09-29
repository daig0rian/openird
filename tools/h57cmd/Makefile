# SPDX-License-Identifier: GPL-2.0-only
# h57cmd: stage-B Dione (/dev/h57fmb0) USB-tunnel tool. Static musl ARM.
#   make CROSS=/path/to/arm-buildroot-linux-musleabi-
# scripts/build-h57cmd.sh finds CROSS from the Buildroot output tree.
CROSS ?= arm-buildroot-linux-musleabi-
CC     = $(CROSS)gcc
CFLAGS  = -Os -static -Wall -Wextra
LDFLAGS =

h57cmd: h57cmd.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	$(CROSS)strip --strip-unneeded $@

clean:
	rm -f h57cmd
.PHONY: clean
