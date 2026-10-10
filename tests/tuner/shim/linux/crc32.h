/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SHIM_LINUX_CRC32_H
#define SHIM_LINUX_CRC32_H
#include <linux/types.h>
/* the kernel's crc32_be(): MSB first, polynomial 0x04c11db7, no final XOR */
static inline u32 crc32_be(u32 crc, const unsigned char *p, size_t len)
{
	int i;

	while (len--) {
		crc ^= (u32)*p++ << 24;
		for (i = 0; i < 8; i++)
			crc = (crc << 1) ^ ((crc & 0x80000000u) ? 0x04c11db7u : 0);
	}
	return crc;
}
#endif
