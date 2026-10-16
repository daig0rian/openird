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

	/* Get the front end going, once: power, firmware, initialisation.
	 * On failure *why says what is missing, in words; on success *how
	 * may say how it went (both ASCII, both optional). */
	int (*start)(const u8 *fw, size_t fw_len, const char **why,
		     const char **how);
	/* ISDB-T: the frequency as the core numbers it (kHz, ird_chrdev.c) */
	int (*tune)(u32 freq_khz);
	/* 1 = locked, 0 = not (yet), < 0 = error */
	int (*locked)(void);
	/* carrier to noise, in 1/1000 dB */
	int (*read_cnr)(u32 *mdb);
};

#ifdef CONFIG_IRD_TUNER_YB1
extern const struct emma3sep_fe emma3sep_fe_yb1;
#endif
#ifdef CONFIG_IRD_TUNER_WRIP
extern const struct emma3sep_fe emma3sep_fe_wrip;
#endif

#endif /* _EMMA3SEP_FE_H */
