// SPDX-License-Identifier: GPL-2.0-only
/*
 * EMMA3SEP boards: the tuner behind /dev/irdvideo0.
 *
 * Puts the board's pieces together behind the OpenIRD tuner core:
 *   the board's front end                       (emma3sep_fe.h: fe_*.c)
 *   PID filters of the SoC's AV sub-CPU         (emma3sep_av.c)
 *   TS input FIFOs of the AV data SRAM          (emma3sep_tsfifo.c)
 *   the PIDs of the tuned multiplex             (ird_psi.c)
 *
 * The SoC only passes the PIDs its 72 filters name, so after every tune this
 * driver follows the PAT, the PMTs and the CAT and keeps the filters on
 * everything they announce (docs/specs/poe-br100-yb1-ts-fifo.md §5, §6, §11).
 * Scrambled packets come out scrambled; the descrambler is never opened.
 *
 * One kernel serves the family; the boot loader names the board in
 * PRODUCT_FWIDS=, and that picks the front end.  On a board with none of
 * them this driver does nothing.
 *
 * Start-up needs things from the owner's own device that OpenIRD does not
 * ship (docs/specs/poe-br100-yb1-av-command.md §1): the sub-CPU's firmware
 * and the vendor's csd.ko, which userland uses to get the sub-CPU running
 * before this driver is told to start, and on some boards the demodulator's
 * firmware, which userland writes to the `firmware' attribute:
 *
 *   /sys/devices/platform/ird-tuner-<board>.0/firmware  write: (if it has one)
 *   /sys/devices/platform/ird-tuner-<board>.0/start     write: bring everything up
 *   /sys/devices/platform/ird-tuner-<board>.0/state     read:  where things stand
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/vmalloc.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/string.h>

#include "ird_tuner.h"
#include "ird_psi.h"
#include "ird_pidalloc.h"
#include "emma3sep_fe.h"
#include "emma3sep_av.h"

#define NFILT		EMMA3SEP_DEMUX_FILTERS
#define PID_NULL	0x1fff
/* where filter i waits while nothing needs it: a PID no multiplex carries */
#define PID_PARK(i)	(0x1fd0 + (i))
/* a lock takes up to about 1.7 s where there is a signal; give it 3 s */
#define LOCK_POLL_MS	50
#define LOCK_POLLS	60

static const struct emma3sep_fe *const fronts[] = {
#ifdef CONFIG_IRD_TUNER_YB1
	&emma3sep_fe_yb1,
#endif
#ifdef CONFIG_IRD_TUNER_WRIP
	&emma3sep_fe_wrip,
#endif
};

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

struct et_filter {
	u32 handle;
	u16 pid;
};

static struct {
	struct mutex lock;	/* the hardware and everything below */
	const struct emma3sep_fe *fe;
	struct platform_device *pdev;
	struct ird_tuner tuner;
	u8 *fw;
	size_t fw_len;

	int ready;
	const char *why;	/* why not, in words */
	u32 dev[3];
	struct et_filter filt[NGROUP][NFILT];
	int nfilt[NGROUP];

	int draining, capturing;
	int tuned;		/* a multiplex is locked and being followed */
	u32 freq_khz;
	struct ird_psi *psi;
	int psi_done;
	unsigned int dropped;	/* PIDs that found no filter, last time */
	unsigned int filter_writes;	/* demultiplexer filter re-points, running */
	struct ird_pidalloc alloc;	/* last placement, fed back for stickiness */
	wait_queue_head_t psi_wq;
	struct task_struct *psi_task;
} et;

/* --- PID filters ------------------------------------------------------------ */

/* Put filter i of group g on pid, allocating it first if need be. */
static int filter_set(int g, int i, u16 pid)
{
	struct et_filter *f = &et.filt[g][i];
	int ret;

	if (i == et.nfilt[g]) {
		ret = emma3sep_demux_filter_alloc(et.dev[group[g].dev], &f->handle);
		if (ret)
			return ret;
		f->pid = PID_NULL + 1;		/* nothing yet */
		et.nfilt[g]++;
	}
	if (f->pid == pid)
		return 0;			/* already there: no gap on this PID */
	ret = emma3sep_demux_filter_pid(f->handle, pid);
	if (!ret) {
		f->pid = pid;
		et.filter_writes++;		/* a real re-point = a brief gap */
	}
	return ret;
}

