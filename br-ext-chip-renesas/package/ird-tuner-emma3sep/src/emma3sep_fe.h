/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The front end of an EMMA3SEP board's tuner: what differs from board to
 * board behind the shared tuner (emma3sep_tuner.c), which owns the SoC side --
 * the AV sub-CPU's PID filters, the TS input FIFOs, the PSI follower and the
 * character device.
 *
 * One kernel serves the whole family; the boot loader names the board in
 * PRODUCT_FWIDS=, and the tuner picks the front end whose `board' it is.
 * The calls come one at a time, under the tuner's lock, from process context.
 */
#ifndef _EMMA3SEP_FE_H
#define _EMMA3SEP_FE_H

#include <linux/types.h>

struct emma3sep_fe {
	const char *board;	/* the PRODUCT_FWIDS= this front end is on */
	const char *name;	/* the platform device: ird-tuner-<name>.0 */
	/* > 0: the demodulator needs firmware, written by userland to the
	 * device's `firmware' attribute before `start' (at most this long) */
	size_t fw_max;
	/* the PTX_ISDB_*_SYSTEMs it receives, OR-ed (ird_ptx_ioctl.h) */
	u32 systems;

	/* Get the front end going, once: power, firmware, initialisation.
	 * On failure *why says what is missing, in words; on success *how
	 * may say how it went (both ASCII, both optional). */
	int (*start)(const u8 *fw, size_t fw_len, const char **why,
		     const char **how);
	/* Tune: one of its systems, the frequency as the core numbers it
	 * (kHz, ird_chrdev.c) */
	int (*tune)(int system, u32 freq_khz);
	/* for what was tuned last: 1 = locked, 0 = not (yet), < 0 = error */
	int (*locked)(void);
	/* ISDB-S, once locked: pick the transport stream (the core's
	 * stream_id, ird_tuner.h); 0 = it is coming out, -EAGAIN = it is not */
	int (*select_ts)(u16 stream_id);
	/* Carrier to noise of what was tuned last, one of the two (as in
	 * ird_tuner.h): the demodulator's own value, or 1/1000 dB */
	int (*read_cnr_raw)(u32 *raw);
	int (*read_cnr)(u32 *mdb);
};

#ifdef CONFIG_IRD_TUNER_YB1
extern const struct emma3sep_fe emma3sep_fe_yb1;
#endif
#ifdef CONFIG_IRD_TUNER_WRIP
extern const struct emma3sep_fe emma3sep_fe_wrip;
#endif

#endif /* _EMMA3SEP_FE_H */
