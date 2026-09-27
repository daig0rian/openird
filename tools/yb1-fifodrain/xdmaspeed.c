// SPDX-License-Identifier: GPL-2.0-only
/*
 * yb1-fifodrain / xdmaspeed - can the SoC's memory DMA (XDMA) move bytes out
 * of the AV data SRAM, and what does it cost the CPU?
 *
 * The main CPU pays about 0.23 us for every 32-bit word it reads from the AV
 * SRAM, cached or not (sramspeed.ko; spec §74).  A DMA engine pays the same
 * bus time, but the CPU does not have to wait for it.
 *
 * XDMA is documented by the GPL kernel: include/asm-mips/emma/emma3sep.h names
 * the registers, and drivers/mtd/nand/nand_emma.c programs a memory channel:
 *   MEM_LIST0 + 0x20*ch  source (physical)      MEM_LIST1  destination
 *   MEM_LIST2            size in bytes          MEM_LIST3  setup: bit 28 source
 *                                                          increments, bit 29
 *                                                          destination does
 *   MEM_LIST4            0 (no list)            LIST_ENABLE  1 << ch starts it
 *   MAIN_INT2            bit ch = done (cleared by writing it back as 0)
 * platform.c gives channel 0 to NAND and notes that the vendor application
 * uses channel 1.  Channel 2 is used here; on the stage-C rig its registers
 * still hold power-on garbage, i.e. nothing has touched it since boot.
 *
 * Source is the static SRAM header (written once at Initialise); the copy is
 * compared word by word with what the CPU reads directly.  The destination
 * is a buffer of our own.  Prints, then refuses to stay loaded.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/ktime.h>
#include <linux/hrtimer.h>
#include <linux/slab.h>
#include <linux/irqflags.h>
#include <linux/delay.h>
#include <asm/io.h>
#include <asm/addrspace.h>

#define SRAM_PHYS	0x10030000ul
#define XDMA_PHYS	0x10009000ul
#define LIST_ENABLE	0x088
#define MEM_LIST(ch, n)	(0x100 + 0x20 * (ch) + 4 * (n))
#define MAIN_INT2	0x508
#define CH		2
#define SETUP_INC	((1u << 29) | (1u << 28))	/* both addresses increment */
#define SRC_UM		(1u << 26)
#define DST_UM		(1u << 27)

static void __iomem *x;

static u32 elapsed_ns(ktime_t t0)
{
	return (u32)ktime_to_ns(ktime_sub(ktime_get(), t0));
}

/* One transfer.  Completion is watched two ways, since the GPL driver never
 * shows which one the hardware provides for a channel with interrupts off:
 * the MAIN_INT2 bit, and MEM_LIST2 (the remaining size) reaching zero.
 * Returns ns to the first of them, or 0 on timeout. */
static u32 xfer(u32 src, u32 dst, u32 size, u32 setup, u32 *prog_ns,
		const char **how)
{
	unsigned long flags;
	ktime_t t0;
	u32 ns = 0, w;

	*how = "none";
	local_irq_save(flags);
	w = __raw_readl(x + MAIN_INT2);
	__raw_writel(w & ~(1u << CH), x + MAIN_INT2);
	t0 = ktime_get();
	__raw_writel(src, x + MEM_LIST(CH, 0));
	__raw_writel(dst, x + MEM_LIST(CH, 1));
	__raw_writel(size, x + MEM_LIST(CH, 2));
	__raw_writel(setup, x + MEM_LIST(CH, 3));
	__raw_writel(0, x + MEM_LIST(CH, 4));
	__raw_writel(1u << CH, x + LIST_ENABLE);
	*prog_ns = elapsed_ns(t0);
	while (elapsed_ns(t0) < 5000000) {
		if (__raw_readl(x + MAIN_INT2) & (1u << CH)) {
			ns = elapsed_ns(t0);
			*how = "INT2";
			break;
		}
		if (__raw_readl(x + MEM_LIST(CH, 2)) == 0) {
			ns = elapsed_ns(t0);
			*how = "LIST2=0";
			break;
		}
	}
	local_irq_restore(flags);
	return ns;
}

