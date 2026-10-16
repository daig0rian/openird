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
	/* Tune and wait for lock: 0 = locked, -EAGAIN = no lock.  system is
	 * PTX_ISDB_T_SYSTEM or PTX_ISDB_S_SYSTEM (one the tuner's system_cap
	 * has).  stream_id is ISDB-S's: 0..7 = the n-th transport stream of
	 * the transponder, 12 and up = a TSID; 0 for ISDB-T.  The stream of
	 * the new multiplex may be read as soon as this returns. */
	int (*tune)(struct ird_tuner *t, int system, u32 freq_khz,
		    u16 stream_id);
	/* start / stop handing packets to read() */
	int (*set_capture)(struct ird_tuner *t, int on);
	/* Carrier to noise, one of the two: the demodulator's own value in the
	 * units px4_drv hands out for the system last tuned (PTX_GET_CNR
	 * passes it on as it is), or, for an ISDB-T-only tuner whose
	 * demodulator speaks dB, 1/1000 dB (the core turns it into those
	 * units). */
	int (*read_cnr_raw)(struct ird_tuner *t, u32 *raw);
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
	/* the PTX_ISDB_*_SYSTEMs it receives, OR-ed; 0 = ISDB-T only */
	u32 system_cap;

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
