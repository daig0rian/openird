// SPDX-License-Identifier: GPL-2.0-only
/*
 * OpenIRD tuner core: the character device.
 *
 * One node per tuner (/dev/irdvideoN), speaking the ABI of px4_drv so that
 * the programs written for it (recpt1, recisdb) work unchanged: open, tune
 * with PTX_SET_CHANNEL, PTX_START_STREAMING, read() the transport stream.
 * The behaviour follows px4_drv's own chardev where a caller could tell the
 * difference -- one opener at a time, the channel numbering, the error
 * codes -- see docs/specs/poe-br100-yb1-ts-fifo.md §11 for what each call
 * does on the first board.
 *
 * What comes out of read() is the stream as broadcast.  Nothing here, or in
 * the drivers behind it, descrambles.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>

#include "ird_tuner.h"
#include "ird_ptx_ioctl.h"

#define IRD_MAX_TUNERS	4

static struct ird_tuner *tuners[IRD_MAX_TUNERS];
static DEFINE_MUTEX(tuners_lock);

/*
 * PTX_GET_CNR hands out a number in the units of the demodulator the PT1
 * had, and every client turns it into dB itself.  recpt1 does it with
 *
 *	P = 10 * log10(5505024 / raw)
 *	C/N = 0.000024 P^4 - 0.0016 P^3 + 0.0398 P^2 + 0.5491 P + 3.0965
 *
 * So a driver that knows its C/N in dB has to go the other way.  The table
 * is that function inverted: the raw value for 4.0 dB, 4.5 dB, ... 40.0 dB.
 */
#define CNR_TAB_MIN_MDB		4000
#define CNR_TAB_STEP_MDB	500
static const u32 cnr_tab[] = {
	3903295, 3296173, 2811329, 2416198, 2089200, 1815312, 1583680, 1386228,
	1216799, 1070606,  943867,  833547,  737185,  652760,  578602,  513314,
	 455722,  404834,  359803,  319904,  284517,  253101,  225192,  200382,
	 178318,  158687,  141219,  125672,  111834,   99517,   88556,   78803,
	  70125,   62408,   55546,   49446,   44027,   39212,   34937,   31142,
	  27774,   24785,   22134,   19782,   17696,   15846,   14205,   12748,
	  11455,   10307,    9287,    8380,    7573,    6855,    6214,    5643,
	   5132,    4676,    4267,    3901,    3571,    3276,    3009,    2769,
	   2552,    2356,    2178,    2017,    1870,    1737,    1615,    1505,
	   1403,
};

static u32 cnr_mdb_to_ptx(u32 mdb)
{
	u32 i, frac;

	if (mdb <= CNR_TAB_MIN_MDB)
		return cnr_tab[0];
	i = (mdb - CNR_TAB_MIN_MDB) / CNR_TAB_STEP_MDB;
	if (i >= ARRAY_SIZE(cnr_tab) - 1)
		return cnr_tab[ARRAY_SIZE(cnr_tab) - 1];
	frac = (mdb - CNR_TAB_MIN_MDB) % CNR_TAB_STEP_MDB;
	return cnr_tab[i] - (cnr_tab[i] - cnr_tab[i + 1]) * frac / CNR_TAB_STEP_MDB;
}

/* The ISDB-T channel numbering of px4_drv (ird_ptx_ioctl.h). */
static int isdbt_freq_khz(int system, const struct ptx_freq *f, u32 *khz)
{
	int n = f->freq_no;

	if (n >= 63 && n <= 112) {
		*khz = 95143 + n * 6000 + f->slot;
	} else if ((n >= 22 && n <= 62) ||
		   (system == PTX_ISDB_T_SYSTEM && n >= 3 && n <= 12)) {
		if (system != PTX_ISDB_T_SYSTEM && n < 24)
			return -EINVAL;
		*khz = 93143 + n * 6000 + f->slot;
		if (n == 12)
			*khz += 2000;
	} else {
		return -EINVAL;
	}
	return 0;
}

static struct ird_tuner *tuner_of(struct inode *inode)
{
	struct ird_tuner *t = NULL;
	int i;

	mutex_lock(&tuners_lock);
	for (i = 0; i < IRD_MAX_TUNERS; i++)
		if (tuners[i] && tuners[i]->misc.minor == iminor(inode))
			t = tuners[i];
	mutex_unlock(&tuners_lock);
	return t;
}