static void show_regs(const char *when)
{
	printk(KERN_INFO "yb1xdma:   %s: LIST_ENABLE %08x INT2 %08x EN2 %08x "
	       "ch%d %08x %08x %08x %08x %08x\n", when,
	       __raw_readl(x + LIST_ENABLE), __raw_readl(x + MAIN_INT2),
	       __raw_readl(x + 0x518), CH,
	       __raw_readl(x + MEM_LIST(CH, 0)), __raw_readl(x + MEM_LIST(CH, 1)),
	       __raw_readl(x + MEM_LIST(CH, 2)), __raw_readl(x + MEM_LIST(CH, 3)),
	       __raw_readl(x + MEM_LIST(CH, 4)));
}

/* Run one case: src is either the SRAM (uc != NULL) or a DRAM buffer. */
static int one(const char *name, u32 src_phys, const u32 *src_check,
	       void __iomem *uc, u32 *buf, u32 size, u32 setup)
{
	u32 ns, prog, bad = 0, i;
	const char *how;

	memset(buf, 0x5a, 4096);
	dma_cache_wback_inv((unsigned long)buf, 4096);
	printk(KERN_INFO "yb1xdma: %s, %u bytes, setup %08x ...\n", name, size,
	       setup);
	ns = xfer(src_phys, virt_to_phys(buf), size, setup, &prog, &how);
	udelay(100);				/* let a late one land */
	dma_cache_inv((unsigned long)buf, 4096);
	for (i = 0; i < size / 4; i++) {
		u32 want = uc ? __raw_readl(uc + 4 * i) : src_check[i];
		const volatile u32 *ub = (const volatile u32 *)KSEG1ADDR(buf);

		if (buf[i] != want || ub[i] != want)
			bad++;
	}
	printk(KERN_INFO "yb1xdma: %s: %s %u ns via %s (programming %u ns); "
	       "%u of %u words differ; dst[0] %08x\n", name,
	       ns ? "done in" : "TIMEOUT", ns ? ns : 5000000, how, prog, bad,
	       size / 4, buf[0]);
	show_regs("after");
	{
		u32 w = __raw_readl(x + MAIN_INT2);

		__raw_writel(w & ~(1u << CH), x + MAIN_INT2);
	}
	return ns && !bad;
}

static int __init xdmaspeed_init(void)
{
	void __iomem *uc;
	u32 *buf, *src;
	int i;

	x = ioremap_nocache(XDMA_PHYS, 0x1000);
	uc = ioremap_nocache(SRAM_PHYS, 0x1000);
	buf = kmalloc(4096, GFP_KERNEL);
	src = kmalloc(4096, GFP_KERNEL);
	if (!x || !uc || !buf || !src)
		goto out;

	show_regs("before");
	if (__raw_readl(x + LIST_ENABLE) & (1u << CH)) {
		printk(KERN_INFO "yb1xdma: channel %d busy, not touching it\n", CH);
		goto out;
	}
	for (i = 0; i < 1024; i++)
		src[i] = 0x12340000u + i;
	dma_cache_wback_inv((unsigned long)src, 4096);

	/* bits 26/27 are what nand_emma.c sets on the MEMORY side only (NAND read:
	 * dstinc + dst_um; write: srcinc + src_um): read as "this side is DRAM".
	 * A first pass without them moved the addresses but put nothing in DRAM. */
	one("A dram->dram um", virt_to_phys(src), src, NULL, buf, 188,
	    SETUP_INC | SRC_UM | DST_UM);
	one("C sram->dram dst_um", SRAM_PHYS, NULL, uc, buf, 188, SETUP_INC | DST_UM);
	one("C2 sram->dram both um", SRAM_PHYS, NULL, uc, buf, 188,
	    SETUP_INC | SRC_UM | DST_UM);
	one("D sram->dram 1540", SRAM_PHYS, NULL, uc, buf, 1540, SETUP_INC | DST_UM);
	one("E sram->dram 4096", SRAM_PHYS, NULL, uc, buf, 4096, SETUP_INC | DST_UM);
out:
	kfree(src);
	kfree(buf);
	if (uc)
		iounmap(uc);
	if (x)
		iounmap(x);
	return -EAGAIN;
}

module_init(xdmaspeed_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("POE-BR100-YB1: XDMA from the AV SRAM, speed and data check");
