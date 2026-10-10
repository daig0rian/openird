/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SHIM_LINUX_I2C_H
#define SHIM_LINUX_I2C_H
#include <linux/types.h>
#define I2C_M_RD	0x0001
struct i2c_adapter { int unused; };
struct i2c_msg {
	u16 addr;
	u16 flags;
	u16 len;
	u8 *buf;
};
/* provided by the harness: plays the chip */
int i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);
#endif
