// SPDX-License-Identifier: GPL-2.0-only
/*
 * POE-BR100-YB1 (BMMPX1301): the tuner behind /dev/irdvideo0.
 *
 * Puts the board's pieces together behind the OpenIRD tuner core:
 *   MXL683 demodulator on I2C bus 1            (mxl683.c)
 *   PID filters of the SoC's AV sub-CPU         (emma3sep_av.c)
 *   TS input FIFOs of the AV data SRAM          (emma3sep_tsfifo.c)
 *   the PIDs of the tuned multiplex             (ird_psi.c)
 *
 * The SoC only passes the PIDs its 72 filters name, so after every tune this
 * driver follows the PAT, the PMTs and the CAT and keeps the filters on
 * everything they announce (docs/specs/poe-br100-yb1-ts-fifo.md §5, §6, §11).
 * Scrambled packets come out scrambled; the descrambler is never opened.
 *
 * Start-up needs three things from the owner's own device that OpenIRD does
 * not ship (docs/specs/poe-br100-yb1-av-command.md §1): the sub-CPU's
 * firmware and the vendor's csd.ko, which userland uses to get the sub-CPU
 * running before this driver is told to start, and the demodulator's
 * firmware, which userland writes to this driver's `firmware' attribute:
 *
 *   /sys/devices/platform/ird-tuner-yb1.0/firmware   write: the MBIN container
 *   /sys/devices/platform/ird-tuner-yb1.0/start      write: bring everything up
 *   /sys/devices/platform/ird-tuner-yb1.0/state      read:  where things stand
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/i2c.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/vmalloc.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/sched.h>

#include "ird_tuner.h"
#include "ird_psi.h"
#include "mxl683.h"
#include "emma3sep_av.h"

#define YB1_I2C_BUS	1
#define YB1_DEMOD_ADDR	0x60
#define FW_MAX		(256 * 1024)
#define NFILT		EMMA3SEP_DEMUX_FILTERS
#define PID_NULL	0x1fff
/* where filter i waits while nothing needs it: a PID no multiplex carries */
#define PID_PARK(i)	(0x1fd0 + (i))

/* The two demultiplexer devices in use.  The CPU-copied FIFO takes the
 * tables (the PSI follower taps them there) and the other light PIDs, the
 * XDMA-copied one the elementary streams. */
enum { G_LIGHT, G_HEAVY, NGROUP };
static const struct {
	int dev;		/* index of the opened device */
	int fifo;
} group[NGROUP] = {
	[G_LIGHT] = { 0, 0 },
	[G_HEAVY] = { 2, 2 },
};
#define FIFO_MASK	((1u << 0) | (1u << 2))
#define FIFO_XDMA	2

struct yb1_filter {
	u32 handle;
	u16 pid;
};

static struct {
	struct mutex lock;	/* the hardware and everything below */
	struct platform_device *pdev;
	struct ird_tuner tuner;
	struct mxl683 demod;
	u8 *fw;
	size_t fw_len;

	int ready;
	const char *why;	/* why not, in words */
	u32 dev[3];
	struct yb1_filter filt[NGROUP][NFILT];
	int nfilt[NGROUP];

	int draining, capturing;
	int tuned;		/* a multiplex is locked and being followed */
	unsigned int gen;	/* counts tunes: tells old tap packets apart */
	u32 freq_khz;
	struct ird_psi *psi;
	int psi_done;
	unsigned int dropped;	/* PIDs that found no filter, last time */
	wait_queue_head_t psi_wq;
	struct task_struct *psi_task;
} yb;

/* --- PID filters ------------------------------------------------------------ */

/* Put filter i of group g on pid, allocating it first if need be. */
static int filter_set(int g, int i, u16 pid)
{
	struct yb1_filter *f = &yb.filt[g][i];
	int ret;

	if (i == yb.nfilt[g]) {
		ret = emma3sep_demux_filter_alloc(yb.dev[group[g].dev], &f->handle);
		if (ret)
			return ret;
		f->pid = PID_NULL + 1;		/* nothing yet */
		yb.nfilt[g]++;
	}
	if (f->pid == pid)
		return 0;
	ret = emma3sep_demux_filter_pid(f->handle, pid);
	if (!ret)
		f->pid = pid;
	return ret;
}

/* Make group g carry exactly want[0..n): keep the filters that already do,
 * give the rest of the PIDs to free ones, and park what is left over. */