/* Make group g carry exactly want[0..n): keep the filters that already do,
 * give the rest of the PIDs to free ones, and park what is left over. */
static int group_set(int g, const u16 *want, int n)
{
	u8 used[NFILT] = { 0 }, have[NFILT] = { 0 };
	int i, k, ret = 0;

	for (k = 0; k < n; k++)
		for (i = 0; i < et.nfilt[g]; i++)
			if (!used[i] && et.filt[g][i].pid == want[k]) {
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
	for (i = 0; i < et.nfilt[g] && !ret; i++)
		if (!used[i])
			ret = filter_set(g, i, PID_PARK(i));
	return ret;
}

/* Bring the filters in line with what the PSI follower knows.
 *
 * G_LIGHT is the device whose FIFO the drainer taps, so PAT/CAT/PMT must
 * land there or the follower never sees the tables.  G_HEAVY is the XDMA
 * device, preferred for the elementary streams.  ird_pidalloc_run() enforces
 * both, and drops in a defined order if 72 filters are not enough. */
static int apply_pids(void)
{
	u16 light[2 * NFILT], heavy[2 * NFILT], must[2 + IRD_PSI_MAX_PROG];
	int nl, nh, nm = 0, i, ret;
	const struct ird_psi *p = et.psi;
	struct ird_pidalloc a;

	ird_psi_pids(p, light, &nl, heavy, &nh, 2 * NFILT);
	must[nm++] = 0x0000;			/* PAT  - must be tapped */
	must[nm++] = 0x0001;			/* CAT  - must be tapped */
	for (i = 0; i < p->npmt; i++)		/* every PMT - must be tapped */
		must[nm++] = p->pmt_pid[i];

	/* feed last time's placement back so surviving PIDs keep their device
	 * across the staged discovery and version changes (fewer re-points) */
	ird_pidalloc_run(&a, NFILT, must, nm, heavy, nh, light, nl, &et.alloc);
	et.alloc = a;
	et.dropped = a.dropped;
	ret = group_set(G_LIGHT, a.g[0], a.n[0]);
	if (!ret)
		ret = group_set(G_HEAVY, a.g[1], a.n[1]);

	/* tap exactly the tables, now guaranteed to be on the tapped device */
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
		int changed, done;

		if (emma3sep_tsfifo_tap_read(pkt, 200) <= 0)
			continue;
		mutex_lock(&et.lock);
		if (et.tuned) {
			changed = ird_psi_feed(et.psi, pkt);
			if (changed)
				apply_pids();	/* re-point only the delta */
			done = ird_psi_complete(et.psi);
			/* wake the tuner's wait on any table change or when the
			 * current multiplex's completeness flips (a new PAT makes
			 * it incomplete until its PMTs are read) */
			if (changed || done != et.psi_done) {
				et.psi_done = done;
				wake_up(&et.psi_wq);
			}
		}
		mutex_unlock(&et.lock);
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

static int et_open(struct ird_tuner *t)
{
	return et.ready ? 0 : -ENODEV;
}

static void stop_following(void)
{
	et.tuned = 0;
	if (et.draining) {
		emma3sep_tsfifo_stop();
		et.draining = 0;
	}
}

static void et_release(struct ird_tuner *t)
{
	mutex_lock(&et.lock);
	stop_following();
	mutex_unlock(&et.lock);
}

static int et_tune(struct ird_tuner *t, u32 freq_khz)
{
	int ret = 0, i, locked = 0, was_tuned, same;
	u32 old_crc;

	mutex_lock(&et.lock);
	was_tuned = et.tuned;
	same = was_tuned && freq_khz == et.freq_khz;

	/* Already locked onto this exact channel and following it: the filter
	 * table is already right, so do nothing -- no re-lock, no re-point.
	 * (This is what makes a re-tune of the same channel free of glitches.) */
	if (same && et.fe->locked() == 1) {
		mutex_unlock(&et.lock);
		return 0;
	}

	ret = et.fe->tune(freq_khz);
	for (i = 0; i < LOCK_POLLS && !ret && !locked; i++) {
		msleep(LOCK_POLL_MS);
		locked = et.fe->locked();
		if (locked < 0)
			ret = locked;
	}
	if (!ret && !locked)
		ret = -EAGAIN;
	if (ret)
		goto out;

	/* find the FIFO slots once (needs packets flowing: filter 0 -> null) */
	if (!emma3sep_tsfifo_slots(group[G_LIGHT].fifo) ||
	    !emma3sep_tsfifo_slots(group[G_HEAVY].fifo)) {
		if (et.draining) {
			emma3sep_tsfifo_stop();
			et.draining = 0;
		}
		ret = learn_slots();
		if (ret)
			goto out;
	}

	/* Bootstrap once: set the fixed PIDs so PAT/CAT flow and are tapped,
	 * and start draining.  After this the PSI follower keeps the filters up
	 * to date on its own -- across a PMT version change (re-points only the
	 * changed PIDs) and across a re-tune (PAT/CAT stay filtered, so a new
	 * multiplex's PAT arrives and re-enumerates the rest). */
	if (!et.draining) {
		ret = apply_pids();
		if (ret)
			goto out;
		ret = emma3sep_tsfifo_start(FIFO_MASK, FIFO_XDMA);
		if (ret)
			goto out;
		et.draining = 1;
		if (et.capturing)
			emma3sep_tsfifo_capture(1);
	}

	old_crc = et.psi->pat_crc;
	et.freq_khz = freq_khz;
	et.tuned = 1;			/* let the follower run while we wait below */
	mutex_unlock(&et.lock);

	if (same)
		return 0;		/* re-locked the same multiplex: already followed */

	/* A (possibly) new multiplex: wait until its PAT -- a new CRC -- and all
	 * its PMTs have been read, so a reader that starts at once gets whole
	 * programs.  The fixed PIDs were already filtered, so this needs no
	 * teardown; the follower re-points only the delta. */
	wait_event_timeout(et.psi_wq,
			   et.psi->pat_valid && et.psi->pat_crc != old_crc &&
			   et.psi_done, 3 * HZ);
	return 0;
out:
	mutex_unlock(&et.lock);
	return ret;
}

static int et_set_capture(struct ird_tuner *t, int on)
{
	et.capturing = on;
	emma3sep_tsfifo_capture(on);
	return 0;
}

static int et_read_cnr(struct ird_tuner *t, u32 *mdb)
{
	int ret;

	mutex_lock(&et.lock);
	ret = et.fe->read_cnr(mdb);
	mutex_unlock(&et.lock);
	return ret;
}

static ssize_t et_read(struct ird_tuner *t, char __user *buf, size_t count,
		       int nonblock)
{
	return emma3sep_tsfifo_read(buf, count, nonblock);
}

static const struct ird_tuner_ops et_ops = {
	.open		= et_open,
	.release	= et_release,
	.tune		= et_tune,
	.set_capture	= et_set_capture,
	.read_cnr	= et_read_cnr,
	.read		= et_read,
};

/* --- start-up ----------------------------------------------------------------- */

static int et_start(void)
{
	const char *how = NULL;
	int ret, fresh;

	if (et.ready)
		return 0;
	if (!emma3sep_av_up()) {
		et.why = "the AV sub-CPU is not running (start it with the vendor csd.ko)";
		return -ENODEV;
	}
	if (emma3sep_av_shared()) {
		et.why = "the vendor csd.ko is still loaded (unload it first)";
		return -EBUSY;
	}
	ret = et.fe->start(et.fw, et.fw_len, &et.why, &how);
	if (ret)
		return ret;
	ret = emma3sep_av_attach(&fresh);
	if (!ret && !fresh)
		ret = -EBUSY;
	if (ret) {
		et.why = "the AV firmware has been used since it started (restart it)";
		return ret;
	}
	ret = emma3sep_demux_init(et.dev);
	if (ret) {
		et.why = "the demultiplexer did not initialise";
		return ret;
	}
	et.why = NULL;
	et.ready = 1;
	if (how)
		printk(KERN_INFO "ird-tuner-%s: ready (%s)\n", et.fe->name, how);
	else
		printk(KERN_INFO "ird-tuner-%s: ready\n", et.fe->name);
	return 0;
}

static ssize_t start_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	int ret;

	mutex_lock(&et.lock);
	ret = et_start();
	mutex_unlock(&et.lock);
	return ret ? ret : count;
}

static ssize_t state_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	int len = 0, g, i;

	mutex_lock(&et.lock);
	len += sprintf(buf + len, "state %s\n", et.ready ? "ready" : "stopped");
	if (et.why)
		len += sprintf(buf + len, "reason %s\n", et.why);
	if (et.fe->fw_max)
		len += sprintf(buf + len, "firmware %u bytes\n",
			       (unsigned int)et.fw_len);
	if (et.ready) {
		len += sprintf(buf + len, "tuned %d freq_khz %u programs %d "
			       "tables_read %d pids_without_filter %u filter_writes %u\n",
			       et.tuned, et.freq_khz, et.psi->nprog,
			       et.psi_done, et.dropped, et.filter_writes);
		for (g = 0; g < NGROUP; g++) {
			len += sprintf(buf + len, "fifo %d pids", group[g].fifo);
			for (i = 0; i < et.nfilt[g]; i++)
				if (et.filt[g][i].pid != PID_PARK(i))
					len += sprintf(buf + len, " %04x",
						       et.filt[g][i].pid);
			len += sprintf(buf + len, "\n");
		}
		len += emma3sep_tsfifo_show(buf + len);
	}
	mutex_unlock(&et.lock);
	return len;
}

