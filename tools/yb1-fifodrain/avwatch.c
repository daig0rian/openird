// SPDX-License-Identifier: GPL-2.0-only
/*
 * avwatch - what changes, and in what order, while the POE-BR100-YB1's AV
 * sub-CPU is brought up (docs/specs/poe-br100-yb1-subcpu-control.md §5).
 *
 * Reads only.  An hrtimer in hard-IRQ context samples every period_us, for
 * secs seconds after loading:
 *   - BHIF (0x10000000): +0x110 SUB_INT_EN_0, +0x180 (reset), +0x190 (vector
 *     base), +0x1a0 SW_INT, +0x1a4 SW_INT_EN, +0x1ac (shared memory),
 *     +0x1b0 (doorbell), +0x1b4 (doorbell mask)
 *   - the clock/reset page (0x10111000): +0x030 +0x034 +0x058 +0x3b0
 *   - the sub-CPU's low window (physical 0x2000): only WHETHER the words at
 *     +0x000 and +0x180 are zero -- never their value, which is the vendor
 *     firmware's code
 *   - the shared memory 0x10000..0x1ffff, `sweep' words a tick, round robin
 * and logs every change with the time since loading.  Writes that come closer
 * together than one period land in the same sample and cannot be ordered.
 *
 * /dev/<misc "avwatch">: the log as text, one change a line.
 * Load it before the thing to watch (insmod csd.ko); the summary goes to the
 * kernel log when the time is up.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/delay.h>
#include <linux/vmalloc.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <asm/io.h>
#include <asm/mipsregs.h>

#define BHIF_PHYS	0x10000000ul
#define CLK_PHYS	0x10111000ul
#define LOW_PHYS	0x00002000ul
#define SHM_PHYS	0x00010000ul
#define SHM_WORDS	0x4000		/* 64 KB */
#define LOGN		16384

static unsigned int period_us = 20;
module_param(period_us, uint, 0444);
MODULE_PARM_DESC(period_us, "sampling period in microseconds (default 20)");
static unsigned int secs = 30;
module_param(secs, uint, 0444);
MODULE_PARM_DESC(secs, "stop sampling after this many seconds (default 30)");
static unsigned int sweep = 32;
module_param(sweep, uint, 0444);
MODULE_PARM_DESC(sweep, "shared-memory words read per sample (default 32; "
		 "0 = do not watch it)");

static const u16 bhif_off[] = { 0x110, 0x180, 0x190, 0x1a0, 0x1a4, 0x1ac,
				0x1b0, 0x1b4 };
static const u16 clk_off[] = { 0x030, 0x034, 0x058, 0x3b0 };
static const u16 low_off[] = { 0x000, 0x180 };
#define NB	ARRAY_SIZE(bhif_off)
#define NC	ARRAY_SIZE(clk_off)
#define NL	ARRAY_SIZE(low_off)

static void __iomem *bhif, *clk, *low, *shm;
static u32 bhif_v[NB], clk_v[NC], low_nz[NL], *shadow;
static u32 shm_pos;

struct ev {
	u32 us;		/* since loading */
	u32 addr;	/* physical */
	u32 old, new;	/* for the low window: 0 = zero, 1 = not zero */
};
static struct ev *evlog;
static u32 nev, dropped;

static struct hrtimer timer;
static ktime_t period;
static struct timeval tv0;
static u32 count_per_us, last_count, now_us, now_rem;
static u32 ticks, late, max_gap_us, prev_us, done;

static void log_ev(u32 addr, u32 old, u32 new)
{
	if (nev >= LOGN) {
		dropped++;
		return;
	}
	evlog[nev].us = now_us;
	evlog[nev].addr = addr;
	evlog[nev].old = old;
	evlog[nev].new = new;
	nev++;
}

static enum hrtimer_restart tick(struct hrtimer *t)
{
	u32 c = read_c0_count(), v;
	u64 missed;
	unsigned int i;

	missed = hrtimer_forward_now(t, period);
	if (missed > 1)
		late += (u32)(missed - 1);
	now_rem += c - last_count;
	last_count = c;
	now_us += now_rem / count_per_us;
	now_rem %= count_per_us;
	if (ticks && now_us - prev_us > max_gap_us)
		max_gap_us = now_us - prev_us;
	prev_us = now_us;
	ticks++;

	for (i = 0; i < NB; i++) {
		v = __raw_readl(bhif + bhif_off[i]);
		if (v != bhif_v[i]) {
			log_ev(BHIF_PHYS + bhif_off[i], bhif_v[i], v);
			bhif_v[i] = v;
		}
	}
	for (i = 0; i < NC; i++) {
		v = __raw_readl(clk + clk_off[i]);
		if (v != clk_v[i]) {
			log_ev(CLK_PHYS + clk_off[i], clk_v[i], v);
			clk_v[i] = v;
		}
	}
	for (i = 0; i < NL; i++) {
		v = __raw_readl(low + low_off[i]) != 0;
		if (v != low_nz[i]) {
			log_ev(LOW_PHYS + low_off[i], low_nz[i], v);
			low_nz[i] = v;
		}
	}
	for (i = 0; i < sweep; i++) {
		v = __raw_readl(shm + 4 * shm_pos);
		if (v != shadow[shm_pos]) {
			log_ev(SHM_PHYS + 4 * shm_pos, shadow[shm_pos], v);
			shadow[shm_pos] = v;
		}
		shm_pos = (shm_pos + 1) % SHM_WORDS;
	}

