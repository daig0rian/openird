// SPDX-License-Identifier: GPL-2.0-only
/*
 * EMMA3SEP boards with a card slot (SB-TV06-WRIP): the slot as a card
 * reader, /dev/irdcard0 (ird_card_ioctl.h), for OpenIRD's pcsc-lite driver.
 *
 * Written from docs/specs/emma3sep-card-reader.md (what this driver does,
 * §2 and §3) and docs/specs/sb-tv06-wrip-card-slot.md (the firmware's SCARD
 * commands, its record of the slot and its signal).  The firmware of the AV
 * sub-CPU runs the T=1 protocol with the card; this driver hands it whole
 * APDUs, takes back whole responses, and never looks at what they say.
 *
 * The commands go onto the AV command stack the tuner uses (emma3sep_av.c
 * serialises them), so the device opens only once the tuner has been
 * started.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "emma3sep_av.h"
#include "ird_card_ioctl.h"

/* the boards with a slot, as the boot loader names them in PRODUCT_FWIDS= */
static const char *const slot_boards[] = { "PIX_AIRTUNER_BR310L" };

/* card-slot spec §2 */
#define CMD_INIT	0x1100
#define CMD_OPEN	0x1101
#define CMD_XFER	0x1105
#define CMD_CNTRL	0x1106
#define CMD_CONFIG	0x1107

#define CNTRL_RESET	0
#define CNTRL_GET	1
#define CNTRL_SET	2
#define CNTRL_ATR	6
#define CNTRL_WORDS	22
#define CNTRL_RESP	9		/* 36 bytes: room for the longest ATR */
#define ST_NO_CARD	15		/* reset refused: the slot is empty */

static const u32 config_req[6] = {
	0, 0x01000000, 0x00000087, 0x01000000, 0x00960064, 0x00960064
};

/* the firmware's record (card-slot spec §3): state at P - 8, length at P - 4,
 * bytes from P.  P is learned from the first transmit. */
#define REC_EMPTY	0
#define REC_IN		1
#define REC_ATR		2
#define REC_DONE	4

/* its signal: bit 24 of this shared memory word (physical 0x1d500) */
#define EVENT_WORD	((volatile u32 *)0xa001d500ul)
#define EVENT_BIT	0x01000000u

#define RESET_WAIT_MS	1000
#define XFER_WAIT_MS	5000
#define SETTLE_MS	500		/* the contacts bounce ~350 ms on the way out */
#define RX_LIMIT	0x100		/* what the vendor passes */

static struct {
	struct mutex lock;
	int open;
	int set_up;			/* 0x1100, 0x1107 and 0x1101 done */
	int present, powered;
	unsigned int changes;
	int checked;			/* presence established at least once */
	int disturbed;			/* a signal came that was not ours */
	unsigned long last_signal;	/* jiffies of the last such signal */
	volatile u32 *rec;		/* P, or NULL until a transmit shows it */
	u8 atr[IRD_CARD_ATR_MAX];
	int atr_len;
} card;

/* --- the signal --------------------------------------------------------------- */

static int signal_seen(void)
{
	return (*EVENT_WORD & EVENT_BIT) != 0;
}

static void signal_clear(void)
{
	*EVENT_WORD = *EVENT_WORD & ~EVENT_BIT;
}

static int signal_wait(unsigned int ms)
{
	unsigned long end = jiffies + msecs_to_jiffies(ms);

	while (!signal_seen()) {
		if (time_after(jiffies, end))
			return -ETIMEDOUT;
		msleep(1);
	}
	return 0;
}

/* A signal outside our own commands: the card went in or out (or bounces). */
static void note_foreign_signal(void)
{
	if (!signal_seen())
		return;
	signal_clear();
	card.disturbed = 1;
	card.last_signal = jiffies;
	card.powered = 0;
}

static int settling(void)
{
	return time_before(jiffies, card.last_signal + msecs_to_jiffies(SETTLE_MS));
}

static void set_present(int present)
{
	if (present != card.present)
		card.changes++;
	card.present = present;
	if (!present)
		card.powered = 0;
}

/* --- commands ----------------------------------------------------------------- */

static int command(u16 cmd, u32 *req, int nreq, u32 *resp, int nresp, u32 self)
{
	int st = emma3sep_av_cmd_rw(cmd, req, nreq, resp, nresp, self);

	if (st > 0 && !(cmd == CMD_CNTRL && st == ST_NO_CARD))
		printk(KERN_WARNING "emma3sep-card: command 0x%04x refused, "
		       "status %d\n", cmd, st);
	return st;
}

