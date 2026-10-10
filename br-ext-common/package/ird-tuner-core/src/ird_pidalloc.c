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

/* Put pid in the first group of `order` that has room; else count a drop. */
static void place(struct ird_pidalloc *o, int per_group, unsigned pid,
		  const int *order, int norder)
{
	int k;

	if (placed(o, pid))
		return;
	for (k = 0; k < norder; k++) {
		int grp = order[k];

		if (o->n[grp] < per_group) {
			o->g[grp][o->n[grp]++] = pid;
			return;
		}
	}
	o->dropped++;
}

void ird_pidalloc_run(struct ird_pidalloc *o, int per_group,
		      const u16 *musttap, int nmust,
		      const u16 *heavy, int nheavy,
		      const u16 *light, int nlight)
{
	static const int only0[] = { 0 }, heavy_order[] = { 1, 0 }, light_order[] = { 0, 1 };
	int i;

	o->n[0] = o->n[1] = 0;
	o->dropped = 0;
	o->musttap_dropped = 0;
	if (per_group > IRD_PIDALLOC_MAX)
		per_group = IRD_PIDALLOC_MAX;

	for (i = 0; i < nmust; i++) {
		int before = o->dropped;

		place(o, per_group, musttap[i], only0, 1);
		if (o->dropped != before)
			o->musttap_dropped++;	/* could not be tapped: enumeration will suffer */
	}
	for (i = 0; i < nheavy; i++)
		place(o, per_group, heavy[i], heavy_order, 2);
	for (i = 0; i < nlight; i++)
		place(o, per_group, light[i], light_order, 2);
}
EXPORT_SYMBOL_GPL(ird_pidalloc_run);

MODULE_LICENSE("GPL");