	if (now_us >= secs * 1000000u) {
		done = 1;
		printk(KERN_INFO "avwatch: done after %u us: %u samples, %u late, "
		       "longest gap %u us, %u changes (%u dropped)\n", now_us,
		       ticks, late, max_gap_us, nev, dropped);
		return HRTIMER_NORESTART;
	}
	return HRTIMER_RESTART;
}

static ssize_t aw_read(struct file *file, char __user *buf, size_t count,
		       loff_t *ppos)
{
	char line[96];
	size_t done_bytes = 0;
	u32 i = (u32)*ppos;

	while (i < nev) {
		const struct ev *e = &evlog[i];
		int n;

		if (e->addr >= LOW_PHYS && e->addr < LOW_PHYS + 0x2000)
			n = snprintf(line, sizeof(line), "%10u us  %08x  %s -> %s\n",
				     e->us, e->addr, e->old ? "nonzero" : "zero",
				     e->new ? "nonzero" : "zero");
		else
			n = snprintf(line, sizeof(line),
				     "%10u us  %08x  %08x -> %08x\n", e->us,
				     e->addr, e->old, e->new);
		if (done_bytes + n > count)
			break;
		if (copy_to_user(buf + done_bytes, line, n))
			return -EFAULT;
		done_bytes += n;
		i++;
	}
	*ppos = i;
	return done_bytes;
}

static const struct file_operations aw_fops = {
	.owner	= THIS_MODULE,
	.read	= aw_read,
};

static struct miscdevice aw_dev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "avwatch",
	.fops	= &aw_fops,
};

static int __init avwatch_init(void)
{
	u32 c0, ns, i;
	ktime_t t0;
	int err = -ENOMEM;

	if (period_us < 10 || !secs || sweep > SHM_WORDS)
		return -EINVAL;
	bhif = ioremap_nocache(BHIF_PHYS, 0x1000);
	clk = ioremap_nocache(CLK_PHYS, 0x1000);
	low = ioremap_nocache(LOW_PHYS, 0x2000);
	shm = ioremap_nocache(SHM_PHYS, 4 * SHM_WORDS);
	shadow = vmalloc(4 * SHM_WORDS);
	evlog = vmalloc(LOGN * sizeof(*evlog));
	if (!bhif || !clk || !low || !shm || !shadow || !evlog)
		goto fail;

	c0 = read_c0_count();
	t0 = ktime_get();
	msleep(100);
	ns = (u32)ktime_to_ns(ktime_sub(ktime_get(), t0));
	count_per_us = (read_c0_count() - c0) / (ns / 1000);
	if (!count_per_us)
		count_per_us = 1;

	for (i = 0; i < NB; i++)
		bhif_v[i] = __raw_readl(bhif + bhif_off[i]);
	for (i = 0; i < NC; i++)
		clk_v[i] = __raw_readl(clk + clk_off[i]);
	for (i = 0; i < NL; i++)
		low_nz[i] = __raw_readl(low + low_off[i]) != 0;
	for (i = 0; i < SHM_WORDS; i++)
		shadow[i] = __raw_readl(shm + 4 * i);

	err = misc_register(&aw_dev);
	if (err)
		goto fail;
	tv0 = ktime_to_timeval(ktime_get());	/* = /proc/uptime's clock */
	last_count = read_c0_count();
	period = ktime_set(0, period_us * 1000);
	hrtimer_init(&timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	timer.function = tick;
	timer.cb_mode = HRTIMER_CB_IRQSAFE_UNLOCKED;
	hrtimer_start(&timer, period, HRTIMER_MODE_REL);
	printk(KERN_INFO "avwatch: sampling every %u us for %u s, %u shared-memory "
	       "words a sample, CP0 Count %u/us, misc minor %d, time 0 = "
	       "uptime %ld.%06ld\n", period_us, secs, sweep, count_per_us,
	       aw_dev.minor, (long)tv0.tv_sec, (long)tv0.tv_usec);
	return 0;

fail:
	vfree(evlog);
	vfree(shadow);
	if (shm)
		iounmap(shm);
	if (low)
		iounmap(low);
	if (clk)
		iounmap(clk);
	if (bhif)
		iounmap(bhif);
	return err;
}

static void __exit avwatch_exit(void)
{
	hrtimer_cancel(&timer);
	misc_deregister(&aw_dev);
	vfree(evlog);
	vfree(shadow);
	iounmap(shm);
	iounmap(low);
	iounmap(clk);
	iounmap(bhif);
}

module_init(avwatch_init);
module_exit(avwatch_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("POE-BR100-YB1: watch the AV sub-CPU being brought up");
