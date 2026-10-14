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
 *   - the shared memory 0x10000..0x1ffff (or the part shm_lo..shm_hi of it),
 *     `sweep' words a tick, round robin
 *   - with cmdlog=1, the commands to the sub-CPU: the whole entry table
 *     (0x10400, 32 entries of 32 bytes) every tick, and for each entry that
 *     is taken into use the request it points to in the data ring; the
 *     response is copied when the entry is used again, 31 commands later, or
 *     when the time is up.  The sweep is too slow to follow a burst of
 *     commands and a fast sweep leaves the system no time, hence this mode
 *     (use it with sweep=0 and a period of 250 us or so).
 *   - with evcmd=<command ID> (and cmdlog=1), what the sub-CPU leaves for an
 *     event: when that command is seen, the sampler stays in its interrupt
 *     handler -- nothing else runs on this CPU, so nothing consumes the event
 *     -- until SW_INT bit evbit rises or evwait_us is over, and then logs
 *     every word of the low shared areas that changed meanwhile.  The log
 *     has a marker before (address fffffff1) and after the wait (fffffff0:
 *     "old" = microseconds until SW_INT bit 0 rose, "new" = SW_INT).
 *   - with xphys=/xwords=, up to four more ranges of physical memory, every
 *     word of them every tick -- for a caller's parameter block, which is
 *     rewritten for each call and so shows only the last one afterwards.
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
#define LOGN		262144
#define ENT_OFF		0x400		/* the entry table, in the shared memory */
#define NENT		32
#define CMD_WORDS	128		/* copied of a request or a response */

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
/*
 * Watching only part of the shared memory makes a full pass short enough to
 * catch every command: the entry table and the start of the data ring
 * (0x400..0x4000) are 3840 words, 3 ms at 64 words every 50 us.
 */
static unsigned int shm_lo;
module_param(shm_lo, uint, 0444);
MODULE_PARM_DESC(shm_lo, "first shared-memory byte offset to watch (default 0)");
static unsigned int shm_hi = 4 * SHM_WORDS;
module_param(shm_hi, uint, 0444);
MODULE_PARM_DESC(shm_hi, "end of the watched shared-memory range in bytes "
		 "(default 0x10000)");

static unsigned int cmdlog;
module_param(cmdlog, uint, 0444);
MODULE_PARM_DESC(cmdlog, "1 = follow the commands: the entry table every "
		 "sample, and each command's request and response");

#define NX		4
#define X_WORDS		1024		/* in all */
static unsigned long xphys[NX];
static unsigned int nxphys;
module_param_array(xphys, ulong, &nxphys, 0444);
MODULE_PARM_DESC(xphys, "more physical ranges to watch, every word every sample");
static unsigned int xwords[NX];
static unsigned int nxwords;
module_param_array(xwords, uint, &nxwords, 0444);
MODULE_PARM_DESC(xwords, "their lengths in 32-bit words");
static void __iomem *xmap[NX];
static u32 *xshadow[NX];

static unsigned int evcmd;
module_param(evcmd, uint, 0444);
MODULE_PARM_DESC(evcmd, "command ID after which to wait for an event (0 = off)");
static unsigned int evbit = 24;
module_param(evbit, uint, 0444);
MODULE_PARM_DESC(evbit, "SW_INT bit of that event (default 24)");
static unsigned int evskip;
module_param(evskip, uint, 0444);
MODULE_PARM_DESC(evskip, "let this many commands with that ID pass first");
static unsigned int evwait_us = 200000;
module_param(evwait_us, uint, 0444);
MODULE_PARM_DESC(evwait_us, "give up after this long (default 200000)");
static unsigned int evcount = 1;
module_param(evcount, uint, 0444);
MODULE_PARM_DESC(evcount, "how many times (default 1)");
static unsigned int ev_armed;

/*
 * Compared only around an event.  The first 512 bytes of the sub-CPU's low
 * window hold code and are left out.
 */
static const struct {
	unsigned long phys;
	unsigned int words;
} evr[] = {
	{ 0x00002200, (0x10000 - 0x2200) / 4 },
	{ 0x00020000, 0x10000 / 4 },
};
#define NEVR	ARRAY_SIZE(evr)
static void __iomem *evr_map[NEVR];
static u32 *evr_shadow[NEVR];

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

/* request [a, b) and response [b, c) of the command in each entry (KSEG1) */
static struct {
	u32 a, b, c;
	u8 fresh, live;
} ent[NENT];

static void log_ring(u32 a, u32 b)
{
	u32 n;

	for (n = 0; a + 4 <= b && n < CMD_WORDS; a += 4, n++) {
		u32 off = (a & 0x1fffffff) - SHM_PHYS;

		if (off >= 4 * SHM_WORDS)
			break;
		shadow[off / 4] = __raw_readl(shm + off);
		log_ev(SHM_PHYS + off, 0, shadow[off / 4]);
	}
}

static void compare_all(void)
{
	unsigned int i, j;
	u32 v;

	for (j = 0; j < SHM_WORDS; j++) {
		v = __raw_readl(shm + 4 * j);
		if (v != shadow[j]) {
			log_ev(SHM_PHYS + 4 * j, shadow[j], v);
			shadow[j] = v;
		}
	}
	for (i = 0; i < NEVR; i++)
		for (j = 0; j < evr[i].words; j++) {
			v = __raw_readl(evr_map[i] + 4 * j);
			if (v != evr_shadow[i][j]) {
				log_ev(evr[i].phys + 4 * j, evr_shadow[i][j], v);
				evr_shadow[i][j] = v;
			}
		}
}

static void catch_up(void)
{
	u32 c = read_c0_count();

	now_rem += c - last_count;
	last_count = c;
	now_us += now_rem / count_per_us;
	now_rem %= count_per_us;
}

/* In the timer interrupt: interrupts are off, so the event stays unserved. */
static void wait_for_event(void)
{
	u32 c0, sw, t_api = 0, limit = evwait_us * count_per_us;

	log_ev(0xfffffff1, 0, __raw_readl(bhif + 0x1a0));
	compare_all();
	catch_up();
	c0 = read_c0_count();
	for (;;) {
		sw = __raw_readl(bhif + 0x1a0);
		if ((sw & 1) && !t_api)
			t_api = (read_c0_count() - c0) / count_per_us + 1;
		if ((sw & (1u << evbit)) || read_c0_count() - c0 > limit)
			break;
	}
	catch_up();
	log_ev(0xfffffff0, t_api, sw);
	compare_all();
	catch_up();
}

static void follow_cmds(void)
{
	unsigned int i, j;
	u32 v;

	for (i = 0; i < NENT; i++) {
		u32 w = (ENT_OFF + 32 * i) / 4;

		if (ent[i].fresh) {
			/* seen changing a sample ago: all its words are there now */
			ent[i].fresh = 0;
			ent[i].a = __raw_readl(shm + 4 * (w + 2));
			ent[i].b = __raw_readl(shm + 4 * (w + 3));
			ent[i].c = ent[i].a + __raw_readl(shm + 4 * (w + 6));
			ent[i].live = 1;
			log_ring(ent[i].a, ent[i].b);
		}
		v = __raw_readl(shm + 4 * (w + 1));
		if (v != shadow[w + 1]) {
			/*
			 * Used again: first what the last command left, the
			 * request too (a response can come back in it).
			 */
			if (ent[i].live)
				log_ring(ent[i].a, ent[i].c);
			ent[i].live = 0;
			ent[i].fresh = 1;
			if (evcmd && (v >> 16) == evcmd && evcount) {
				if (evskip) {
					evskip--;
				} else {
					evcount--;
					ev_armed = 1;
				}
			}
		}
		for (j = 0; j < 8; j++) {
			v = __raw_readl(shm + 4 * (w + j));
			if (v != shadow[w + j]) {
				log_ev(SHM_PHYS + 4 * (w + j), shadow[w + j], v);
				shadow[w + j] = v;
			}
		}
	}
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
		if (++shm_pos >= shm_hi / 4)
			shm_pos = shm_lo / 4;
	}

	if (cmdlog)
		follow_cmds();
	if (ev_armed) {
		ev_armed = 0;
		wait_for_event();
	}
	for (i = 0; i < nxphys; i++) {
		unsigned int j;

		for (j = 0; j < xwords[i]; j++) {
			v = __raw_readl(xmap[i] + 4 * j);
			if (v != xshadow[i][j]) {
				log_ev(xphys[i] + 4 * j, xshadow[i][j], v);
				xshadow[i][j] = v;
			}
		}
	}

	if (now_us >= secs * 1000000u) {
		done = 1;
		if (cmdlog)
			for (i = 0; i < NENT; i++)
				if (ent[i].live)
					log_ring(ent[i].a, ent[i].c);
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

		if (e->addr >= LOW_PHYS && e->addr < LOW_PHYS + 0x200)
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
	u32 c0, ns, i, j, xtotal = 0;
	ktime_t t0;
	int err = -ENOMEM;

	if (period_us < 10 || !secs || sweep > SHM_WORDS || (shm_lo & 3) ||
	    (shm_hi & 3) || shm_lo >= shm_hi || shm_hi > 4 * SHM_WORDS)
		return -EINVAL;
	if (nxphys != nxwords)
		return -EINVAL;
	for (i = 0; i < nxphys; i++) {
		xtotal += xwords[i];
		if ((xphys[i] & 3) || !xwords[i] || xtotal > X_WORDS)
			return -EINVAL;
	}
	shm_pos = shm_lo / 4;
	bhif = ioremap_nocache(BHIF_PHYS, 0x1000);
	clk = ioremap_nocache(CLK_PHYS, 0x1000);
	low = ioremap_nocache(LOW_PHYS, 0x2000);
	shm = ioremap_nocache(SHM_PHYS, 4 * SHM_WORDS);
	shadow = vmalloc(4 * SHM_WORDS);
	evlog = vmalloc(LOGN * sizeof(*evlog));
	if (!bhif || !clk || !low || !shm || !shadow || !evlog)
		goto fail;
	for (i = 0; evcmd && i < NEVR; i++) {
		evr_map[i] = ioremap_nocache(evr[i].phys, 4 * evr[i].words);
		evr_shadow[i] = vmalloc(4 * evr[i].words);
		if (!evr_map[i] || !evr_shadow[i])
			goto fail;
		for (j = 0; j < evr[i].words; j++)
			evr_shadow[i][j] = __raw_readl(evr_map[i] + 4 * j);
	}
	for (i = 0; i < nxphys; i++) {
		xmap[i] = ioremap_nocache(xphys[i], 4 * xwords[i]);
		xshadow[i] = vmalloc(4 * xwords[i]);
		if (!xmap[i] || !xshadow[i])
			goto fail;
	}

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
	/*
	 * With a narrowed range, start the log with what is there already, so
	 * that replaying it gives the whole content at any later time.
	 */
	if (shm_lo || shm_hi < 4 * SHM_WORDS)
		for (i = shm_lo / 4; i < shm_hi / 4; i++)
			if (shadow[i])
				log_ev(SHM_PHYS + 4 * i, 0, shadow[i]);
	if (cmdlog)
		for (i = ENT_OFF / 4; i < (ENT_OFF + 32 * NENT) / 4; i++)
			if (shadow[i])
				log_ev(SHM_PHYS + 4 * i, 0, shadow[i]);
	for (i = 0; i < nxphys; i++)
		for (j = 0; j < xwords[i]; j++) {
			xshadow[i][j] = __raw_readl(xmap[i] + 4 * j);
			log_ev(xphys[i] + 4 * j, 0, xshadow[i][j]);
		}

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
	for (i = 0; i < NEVR; i++) {
		vfree(evr_shadow[i]);
		if (evr_map[i])
			iounmap(evr_map[i]);
	}
	for (i = 0; i < NX; i++) {
		vfree(xshadow[i]);
		if (xmap[i])
			iounmap(xmap[i]);
	}
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
	unsigned int i;

	hrtimer_cancel(&timer);
	misc_deregister(&aw_dev);
	for (i = 0; i < nxphys; i++) {
		vfree(xshadow[i]);
		iounmap(xmap[i]);
	}
	for (i = 0; i < NEVR; i++) {
		vfree(evr_shadow[i]);
		if (evr_map[i])
			iounmap(evr_map[i]);
	}
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
