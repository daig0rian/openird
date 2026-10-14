// SPDX-License-Identifier: GPL-2.0-only
/*
 * MaxLinear MXL683 ISDB-T demodulator: cold start, firmware transfer, tuning,
 * lock and signal quality.
 *
 * Written from the written specification only:
 *   docs/specs/poe-br100-yb1-reception-stack.md  §29 (register sequences),
 *                                                §84 (tuning word, C/N),
 *                                                §86 (lock)
 *   docs/specs/mbin-firmware-container.md        §2-§4 (container), §7 (transfer)
 * The sequences there were taken from the I2C bus.  Their registers are
 * replayed as observed: what most of them mean is not known, and reads are
 * kept because they may have side effects (§29.5).
 *
 * The firmware is not part of OpenIRD.  The caller hands in the container the
 * owner's device carries.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/bitops.h>

#include "mxl683.h"

/* reads go through a selector register: 0xfffb for 2 bytes, 0xfffd for 4 */
#define SEL_R16		0xfffb
#define SEL_R32		0xfffd

#define REG_FW_ID	0xa8cc		/* 0xabcd once the firmware runs */
#define FW_ID_ALIVE	0xabcd
#define REG_FREQ	0x3029		/* centre frequency, 1/64 MHz */
#define REG_LOCK_STAGES	0x1129		/* one bit per stage, see stages_locked() */
#define REG_LOCK_FINAL	0x112e		/* 1: the last stage */
#define REG_CNR		0x112d

/* Parts of the cold start take the chip off the bus for a while (about 30 ms
 * after register 0x0003 is pulsed), so every access is retried.  A transfer
 * that was not acknowledged did nothing, so repeating it is safe. */
#define IO_TRIES	400
#define IO_WAIT_MS	10

/* firmware transfer framing (MBIN spec §7.1-§7.3) */
#define DL_TAG		0xe0
#define DL_OP_ADDR	0x01
#define DL_OP_DATA	0x02
#define DL_OP_START	0x03
#define DL_MAX_PAYLOAD	44
#define DL_SEQ_MOD	15

#define MBIN_HDR_LEN	16
#define MBIN_SEG_HDR	8
#define MBIN_MAX_SEG	32

static int raw_xfer(struct mxl683 *m, u16 flags, u8 *buf, int len)
{
	struct i2c_msg msg = {
		.addr = m->addr, .flags = flags, .len = len, .buf = buf,
	};

	return i2c_transfer(m->adap, &msg, 1) == 1 ? 0 : -EIO;
}

static int reg_write(struct mxl683 *m, u16 reg, u16 val)
{
	u8 b[4] = { reg >> 8, reg, val >> 8, val };
	int i;

	for (i = 0; i < IO_TRIES; i++) {
		if (!raw_xfer(m, 0, b, 4))
			return 0;
		msleep(IO_WAIT_MS);
	}
	return -EIO;
}

/* The selector write and the read are retried as one: a read that lost its
 * selector would answer for another register. */
static int reg_read_n(struct mxl683 *m, u16 sel, u16 reg, int n, u32 *val)
{
	u8 b[4] = { sel >> 8, sel, reg >> 8, reg }, v[4];
	int i, k;

	for (i = 0; i < IO_TRIES; i++) {
		if (!raw_xfer(m, 0, b, 4) && !raw_xfer(m, I2C_M_RD, v, n)) {
			*val = 0;
			for (k = 0; k < n; k++)
				*val = (*val << 8) | v[k];
			return 0;
		}
		msleep(IO_WAIT_MS);
	}
	return -EIO;
}

static int reg_read(struct mxl683 *m, u16 reg, u32 *val)
{
	return reg_read_n(m, SEL_R16, reg, 2, val);
}

/* --- the observed sequences (reception-stack spec §29) ---------------------- */

enum { OP_END, OP_W, OP_R, OP_FREQ };

struct op {
	u8 kind;
	u16 reg, val;
};

#define W(r, v)	{ OP_W, r, v }
#define R(r)	{ OP_R, r, 0 }

