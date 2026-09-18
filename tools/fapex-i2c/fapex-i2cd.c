// SPDX-License-Identifier: GPL-2.0-only
//
// fapex-i2cd: cross-process I2C helper for the stage-B daemon observation.
//
// The vendor daemon's FAPex I2C write to the demod hangs forever (D-state) in
// the daemon's own process, yet the identical transfer in a SEPARATE process
// ACKs -- the hang is daemon-process-specific (spec §5.5).  fapex-trace, running
// LD_PRELOAD'd inside the daemon, forwards each FAPex I2C read/write to THIS
// helper over an abstract AF_UNIX datagram socket "\0fapex-i2cd".  The helper
// performs the transfer on a FRESH handle in its OWN process (like the
// standalone fapex-i2c, which is known to work), and returns the result, so the
// daemon proceeds through demod + tuner init for us to observe.
//
// It uses ONLY the published FAPEX I2C chardev ABI (abst_libi2c.h) -- no vendor
// code.  Run it on the host rootfs (musl static) alongside the daemon:
//     fapex-i2cd [/dev/fapi/fapi174]      (default I2C node; blockIndex selects bus)
//
// Wire protocol (little-endian), one datagram each way:
//   req: "I2CQ" openParam[24] dir(1:'W'/'R') slave(1) txlen(u16) rxlen(u16) tx[txlen]
//   rep: "I2CR" status(i32) rxlen(u16) rx[rxlen]
//
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/syscall.h>

#define FAPEX_DEV_TYPE 0xFF
#define NB_I2C         0x60

typedef struct {
	uint32_t blockIndex;
	int32_t  speed;
	int32_t  protocol;
	uint32_t slaveAddress;
	int32_t  type;
	uint32_t timeOut;
} i2c_open_t;                    /* 24 bytes */

typedef struct {
	uint8_t   msSlaveAddress;
	uint8_t  *txDataPtr;
	uint32_t  txDataLength;
	uint8_t  *rxDataPtr;
	uint32_t  rxDataLength;
} i2c_rw_t;

#define I2C_OPEN  _IOW(FAPEX_DEV_TYPE, NB_I2C + 0, i2c_open_t)
#define I2C_READ  _IOR(FAPEX_DEV_TYPE, NB_I2C + 2, i2c_rw_t)
#define I2C_WRITE _IOW(FAPEX_DEV_TYPE, NB_I2C + 2, i2c_rw_t)

#define SOCK_NAME "fapex-i2cd"   /* abstract namespace */

/* One I2C transfer on a fresh handle: open, I2C_OPEN(openParam), RW, close.
 * Raw syscalls so we return the FAPex status faithfully (musl's ioctl() would
 * fold a negative FAPex code into -1). */
static long do_xfer(const char *dev, const unsigned char *openparam, char dir,
                    uint8_t slave, uint8_t *tx, uint32_t txlen,
                    uint8_t *rx, uint32_t rxlen)
{
	long st;
	int fd = open(dev, O_RDWR);
	if (fd < 0)
		return -1;
	if (syscall(SYS_ioctl, fd, (long)I2C_OPEN, (long)openparam) != 0) {
		close(fd);
		return -2;
	}
	i2c_rw_t rw;
	rw.msSlaveAddress = slave;
	if (dir == 'W') {
		rw.txDataPtr = txlen ? tx : NULL;
		rw.txDataLength = txlen;
		rw.rxDataPtr = NULL;
		rw.rxDataLength = 0;
		st = syscall(SYS_ioctl, fd, (long)I2C_WRITE, (long)&rw);
	} else {
		rw.txDataPtr = txlen ? tx : NULL;
		rw.txDataLength = txlen;
		rw.rxDataPtr = rx;
		rw.rxDataLength = rxlen;
		st = syscall(SYS_ioctl, fd, (long)I2C_READ, (long)&rw);
	}
	close(fd);
	return st;
}

int main(int argc, char **argv)
{
	const char *dev = (argc > 1) ? argv[1] : "/dev/fapi/fapi174";

	int s = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (s < 0) { perror("socket"); return 1; }

	struct sockaddr_un a;
	memset(&a, 0, sizeof a);
	a.sun_family = AF_UNIX;
	a.sun_path[0] = 0;                                  /* abstract */
	memcpy(a.sun_path + 1, SOCK_NAME, sizeof SOCK_NAME - 1);
	socklen_t alen = offsetof(struct sockaddr_un, sun_path) + 1 +
	                 (sizeof SOCK_NAME - 1);
	if (bind(s, (struct sockaddr *)&a, alen) < 0) {
		perror("bind (already running?)");
		return 1;
	}
	fprintf(stderr, "fapex-i2cd: listening on @%s, dev=%s\n", SOCK_NAME, dev);

	for (;;) {
		unsigned char req[300], rep[300];
		uint8_t tx[256], rx[256];
		struct sockaddr_un src;
		socklen_t sl = sizeof src;

		ssize_t n = recvfrom(s, req, sizeof req, 0,
				     (struct sockaddr *)&src, &sl);
		if (n < 34 || memcmp(req, "I2CQ", 4) != 0)
			continue;

		char dir = (char)req[28];
		uint8_t slave = req[29];
		uint32_t txlen = req[30] | (req[31] << 8);
		uint32_t rxlen = req[32] | (req[33] << 8);
		if (txlen > 256) txlen = 256;
		if (rxlen > 256) rxlen = 256;
		if ((ssize_t)(34 + txlen) > n) txlen = (n > 34) ? n - 34 : 0;
		memcpy(tx, req + 34, txlen);

		long st = do_xfer(dev, req + 4, dir, slave, tx, txlen, rx, rxlen);

		rep[0] = 'I'; rep[1] = '2'; rep[2] = 'C'; rep[3] = 'R';
		int32_t s32 = (int32_t)st;
		rep[4] = s32 & 0xff; rep[5] = (s32 >> 8) & 0xff;
		rep[6] = (s32 >> 16) & 0xff; rep[7] = (s32 >> 24) & 0xff;
		uint32_t rl = (dir == 'R' && st == 0) ? rxlen : 0;
		rep[8] = rl & 0xff; rep[9] = (rl >> 8) & 0xff;
		if (rl)
			memcpy(rep + 10, rx, rl);
		sendto(s, rep, 10 + rl, 0, (struct sockaddr *)&src, sl);
	}
	return 0;
}
