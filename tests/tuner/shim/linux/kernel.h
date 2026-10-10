/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SHIM_LINUX_KERNEL_H
#define SHIM_LINUX_KERNEL_H
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <linux/types.h>
#define ARRAY_SIZE(a)	(sizeof(a) / sizeof((a)[0]))
#define min(a, b)	((a) < (b) ? (a) : (b))
#define min_t(t, a, b)	((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define KERN_INFO	""
#define KERN_WARNING	""
#define KERN_ERR	""
#define printk(...)	((void)0)
#define __iomem
#endif