/* §29.1 A: from the first touch to the start of the transfer */
static const struct op seq_a[] = {
	R(0x300c), R(0x300d), W(0x300c, 0x0000), W(0x300d, 0x0003),
	R(0x3002), R(0x3003), W(0x3002, 0x0100), W(0x3003, 0x0008),
	W(0x3002, 0x0000), W(0x3003, 0x0008),
	R(0x3001),
	W(0x0000, 0x0000), W(0x000e, 0x0014),
	W(0x0000, 0x0000), R(0x0001),
	W(0x0000, 0x0000), W(0x0001, 0x0005),
	W(0x0000, 0x0000), R(0x0002),
	W(0x0000, 0x0000), W(0x0002, 0x0000),
	W(0x0000, 0x0000), W(0x006d, 0x008a),
	W(0x0000, 0x0000), W(0x0003, 0x0000),
	W(0x0000, 0x0000), W(0x0003, 0x0001),
	W(0x0000, 0x0000), W(0x0003, 0x0000),
	W(0x0000, 0x0000), W(0x000b, 0x0001),
	W(0x0000, 0x0000), W(0x0012, 0x0001),
	W(0x0000, 0x0001), R(0x00d5),
	W(0x0000, 0x0001), R(0x00d5),
	W(0x0000, 0x0001), R(0x00d5),
	R(0x3002), R(0x3003), W(0x3002, 0x0000), W(0x3003, 0x2008),
	R(0x302a), R(0x302b), R(0x300a), R(0x300b),
	{ OP_END }
};

/* §29.3 C: after the firmware answers, up to the first tuning */
static const struct op seq_c[] = {
	R(0x300a), R(0x300b), W(0x300a, 0x0270), W(0x300b, 0x0000),
	R(0x3002), W(0x3002, 0x0000), W(0x3003, 0x0f89),
	R(0x3002), R(0x3003), W(0x3002, 0x0000), W(0x3003, 0x0f89),
	W(0x0012, 0x0000),
	R(0x300a), R(0x300b), W(0x300a, 0x0270), W(0x300b, 0x0000),
	R(0x3002), R(0x3003), W(0x3002, 0x0000), W(0x3003, 0x0f89),
	W(0x0012, 0x0000),
	R(0x302a), R(0x302b), R(0x300a), R(0x300b),
	W(0xa85c, 0x01da), W(0xe034, 0x0000), R(0xa85c), W(0xa85c, 0x0000),
	R(0x302a), R(0x302b), R(0x300a), R(0x300b),
	W(0x000b, 0x0001),
	R(0x0196), W(0x0196, 0x0000),
	R(0x1122), R(0x11ad), W(0x11ad, 0x0000), W(0x1122, 0x0220),
	R(0x300c), R(0x300d), W(0x300c, 0x0000), W(0x300d, 0x0003),
	R(0xa80a),
	R(0x301e), R(0x301f), W(0x301e, 0x0000), W(0x301f, 0x0000),
	R(0x3002), R(0x3003), W(0x3002, 0x0000), W(0x3003, 0x0f89),
	W(0x0012, 0x0000),
	{ OP_END }
};

/* §29.4 D: tune */
static const struct op seq_d[] = {
	W(0x3028, 0x0000), W(0x3029, 0x0000),
	W(0x007c, 0x001f), W(0x0131, 0x0081),
	W(0x0049, 0x001c), W(0x00db, 0x0017),
	R(0x00ea), W(0x00ea, 0x0000), W(0x00eb, 0x00d8),
	R(0x00b6), W(0x00b6, 0x0040),
	R(0x015f), W(0x015f, 0x0037),
	R(0x0160), W(0x0160, 0x0037),
	{ OP_FREQ, REG_FREQ, 0 },
	R(0x3028), W(0x3028, 0x0000),
	W(0xe033, 0x0000),
	R(0x101c), W(0x101c, 0x0004),
	R(0x101b), W(0x101b, 0x0000), W(0x101b, 0x0001),
	{ OP_END }
};

static int replay(struct mxl683 *m, const struct op *s, u16 freq)
{
	u32 v;
	int ret = 0;

	for (; s->kind != OP_END && !ret; s++) {
		if (s->kind == OP_R)
			ret = reg_read(m, s->reg, &v);
		else
			ret = reg_write(m, s->reg,
					s->kind == OP_FREQ ? freq : s->val);
	}
	return ret;
}

