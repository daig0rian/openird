// SPDX-License-Identifier: GPL-2.0-only
/*
 * POE-BR100-YB1 (BMMPX1301): the front end of its tuner.
 *
 * One MXL683 demodulator on I2C bus 1 (mxl683.c), which runs on firmware the
 * owner's device carries: userland finds it in the vendor application and
 * writes it to the tuner's `firmware' attribute (S30tuner).  A demodulator
 * that is already running (a warm restart) is used as it is.
 */

#include <linux/kernel.h>
#include <linux/i2c.h>

#include "emma3sep_fe.h"
#include "mxl683.h"

#define YB1_I2C_BUS	1
#define YB1_DEMOD_ADDR	0x60
#define YB1_FW_MAX	(256 * 1024)

static struct mxl683 demod = { .addr = YB1_DEMOD_ADDR };

static int yb1_start(const u8 *fw, size_t fw_len, const char **why,
		     const char **how)
{
	int ret, alive;

	if (!demod.adap)
		demod.adap = i2c_get_adapter(YB1_I2C_BUS);
	if (!demod.adap) {
		*why = "no I2C bus 1";
		return -ENODEV;
	}
	/* no answer counts as "not running": the cold start that follows
	 * finds out soon enough whether the chip is there at all */
	alive = mxl683_alive(&demod) > 0;
	if (!alive) {
		if (!fw_len) {
			*why = "the demodulator needs its firmware (write it to `firmware')";
			return -ENOENT;
		}
		ret = mxl683_boot(&demod, fw, fw_len);
		if (ret) {
			*why = ret == -EINVAL ? "that is not a firmware container" :
			       "the demodulator did not start";
			return ret;
		}
	}
	*how = alive ? "demodulator firmware was running" :
	       "demodulator firmware loaded";
	return 0;
}

static int yb1_tune(u32 freq_khz)
{
	return mxl683_tune(&demod, freq_khz);
}

static int yb1_locked(void)
{
	return mxl683_locked(&demod);
}

static int yb1_read_cnr(u32 *mdb)
{
	return mxl683_read_cnr(&demod, mdb);
}

const struct emma3sep_fe emma3sep_fe_yb1 = {
	.board		= "PIX_AIRTUNER_LITE",
	.name		= "yb1",
	.fw_max		= YB1_FW_MAX,
	.start		= yb1_start,
	.tune		= yb1_tune,
	.locked		= yb1_locked,
	.read_cnr	= yb1_read_cnr,
};
