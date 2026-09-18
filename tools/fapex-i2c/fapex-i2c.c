// SPDX-License-Identifier: GPL-2.0-only
//
// fapex-i2c: stage-B bring-up helper for the FAPex I2C character device.
//
// Milestone 1 observation tool.  It talks to the vendor FAPex I2C chardev
// (major 244; SoC I2C0 = fapi174, I2C1 = fapi175 on SB-TV02-WFPL) using the
// *published* FAPEX ioctl ABI from the GPL header abst_libi2c.h / abst_lib.h.
// It contains NO vendor code -- it is a clean user of the chardev contract,
// the same one the vendor userland uses.  We use it to probe the ISDB-T demod
// (MB86A22, I2C slave 0x20) by hand, without the vendor daemon (see spec §5.5).
//
// ABI (GPL abst_lib.h / abst_libi2c.h):
//   FAPEX_DEV_TYPE       = 0xFF
//   FAPEX_IOCCOM_NB_I2C  = 0x60
//   OPEN  = _IOW(0xFF, 0x60, FAPEX_I2C_OpenParamStrT[24])  -> 0x4018FF60
//   READ  = _IOR(0xFF, 0x62, FAPEX_I2C_RWParamStrT[20])    -> 0x8014FF62
//   WRITE = _IOW(0xFF, 0x62, FAPEX_I2C_RWParamStrT[20])    -> 0x4014FF62
//   speed: 1=100kHz 2=400kHz;  protocol: 0=common 1=restart
//   type:  0=hardware 1=software;  timeOut in ms (>=100)
//
// A READ transaction writes the tx bytes (the register/subaddress) then does a
// repeated-start read of rxLen bytes -- exactly what the vendor
// MB86A22_..._Detect does: FE_I2C_Read(h, 0x20, {reg}, 1, {val}, 1) to read a
// demod register.  MB86A22 chip-ID register 0x00 reads back 0x23 (0x13 for
// MB86A20S), so `read 0x20 0x00` is the definitive ACK/identity probe.
//
// Usage:
//   fapex-i2c DEV BLOCK read  SLAVE REG [LEN]      OPEN + read LEN bytes from REG
//   fapex-i2c DEV BLOCK write SLAVE REG [BYTE...]  OPEN + write REG(+bytes)
//   fapex-i2c DEV BLOCK scan  [FIRST LAST]         OPEN+read reg0 across slaves
//   options (before the mode): --speed {100|400}  --timeout MS  --restart
//
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>

#define FAPEX_DEV_TYPE 0xFF
#define NB_I2C         0x60

typedef struct {
	uint32_t blockIndex;    // 0=I2C0 1=I2C1 2=I2C2
	int32_t  speed;         // 1=100k 2=400k
	int32_t  protocol;      // 0=common 1=restart
	uint32_t slaveAddress;  // default slave (per-xfer addr is in RWParam)
	int32_t  type;          // 0=hardware 1=software
	uint32_t timeOut;       // ms
} i2c_open_t;

typedef struct {
	uint8_t   msSlaveAddress;
	uint8_t  *txDataPtr;
	uint32_t  txDataLength;
	uint8_t  *rxDataPtr;
	uint32_t  rxDataLength;
} i2c_rw_t;

#define I2C_OPEN  _IOW(FAPEX_DEV_TYPE, NB_I2C + 0, i2c_open_t)   /* 0x4018FF60 */
#define I2C_READ  _IOR(FAPEX_DEV_TYPE, NB_I2C + 2, i2c_rw_t)     /* 0x8014FF62 */
#define I2C_WRITE _IOW(FAPEX_DEV_TYPE, NB_I2C + 2, i2c_rw_t)     /* 0x4014FF62 */

static int g_speed = 1;      /* 100k */
static int g_proto = 0;      /* common */
static uint32_t g_timeout = 500;
static long g_openslave = -1; /* OpenParam.slaveAddress override; -1 = use RW slave */

