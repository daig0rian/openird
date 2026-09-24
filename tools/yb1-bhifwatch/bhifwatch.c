// SPDX-License-Identifier: GPL-2.0-only
/*
 * bhifwatch - find which MMIO word csd writes to signal the back CPU.
 *
 * The command format is known (docs/specs/poe-br100-yb1-reception-stack.md
 * section 66): entry word 1 is (command << 16) | sequence, words 2 and 3 are
 * the request and response addresses in the data ring, word 5 is 2 and word 6
 * is the total length.  What is not known is how the main CPU says "execute".
 * It cannot be caught by polling from userspace: this SoC has one CPU, so
 * while csd is writing, nothing else runs.
 *
 * MIPS32 has hardware data watchpoints (CP0 WatchLo/WatchHi) and WatchHi has
 * a mask field covering VAddr[11:3], so one watch can cover a whole 4 KB page
 * and then be narrowed by bisection.  When the watch fires, the kernel's
 * do_watch() clears WatchLo for a thread that did not ask for it, so a zero
 * WatchLo is the signal that a store landed in the watched range.
 *
 * Only the ADDRESS is wanted.  The faulting instruction is never read or
 * decoded -- that would be disassembling a vendor binary, which this project
 * does not do (docs/clean-room.md).
 *
 *   insmod bhifwatch.ko addr=0xb0000000 mask=0x1ff ms=5000
 *     addr  KSEG1 address to watch (phys | 0xa0000000 for phys < 512M)
 *     mask  VAddr[11:3] bits to ignore: 0 = one 8 byte slot, 0x1ff = 4 KB
 *     ms    how long to watch before reporting
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <asm/mipsregs.h>
#include <asm/hazards.h>
#include <asm/cpu-features.h>

static unsigned long addr = 0xb0000000;
static unsigned int mask = 0x1ff;
static int ms = 5000;
module_param(addr, ulong, 0444);
module_param(mask, uint, 0444);
module_param(ms, int, 0444);
MODULE_PARM_DESC(addr, "KSEG1 address to watch for stores");
MODULE_PARM_DESC(mask, "VAddr[11:3] bits to ignore (0x1ff = whole 4 KB page)");
MODULE_PARM_DESC(ms, "milliseconds to watch");

static struct task_struct *th;

static int watcher(void *unused)
{
	unsigned long end = jiffies + msecs_to_jiffies(ms);
	unsigned long lo, hi;
	int fired = 0;

	write_c0_watchhi0((1u << 30) | ((mask & 0x1ff) << 3));
	write_c0_watchlo0((addr & ~7UL) | 1);
	back_to_back_c0_hazard();
	printk(KERN_INFO "bhifwatch: armed lo=%08lx hi=%08lx (addr=%08lx mask=%03x)\n",
	       (unsigned long)read_c0_watchlo0(),
	       (unsigned long)read_c0_watchhi0(), addr, mask);

	while (time_before(jiffies, end)) {
		lo = read_c0_watchlo0();
		if (!(lo & 1)) {
			fired = 1;
			break;
		}
		msleep(2);
	}
	lo = read_c0_watchlo0();
	hi = read_c0_watchhi0();
	printk(KERN_INFO "bhifwatch: %s  lo=%08lx hi=%08lx\n",
	       fired ? "FIRED" : "quiet", lo, hi);

	write_c0_watchlo0(0);
	write_c0_watchhi0(0);
	back_to_back_c0_hazard();
	return 0;
}

static int __init bhifwatch_init(void)
{
	if (!cpu_has_watch) {
		printk(KERN_INFO "bhifwatch: this CPU has no watch registers\n");
		return -ENODEV;
	}
	th = kthread_run(watcher, NULL, "bhifwatch");
	if (IS_ERR(th))
		return PTR_ERR(th);
	return 0;
}

static void __exit bhifwatch_exit(void)
{
	write_c0_watchlo0(0);
	write_c0_watchhi0(0);
	printk(KERN_INFO "bhifwatch: unloaded\n");
}

module_init(bhifwatch_init);
module_exit(bhifwatch_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Locate the back-CPU doorbell by MIPS data watchpoint");
