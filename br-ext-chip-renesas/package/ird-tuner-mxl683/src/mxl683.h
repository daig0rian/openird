/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MaxLinear MXL683 ISDB-T demodulator (with its tuner) on I2C.
 */
#ifndef _IRD_MXL683_H
#define _IRD_MXL683_H

#include <linux/types.h>

struct i2c_adapter;

struct mxl683 {
	struct i2c_adapter *adap;
	u8 addr;
};

/* Is the chip's firmware running? 1 = yes, 0 = no, < 0 = no answer */
int mxl683_alive(struct mxl683 *m);

/*
 * Bring the chip up from power-on: the register preamble, the firmware
 * (an MBIN container, taken at run time from the owner's own device -- it
 * is not part of OpenIRD), and the sequence that follows it.
 */
int mxl683_boot(struct mxl683 *m, const u8 *mbin, size_t len);

/* Check an MBIN container without touching the chip: 0 = well-formed. */
int mxl683_check_mbin(const u8 *mbin, size_t len);

/* Start tuning to a centre frequency.  Lock is polled with mxl683_locked(). */
int mxl683_tune(struct mxl683 *m, u32 freq_khz);

/* 1 = locked, 0 = not (yet), < 0 = I/O error */
int mxl683_locked(struct mxl683 *m);

/* Carrier to noise in 1/1000 dB; 0 when not locked. */
int mxl683_read_cnr(struct mxl683 *m, u32 *mdb);

#endif /* _IRD_MXL683_H */
