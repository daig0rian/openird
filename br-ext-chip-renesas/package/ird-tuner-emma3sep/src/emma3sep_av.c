// SPDX-License-Identifier: GPL-2.0-only
/*
 * Renesas EMMA3SEP: commands to the firmware of the AV sub-CPU, posted
 * straight onto its command stack with no vendor kernel module in the path.
 *
 * Written from docs/specs/poe-br100-yb1-av-command.md only (§2 the stack,
 * §3 the commands, §4 how a PID filter behaves, §5.1 the smallest set raw
 * TS needs).  Register names are the GPL kernel's (asm/emma/emma3sep.h).
 * The tuner and the card reader share the stack; what that takes is in
 * docs/specs/emma3sep-card-reader.md §3.5.
 *
 * The sub-CPU must be running: the owner's csd.ko starts it and may then be
 * unloaded (spec §1).  This file never resets it, never writes the BHIF
 * registers the spec says to leave alone, and never opens the descrambler.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/mutex.h>
#include <asm/io.h>
#include <asm/addrspace.h>

#include "emma3sep_av.h"

#define BHIF_PHYS	0x10000000ul
#define BHIF_SUB_RESET	0x180		/* 0 = the sub-CPU runs */
#define BHIF_SW_INT_EN	0x1a4		/* the main CPU's listeners for replies */
#define BHIF_SHM	0x1ac		/* the shared memory, as the sub-CPU sees it */
#define BHIF_DOORBELL	0x1b0
#define SHM_KSEG1	0xa0010000ul	/* physical 0x10000 */

#define ENT_KSEG1	0xa0010400ul	/* 32 entries of 8 words; the ring is 1..31 */
#define ENT_FIRST	1
#define ENT_LAST	31
#define ENT_PENDING	0x00020000u
#define ENT_DONE	0x00040002u
#define SEQ_FIRST	0x0100		/* counts in its low 8 bits */

/* Requests and responses go into the data ring.  Where the firmware's own
 * clients wrap around is not known; they were never seen past 0xa00112fc, so
 * stay below that.  One command is in flight at a time, so room for the
 * largest request and response is all that is needed. */
#define DATA_START	0xa0010800ul
#define DATA_WRAP	0xa0011200ul
#define MAX_WORDS	EMMA3SEP_AV_MAX_WORDS

#define CMD_INITIALISE	0x1900
#define CMD_OPEN_DEVICE	0x1901
#define CMD_INPUT	0x1904
#define CMD_FILTER_ALLOC 0x190a
#define CMD_FILTER_PID	0x190b
#define CMD_FILTER_OFF	0x190d
#define INPUT_DEMOD	3		/* the demodulator's TS */
#define FILTER_PID	7		/* a PID filter: packets go to the FIFO */
#define STATUS_ENABLED	8		/* PID change refused: the filter is on */

static void __iomem *bhif;
static unsigned int nx_entry = ENT_FIRST, nx_seq = SEQ_FIRST;
static unsigned long nx_addr = DATA_START;
static int attached;
/* one command on the stack at a time, whoever posts it */
static DEFINE_MUTEX(av_lock);

static inline u32 *entry(unsigned int i)
{
	return (u32 *)(ENT_KSEG1 + 32 * i);
}

static inline unsigned int entry_after(unsigned int i)
{
	return i == ENT_LAST ? ENT_FIRST : i + 1;
}

static int map_bhif(void)
{
	if (!bhif)
		bhif = ioremap_nocache(BHIF_PHYS, 0x200);
	return bhif ? 0 : -ENOMEM;
}

int emma3sep_av_up(void)
{
	if (map_bhif())
		return 0;
	return __raw_readl(bhif + BHIF_SUB_RESET) == 0 &&
	       __raw_readl(bhif + BHIF_SHM) == SHM_KSEG1;
}
EXPORT_SYMBOL_GPL(emma3sep_av_up);

