# SPDX-License-Identifier: GPL-2.0-only
# yb1-cmd: post commands to the AV sub-CPU command stack (stage-C prototype).
#
# Static, non-PIC, no libc: the vendor rootfs has uClibc 0.9.31 but we do not
# want to depend on it, and the kernel.org cross toolchain ships no target libc
# at all.  -G0 turns off small-data addressing so no $gp setup is needed.
#
#   make CROSS=/path/to/mips-linux-
# scripts/build-yb1-cmd.sh finds CROSS from the board's kernel build.env pin.

CROSS ?= mips-linux-
CC     = $(CROSS)gcc

ARCHFLAGS = -march=mips32r2 -mabi=32 -EB -msoft-float
CFLAGS    = $(ARCHFLAGS) -Os -mno-abicalls -fno-pic -G0 \
            -fno-stack-protector -ffreestanding -Wall -Wextra
LDFLAGS   = -nostdlib -static -Wl,-e,_start

yb1-cmd: yb1_cmd.o syscall.o
	$(CC) $(ARCHFLAGS) -mno-abicalls -fno-pic -G0 $(LDFLAGS) -o $@ $^
	$(CROSS)strip $@

yb1_cmd.o: yb1_cmd.c
	$(CC) $(CFLAGS) -c -o $@ $<

syscall.o: syscall.S
	$(CC) $(ARCHFLAGS) -mno-abicalls -fno-pic -G0 -c -o $@ $<

clean:
	rm -f yb1_cmd.o syscall.o yb1-cmd

.PHONY: clean