static int i2c_open_bus(const char *dev, uint32_t block, uint32_t slave)
{
	int fd = open(dev, O_RDWR);
	if (fd < 0) { fprintf(stderr, "open(%s): %s\n", dev, strerror(errno)); return -1; }
	uint32_t oslave = (g_openslave >= 0) ? (uint32_t)g_openslave : slave;
	i2c_open_t op = { block, g_speed, g_proto, oslave, 0 /*hw*/, g_timeout };
	if (ioctl(fd, I2C_OPEN, &op) != 0) {
		fprintf(stderr, "I2C_OPEN block=%u slave=0x%02x: %s\n",
			block, slave, strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

static int cmd_read(const char *dev, uint32_t block, int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "read SLAVE REG [LEN]\n"); return 2; }
	uint32_t slave = (uint32_t)strtoul(argv[0], NULL, 0);
	uint8_t  reg   = (uint8_t)strtoul(argv[1], NULL, 0);
	uint32_t len   = (argc >= 3) ? (uint32_t)strtoul(argv[2], NULL, 0) : 1;
	if (len == 0 || len > 256) { fprintf(stderr, "bad LEN\n"); return 2; }

	int fd = i2c_open_bus(dev, block, slave);
	if (fd < 0) return 1;

	uint8_t rx[256];
	memset(rx, 0, sizeof rx);
	i2c_rw_t rw = { (uint8_t)slave, &reg, 1, rx, len };
	if (ioctl(fd, I2C_READ, &rw) != 0) {
		fprintf(stderr, "I2C_READ slave=0x%02x reg=0x%02x len=%u: %s (NO ACK / timeout?)\n",
			slave, reg, len, strerror(errno));
		close(fd);
		return 1;
	}
	printf("read slave 0x%02x reg 0x%02x:", slave, reg);
	for (uint32_t i = 0; i < len; i++) printf(" %02x", rx[i]);
	printf("\n");
	close(fd);
	return 0;
}

static int cmd_write(const char *dev, uint32_t block, int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "write SLAVE REG [BYTE...]\n"); return 2; }
	uint32_t slave = (uint32_t)strtoul(argv[0], NULL, 0);
	uint8_t  tx[256];
	uint32_t n = 0;
	tx[n++] = (uint8_t)strtoul(argv[1], NULL, 0);          /* reg */
	for (int i = 2; i < argc && n < sizeof tx; i++)
		tx[n++] = (uint8_t)strtoul(argv[i], NULL, 0);

	int fd = i2c_open_bus(dev, block, slave);
	if (fd < 0) return 1;

	i2c_rw_t rw = { (uint8_t)slave, tx, n, NULL, 0 };
	if (ioctl(fd, I2C_WRITE, &rw) != 0) {
		fprintf(stderr, "I2C_WRITE slave=0x%02x len=%u: %s (NO ACK / timeout?)\n",
			slave, n, strerror(errno));
		close(fd);
		return 1;
	}
	printf("write slave 0x%02x:", slave);
	for (uint32_t i = 0; i < n; i++) printf(" %02x", tx[i]);
	printf(" OK\n");
	close(fd);
	return 0;
}

static int cmd_scan(const char *dev, uint32_t block, int argc, char **argv)
{
	uint32_t first = (argc >= 1) ? (uint32_t)strtoul(argv[0], NULL, 0) : 0x08;
	uint32_t last  = (argc >= 2) ? (uint32_t)strtoul(argv[1], NULL, 0) : 0x77;
	int found = 0;
	printf("scan block %u (0x%02x..0x%02x), reg0 read:\n", block, first, last);
	for (uint32_t a = first; a <= last; a++) {
		int fd = i2c_open_bus(dev, block, a);
		if (fd < 0) continue;
		uint8_t reg = 0x00, rx = 0;
		i2c_rw_t rw = { (uint8_t)a, &reg, 1, &rx, 1 };
		if (ioctl(fd, I2C_READ, &rw) == 0) {
			printf("  0x%02x ACK reg0=0x%02x\n", a, rx);
			found++;
		}
		close(fd);
	}
	printf("scan done: %d device(s)\n", found);
	return found ? 0 : 1;
}

/* Generic transfer: write TXBYTES to SLAVE, then read RXLEN bytes.
 *   RXLEN>0, TX given  -> write-then-read (one transaction)
 *   RXLEN==0, TX given -> pure write
 *   RXLEN>0, no TX      -> pure read
 * This covers the Toshiba TC90522/TC90532 tuner I2C gateway (reg 0xfe):
 *   tuner write:  xfer 0x20 0  0xfe <taddr<<1>   <data...>
 *   tuner read:   xfer 0x20 N  0xfe <taddr<<1|1>            (write setup, then read N) */