static int group_set(int g, const u16 *want, int n)
{
	u8 used[NFILT] = { 0 }, have[NFILT] = { 0 };
	int i, k, ret = 0;

	for (k = 0; k < n; k++)
		for (i = 0; i < yb.nfilt[g]; i++)
			if (!used[i] && yb.filt[g][i].pid == want[k]) {
				used[i] = have[k] = 1;
				break;
			}
	for (k = 0, i = 0; k < n && !ret; k++) {
		if (have[k])
			continue;
		while (i < NFILT && used[i])
			i++;
		if (i == NFILT)
			break;
		ret = filter_set(g, i, want[k]);
		used[i] = 1;
	}
	for (i = 0; i < yb.nfilt[g] && !ret; i++)
		if (!used[i])
			ret = filter_set(g, i, PID_PARK(i));
	return ret;
}

/* Bring the filters in line with what the PSI follower knows. */
static int apply_pids(void)
{
	u16 light[2 * NFILT], heavy[2 * NFILT], want[NGROUP][NFILT];
	int nl, nh, n[NGROUP] = { 0, 0 }, i, ret;
	const struct ird_psi *p = yb.psi;

	ird_psi_pids(p, light, &nl, heavy, &nh, 2 * NFILT);
	yb.dropped = 0;
	/* each kind to its own device first; what does not fit, to the other.
	 * The tables come first in `light', so they always stay on G_LIGHT. */
	for (i = 0; i < nl; i++) {
		int g = n[G_LIGHT] < NFILT ? G_LIGHT : G_HEAVY;

		if (n[g] < NFILT)
			want[g][n[g]++] = light[i];
		else
			yb.dropped++;
	}
	for (i = 0; i < nh; i++) {
		int g = n[G_HEAVY] < NFILT ? G_HEAVY : G_LIGHT;

		if (n[g] < NFILT)
			want[g][n[g]++] = heavy[i];
		else
			yb.dropped++;
	}
	ret = group_set(G_LIGHT, want[G_LIGHT], n[G_LIGHT]);
	if (!ret)
		ret = group_set(G_HEAVY, want[G_HEAVY], n[G_HEAVY]);

	emma3sep_tsfifo_tap_pid(0x0000);
	emma3sep_tsfifo_tap_pid(0x0001);
	for (i = 0; i < p->npmt; i++)
		emma3sep_tsfifo_tap_pid(p->pmt_pid[i]);
	return ret;
}

/* The PSI follower: reads the tapped tables, moves the filters along. */
static int psi_thread(void *unused)
{
	static u8 pkt[188];

	while (!kthread_should_stop()) {
		unsigned int gen = yb.gen;

		if (emma3sep_tsfifo_tap_read(pkt, 200) <= 0)
			continue;
		mutex_lock(&yb.lock);
		if (yb.tuned && gen == yb.gen) {
			if (ird_psi_feed(yb.psi, pkt))
				apply_pids();
			if (!yb.psi_done && ird_psi_complete(yb.psi)) {
				yb.psi_done = 1;
				wake_up(&yb.psi_wq);
			}
		}
		mutex_unlock(&yb.lock);
	}
	return 0;
}

/*
 * The FIFOs' slots can only be found while packets flow (ts-fifo spec §4.2).
 * Null packets do whenever the demodulator is locked, so point one filter of
 * each device at them for the moment.  Done once: the slots stay where they
 * are for as long as the sub-CPU's firmware runs.
 */
static int learn_slots(void)
{
	int g, ret = 0;

	for (g = 0; g < NGROUP && !ret; g++)
		ret = filter_set(g, 0, PID_NULL);
	if (!ret) {
		msleep(20);
		ret = emma3sep_tsfifo_find_slots(FIFO_MASK);
	}
	/* apply_pids() takes the two filters back */
	return ret;
}

/* --- tuner ops ---------------------------------------------------------------- */

static int yb1_open(struct ird_tuner *t)
{
	return yb.ready ? 0 : -ENODEV;
}

static void stop_following(void)
{
	yb.tuned = 0;
	yb.gen++;
	if (yb.draining) {
		emma3sep_tsfifo_stop();
		yb.draining = 0;
	}
}

static void yb1_release(struct ird_tuner *t)
{
	mutex_lock(&yb.lock);
	stop_following();
	mutex_unlock(&yb.lock);
}

