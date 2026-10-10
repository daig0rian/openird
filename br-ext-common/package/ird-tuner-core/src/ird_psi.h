/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * OpenIRD tuner core: which PIDs make up a multiplex.
 *
 * For hardware that cannot pass a whole transport stream and has to be told
 * every PID (the first board: 72 PID filters and nothing else).  Feed it the
 * packets of the PAT, the CAT and the PMTs it asks for; it keeps the list of
 * PIDs to carry, by the rules of docs/specs/poe-br100-yb1-ts-fifo.md §6.
 *
 * It reads program-specific information only.  ECM and EMM PIDs are found
 * (they are announced in descriptors) so that their packets can be passed
 * on like any others; nothing in them is ever looked at.
 */
#ifndef _IRD_PSI_H
#define _IRD_PSI_H

#include <linux/types.h>

#define IRD_PSI_MAX_PROG	16
#define IRD_PSI_MAX_ES		40	/* PIDs one PMT may name */
#define IRD_PSI_MAX_CAT		8
#define IRD_PSI_SECT_MAX	1024	/* a PSI section is at most this long */

struct ird_psi_sect {			/* a section being put together */
	u8 active;
	u8 cc;				/* last continuity counter, 0xff = none */
	u16 have;
	u8 buf[IRD_PSI_SECT_MAX];
};

struct ird_psi_prog {
	u16 number;			/* program_number */
	u16 pmt_pid;
	u8 valid;			/* its PMT has been taken */
	u32 crc;			/* that section's CRC_32: tells a new one apart */
	u16 npids;
	u16 pids[IRD_PSI_MAX_ES];
	u8 heavy[IRD_PSI_MAX_ES];	/* PCR or elementary stream */
};

struct ird_psi {
	struct ird_psi_sect sect_pat, sect_cat;
	struct ird_psi_sect sect_pmt[IRD_PSI_MAX_PROG];
	u16 pmt_pid[IRD_PSI_MAX_PROG];	/* the distinct PMT PIDs */
	int npmt;

	u8 pat_valid, cat_valid;
	u32 pat_crc, cat_crc;
	u16 nit_pid;
	struct ird_psi_prog prog[IRD_PSI_MAX_PROG];
	struct ird_psi_prog scratch[IRD_PSI_MAX_PROG];	/* take_pat()'s */
	int nprog;
	u16 cat_pids[IRD_PSI_MAX_CAT];
	int ncat;
};

void ird_psi_reset(struct ird_psi *p);

/* Does the collector want the packets of this PID? */
int ird_psi_wants(const struct ird_psi *p, unsigned int pid);

/* One 188-byte packet of a wanted PID.  Returns 1 when the PID list changed. */
int ird_psi_feed(struct ird_psi *p, const u8 *pkt);

/* The PAT and the PMT of every program it names have been seen. */
int ird_psi_complete(const struct ird_psi *p);

/*
 * The PIDs to carry, without duplicates, in two groups: `light' (tables,
 * ECM, EMM -- few packets) and `heavy' (PCR and elementary streams).  Each
 * array holds up to max entries; the counts come back in *nlight / *nheavy.
 */
void ird_psi_pids(const struct ird_psi *p, u16 *light, int *nlight,
		  u16 *heavy, int *nheavy, int max);

#endif /* _IRD_PSI_H */
