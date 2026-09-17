# SPDX-License-Identifier: GPL-2.0-only
# fapex-trace: LD_PRELOAD observer for the FAPex character devices (stage B).
#
# Built with the same pinned ARM cross-gcc as the kernel (gcc 4.9.4,
# arm-linux-gnueabi).  The library links against NO libc: it uses raw syscalls
# and imports only __errno_location, which the vendor process's uClibc 0.9.31
# resolves at load time.  So no uClibc toolchain is needed to build it.
#
#   make CROSS=/path/to/arm-linux-gnueabi-
# scripts/build-fapex-trace.sh finds CROSS from the kernel build.env pin.

CROSS ?= arm-linux-gnueabi-
CC     = $(CROSS)gcc

# ARMv6 (ARM1176) to match MB86H61; soft-float to match the vendor userland.
ARCHFLAGS = -march=armv6 -marm -mfloat-abi=soft
# NOT -fvisibility=hidden: the interposer entry points (open, ioctl, mmap, ...)
# must stay in .dynsym to override the vendor process's libc symbols.  Internal
# helpers are static; tr_syscall is marked hidden in the source.
CFLAGS    = $(ARCHFLAGS) -Os -fPIC -fno-stack-protector -ffreestanding \
            -Wall -Wextra
LDFLAGS   = -nostdlib -shared -Wl,-soname,fapex_trace.so \
            -Wl,--no-undefined-version

fapex_trace.so: fapex_trace.o syscall.o
	$(CC) $(ARCHFLAGS) $(LDFLAGS) -o $@ $^
	$(CROSS)strip --strip-unneeded $@

fapex_trace.o: fapex_trace.c
	$(CC) $(CFLAGS) -c -o $@ $<

syscall.o: syscall.S
	$(CC) $(ARCHFLAGS) -c -o $@ $<

clean:
	rm -f fapex_trace.o syscall.o fapex_trace.so

.PHONY: clean