/*
 * Is somebody on the main CPU still listening for the firmware's replies?
 * The vendor's csd.ko enables these interrupts when it loads and puts them
 * back to 0 when it is unloaded (spec §1), and a second poster would put the
 * command ring out of step.
 */
int emma3sep_av_shared(void)
{
	return bhif && __raw_readl(bhif + BHIF_SW_INT_EN) != 0;
}
EXPORT_SYMBOL_GPL(emma3sep_av_shared);

static int attach(int *fresh)
{
	unsigned int i, newest = 0, used = 0;

	if (!emma3sep_av_up())
		return -ENODEV;
	for (i = ENT_FIRST; i <= ENT_LAST; i++) {
		const u32 *e = entry(i), *n = entry(entry_after(i));

		if (!e[0] && !e[1])
			continue;
		used++;
		if (e[0] != ENT_DONE)
			return -EBUSY;		/* somebody's command is in flight */
		/* the newest is the one its successor does not follow */
		if (n[0] != ENT_DONE ||
		    (n[1] & 0xff) != ((e[1] + 1) & 0xff))
			newest = i;
	}
	*fresh = !used;
	if (!used) {
		nx_entry = ENT_FIRST;
		nx_seq = SEQ_FIRST;
		nx_addr = DATA_START;
		return 0;
	}
	if (!newest)
		return -EBUSY;			/* a full lap in step: cannot tell */
	nx_entry = entry_after(newest);
	nx_seq = (entry(newest)[1] & 0xff00) | ((entry(newest)[1] + 1) & 0xff);
	nx_addr = DATA_START;
	return 0;
}