static DEVICE_ATTR(start, 0200, NULL, start_store);
static DEVICE_ATTR(state, 0444, state_show, NULL);

static ssize_t fw_write(struct kobject *kobj, struct bin_attribute *attr,
			char *buf, loff_t off, size_t count)
{
	if (off < 0 || off + count > et.fe->fw_max)
		return -EFBIG;
	mutex_lock(&et.lock);
	if (off == 0)
		et.fw_len = 0;			/* a new upload */
	if (off != et.fw_len) {
		mutex_unlock(&et.lock);
		return -EINVAL;			/* in one go, front to back */
	}
	memcpy(et.fw + off, buf, count);
	et.fw_len = off + count;
	mutex_unlock(&et.lock);
	return count;
}

static struct bin_attribute fw_attr = {
	.attr	= { .name = "firmware", .mode = 0200 },
	.write	= fw_write,
};

/* The front end of the board the boot loader names, if this kernel has one. */
static const struct emma3sep_fe *__init board_front_end(void)
{
	static const char key[] = "PRODUCT_FWIDS=";
	const char *p = strstr(saved_command_line, key);
	size_t n;
	int i;

	if (!p)
		return NULL;
	p += sizeof(key) - 1;
	n = strcspn(p, " \t\n");
	for (i = 0; i < ARRAY_SIZE(fronts); i++)
		if (strlen(fronts[i]->board) == n &&
		    !strncmp(p, fronts[i]->board, n))
			return fronts[i];
	return NULL;
}