/* spec §3.1 */
static int set_up(void)
{
	u32 req[6];
	int st;

	if (card.set_up)
		return 0;
	st = command(CMD_INIT, NULL, 0, NULL, 0, 0);
	if (!st) {
		memcpy(req, config_req, sizeof(config_req));
		st = command(CMD_CONFIG, req, 6, NULL, 0, 0);
	}
	if (!st) {
		req[0] = 0;
		req[1] = 1;
		st = command(CMD_OPEN, req, 2, NULL, 0, 0);
	}
	if (st)
		return st < 0 ? st : -EIO;
	card.set_up = 1;
	return 0;
}

/* spec §3.2: wake the card and take its ATR */
static int reset(void)
{
	u32 req[CNTRL_WORDS], resp[CNTRL_RESP];
	int st, len;

	card.powered = 0;
	card.atr_len = 0;

	memset(req, 0, sizeof(req));
	req[1] = CNTRL_GET;		/* the settings come back over the request */
	st = command(CMD_CNTRL, req, CNTRL_WORDS, resp, CNTRL_RESP, 0);
	if (st)
		return st < 0 ? st : -EIO;
	req[1] = CNTRL_SET;
	req[12] = 0x0000000f;
	req[15] = 0x03000000;
	st = command(CMD_CNTRL, req, CNTRL_WORDS, resp, CNTRL_RESP, 0);
	if (st)
		return st < 0 ? st : -EIO;

	note_foreign_signal();
	memset(req, 0, sizeof(req));
	req[1] = CNTRL_RESET;
	st = command(CMD_CNTRL, req, CNTRL_WORDS, resp, CNTRL_RESP, 0);
	if (st == ST_NO_CARD)
		return -ENOMEDIUM;
	if (st)
		return st < 0 ? st : -EIO;
	st = signal_wait(RESET_WAIT_MS);
	if (st)
		return st;
	signal_clear();

	memset(req, 0, sizeof(req));
	req[1] = CNTRL_ATR;
	req[19] = 4 * CNTRL_WORDS;	/* the response: right after the request */
	st = command(CMD_CNTRL, req, CNTRL_WORDS, resp, CNTRL_RESP, 1u << 19);
	if (st)
		return st < 0 ? st : -EIO;
	len = req[20] >> 24;
	if (!len || len > IRD_CARD_ATR_MAX)
		return -EIO;
	memcpy(card.atr, resp, len);
	card.atr_len = len;
	card.powered = 1;
	return 0;
}

/* spec §3.3: one APDU there, its response back */
static int transmit(struct ird_card_xfer *x)
{
	u32 req[5 + (IRD_CARD_TX_MAX + 3) / 4];
	volatile u32 *p;
	volatile u8 *b;
	unsigned long a;
	u32 state, n, i;
	int st;

	if (!card.powered)
		return -ENODEV;
	if (!x->tx_len || x->tx_len > IRD_CARD_TX_MAX)
		return -EINVAL;

	memset(req, 0, sizeof(req));
	req[1] = 20;			/* the command: inside the request */
	req[2] = x->tx_len << 16;
	req[3] = 20;			/* the firmware puts its own here */
	req[4] = RX_LIMIT;
	memcpy(&req[5], x->tx, x->tx_len);

	note_foreign_signal();
	if (!card.powered)
		return -ENOMEDIUM;
	st = command(CMD_XFER, req, 5 + (x->tx_len + 3) / 4, NULL, 0,
		     (1u << 1) | (1u << 3));
	if (st)
		return st < 0 ? st : -EIO;
	a = req[3];
	if ((a & 3) || a < 0xa0000008ul || a >= 0xa0100000ul) {
		printk(KERN_WARNING "emma3sep-card: the firmware's record is at "
		       "%08lx?\n", a);
		return -EIO;
	}
	p = (volatile u32 *)a;
	card.rec = p;

	st = signal_wait(XFER_WAIT_MS);
	if (st)
		return st;
	signal_clear();
	state = p[-2];
	if (state != REC_DONE) {
		if (state == REC_EMPTY) {
			set_present(0);
			return -ENOMEDIUM;
		}
		return -EIO;
	}
	n = p[-1] & 0xffff;
	if (n > IRD_CARD_RX_MAX)
		return -EIO;
	b = (volatile u8 *)p;
	for (i = 0; i < n; i++)
		x->rx[i] = b[i];
	x->rx_len = n;
	return 0;
}