int emma3sep_av_attach(int *fresh)
{
	int ret;

	mutex_lock(&av_lock);
	ret = attach(fresh);
	if (!ret)
		attached = 1;
	mutex_unlock(&av_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(emma3sep_av_attach);

int emma3sep_av_attached(void)
{
	return attached;
}
EXPORT_SYMBOL_GPL(emma3sep_av_attached);

/*
 * Post one command and wait for it; av_lock held.  Bit i of self adds the
 * request's own address to word i; back, if given, gets the request words
 * as they stand when the command is done (the firmware writes some).
 */
static int post(u16 cmd, const u32 *req, int nreq, u32 *resp, int nresp,
		u32 self, u32 *back)
{
	volatile u32 *e = entry(nx_entry);
	volatile u32 *d;
	unsigned long timeout;
	int i;

	if (nreq + nresp > MAX_WORDS || !bhif)
		return -EINVAL;
	if (nx_addr + 4 * (nreq + nresp) > DATA_WRAP)
		nx_addr = DATA_START;
	d = (volatile u32 *)nx_addr;
	for (i = 0; i < nreq; i++)
		d[i] = req[i] + (i < 32 && (self & (1u << i)) ? nx_addr : 0);
	for (i = 0; i < nresp; i++)
		d[nreq + i] = 0;
	e[1] = ((u32)cmd << 16) | nx_seq;
	e[2] = nx_addr;
	e[3] = nx_addr + 4 * nreq;
	e[4] = 0x7fffffff;
	e[5] = 0;
	e[6] = 4 * (nreq + nresp);
	e[7] = 0;
	e[0] = ENT_PENDING;			/* last: this makes it valid */
	__raw_writel(ENT_PENDING, bhif + BHIF_DOORBELL);

	timeout = jiffies + 2 * HZ;
	for (i = 0; e[0] != ENT_DONE; i++) {
		if (time_after(jiffies, timeout))
			return -ETIMEDOUT;	/* the ring is now out of step */
		if (i < 40)
			udelay(50);
		else
			msleep(1);
	}
	for (i = 0; i < nresp; i++)
		resp[i] = d[nreq + i];
	for (i = 0; back && i < nreq; i++)
		back[i] = d[i];

	/* the firmware takes entries and sequence numbers in order */
	nx_entry = entry_after(nx_entry);
	nx_seq = (nx_seq & 0xff00) | ((nx_seq + 1) & 0xff);
	nx_addr += 4 * (nreq + nresp);
	return e[5];
}

int emma3sep_av_cmd(u16 cmd, const u32 *req, int nreq, u32 *resp, int nresp)
{
	int st;

	mutex_lock(&av_lock);
	st = post(cmd, req, nreq, resp, nresp, 0, NULL);
	mutex_unlock(&av_lock);
	return st;
}
EXPORT_SYMBOL_GPL(emma3sep_av_cmd);

int emma3sep_av_cmd_rw(u16 cmd, u32 *req, int nreq, u32 *resp, int nresp,
		       u32 self)
{
	int st;

	mutex_lock(&av_lock);
	st = post(cmd, req, nreq, resp, nresp, self, req);
	mutex_unlock(&av_lock);
	return st;
}
EXPORT_SYMBOL_GPL(emma3sep_av_cmd_rw);

/* --- the demultiplexer ------------------------------------------------------- */

static int refused(u16 cmd, int st)
{
	if (st > 0) {
		printk(KERN_WARNING "emma3sep-av: command 0x%04x refused, status %d\n",
		       cmd, st);
		return -EIO;
	}
	return st;
}

static int cmd_ok(u16 cmd, const u32 *req, int nreq, u32 *resp, int nresp)
{
	return refused(cmd, emma3sep_av_cmd(cmd, req, nreq, resp, nresp));
}

/*
 * Devices are numbered in the order they are opened: 0, 1, 2.  Device 0
 * feeds FIFO 0 and device 2 feeds FIFO 2; device 1 is opened only so that
 * the third open yields device 2.
 */
int emma3sep_demux_init(u32 dev[3])
{
	u32 req[2];
	int i, ret;

	ret = cmd_ok(CMD_INITIALISE, NULL, 0, NULL, 0);
	for (i = 0; i < 3 && !ret; i++) {
		req[0] = 1;
		ret = cmd_ok(CMD_OPEN_DEVICE, req, 1, &dev[i], 1);
	}
	for (i = 0; i < 3 && !ret; i += 2) {
		req[0] = dev[i];
		req[1] = INPUT_DEMOD;
		ret = cmd_ok(CMD_INPUT, req, 2, NULL, 0);
	}
	return ret;
}
EXPORT_SYMBOL_GPL(emma3sep_demux_init);

int emma3sep_demux_filter_alloc(u32 dev, u32 *filter)
{
	/* the request's second word points at its own fourth */
	u32 req[4] = { FILTER_PID, 12, dev, 0xffffff00 };

	return refused(CMD_FILTER_ALLOC,
		       emma3sep_av_cmd_rw(CMD_FILTER_ALLOC, req, 4, filter, 1,
					  1u << 1));
}
EXPORT_SYMBOL_GPL(emma3sep_demux_filter_alloc);

/*
 * What a filter lets into its device's FIFO is decided by the PID alone; it
 * does not have to be enabled, and this driver never enables one.  The PID
 * of an enabled filter cannot be changed, so should one be found enabled,
 * it is disabled first.
 */
int emma3sep_demux_filter_pid(u32 filter, u16 pid)
{
	u32 req[2] = { filter, pid };
	int st = emma3sep_av_cmd(CMD_FILTER_PID, req, 2, NULL, 0);

	if (st == STATUS_ENABLED) {
		st = cmd_ok(CMD_FILTER_OFF, req, 1, NULL, 0);
		if (!st)
			st = emma3sep_av_cmd(CMD_FILTER_PID, req, 2, NULL, 0);
	}
	if (st > 0) {
		printk(KERN_WARNING "emma3sep-av: PID 0x%04x refused for filter "
		       "0x%08x, status %d\n", pid, filter, st);
		return -EIO;
	}
	return st;
}
EXPORT_SYMBOL_GPL(emma3sep_demux_filter_pid);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Renesas EMMA3SEP AV sub-CPU command path and TS input FIFOs");
