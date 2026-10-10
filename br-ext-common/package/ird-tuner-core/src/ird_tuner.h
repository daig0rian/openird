/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * OpenIRD tuner core: what a board's tuner driver gives the character device.
 *
 * The core (ird_chrdev.c) owns the device node and the px4_drv-compatible
 * ioctls; the driver behind it owns the hardware.  All ops are called with
 * the tuner's mutex held, from process context, and may sleep.
 */
#ifndef _IRD_TUNER_H
#define _IRD_TUNER_H

#include <linux/types.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <asm/atomic.h>

struct ird_tuner;

struct ird_tuner_ops {
	/* the node was opened / closed (one opener at a time) */
	int (*open)(struct ird_tuner *t);
	void (*release)(struct ird_tuner *t);
	/* Tune and wait for lock: 0 = locked, -EAGAIN = no lock.  The stream
	 * of the new multiplex may be read as soon as this returns. */
	int (*tune)(struct ird_tuner *t, u32 freq_khz);
	/* start / stop handing packets to read() */
	int (*set_capture)(struct ird_tuner *t, int on);
	/* carrier to noise, in 1/1000 dB */
	int (*read_cnr)(struct ird_tuner *t, u32 *mdb);
	/* Whole and partial 188-byte packets, like read(2).  Called WITHOUT
	 * the tuner's mutex (it blocks), only while capturing. */
	ssize_t (*read)(struct ird_tuner *t, char __user *buf, size_t count,
			int nonblock);
};

struct ird_tuner {
	const char *name;		/* device node, "irdvideo0" */
	const struct ird_tuner_ops *ops;
	void *priv;

	/* the core's */
	struct miscdevice misc;
	struct mutex lock;
	atomic_t opened;
	int system;			/* enum ptx_system_type asked for */
	int streaming;
};

int ird_tuner_register(struct ird_tuner *t);
void ird_tuner_unregister(struct ird_tuner *t);

#endif /* _IRD_TUNER_H */