/* spec §3.4 */
static void update_presence(void)
{
	note_foreign_signal();
	if (card.disturbed && settling()) {
		set_present(0);
		return;
	}

	if (card.rec) {
		u32 state = card.rec[-2];

		card.disturbed = 0;
		set_present(state == REC_IN || state == REC_ATR ||
			    state == REC_DONE);
		return;
	}

	/* nothing to read it from: settle it with a reset, when it may have
	 * changed - nobody can be using the card yet (no transmit so far) */
	if (card.checked && !card.disturbed)
		return;
	card.checked = 1;
	card.disturbed = 0;
	set_present(reset() == 0);
}

/* --- the device --------------------------------------------------------------- */

static int card_open(struct inode *inode, struct file *file)
{
	int ret;

	if (!emma3sep_av_attached())
		return -EAGAIN;		/* the tuner has not been started yet */
	mutex_lock(&card.lock);
	if (card.open) {
		ret = -EBUSY;
	} else {
		ret = set_up();
		if (!ret)
			card.open = 1;
	}
	mutex_unlock(&card.lock);
	return ret;
}

static int card_release(struct inode *inode, struct file *file)
{
	mutex_lock(&card.lock);
	card.open = 0;
	mutex_unlock(&card.lock);
	return 0;
}

static long card_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	void __user *up = (void __user *)arg;
	struct ird_card_status s;
	struct ird_card_atr a;
	struct ird_card_xfer *x;
	long ret = 0;

	switch (cmd) {
	case IRD_CARD_STATUS:
		mutex_lock(&card.lock);
		update_presence();
		memset(&s, 0, sizeof(s));
		s.present = card.present;
		s.powered = card.powered;
		s.changes = card.changes;
		mutex_unlock(&card.lock);
		return copy_to_user(up, &s, sizeof(s)) ? -EFAULT : 0;

	case IRD_CARD_RESET:
		mutex_lock(&card.lock);
		ret = reset();
		card.checked = 1;
		card.disturbed = 0;
		set_present(ret != -ENOMEDIUM);
		memset(&a, 0, sizeof(a));
		if (!ret) {
			a.len = card.atr_len;
			memcpy(a.atr, card.atr, card.atr_len);
		}
		mutex_unlock(&card.lock);
		if (!ret && copy_to_user(up, &a, sizeof(a)))
			ret = -EFAULT;
		return ret;

	case IRD_CARD_POWER_OFF:
		mutex_lock(&card.lock);
		card.powered = 0;
		mutex_unlock(&card.lock);
		return 0;

	case IRD_CARD_TRANSMIT:
		x = kmalloc(sizeof(*x), GFP_KERNEL);
		if (!x)
			return -ENOMEM;
		if (copy_from_user(x, up, sizeof(*x))) {
			kfree(x);
			return -EFAULT;
		}
		mutex_lock(&card.lock);
		ret = transmit(x);
		mutex_unlock(&card.lock);
		if (!ret && copy_to_user(up, x, sizeof(*x)))
			ret = -EFAULT;
		kfree(x);
		return ret;
	}
	return -ENOTTY;
}

static const struct file_operations card_fops = {
	.owner		= THIS_MODULE,
	.open		= card_open,
	.release	= card_release,
	.unlocked_ioctl	= card_ioctl,
};

static struct miscdevice card_misc = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "irdcard0",
	.fops	= &card_fops,
};

static int __init board_has_slot(void)
{
	static const char key[] = "PRODUCT_FWIDS=";
	const char *p = strstr(saved_command_line, key);
	size_t n;
	int i;

	if (!p)
		return 0;
	p += sizeof(key) - 1;
	n = strcspn(p, " \t\n");
	for (i = 0; i < ARRAY_SIZE(slot_boards); i++)
		if (strlen(slot_boards[i]) == n && !strncmp(p, slot_boards[i], n))
			return 1;
	return 0;
}

static int __init card_init(void)
{
	int ret;

	if (!board_has_slot())
		return -ENODEV;
	mutex_init(&card.lock);
	card.last_signal = jiffies - msecs_to_jiffies(SETTLE_MS);
	ret = misc_register(&card_misc);
	if (!ret)
		printk(KERN_INFO "emma3sep-card: /dev/irdcard0 registered\n");
	return ret;
}
late_initcall(card_init);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("EMMA3SEP card slot as a card reader (/dev/irdcard0)");