static int ird_open(struct inode *inode, struct file *file)
{
	struct ird_tuner *t = tuner_of(inode);
	int ret = 0;

	if (!t)
		return -ENOENT;
	if (atomic_cmpxchg(&t->opened, 0, 1))
		return -EALREADY;
	mutex_lock(&t->lock);
	t->system = PTX_UNSPECIFIED_SYSTEM;
	t->streaming = 0;
	if (t->ops->open)
		ret = t->ops->open(t);
	mutex_unlock(&t->lock);
	if (ret) {
		atomic_set(&t->opened, 0);
		return ret;
	}
	file->private_data = t;
	return 0;
}

static int ird_release(struct inode *inode, struct file *file)
{
	struct ird_tuner *t = file->private_data;

	mutex_lock(&t->lock);
	if (t->streaming) {
		t->ops->set_capture(t, 0);
		t->streaming = 0;
	}
	if (t->ops->release)
		t->ops->release(t);
	mutex_unlock(&t->lock);
	atomic_set(&t->opened, 0);
	return 0;
}

static ssize_t ird_read(struct file *file, char __user *buf, size_t count,
			loff_t *ppos)
{
	struct ird_tuner *t = file->private_data;

	if (!t->streaming)
		return 0;
	return t->ops->read(t, buf, count, file->f_flags & O_NONBLOCK);
}

static long ird_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct ird_tuner *t = file->private_data;
	long ret = 0;

	mutex_lock(&t->lock);
	switch (cmd) {
	case PTX_SET_CHANNEL: {
		struct ptx_freq f;
		u32 khz;

		if (copy_from_user(&f, (void __user *)arg, sizeof(f))) {
			ret = -EFAULT;
			break;
		}
		ret = isdbt_freq_khz(t->system, &f, &khz);
		if (!ret)
			ret = t->ops->tune(t, khz);
		break;
	}
	case PTX_START_STREAMING:
		if (t->streaming) {
			ret = -EALREADY;
			break;
		}
		ret = t->ops->set_capture(t, 1);
		if (!ret)
			t->streaming = 1;
		break;
	case PTX_STOP_STREAMING:
		if (!t->streaming) {
			ret = -EALREADY;
			break;
		}
		ret = t->ops->set_capture(t, 0);
		if (!ret)
			t->streaming = 0;
		break;
	case PTX_GET_CNR: {
		u32 mdb, raw;

		ret = t->ops->read_cnr(t, &mdb);
		if (ret)
			break;
		raw = cnr_mdb_to_ptx(mdb);
		if (copy_to_user((void __user *)arg, &raw, sizeof(raw)))
			ret = -EFAULT;
		break;
	}
	case PTX_ENABLE_LNB_POWER:
		/* terrestrial only: there is no LNB to power */
		if (arg)
			ret = -ENOSYS;
		break;
	case PTX_DISABLE_LNB_POWER:
		break;
	case PTX_SET_SYSTEM_MODE:
		if (arg == PTX_ISDB_T_SYSTEM)
			t->system = PTX_ISDB_T_SYSTEM;
		else
			ret = -EINVAL;
		break;
	default:
		ret = -ENOSYS;
		break;
	}
	mutex_unlock(&t->lock);
	return ret;
}

static const struct file_operations ird_fops = {
	.owner		= THIS_MODULE,
	.open		= ird_open,
	.release	= ird_release,
	.read		= ird_read,
	.unlocked_ioctl	= ird_ioctl,
};

int ird_tuner_register(struct ird_tuner *t)
{
	int i, ret;

	if (!t->ops || !t->ops->tune || !t->ops->set_capture ||
	    !t->ops->read_cnr || !t->ops->read)
		return -EINVAL;
	mutex_init(&t->lock);
	atomic_set(&t->opened, 0);
	t->misc.minor = MISC_DYNAMIC_MINOR;
	t->misc.name = t->name;
	t->misc.fops = &ird_fops;

	mutex_lock(&tuners_lock);
	for (i = 0; i < IRD_MAX_TUNERS && tuners[i]; i++)
		;
	if (i == IRD_MAX_TUNERS) {
		mutex_unlock(&tuners_lock);
		return -ENOSPC;
	}
	ret = misc_register(&t->misc);
	if (!ret)
		tuners[i] = t;
	mutex_unlock(&tuners_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(ird_tuner_register);

void ird_tuner_unregister(struct ird_tuner *t)
{
	int i;

	mutex_lock(&tuners_lock);
	for (i = 0; i < IRD_MAX_TUNERS; i++)
		if (tuners[i] == t)
			tuners[i] = NULL;
	mutex_unlock(&tuners_lock);
	misc_deregister(&t->misc);
}
EXPORT_SYMBOL_GPL(ird_tuner_unregister);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("OpenIRD tuner core: px4_drv-compatible character device");