static int yb1_tune(struct ird_tuner *t, u32 freq_khz)
{
	int ret, i, locked = 0;

	mutex_lock(&yb.lock);
	yb.tuned = 0;
	yb.gen++;
	ret = mxl683_tune(&yb.demod, freq_khz);
	/* lock takes 1.4 to 1.7 s where there is a signal; give it 3 s */
	for (i = 0; i < 60 && !ret && !locked; i++) {
		msleep(50);
		locked = mxl683_locked(&yb.demod);
		if (locked < 0)
			ret = locked;
	}
	if (!ret && !locked)
		ret = -EAGAIN;
	if (ret)
		goto out;
	yb.freq_khz = freq_khz;

	if (!emma3sep_tsfifo_slots(group[G_LIGHT].fifo) ||
	    !emma3sep_tsfifo_slots(group[G_HEAVY].fifo)) {
		if (yb.draining) {
			emma3sep_tsfifo_stop();
			yb.draining = 0;
		}
		ret = learn_slots();
		if (ret)
			goto out;
	}

	/* the new multiplex: start from the fixed PIDs and let the tables,
	 * as they come in, name the rest */
	ird_psi_reset(yb.psi);
	emma3sep_tsfifo_tap_clear();
	yb.psi_done = 0;
	ret = apply_pids();
	if (ret)
		goto out;
	if (!yb.draining) {
		ret = emma3sep_tsfifo_start(FIFO_MASK, FIFO_XDMA);
		if (ret)
			goto out;
		yb.draining = 1;
	}
	if (yb.capturing)
		emma3sep_tsfifo_capture(1);	/* drop the old multiplex's */
	yb.tuned = 1;
out:
	mutex_unlock(&yb.lock);
	if (ret)
		return ret;
	/* The stream is complete only once the PMTs have been read (they come
	 * about ten times a second).  Give that a moment, so that a reader
	 * that starts right away gets whole programs from the start. */
	wait_event_timeout(yb.psi_wq, yb.psi_done, HZ);
	return 0;
}

static int yb1_set_capture(struct ird_tuner *t, int on)
{
	yb.capturing = on;
	emma3sep_tsfifo_capture(on);
	return 0;
}

static int yb1_read_cnr(struct ird_tuner *t, u32 *mdb)
{
	int ret;

	mutex_lock(&yb.lock);
	ret = mxl683_read_cnr(&yb.demod, mdb);
	mutex_unlock(&yb.lock);
	return ret;
}

static ssize_t yb1_read(struct ird_tuner *t, char __user *buf, size_t count,
			int nonblock)
{
	return emma3sep_tsfifo_read(buf, count, nonblock);
}

static const struct ird_tuner_ops yb1_ops = {
	.open		= yb1_open,
	.release	= yb1_release,
	.tune		= yb1_tune,
	.set_capture	= yb1_set_capture,
	.read_cnr	= yb1_read_cnr,
	.read		= yb1_read,
};

/* --- start-up ----------------------------------------------------------------- */

static int yb1_start(void)
{
	int ret, alive, fresh;

	if (yb.ready)
		return 0;
	if (!emma3sep_av_up()) {
		yb.why = "the AV sub-CPU is not running (start it with the vendor csd.ko)";
		return -ENODEV;
	}
	if (emma3sep_av_shared()) {
		yb.why = "the vendor csd.ko is still loaded (unload it first)";
		return -EBUSY;
	}
	if (!yb.demod.adap)
		yb.demod.adap = i2c_get_adapter(YB1_I2C_BUS);
	if (!yb.demod.adap) {
		yb.why = "no I2C bus 1";
		return -ENODEV;
	}
	/* no answer counts as "not running": the cold start that follows
	 * finds out soon enough whether the chip is there at all */
	alive = mxl683_alive(&yb.demod) > 0;
	if (!alive) {
		if (!yb.fw_len) {
			yb.why = "the demodulator needs its firmware (write it to `firmware')";
			return -ENOENT;
		}
		ret = mxl683_boot(&yb.demod, yb.fw, yb.fw_len);
		if (ret) {
			yb.why = ret == -EINVAL ? "that is not a firmware container" :
				 "the demodulator did not start";
			return ret;
		}
	}
	ret = emma3sep_av_attach(&fresh);
	if (!ret && !fresh)
		ret = -EBUSY;
	if (ret) {
		yb.why = "the AV firmware has been used since it started (restart it)";
		return ret;
	}
	ret = emma3sep_demux_init(yb.dev);
	if (ret) {
		yb.why = "the demultiplexer did not initialise";
		return ret;
	}
	yb.why = NULL;
	yb.ready = 1;
	printk(KERN_INFO "ird-tuner-yb1: ready (demodulator firmware %s)\n",
	       alive ? "was running" : "loaded");
	return 0;
}

