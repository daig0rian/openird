// SPDX-License-Identifier: GPL-2.0-only
/*
 * SB-TV06-WRIP (POE-BR310L-SB1): the front end of its tuner.
 *
 * A TC90532 demodulator on I2C bus 1, terrestrial half at 0x10 and satellite
 * half at 0x11, with the tuners behind each half's I2C gate (register 0xfe):
 * MxL601RF on the terrestrial one, TDA8263HN on the satellite one.  No
 * firmware.  The whole front end is powered by GPIO 43, which nothing raises
 * until this driver does.
 *
 * The byte sequences are the ones docs/specs/sb-tv06-wrip-frontend.md gives
 * (section numbers below), in the order and form the mainline tc90522.c
 * uses: set the IF AGC to manual, set the tuner, AGC back to automatic, reset
 * the demodulator.  Both halves put their transport stream out on the same
 * pins, and only the awake one does: a tune first wakes its half and puts
 * the other to sleep (section 3).  On a satellite transponder the
 * demodulator passes one of up to 8 transport streams, chosen by TSID
 * (section 5 (4), section 11).
 */

#include <linux/kernel.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/string.h>

#include "emma3sep_fe.h"
#include "ird_ptx_ioctl.h"

#define WRIP_I2C_BUS	1
#define WRIP_TERR	0x10	/* TC90532, terrestrial (OFDM) half */
#define WRIP_SAT	0x11	/* TC90532, satellite (PSK) half */
#define WRIP_TUNER	0xc0	/* the tuner behind a half's gate, 8-bit address */
#define WRIP_GPIO_POWER	43

static struct i2c_adapter *adap;
static int powered;
static int band;	/* the PTX_ISDB_*_SYSTEM last tuned */

static int wr(u8 addr, const u8 *buf, int len)
{
	struct i2c_msg m = { .addr = addr, .flags = 0, .len = len,
			     .buf = (u8 *)buf };
	int ret = i2c_transfer(adap, &m, 1);

	return ret == 1 ? 0 : ret < 0 ? ret : -EIO;
}

static int wreg(u8 addr, u8 reg, u8 val)
{
	u8 b[2] = { reg, val };

	return wr(addr, b, 2);
}

/* the register number, then n bytes (section 1) */
static int rreg(u8 addr, u8 reg, u8 *val, int n)
{
	struct i2c_msg m[2] = {
		{ .addr = addr, .flags = 0, .len = 1, .buf = &reg },
		{ .addr = addr, .flags = I2C_M_RD, .len = n, .buf = val },
	};
	int ret = i2c_transfer(adap, m, 2);

	return ret == 2 ? 0 : ret < 0 ? ret : -EIO;
}

static int rmw(u8 addr, u8 reg, u8 and, u8 or)
{
	u8 v;
	int ret = rreg(addr, reg, &v, 1);

	return ret ? ret : wreg(addr, reg, (v & and) | or);
}

/* one message to the tuner behind a half's gate: [0xfe, 0xc0, bytes] */
static int gate(u8 half, const u8 *bytes, int n)
{
	u8 b[2 + 10];

	if (n > 10)
		return -EINVAL;
	b[0] = 0xfe;
	b[1] = WRIP_TUNER;
	memcpy(b + 2, bytes, n);
	return wr(half, b, n + 2);
}

#define GATE(half, ...) ({					\
	static const u8 _b[] = { __VA_ARGS__ };			\
	gate(half, _b, sizeof(_b));				\
})

/* (register, value) pairs, one message each */
static int pairs(u8 addr, const u8 *p, int n)
{
	int i, ret = 0;

	for (i = 0; i + 1 < n && !ret; i += 2)
		ret = wreg(addr, p[i], p[i + 1]);
	return ret;
}

