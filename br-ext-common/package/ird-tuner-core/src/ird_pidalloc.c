// SPDX-License-Identifier: GPL-2.0-only
/*
 * OpenIRD tuner core: PID -> filter-table assignment (see ird_pidalloc.h).
 */
#include <linux/module.h>
#include <linux/kernel.h>

#include "ird_pidalloc.h"

static int placed(const struct ird_pidalloc *o, unsigned pid)
{
	int grp, i;

	for (grp = 0; grp < IRD_PIDALLOC_GROUPS; grp++)
		for (i = 0; i < o->n[grp]; i++)
			if (o->g[grp][i] == pid)
				return 1;
	return 0;
}

/* Which group held pid last time, or -1. */
static int group_of(const struct ird_pidalloc *prev, unsigned pid)
{
	int grp, i;

	if (!prev)
		return -1;
	for (grp = 0; grp < IRD_PIDALLOC_GROUPS; grp++)
		for (i = 0; i < prev->n[grp]; i++)
			if (prev->g[grp][i] == pid)
				return grp;
	return -1;
}

static int allowed(const int *order, int norder, int grp)
{
	int k;

	for (k = 0; k < norder; k++)
		if (order[k] == grp)
			return 1;
	return 0;
}

/*
 * Put pid in a group: its previous group first when that is one of the
 * allowed ones and has room (stickiness), then the allowed groups in
 * preference order; else count a drop.  Returns 1 if it was dropped.
 */
static int place(struct ird_pidalloc *o, int per_group, unsigned pid,
		 const int *order, int norder, int sticky)
{
	int k;

	if (placed(o, pid))
		return 0;
	if (sticky >= 0 && allowed(order, norder, sticky) &&
	    o->n[sticky] < per_group) {
		o->g[sticky][o->n[sticky]++] = pid;
		return 0;
	}
	for (k = 0; k < norder; k++) {
		int grp = order[k];

		if (o->n[grp] < per_group) {
			o->g[grp][o->n[grp]++] = pid;
			return 0;
		}
	}
	o->dropped++;
	return 1;
}

void ird_pidalloc_run(struct ird_pidalloc *o, int per_group,
		      const u16 *musttap, int nmust,
		      const u16 *heavy, int nheavy,
		      const u16 *light, int nlight,
		      const struct ird_pidalloc *prev)
{
	static const int only0[] = { 0 }, heavy_order[] = { 1, 0 }, light_order[] = { 0, 1 };
	struct ird_pidalloc saved;
	int i;

	/* allow prev == o: work against a copy so we can overwrite o */
	if (prev == o) {
		saved = *o;
		prev = &saved;
	}
	o->n[0] = o->n[1] = 0;
	o->dropped = 0;
	o->musttap_dropped = 0;
	if (per_group > IRD_PIDALLOC_MAX)
		per_group = IRD_PIDALLOC_MAX;

	for (i = 0; i < nmust; i++)
		o->musttap_dropped += place(o, per_group, musttap[i], only0, 1,
					    group_of(prev, musttap[i]));
	for (i = 0; i < nheavy; i++)
		place(o, per_group, heavy[i], heavy_order, 2,
		      group_of(prev, heavy[i]));
	for (i = 0; i < nlight; i++)
		place(o, per_group, light[i], light_order, 2,
		      group_of(prev, light[i]));
}
EXPORT_SYMBOL_GPL(ird_pidalloc_run);

MODULE_LICENSE("GPL");
