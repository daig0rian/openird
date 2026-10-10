/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SHIM_ASM_IO_H
#define SHIM_ASM_IO_H
#include <linux/types.h>
/* provided by the harness: play the registers */
u32 __raw_readl(const void *addr);
void __raw_writel(u32 val, void *addr);
void *ioremap_nocache(unsigned long phys, unsigned long len);
#endif