/* section 2 (1), (2) */
static const u8 sat_init[] = {
	0x04, 0x02, 0x1c, 0x2a, 0x1d, 0xaa, 0x1e, 0x20, 0x1f, 0x08,
	0x85, 0x7a, 0x87, 0x00, 0xa3, 0x55, 0xa5, 0x40,
};
static const u8 terr_init[] = {
	0x0e, 0x01, 0x0f, 0x00, 0x1c, 0x20, 0x1d, 0x02, 0x1e, 0xaa,
	0x1f, 0xaa, 0x31, 0x0d, 0x32, 0x21, 0x58, 0x08, 0x75, 0x02,
	0x77, 0x01, 0xe4, 0xf4,
};

/* section 2 (3): MxL601RF, one message per row, each row "length, bytes" */
static const u8 mxl601_init[] = {
	1, 0xff,
	10, 0x00, 0x01, 0x96, 0x00, 0x00, 0x00, 0x14, 0x13, 0x6d, 0x8a,
	10, 0x6d, 0x0a, 0xdf, 0x19, 0x45, 0x1b, 0xa9, 0x59, 0xaa, 0x6a,
	10, 0xbe, 0x4c, 0xcf, 0x25, 0xd0, 0x34, 0x77, 0xe7, 0x78, 0xe3,
	10, 0x6f, 0x51, 0x7b, 0x84, 0x7c, 0x9f, 0x56, 0x41, 0xcd, 0x64,
	10, 0xc3, 0x2c, 0x9d, 0x61, 0xf7, 0x52, 0x58, 0x81, 0xb1, 0x30,
	2, 0x01, 0x89,
	2, 0x02, 0x00,
	4, 0x04, 0x01, 0x05, 0x1b,
	6, 0x08, 0x05, 0x09, 0x42, 0x5e, 0x00,
	2, 0x0a, 0x00,
	10, 0x0c, 0x00, 0x13, 0x04, 0x53, 0xfe, 0x57, 0x91, 0x5d, 0x0e,
	10, 0x94, 0x44, 0x5a, 0xd9, 0x5b, 0x16, 0x5c, 0xb1, 0x62, 0xc2,
	10, 0x6e, 0x01, 0x6f, 0x51, 0x87, 0x77, 0x88, 0x55, 0x93, 0x22,
	10, 0x97, 0x02, 0xba, 0x30, 0x98, 0xaf, 0x9b, 0x20, 0x9c, 0x1e,
	10, 0xa0, 0x18, 0xa5, 0x09, 0xc2, 0xa9, 0xc5, 0x7c, 0xcd, 0xe8,
	10, 0xce, 0x7f, 0xd5, 0x03, 0xd9, 0x04, 0xea, 0x0d, 0xdc, 0x1c,
	4, 0x03, 0x00, 0x03, 0x01,
	0
};

static int init_front_end(void)
{
	const u8 *p;
	int ret;

	ret = pairs(WRIP_SAT, sat_init, sizeof(sat_init));
	if (!ret)
		ret = pairs(WRIP_TERR, terr_init, sizeof(terr_init));
	for (p = mxl601_init; !ret && *p; p += 1 + *p)
		ret = gate(WRIP_TERR, p + 1, *p);
	if (ret)
		return ret;
	/* (4) the satellite tuner, (5) the attenuator off (section 6.3) */
	msleep(100);
	ret = GATE(WRIP_SAT, 0x60, 0x08);
	msleep(16);
	if (!ret)
		ret = GATE(WRIP_SAT, 0x60, 0x00);
	msleep(250);
	if (!ret)
		ret = rmw(WRIP_TERR, 0x1c, 0xff, 0x10);
	return ret;
}

/* section 3: the satellite half asleep, the terrestrial half awake, and the
 * transport stream output from the terrestrial half */
