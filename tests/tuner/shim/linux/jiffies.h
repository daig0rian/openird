/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SHIM_LINUX_JIFFIES_H
#define SHIM_LINUX_JIFFIES_H
#define HZ		250
extern unsigned long jiffies;		/* the harness advances it */
#define time_after(a, b)	((long)((b) - (a)) < 0)
#endif