static int cmd_xfer(const char *dev, uint32_t block, int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "xfer SLAVE RXLEN [TXBYTE...]\n"); return 2; }
	uint32_t slave = (uint32_t)strtoul(argv[0], NULL, 0);
	uint32_t rxlen = (uint32_t)strtoul(argv[1], NULL, 0);
	uint8_t  tx[256];
	uint32_t n = 0;
	for (int i = 2; i < argc && n < sizeof tx; i++)
		tx[n++] = (uint8_t)strtoul(argv[i], NULL, 0);
	if (rxlen > 256) { fprintf(stderr, "bad RXLEN\n"); return 2; }

	int fd = i2c_open_bus(dev, block, slave);
	if (fd < 0) return 1;

	uint8_t rx[256];
	memset(rx, 0, sizeof rx);
	if (rxlen == 0) {
		i2c_rw_t rw = { (uint8_t)slave, tx, n, NULL, 0 };
		if (ioctl(fd, I2C_WRITE, &rw) != 0) {
			fprintf(stderr, "I2C_WRITE slave=0x%02x: %s (NO ACK/timeout?)\n",
				slave, strerror(errno));
			close(fd); return 1;
		}
		printf("xfer w slave 0x%02x:", slave);
		for (uint32_t i = 0; i < n; i++) printf(" %02x", tx[i]);
		printf(" OK\n");
	} else {
		i2c_rw_t rw = { (uint8_t)slave, n ? tx : NULL, n, rx, rxlen };
		if (ioctl(fd, I2C_READ, &rw) != 0) {
			fprintf(stderr, "I2C_READ slave=0x%02x: %s (NO ACK/timeout?)\n",
				slave, strerror(errno));
			close(fd); return 1;
		}
		printf("xfer slave 0x%02x tx[%u]:", slave, n);
		for (uint32_t i = 0; i < n; i++) printf(" %02x", tx[i]);
		printf(" -> rx[%u]:", rxlen);
		for (uint32_t i = 0; i < rxlen; i++) printf(" %02x", rx[i]);
		printf("\n");
	}
	close(fd);
	return 0;
}

static void usage(void)
{
	fprintf(stderr,
	  "usage: fapex-i2c [--speed 100|400] [--timeout MS] [--restart] DEV BLOCK MODE ARGS\n"
	  "  DEV   = /dev/fapi/fapi174 (I2C0) | /dev/fapi/fapi175 (I2C1)\n"
	  "  BLOCK = 0 (I2C0) | 1 (I2C1) | 2\n"
	  "  read  SLAVE REG [LEN]\n"
	  "  write SLAVE REG [BYTE...]\n"
	  "  xfer  SLAVE RXLEN [TXBYTE...]   (write TX then read RXLEN; RXLEN=0 = write only)\n"
	  "  scan  [FIRST LAST]\n"
	  "  # TC90522 tuner gateway: xfer 0x20 N 0xfe <taddr<<1|1> (read), write 0x20 0xfe <taddr<<1> ...\n");
}

int main(int argc, char **argv)
{
	int i = 1;
	for (; i < argc; i++) {
		if (!strcmp(argv[i], "--speed") && i + 1 < argc) {
			g_speed = (atoi(argv[++i]) >= 400) ? 2 : 1;
		} else if (!strcmp(argv[i], "--timeout") && i + 1 < argc) {
			g_timeout = (uint32_t)strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "--openslave") && i + 1 < argc) {
			g_openslave = (long)strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "--restart")) {
			g_proto = 1;
		} else break;
	}
	fprintf(stderr, "fapex-i2c: OPEN=0x%lx READ=0x%lx WRITE=0x%lx (speed=%s proto=%d to=%ums)\n",
		(unsigned long)I2C_OPEN, (unsigned long)I2C_READ, (unsigned long)I2C_WRITE,
		g_speed == 2 ? "400k" : "100k", g_proto, g_timeout);

	if (argc - i < 3) { usage(); return 2; }
	const char *dev = argv[i];
	uint32_t block  = (uint32_t)strtoul(argv[i + 1], NULL, 0);
	const char *mode = argv[i + 2];
	int rargc = argc - (i + 3);
	char **rargv = argv + (i + 3);

	if (!strcmp(mode, "read"))  return cmd_read(dev, block, rargc, rargv);
	if (!strcmp(mode, "write")) return cmd_write(dev, block, rargc, rargv);
	if (!strcmp(mode, "xfer"))  return cmd_xfer(dev, block, rargc, rargv);
	if (!strcmp(mode, "scan"))  return cmd_scan(dev, block, rargc, rargv);
	usage();
	return 2;
}
