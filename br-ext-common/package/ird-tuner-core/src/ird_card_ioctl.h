/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The character device ABI of an OpenIRD card reader (/dev/irdcard0): the
 * card's presence, a reset with its ATR, and APDUs to the card and its
 * responses back.  The reader does the transmission protocol (T=1) itself,
 * so only whole APDUs and responses cross this interface.  Only carries the
 * bytes; what they mean is the application's business.
 *
 * Spoken by OpenIRD's pcsc-lite driver (ifd-irdcard).  See
 * docs/specs/emma3sep-card-reader.md §2.
 *
 * Usable from the kernel and from userland.
 */
#ifndef _IRD_CARD_IOCTL_H
#define _IRD_CARD_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define IRD_CARD_ATR_MAX	33	/* ISO/IEC 7816-3 */
#define IRD_CARD_TX_MAX		261	/* the longest short APDU */
#define IRD_CARD_RX_MAX		256	/* the most the reader hands back */

struct ird_card_status {
	__u32 present;		/* a card is in the slot */
	__u32 powered;		/* reset since it went in, and not powered off */
	__u32 changes;		/* times present changed since boot */
	__u32 reserved;
};

struct ird_card_atr {
	__u32 len;
	__u8 atr[36];		/* IRD_CARD_ATR_MAX, rounded up */
};

struct ird_card_xfer {
	__u32 tx_len;		/* in: bytes of tx */
	__u32 rx_len;		/* out: bytes of rx, status words included */
	__u8 tx[264];		/* IRD_CARD_TX_MAX, rounded up */
	__u8 rx[IRD_CARD_RX_MAX];
};

#define IRD_CARD_IOC_MAGIC	'c'

/* the card's state; never fails */
#define IRD_CARD_STATUS		_IOR(IRD_CARD_IOC_MAGIC, 0xd0, struct ird_card_status)
/* (re)start the card and return its ATR: ENOMEDIUM without a card */
#define IRD_CARD_RESET		_IOR(IRD_CARD_IOC_MAGIC, 0xd1, struct ird_card_atr)
/* note that the card is no longer in use (the card itself stays powered) */
#define IRD_CARD_POWER_OFF	_IO(IRD_CARD_IOC_MAGIC, 0xd2)
/* send an APDU, get the response: ENODEV if not reset, ENOMEDIUM if pulled */
#define IRD_CARD_TRANSMIT	_IOWR(IRD_CARD_IOC_MAGIC, 0xd3, struct ird_card_xfer)

#endif /* _IRD_CARD_IOCTL_H */
