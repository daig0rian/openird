/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The character device ABI OpenIRD tuners speak: the one px4_drv gives
 * (include/ptx_ioctl.h there), which recpt1 and recisdb already talk.
 *
 * Only the basic ioctls are implemented.  px4_drv declares a second,
 * "extended" set (PTXT_*, type 0xe7) and answers every one of them with
 * -ENOSYS; so does OpenIRD (by not knowing them).
 *
 * Usable from the kernel and from userland.
 */
#ifndef _IRD_PTX_IOCTL_H
#define _IRD_PTX_IOCTL_H

#include <linux/ioctl.h>

enum ptx_system_type {
	PTX_UNSPECIFIED_SYSTEM = 0x00000000,
	PTX_ISDB_T_SYSTEM = 0x00000010,
	PTX_ISDB_S_SYSTEM = 0x00000020
};

/*
 * freq_no counts channels the way the PT1 driver did.  For ISDB-T:
 *    3..12  CATV C13..C22        93143 + 6000 * freq_no kHz (+2000 for 12)
 *   22..62  CATV C23..C63        93143 + 6000 * freq_no kHz
 *   63..112 UHF  13..62          95143 + 6000 * freq_no kHz
 * (without PTX_SET_SYSTEM_MODE, a terrestrial-only device takes 24..112).
 * slot is added to the frequency in kHz.  For ISDB-S:
 *    0..11  BS   1/3/../23           1049480 + 38360 * freq_no kHz
 *   12..23  CS   ND2/4/../24         1613000 + 40000 * (freq_no - 12) kHz
 * (without PTX_SET_SYSTEM_MODE, a device that receives ISDB-S takes 0..23
 * as these), and slot, cut to 16 bits, picks the transport stream the way
 * tsukumijima's px4_drv does from 0.4.0 on: under 12 = the n-th one of the
 * transponder, 12 and up = its TSID (recisdb's --tsid puts the TSID here;
 * nns779's px4_drv refused 8 and up for BS).  A transponder lists at most
 * 8 streams, so 8..11 name none: -EINVAL, as px4_drv's CXD2856ER tuners
 * answer.
 */
struct ptx_freq {
	int freq_no;
	int slot;
};

#define PTX_SET_CHANNEL		_IOW(0x8d, 0x01, struct ptx_freq)
#define PTX_START_STREAMING	_IO(0x8d, 0x02)
#define PTX_STOP_STREAMING	_IO(0x8d, 0x03)
/* out: 32 bits, in the units of the PT1's demodulator (see ird_chrdev.c);
 * ISDB-S: the 16-bit value of the TC90522's register 0xbc */
#define PTX_GET_CNR		_IOR(0x8d, 0x04, int *)
/* the argument itself is the value: 0 = off, 1 = 11 V, 2 = 15 V */
#define PTX_ENABLE_LNB_POWER	_IOW(0x8d, 0x05, int)
#define PTX_DISABLE_LNB_POWER	_IO(0x8d, 0x06)
/* the argument itself is an enum ptx_system_type */
#define PTX_SET_SYSTEM_MODE	_IOW(0x8d, 0x0b, int)

#endif /* _IRD_PTX_IOCTL_H */