static int __init et_init(void)
{
	char name[32];
	int ret;

	et.fe = board_front_end();
	if (!et.fe)
		return -ENODEV;

	mutex_init(&et.lock);
	init_waitqueue_head(&et.psi_wq);
	et.why = "not started";
	if (et.fe->fw_max) {
		et.fw = vmalloc(et.fe->fw_max);
		if (!et.fw)
			return -ENOMEM;
	}
	et.psi = vmalloc(sizeof(*et.psi));
	if (!et.psi)
		return -ENOMEM;
	ird_psi_reset(et.psi);
	ret = emma3sep_tsfifo_init();
	if (ret)
		return ret;

	snprintf(name, sizeof(name), "ird-tuner-%s", et.fe->name);
	et.pdev = platform_device_register_simple(name, 0, NULL, 0);
	if (IS_ERR(et.pdev))
		return PTR_ERR(et.pdev);
	ret = device_create_file(&et.pdev->dev, &dev_attr_start);
	if (!ret)
		ret = device_create_file(&et.pdev->dev, &dev_attr_state);
	if (!ret && et.fe->fw_max)
		ret = sysfs_create_bin_file(&et.pdev->dev.kobj, &fw_attr);
	if (ret)
		return ret;

	et.psi_task = kthread_run(psi_thread, NULL, "ird-psi");
	if (IS_ERR(et.psi_task))
		return PTR_ERR(et.psi_task);

	et.tuner.name = "irdvideo0";
	et.tuner.ops = &et_ops;
	ret = ird_tuner_register(&et.tuner);
	if (ret)
		return ret;
	printk(KERN_INFO "ird-tuner-%s: /dev/irdvideo0 registered; waiting to "
	       "be started\n", et.fe->name);
	return 0;
}
/* after the I2C bus driver, wherever the link order puts us */
late_initcall(et_init);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("EMMA3SEP boards' tuner (board front end + EMMA3SEP TS input FIFOs)");
