/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * OpenIRD tuner core: assign the enumerated PIDs to the two demultiplexer
 * devices' filter tables, within the hardware's limits.
 *
 * The board has two demultiplexer devices, each with a fixed number of PID
 * filters (36 on the first board).  One device's FIFO is drained by the CPU
 * and is the only one the driver can TAP to read program-specific
 * information; the other is drained by DMA and is preferred for the heavy
 * elementary streams.  So the assignment obeys two rules the pure PSI
 * enumeration cannot know about:
 *
 *   - the PIDs the driver must read back (PAT, CAT and every PMT) have to
 *     land on the tapped device, or enumeration stalls;
 *   - when the filters run out, drop in a defined order rather than at random.
 *
 * ird_pidalloc_run() is pure (no hardware, no kernel services beyond types),
 * so it is unit-tested on the host.
 */
#ifndef _IRD_PIDALLOC_H
#define _IRD_PIDALLOC_H

#include <linux/types.h>

#define IRD_PIDALLOC_GROUPS	2
#define IRD_PIDALLOC_MAX	64	/* >= per_group of any supported board */

struct ird_pidalloc {
	u16 g[IRD_PIDALLOC_GROUPS][IRD_PIDALLOC_MAX];
	int n[IRD_PIDALLOC_GROUPS];	/* how many each group holds */
	int dropped;			/* PIDs that found no filter */
	int musttap_dropped;		/* of those, ones that HAD to be tapped */
};

/*
 * Fill *o for per_group filters per device (<= IRD_PIDALLOC_MAX).  Group 0 is
 * the tapped device, group 1 the DMA device.  The three inputs are taken in
 * priority order and de-duplicated against each other:
 *   musttap  PAT, CAT, every PMT PID.  Group 0 only (so they can be read).
 *   heavy    PCR and elementary streams.  Group 1 preferred, then group 0.
 *   light    everything else to carry (SI, ECM, EMM).  Group 0 then group 1.
 * A PID already placed (e.g. a PMT PID that also appears in `light`) is
 * skipped.  PIDs that do not fit are counted in o->dropped.
 *
 * `prev` is the previous result, or NULL.  A PID that was in `prev` is kept on
 * the same device when that is allowed and the device still has room, so that
 * following a PMT/PAT version change (or the staged PAT->PMT->ES discovery, or
 * a re-tune of the same multiplex) moves a PID between devices only when it
 * really has to -- each move is a demultiplexer filter re-point, i.e. a brief
 * gap on that PID.  Priority still wins: a kept low-priority PID is displaced
 * if a higher-priority one needs its slot.  Passing `o` itself as `prev` is
 * allowed (the caller may keep the result and feed it back next time).
 */
void ird_pidalloc_run(struct ird_pidalloc *o, int per_group,
		      const u16 *musttap, int nmust,
		      const u16 *heavy, int nheavy,
		      const u16 *light, int nlight,
		      const struct ird_pidalloc *prev);

#endif /* _IRD_PIDALLOC_H */
