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
 * had, and every client turns it into dB itself.  For ISDB-T recpt1 does it
 * with
 *
 *	P = 10 * log10(5505024 / raw)
 *	C/N = 0.000024 P^4 - 0.0016 P^3 + 0.0398 P^2 + 0.5491 P + 3.0965
 *
 * So a driver that knows its C/N in dB has to go the other way.  The table
 * is that function inverted: the raw value for 4.0 dB, 4.5 dB, ... 40.0 dB.
 * (A driver whose demodulator gives such a value itself passes it on as it
 * is, for ISDB-S too: read_cnr_raw.)
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

/*
 * The channel numbering of px4_drv (ird_ptx_ioctl.h): the system, the
 * frequency and, for ISDB-S, the stream.  The system asked for with
 * PTX_SET_SYSTEM_MODE decides; with none, a tuner that receives ISDB-S
 * takes freq_no 0..23 as BS and CS.
 */
static u32 caps(const struct ird_tuner *t)
{
	return t->system_cap ? t->system_cap : PTX_ISDB_T_SYSTEM;
}

static int channel(const struct ird_tuner *t, const struct ptx_freq *f,
		   int *system, u32 *khz, u16 *stream_id)
{
	int n = f->freq_no, mode = t->system;

	*stream_id = 0;
	if (n < 0)
		return -EINVAL;
	if (n < 24 && (mode == PTX_ISDB_S_SYSTEM ||
		       (mode == PTX_UNSPECIFIED_SYSTEM &&
			(caps(t) & PTX_ISDB_S_SYSTEM)))) {
		*system = PTX_ISDB_S_SYSTEM;
		*khz = n < 12 ? 1049480 + 38360 * n : 1613000 + 40000 * (n - 12);
		*stream_id = f->slot;	/* a 16-bit number, as in px4_drv */
		return 0;
	}
	if (mode == PTX_ISDB_S_SYSTEM || !(caps(t) & PTX_ISDB_T_SYSTEM))
		return -EINVAL;
	*system = PTX_ISDB_T_SYSTEM;
	if (n >= 63 && n <= 112) {
		*khz = 95143 + n * 6000 + f->slot;
	} else if ((n >= 24 && n <= 62) ||
		   (mode == PTX_ISDB_T_SYSTEM && ((n >= 3 && n <= 12) ||
						  n == 22 || n == 23))) {
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

/*
 * Like px4_drv, read() waits until the whole buffer is filled.  A signal
 * ends it with what it has (-EINTR if nothing), and so does the stream being
 * stopped; with O_NONBLOCK it takes only what is there.  recpt1 allocates a
 * buffer of its read size for every read(): handing it a few packets at a
 * time made its queue grow as fast as the stream came in (ts-fifo spec §11).
 */
static ssize_t ird_read(struct file *file, char __user *buf, size_t count,
			loff_t *ppos)
{
	struct ird_tuner *t = file->private_data;
	int nonblock = file->f_flags & O_NONBLOCK;
	size_t done = 0;
	ssize_t n;

	if (!t->streaming)
		return 0;
	do {
		n = t->ops->read(t, buf + done, count - done, nonblock);
		if (n <= 0)
			return done ? (ssize_t)done : n;
		done += n;
	} while (done < count && !nonblock && t->streaming);
	return done;
}

static long ird_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct ird_tuner *t = file->private_data;
	long ret = 0;

	mutex_lock(&t->lock);
	switch (cmd) {
	case PTX_SET_CHANNEL: {
		struct ptx_freq f;
		int system;
		u32 khz;
		u16 stream_id;

		if (copy_from_user(&f, (void __user *)arg, sizeof(f))) {
			ret = -EFAULT;
			break;
		}
		ret = channel(t, &f, &system, &khz, &stream_id);
		if (!ret)
			ret = t->ops->tune(t, system, khz, stream_id);
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

		if (t->ops->read_cnr_raw) {
			ret = t->ops->read_cnr_raw(t, &raw);
		} else {
			ret = t->ops->read_cnr(t, &mdb);
			raw = cnr_mdb_to_ptx(mdb);
		}
		if (ret)
			break;
		if (copy_to_user((void __user *)arg, &raw, sizeof(raw)))
			ret = -EFAULT;
		break;
	}
	case PTX_ENABLE_LNB_POWER:
		/* no tuner here can power an LNB */
		if (arg)
			ret = -ENOSYS;
		break;
	case PTX_DISABLE_LNB_POWER:
		break;
	case PTX_SET_SYSTEM_MODE:
		/* px4_drv: one the tuner receives (so never "unspecified") */
		if ((arg == PTX_ISDB_T_SYSTEM || arg == PTX_ISDB_S_SYSTEM) &&
		    (caps(t) & arg))
			t->system = arg;
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
	    !(t->ops->read_cnr || t->ops->read_cnr_raw) || !t->ops->read)
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