/* --- the firmware container (MBIN spec §2-§4) -------------------------------- */

static u32 be24(const u8 *p)
{
	return (p[0] << 16) | (p[1] << 8) | p[2];
}

static u32 be32(const u8 *p)
{
	return ((u32)p[0] << 24) | be24(p + 1);
}

/* Walk the segments; with fn == NULL only check the container. */
static int mbin_walk(struct mxl683 *m, const u8 *mbin, size_t len,
		     int (*fn)(struct mxl683 *, u32 addr, const u8 *d, u32 n))
{
	u32 size, p = 0, i;
	u8 sum = 0;
	int k, ret;

	if (len < MBIN_HDR_LEN || mbin[0] != 'M' || mbin[1] != '1' ||
	    mbin[2] != MBIN_HDR_LEN || !mbin[3] || mbin[3] > MBIN_MAX_SEG)
		return -EINVAL;
	size = be24(mbin + 8);
	if (len < MBIN_HDR_LEN + size)
		return -EINVAL;
	for (i = 0; i < size; i++)
		sum += mbin[MBIN_HDR_LEN + i];
	if (sum != mbin[11])
		return -EINVAL;
	for (k = 0; k < mbin[3]; k++) {
		const u8 *h = mbin + MBIN_HDR_LEN + p;
		u32 n;

		if (p + MBIN_SEG_HDR > size || h[0] != 'S')
			return -EINVAL;
		n = be24(h + 1);
		if (p + MBIN_SEG_HDR + n > size)
			return -EINVAL;
		if (fn) {
			ret = fn(m, be32(h + 4), h + MBIN_SEG_HDR, n);
			if (ret)
				return ret;
		}
		/* the next segment starts on a 4-byte boundary */
		p += MBIN_SEG_HDR + ((n + 3) & ~3u);
	}
	return p == size ? 0 : -EINVAL;
}

int mxl683_check_mbin(const u8 *mbin, size_t len)
{
	return mbin_walk(NULL, mbin, len, NULL);
}
EXPORT_SYMBOL_GPL(mxl683_check_mbin);

/* --- the transfer (MBIN spec §7) --------------------------------------------- */

static int dl_cmd(struct mxl683 *m, int *seq, u8 op, const u8 *payload, int len)
{
	u8 b[4 + DL_MAX_PAYLOAD], sum = 0;
	int i;

	for (i = 0; i < len; i++)
		sum += payload[i];
	b[0] = DL_TAG | *seq;
	b[1] = op;
	b[2] = sum;
	b[3] = len;
	memcpy(b + 4, payload, len);
	for (i = 0; i < IO_TRIES; i++) {
		if (!raw_xfer(m, 0, b, 4 + len))
			break;
		msleep(IO_WAIT_MS);
	}
	if (i == IO_TRIES)
		return -EIO;
	/* the counter runs 0..14 over the data blocks of a segment; address
	 * and start commands carry 0 and set it back */
	*seq = op == DL_OP_DATA ? (*seq + 1) % DL_SEQ_MOD : 0;
	return 0;
}

static int dl_addr(struct mxl683 *m, int *seq, u8 op, u32 addr)
{
	u8 a[4] = { addr >> 24, addr >> 16, addr >> 8, addr };

	return dl_cmd(m, seq, op, a, 4);
}

static int dl_segment(struct mxl683 *m, u32 addr, const u8 *d, u32 n)
{
	int seq = 0, ret;

	ret = dl_addr(m, &seq, DL_OP_ADDR, addr);
	while (!ret && n) {
		u32 k = min_t(u32, n, DL_MAX_PAYLOAD);

		ret = dl_cmd(m, &seq, DL_OP_DATA, d, k);
		d += k;
		n -= k;
	}
	return ret;
}

int mxl683_alive(struct mxl683 *m)
{
	u8 b[4] = { SEL_R16 >> 8, SEL_R16 & 0xff, REG_FW_ID >> 8, REG_FW_ID & 0xff };
	u8 v[2];

	/* one try: a chip that is not there must not cost 4 seconds */
	if (raw_xfer(m, 0, b, 4) || raw_xfer(m, I2C_M_RD, v, 2))
		return -EIO;
	return ((v[0] << 8) | v[1]) == FW_ID_ALIVE;
}
EXPORT_SYMBOL_GPL(mxl683_alive);

