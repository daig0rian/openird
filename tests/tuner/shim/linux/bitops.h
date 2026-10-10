/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SHIM_LINUX_BITOPS_H
#define SHIM_LINUX_BITOPS_H
static inline int fls(unsigned int x)
{
	return x ? 32 - __builtin_clz(x) : 0;
}
#endif