static int to_terrestrial(void)
{
	int ret;

	ret = rmw(WRIP_SAT, 0x13, 0xff, 0x80);
	if (!ret)
		ret = rmw(WRIP_SAT, 0x1e, 0xff, 0x08);
	if (!ret)
		ret = wreg(WRIP_SAT, 0x17, 0x01);
	if (!ret)
		ret = rmw(WRIP_TERR, 0x03, 0xef, 0x00);
	if (!ret)
		ret = rmw(WRIP_TERR, 0x1c, 0x7f, 0x00);
	if (!ret)
		ret = wreg(WRIP_TERR, 0x0e, 0x01);
	if (!ret)
		ret = wreg(WRIP_SAT, 0x07, 0x07);
	return ret;
}

/* section 3: the terrestrial half asleep, the satellite half awake, and the
 * transport stream output from the satellite half */
static int to_satellite(void)
{
	int ret;

	ret = GATE(WRIP_SAT, 0x60, 0x08);
	msleep(16);
	if (!ret)
		ret = GATE(WRIP_SAT, 0x60, 0x00);
	if (!ret)
		ret = GATE(WRIP_TERR, 0x12, 0x00);
	if (!ret)
		ret = GATE(WRIP_TERR, 0x0b, 0x00);
	if (!ret)
		ret = wreg(WRIP_TERR, 0x03, 0x80);
	if (!ret)
		ret = wreg(WRIP_TERR, 0x03, 0x90);
	if (!ret)
		ret = rmw(WRIP_TERR, 0x1c, 0xff, 0x80);
	if (!ret)
		ret = wreg(WRIP_SAT, 0x17, 0x00);
	if (!ret)
		ret = rmw(WRIP_SAT, 0x13, 0x7f, 0x00);
	if (!ret)
		ret = rmw(WRIP_SAT, 0x1e, 0xf7, 0x00);
	if (!ret)
		ret = wreg(WRIP_TERR, 0x0e, 0x07);
	if (!ret)
		ret = wreg(WRIP_SAT, 0x07, 0x01);
	return ret;
}

static int wrip_start(const u8 *fw, size_t fw_len, const char **why,
		      const char **how)
{
	int ret;

	if (!adap)
		adap = i2c_get_adapter(WRIP_I2C_BUS);
	if (!adap) {
		*why = "no I2C bus 1";
		return -ENODEV;
	}
	/* section 1: the front end answers about 0.2 s after power-up, with
	 * its registers back at their defaults */
	if (!powered) {
		ret = gpio_request(WRIP_GPIO_POWER, "frontend-power");
		if (!ret)
			ret = gpio_direction_output(WRIP_GPIO_POWER, 1);
		if (ret) {
			*why = "cannot switch the front end on (GPIO 43)";
			return ret;
		}
		powered = 1;
		msleep(250);
	}
	ret = init_front_end();
	if (ret) {
		*why = "the front end does not answer on I2C bus 1";
		return ret;
	}
	*how = "front end initialised";
	return 0;
}

/* section 4.  freq_khz is the centre plus 1/7 MHz (ird_chrdev.c), which is
 * what the tuner's frequency word wants: round(64 * MHz). */
