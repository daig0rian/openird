// SPDX-License-Identifier: GPL-2.0-only
/*
 * yb1-fifodrain / sramspeed - how fast can the main CPU read the AV data SRAM?
 *
 * fifodrain.ko copies each 188-byte packet out of the SRAM with 47 uncached
 * word loads, about 15.6 us a packet (spec §74.3).  A cached read would turn
 * those into a handful of cache-line fills, but only if the SRAM window
 * accepts burst reads -- which nothing documents.  This module measures it
 * and checks the data, then refuses to stay loaded.
 *
 * Steps, each printed before it runs (so a hang shows where it stopped):
 *   1. uncached: read LEN bytes of static SRAM, REPS times
 *   2. cached, ONE line: invalidate 16 bytes at the KSEG0 alias, read 1 word,
 *      compare with the uncached value
 *   3. cached: invalidate LEN bytes, read them, REPS times; compare checksums
 *   4. the same as a packet would be read: 188 bytes at a 4-byte offset
 *
 * Only reads.  The region read (SRAM offset 0 .. LEN) is the header the
 * sub-CPU writes once at Initialise (spec §73.2) and then leaves alone.
 * Output goes to printk (on the rig: /tmp/csd.log).
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/ktime.h>
#include <linux/hrtimer.h>
#include <linux/irqflags.h>
#include <asm/io.h>
#include <asm/addrspace.h>

#define SRAM_PHYS	0x10030000ul
#define LEN		4096
#define REPS		10

static int cached = 1;
module_param(cached, int, 0444);
MODULE_PARM_DESC(cached, "0 = uncached measurement only");

static u32 elapsed_ns(ktime_t t0)
{
	return (u32)ktime_to_ns(ktime_sub(ktime_get(), t0));
}

static int __init sramspeed_init(void)
{
	void __iomem *uc = ioremap_nocache(SRAM_PHYS, LEN);
	const volatile u32 *ck = (const volatile u32 *)CKSEG0ADDR(SRAM_PHYS);
	unsigned long flags;
	u32 sum_uc = 0, sum_ck = 0, ns, v_uc, v_ck;
	int r, i;
	ktime_t t0;

	if (!uc)
		return -ENOMEM;

	printk(KERN_INFO "yb1speed: 1 uncached %d x %d B\n", REPS, LEN);
	local_irq_save(flags);
	t0 = ktime_get();
	for (r = 0; r < REPS; r++)
		for (i = 0; i < LEN; i += 4)
			sum_uc += __raw_readl(uc + i);
	ns = elapsed_ns(t0);
	local_irq_restore(flags);
	printk(KERN_INFO "yb1speed: uncached %u ns total, %u ns per word, "
	       "sum %08x\n", ns, ns / (REPS * LEN / 4), sum_uc / REPS);

	if (!cached)
		goto out;

	printk(KERN_INFO "yb1speed: 2 cached, one line at %p\n", ck);
	dma_cache_inv((unsigned long)ck, 16);
	v_ck = ck[0];
	v_uc = __raw_readl(uc);
	printk(KERN_INFO "yb1speed: one line ok: cached %08x uncached %08x %s\n",
	       v_ck, v_uc, v_ck == v_uc ? "same" : "DIFFERENT");

	printk(KERN_INFO "yb1speed: 3 cached %d x %d B\n", REPS, LEN);
	local_irq_save(flags);
	t0 = ktime_get();
	for (r = 0; r < REPS; r++) {
		dma_cache_inv((unsigned long)ck, LEN);
		for (i = 0; i < LEN / 4; i++)
			sum_ck += ck[i];
	}
	ns = elapsed_ns(t0);
	local_irq_restore(flags);
	printk(KERN_INFO "yb1speed: cached+inv %u ns total, %u ns per word, "
	       "sum %08x %s\n", ns, ns / (REPS * LEN / 4), sum_ck / REPS,
	       sum_ck == sum_uc ? "same" : "DIFFERENT");

	/* as a packet: 188 bytes starting 4 bytes into a line (slot + 0x10 is
	 * 16-byte aligned plus 4 in the FIFO 0 layout: 0x8004 + 0x10) */
	printk(KERN_INFO "yb1speed: 4 packet-shaped reads\n");
	local_irq_save(flags);
	t0 = ktime_get();
	for (r = 0; r < 100; r++) {
		const volatile u32 *p = ck + (0x14 + (r & 7) * 0xdc) / 4;

		dma_cache_inv((unsigned long)p, 188);
		for (i = 0; i < 47; i++)
			sum_ck += p[i];
	}
	ns = elapsed_ns(t0);
	t0 = ktime_get();
	for (r = 0; r < 100; r++) {
		void __iomem *p = uc + 0x14 + (r & 7) * 0xdc;

		for (i = 0; i < 47; i++)
			sum_uc += __raw_readl(p + 4 * i);
	}
	v_uc = elapsed_ns(t0);
	local_irq_restore(flags);
	printk(KERN_INFO "yb1speed: per 188-byte packet: cached+inv %u ns, "
	       "uncached %u ns\n", ns / 100, v_uc / 100);

	/* 5. word by word, cached right after uncached: the region holds a few
	 * counters the sub-CPU bumps while TS flows, so a handful may differ */
	{
		int bad = 0;

		for (i = 0; i < LEN / 4; i++) {
			dma_cache_inv((unsigned long)(ck + i), 4);
			v_uc = __raw_readl(uc + 4 * i);
			v_ck = ck[i];
			if (v_uc != v_ck && bad++ < 8)
				printk(KERN_INFO "yb1speed:   +0x%03x uncached %08x "
				       "cached %08x\n", 4 * i, v_uc, v_ck);
		}
		printk(KERN_INFO "yb1speed: 5 word compare: %d of %d differ\n",
		       bad, LEN / 4);
	}
out:
	iounmap(uc);
	return -EAGAIN;			/* measured; do not stay loaded */
}

module_init(sramspeed_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("POE-BR100-YB1: AV SRAM read speed, uncached vs cached");
