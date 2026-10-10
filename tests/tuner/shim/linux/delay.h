/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SHIM_LINUX_DELAY_H
#define SHIM_LINUX_DELAY_H
#define msleep(x)	((void)(x))
#define udelay(x)	((void)(x))
#endif