static int terr_tune(u32 freq_khz)
{
	u16 word = (64 * freq_khz + 500) / 1000;
	u8 hi_band = freq_khz > 698000;
	u8 f[4], band[8];
	int ret;

	ret = to_terrestrial();
	/* (1) the IF AGC to manual */
	if (!ret)
		ret = rmw(WRIP_TERR, 0x47, 0x7f, 0x00);
	if (!ret)
		ret = rmw(WRIP_TERR, 0x47, 0x7f, 0x80);
	if (!ret)
		ret = wreg(WRIP_TERR, 0x25, 0x00);
	if (!ret)
		ret = wreg(WRIP_TERR, 0x20, 0x00);
	if (!ret)
		ret = rmw(WRIP_TERR, 0x23, 0xfe, 0x01);
	/* (2) the tuner */
	if (!ret)
		ret = GATE(WRIP_TERR, 0x0b, 0x01);
	if (!ret)
		ret = GATE(WRIP_TERR, 0x12, 0x01);
	msleep(36);
	if (!ret)
		ret = GATE(WRIP_TERR, 0x0f, 0x20);
	f[0] = 0x10;
	f[1] = word & 0xff;
	f[2] = 0x11;
	f[3] = word >> 8;
	if (!ret)
		ret = gate(WRIP_TERR, f, sizeof(f));
	band[0] = 0x00; band[1] = 0x01;
	band[2] = 0x31; band[3] = hi_band ? 0x91 : 0x81;
	band[4] = 0x00; band[5] = 0x00;
	band[6] = 0x7c; band[7] = hi_band ? 0x9f : 0x1f;
	if (!ret)
		ret = gate(WRIP_TERR, band, sizeof(band));
	if (!ret)
		ret = GATE(WRIP_TERR, 0x12, 0x00, 0x14, 0x13, 0x13, 0x04);
	if (!ret)
		ret = GATE(WRIP_TERR, 0x12, 0x01);
	msleep(36);
	/* (3) the AGC back to automatic, and reset the demodulator */
	if (!ret)
		ret = rmw(WRIP_TERR, 0x23, 0xfe, 0x00);
	if (!ret)
		ret = wreg(WRIP_TERR, 0x01, 0x40);
	return ret;
}

/* section 5 (2): the satellite tuner's register 1, by the IF in MHz */
static const struct {
	u16 mhz;
	u8 nn;
} sat_nn[] = {
	{ 1049, 0x23 }, { 1087, 0x23 }, { 1126, 0x24 }, { 1164, 0x24 },	/* BS */
	{ 1202, 0x25 }, { 1241, 0x26 }, { 1279, 0x27 }, { 1318, 0x27 },
	{ 1356, 0x28 }, { 1394, 0x29 }, { 1433, 0x2a }, { 1471, 0x2b },
	{ 1613, 0x2e }, { 1653, 0x2e }, { 1693, 0x2f }, { 1733, 0x2f },	/* CS */
	{ 1773, 0x30 }, { 1813, 0x31 }, { 1853, 0x32 }, { 1893, 0x32 },
	{ 1933, 0x33 }, { 1973, 0x33 }, { 2013, 0x34 }, { 2053, 0x34 },
};

/* section 5.  The tuner wants whole MHz: the kHz cut down (section 11) is
 * what the vendor's application gave it on all 24 transponders. */
static int sat_tune(u32 freq_khz)
{
	u32 f = freq_khz / 1000;
	u8 nn = 0, tda[11][2] = {
		{ 0x00, 0x01 }, { 0x10, 0 }, { 0x20, f >> 7 },
		{ 0x30, (f << 1) & 0xff }, { 0x40, 0x88 }, { 0x50, 0xf0 },
		{ 0x60, 0x83 }, { 0x70, f < 1600 ? 0x82 : 0x81 }, /* BS, CS */
		{ 0x80, 0x1a }, { 0x90, 0xd0 }, { 0x50, 0xf0 },
	};
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(sat_nn); i++)
		if (sat_nn[i].mhz == f)
			nn = sat_nn[i].nn;
	if (!nn)
		return -EINVAL;
	tda[1][1] = nn;

	ret = to_satellite();
	/* (1) the IF AGC to manual */
	if (!ret)
		ret = wreg(WRIP_SAT, 0x0a, 0x00);
	if (!ret)
		ret = wreg(WRIP_SAT, 0x11, 0x00);
	if (!ret)
		ret = rmw(WRIP_SAT, 0x10, 0xfd, 0x00);
	if (!ret)
		ret = wreg(WRIP_SAT, 0x03, 0x01);
	/* (2) the tuner, one register a message; registers 2 and 3 hold the
	 * division ratio, which is f */
	for (i = 0; i < ARRAY_SIZE(tda) && !ret; i++)
		ret = gate(WRIP_SAT, tda[i], 2);
	/* (3) the AGC back to automatic, and reset the demodulator */
	if (!ret)
		ret = wreg(WRIP_SAT, 0x0a, 0xff);
	if (!ret)
		ret = wreg(WRIP_SAT, 0x11, 0x00);
	if (!ret)
		ret = rmw(WRIP_SAT, 0x10, 0xfd, 0x02);
	if (!ret)
		ret = wreg(WRIP_SAT, 0x03, 0x01);
	return ret;
}