static ssize_t start_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	int ret;

	mutex_lock(&yb.lock);
	ret = yb1_start();
	mutex_unlock(&yb.lock);
	return ret ? ret : count;
}

static ssize_t state_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	int len = 0, g, i;

	mutex_lock(&yb.lock);
	len += sprintf(buf + len, "state %s\n", yb.ready ? "ready" : "stopped");
	if (yb.why)
		len += sprintf(buf + len, "reason %s\n", yb.why);
	len += sprintf(buf + len, "firmware %u bytes\n", (unsigned int)yb.fw_len);
	if (yb.ready) {
		len += sprintf(buf + len, "tuned %d freq_khz %u programs %d "
			       "tables_read %d pids_without_filter %u\n",
			       yb.tuned, yb.freq_khz, yb.psi->nprog,
			       yb.psi_done, yb.dropped);
		for (g = 0; g < NGROUP; g++) {
			len += sprintf(buf + len, "fifo %d pids", group[g].fifo);
			for (i = 0; i < yb.nfilt[g]; i++)
				if (yb.filt[g][i].pid != PID_PARK(i))
					len += sprintf(buf + len, " %04x",
						       yb.filt[g][i].pid);
			len += sprintf(buf + len, "\n");
		}
		len += emma3sep_tsfifo_show(buf + len);
	}
	mutex_unlock(&yb.lock);
	return len;
}

static DEVICE_ATTR(start, 0200, NULL, start_store);
static DEVICE_ATTR(state, 0444, state_show, NULL);

static ssize_t fw_write(struct kobject *kobj, struct bin_attribute *attr,
			char *buf, loff_t off, size_t count)
{
	if (off < 0 || off + count > FW_MAX)
		return -EFBIG;
	mutex_lock(&yb.lock);
	if (off == 0)
		yb.fw_len = 0;			/* a new upload */
	if (off != yb.fw_len) {
		mutex_unlock(&yb.lock);
		return -EINVAL;			/* in one go, front to back */
	}
	memcpy(yb.fw + off, buf, count);
	yb.fw_len = off + count;
	mutex_unlock(&yb.lock);
	return count;
}

static struct bin_attribute fw_attr = {
	.attr	= { .name = "firmware", .mode = 0200 },
	.write	= fw_write,
};

static int __init yb1_init(void)
{
	int ret;

	/* one kernel serves the boards of this family; only this one has
	 * this tuner (the boot loader names the board) */
	if (!strstr(saved_command_line, "PRODUCT_FWIDS=PIX_AIRTUNER_LITE"))
		return -ENODEV;

	mutex_init(&yb.lock);
	init_waitqueue_head(&yb.psi_wq);
	yb.demod.addr = YB1_DEMOD_ADDR;
	yb.why = "not started";
	yb.fw = vmalloc(FW_MAX);
	yb.psi = vmalloc(sizeof(*yb.psi));
	if (!yb.fw || !yb.psi)
		return -ENOMEM;
	ird_psi_reset(yb.psi);
	ret = emma3sep_tsfifo_init();
	if (ret)
		return ret;

	yb.pdev = platform_device_register_simple("ird-tuner-yb1", 0, NULL, 0);
	if (IS_ERR(yb.pdev))
		return PTR_ERR(yb.pdev);
	ret = device_create_file(&yb.pdev->dev, &dev_attr_start);
	if (!ret)
		ret = device_create_file(&yb.pdev->dev, &dev_attr_state);
	if (!ret)
		ret = sysfs_create_bin_file(&yb.pdev->dev.kobj, &fw_attr);
	if (ret)
		return ret;

	yb.psi_task = kthread_run(psi_thread, NULL, "ird-psi");
	if (IS_ERR(yb.psi_task))
		return PTR_ERR(yb.psi_task);

	yb.tuner.name = "irdvideo0";
	yb.tuner.ops = &yb1_ops;
	ret = ird_tuner_register(&yb.tuner);
	if (ret)
		return ret;
	printk(KERN_INFO "ird-tuner-yb1: /dev/irdvideo0 registered; waiting to "
	       "be started\n");
	return 0;
}
/* after the I2C bus driver, wherever the link order puts us */
late_initcall(yb1_init);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("POE-BR100-YB1 tuner (MXL683 + EMMA3SEP TS input FIFOs)");
