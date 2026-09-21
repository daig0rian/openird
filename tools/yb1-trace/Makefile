# SPDX-License-Identifier: GPL-2.0-only
# yb1-trace: LD_PRELOAD observer for the POE-BR100-YB1 reception chardevs.
#
# Built with the same pinned MIPS cross-gcc as the kernel would be (gcc 4.9.4,
# mips-linux, big endian).  The library links against NO libc: it uses raw
# syscalls and imports only __errno_location, which the vendor process's
# uClibc 0.9.31 resolves at load time.  So no uClibc toolchain is needed.
#
#   make CROSS=/path/to/mips-linux-
# scripts/build-yb1-trace.sh finds CROSS from the board's kernel build.env pin.

CROSS ?= mips-linux-
CC     = $(CROSS)gcc

# MIPS32r2 big endian, o32, soft-float to match the vendor userland.
ARCHFLAGS = -march=mips32r2 -mabi=32 -EB -msoft-float
# NOT -fvisibility=hidden: the interposer entry points (open, ioctl, read, ...)
# must stay in .dynsym to override the vendor process's libc symbols.  Internal
# helpers are static; tr_syscall is marked hidden in the source.
CFLAGS    = $(ARCHFLAGS) -Os -fPIC -fno-stack-protector -ffreestanding \
            -Wall -Wextra
LDFLAGS   = -nostdlib -shared -Wl,-soname,yb1_trace.so \
            -Wl,--no-undefined-version

yb1_trace.so: yb1_trace.o syscall.o
	$(CC) $(ARCHFLAGS) $(LDFLAGS) -o $@ $^
	$(CROSS)strip --strip-unneeded $@

yb1_trace.o: yb1_trace.c
	$(CC) $(CFLAGS) -c -o $@ $<

syscall.o: syscall.S
	$(CC) $(ARCHFLAGS) -c -o $@ $<

clean:
	rm -f yb1_trace.o syscall.o yb1_trace.so

.PHONY: clean