static int wrip_tune(int system, u32 freq_khz)
{
	band = system;
	return system == PTX_ISDB_S_SYSTEM ? sat_tune(freq_khz) :
					     terr_tune(freq_khz);
}

/* section 6.1: any of the three layers locked; section 6.2: none of "no
 * signal", "waiting for the carrier", "waiting for sync" */
static int wrip_locked(void)
{
	u8 v;
	int ret;

	if (band == PTX_ISDB_S_SYSTEM) {
		ret = rreg(WRIP_SAT, 0xc3, &v, 1);
		return ret ? ret : (v & 0xf0) == 0;
	}
	ret = rreg(WRIP_TERR, 0x96, &v, 1);
	return ret ? ret : (v & 0xe0) != 0;
}

static int tsid_now(u16 *tsid)
{
	u8 b[2];
	int ret = rreg(WRIP_SAT, 0xe6, b, 2);

	if (!ret)
		*tsid = (b[0] << 8) | b[1];
	return ret;
}

/* section 11: 0..7 = the n-th TSID of the transponder's table, 12 and up =
 * a TSID */
static int wrip_select_ts(u16 stream_id)
{
	u16 tsid = stream_id, now;
	u8 b[2];
	int i, ret;

	if (stream_id < 8) {
		/* the table fills in a little after the lock */
		for (i = 0; i < 100; i++) {
			ret = rreg(WRIP_SAT, 0xce + 2 * stream_id, b, 2);
			if (ret)
				return ret;
			tsid = (b[0] << 8) | b[1];
			if (tsid)
				break;
			msleep(10);
		}
		if (!tsid || tsid == 0xffff)
			return -EAGAIN;		/* no such stream here */
	} else if (stream_id < 12) {
		return -EINVAL;
	}
	/* already the one coming out: no reset, no re-lock */
	ret = tsid_now(&now);
	if (ret || now == tsid)
		return ret;

	ret = wreg(WRIP_SAT, 0x8f, tsid >> 8);
	if (!ret)
		ret = wreg(WRIP_SAT, 0x90, tsid & 0xff);
	if (!ret)
		ret = wreg(WRIP_SAT, 0x03, 0x01);
	for (i = 0; i < 200 && !ret; i++) {	/* about 1 s seen; allow 2 */
		msleep(10);
		ret = tsid_now(&now);
		if (!ret && now == tsid)
			return 0;
	}
	return ret ? ret : -EAGAIN;
}

/* sections 6.1, 6.2, 11: the demodulator's own value, as px4_drv hands it
 * out (the TC90522 has the same registers) */
static int wrip_read_cnr_raw(u32 *raw)
{
	u8 b[3];
	int ret;

	if (band == PTX_ISDB_S_SYSTEM) {
		ret = rreg(WRIP_SAT, 0xbc, b, 2);
		if (!ret)
			*raw = (b[0] << 8) | b[1];
	} else {
		ret = rreg(WRIP_TERR, 0x8b, b, 3);
		if (!ret)
			*raw = (b[0] << 16) | (b[1] << 8) | b[2];
	}
	return ret;
}

const struct emma3sep_fe emma3sep_fe_wrip = {
	.board		= "PIX_AIRTUNER_BR310L",
	.name		= "wrip",
	.systems	= PTX_ISDB_T_SYSTEM | PTX_ISDB_S_SYSTEM,
	.start		= wrip_start,
	.tune		= wrip_tune,
	.locked		= wrip_locked,
	.select_ts	= wrip_select_ts,
	.read_cnr_raw	= wrip_read_cnr_raw,
};