int mxl683_boot(struct mxl683 *m, const u8 *mbin, size_t len)
{
	u32 v;
	int ret, i, seq = 0;

	ret = mxl683_check_mbin(mbin, len);
	if (ret)
		return ret;
	ret = replay(m, seq_a, 0);
	if (ret)
		return ret;
	ret = mbin_walk(m, mbin, len, dl_segment);
	if (ret)
		return ret;
	/* §29.2: a 32-bit read either side of the start command */
	reg_read_n(m, SEL_R32, 0x0003, 4, &v);
	ret = dl_addr(m, &seq, DL_OP_START, be32(mbin + 4));
	if (ret)
		return ret;
	reg_read_n(m, SEL_R32, 0x0003, 4, &v);
	for (i = 0; i < 100; i++) {		/* seen after 50 ms */
		ret = reg_read(m, REG_FW_ID, &v);
		if (!ret && v == FW_ID_ALIVE)
			break;
		msleep(10);
	}
	if (i == 100)
		return -ETIMEDOUT;
	return replay(m, seq_c, 0);
}
EXPORT_SYMBOL_GPL(mxl683_boot);

int mxl683_tune(struct mxl683 *m, u32 freq_khz)
{
	/* The sequence was observed on UHF only (13..62 ch). */
	if (freq_khz < 470000 || freq_khz > 770000)
		return -EINVAL;
	/* centre frequency in 1/64 MHz, to the nearest (§84) */
	return replay(m, seq_d, (freq_khz * 64 + 500) / 1000);
}
EXPORT_SYMBOL_GPL(mxl683_tune);

/* Bits 0..3 of 0x1129 must be set.  Bit 4 is set on most stations but never
 * on some that otherwise lock just as well (§86.1). */
static int stages_locked(u32 stages)
{
	return (stages & 0x0f) == 0x0f;
}

int mxl683_locked(struct mxl683 *m)
{
	u32 stages, final;
	int ret;

	ret = reg_read(m, REG_LOCK_STAGES, &stages);
	if (!ret)
		ret = reg_read(m, REG_LOCK_FINAL, &final);
	if (ret)
		return ret;
	return stages_locked(stages) && final == 1;
}
EXPORT_SYMBOL_GPL(mxl683_locked);

/* log2(x) in 16.16, x >= 1, by squaring the mantissa */
static u32 log2_q16(u32 x)
{
	int n = fls(x) - 1, i;
	u32 r = n << 16, mant = x << (31 - n);	/* 1.31, in [1, 2) */

	for (i = 15; i >= 0; i--) {
		u64 sq = (u64)mant * mant;	/* 2.62 */

		if (sq >> 63) {
			r |= 1u << i;
			mant = sq >> 32;
		} else {
			mant = sq >> 31;
		}
	}
	return r;
}

int mxl683_read_cnr(struct mxl683 *m, u32 *mdb)
{
	u32 v, sum = 0, mlog;
	int i, ret;

	*mdb = 0;
	ret = reg_read(m, REG_LOCK_STAGES, &v);
	if (ret)
		return ret;
	if (!stages_locked(v))
		return 0;		/* the reading means nothing before lock */
	for (i = 0; i < 4; i++) {	/* it wanders by +-15 %: average */
		ret = reg_read(m, REG_CNR, &v);
		if (ret)
			return ret;
		sum += v;
		msleep(20);
	}
	v = sum / 4;
	if (!v)
		return 0;
	/* C/N = 59.3 dB - 10 log10(raw), good to about 1 dB (§84).
	 * 1000 log10(x) = log2(x) * 301.03 */
	mlog = (log2_q16(v) >> 4) * 30103 / 409600;
	*mdb = 10 * mlog < 59300 ? 59300 - 10 * mlog : 0;
	return 0;
}
EXPORT_SYMBOL_GPL(mxl683_read_cnr);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MaxLinear MXL683 ISDB-T demodulator");
