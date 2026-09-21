// SPDX-License-Identifier: GPL-2.0-only
/*
 * yb1-i2c - talk to the POE-BR100-YB1 I2C buses from userland.
 *
 * Stage-B observation tool (not part of any distributed image).  The board's
 * demodulator and tuner hang off a stock Linux i2c adapter (emma-iic, GPL, by
 * Renesas), so /dev/i2c-N reaches them with the ordinary i2c-dev ABI -- no
 * vendor interface involved.  This is what makes a clean-room tuner driver
 * possible on this board: the bus is not proprietary, only the chips'
 * register semantics are, and for the Toshiba TC90522/TC90532 demodulator
 * those are already described by mainline Linux under GPL.
 *
 * Usage:
 *   yb1-i2c <bus> probe [first] [last]   scan for devices that acknowledge
 *   yb1-i2c <bus> r <addr> <reg> [n]     write reg, repeated START, read n
 *   yb1-i2c <bus> d <addr> <reg> <n>     dump n registers, one read each
 *   yb1-i2c <bus> w <addr> <byte>...     write bytes (see the warning below)
 *
 * <bus> is the number in /dev/i2c-N.  Numbers are decimal unless prefixed
 * with 0x.  Addresses are 7-bit.
 *
 * WARNING: the vendor drivers use bus 1 while the application runs.  The i2c
 * core serialises transfers, so this cannot corrupt the wire, but reading a
 * chip mid-sequence can disturb its internal register pointer.  Prefer to
 * probe while no tuning is in progress, and re-tune afterwards to confirm the
 * board is still healthy.
 *
 * WARNING: bus 0 has nothing on it and a transfer there blocks in the adapter
 * rather than returning an error (measured 2026-09-21).  Probe bus 1.  Output
 * is flushed row by row so a probe that has to be killed still shows what it
 * found.
 */

/* MIPS o32 syscall numbers (arch/mips/kernel/syscalls/syscall_o32.tbl). */
#define SYS_write	4004
#define SYS_open	4005
#define SYS_close	4006
#define SYS_ioctl	4054

#define O_RDWR		0x0002

/* i2c-dev ioctls are plain numbers, not _IOC-encoded, so they do not vary by
 * architecture (include/uapi/linux/i2c-dev.h). */
#define I2C_SLAVE	0x0703
#define I2C_FUNCS	0x0705
#define I2C_RDWR	0x0707
#define I2C_M_RD	0x0001

#define STDOUT		1
#define MAXW		16

typedef unsigned short u16;
typedef unsigned char u8;

long tr_syscall(long nr, long a, long b, long c, long d, long e, long f);

/* include/uapi/linux/i2c.h: 32-bit layout is addr@0 flags@2 len@4 buf@8 (12 B) */
struct i2c_msg {
	u16 addr;
	u16 flags;
	u16 len;
	u8 *buf;
};

struct i2c_rdwr_ioctl_data {
	struct i2c_msg *msgs;
	unsigned int nmsgs;
};

/* --- output (no libc) --------------------------------------------------- */

static char obuf[4096];
static int olen;

static void flushout(void)
{
	if (olen) {
		tr_syscall(SYS_write, STDOUT, (long)obuf, olen, 0, 0, 0);
		olen = 0;
	}
}

static void put(const char *s)
{
	while (*s) {
		if (olen >= (int)sizeof(obuf) - 1)
			flushout();
		obuf[olen++] = *s++;
	}
}

static void puthex(unsigned long v, int digits)
{
	char t[9];
	int i;

	if (digits < 1 || digits > 8)
		digits = 8;
	t[digits] = 0;
	for (i = digits - 1; i >= 0; i--, v >>= 4)
		t[i] = "0123456789abcdef"[v & 15];
	put(t);
}

static void putdec(long v)
{
	char t[12];
	int i = sizeof(t) - 1;
	unsigned long u = v < 0 ? -(unsigned long)v : (unsigned long)v;

	t[i] = 0;
	do {
		t[--i] = '0' + (char)(u % 10);
		u /= 10;
	} while (u && i > 1);
	if (v < 0)
		t[--i] = '-';
	put(t + i);
}

/* --- argument parsing --------------------------------------------------- */

static int streq(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

/* decimal, or hex with a 0x prefix; returns -1 on anything unparsable */
static long parse(const char *s)
{
	long v = 0;
	int hex = 0;

	if (!s || !*s)
		return -1;
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		hex = 1;
		s += 2;
		if (!*s)
			return -1;
	}
	while (*s) {
		int d;

		if (*s >= '0' && *s <= '9')
			d = *s - '0';
		else if (hex && *s >= 'a' && *s <= 'f')
			d = *s - 'a' + 10;
		else if (hex && *s >= 'A' && *s <= 'F')
			d = *s - 'A' + 10;
		else
			return -1;
		v = v * (hex ? 16 : 10) + d;
		s++;
	}
	return v;
}

/* --- i2c ---------------------------------------------------------------- */

static int xfer(int fd, struct i2c_msg *m, int n)
{
	struct i2c_rdwr_ioctl_data d;

	d.msgs = m;
	d.nmsgs = n;
	return (int)tr_syscall(SYS_ioctl, fd, I2C_RDWR, (long)&d, 0, 0, 0);
}

/* one register read: write the register index, repeated START, read len bytes */
static int reg_read(int fd, int addr, int reg, u8 *out, int len)
{
	u8 r = (u8)reg;
	struct i2c_msg m[2];

	m[0].addr = (u16)addr;
	m[0].flags = 0;
	m[0].len = 1;
	m[0].buf = &r;
	m[1].addr = (u16)addr;
	m[1].flags = I2C_M_RD;
	m[1].len = (u16)len;
	m[1].buf = out;
	return xfer(fd, m, 2);
}

static int cmd_probe(int fd, int first, int last)
{
	int a, found = 0;
	u8 b;

	put("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");
	for (a = 0; a < 128; a++) {
		struct i2c_msg m;
		int r;

		if ((a & 15) == 0) {
			puthex(a, 2);
			put(":");
		}
		if (a < first || a > last) {
			put("   ");
			goto eol;
		}
		/* a 1-byte read: harmless on register-file devices and enough
		 * to tell an acknowledging address from an empty one */
		m.addr = (u16)a;
		m.flags = I2C_M_RD;
		m.len = 1;
		m.buf = &b;
		r = xfer(fd, &m, 1);
		if (r >= 0) {
			put(" ");
			puthex(a, 2);
			found++;
		} else {
			put(" --");
		}
eol:
		if ((a & 15) == 15) {
			put("\n");
			/* flush per row: a bus with nothing on it can block in the
			 * adapter (bus 0 on this board does), and a killed process
			 * would otherwise lose everything it had already found */
			flushout();
		}
	}
	put("found ");
	putdec(found);
	put("\n");
	return 0;
}

static int cmd_read(int fd, int addr, int reg, int n)
{
	u8 buf[MAXW];
	int r, i;

	if (n < 1 || n > MAXW)
		n = 1;
	r = reg_read(fd, addr, reg, buf, n);
	if (r < 0) {
		put("read failed: ");
		putdec(r);
		put("\n");
		return 1;
	}
	puthex(addr, 2);
	put(" reg ");
	puthex(reg, 2);
	put(" =");
	for (i = 0; i < n; i++) {
		put(" ");
		puthex(buf[i], 2);
	}
	put("\n");
	return 0;
}

static int cmd_dump(int fd, int addr, int reg, int n)
{
	int i;

	for (i = 0; i < n; i++) {
		u8 b;
		int r;

		if ((i & 15) == 0) {
			if (i)
				put("\n");
			puthex(reg + i, 2);
			put(":");
		}
		r = reg_read(fd, addr, reg + i, &b, 1);
		if (r < 0) {
			put(" --");
		} else {
			put(" ");
			puthex(b, 2);
		}
	}
	put("\n");
	return 0;
}

static int cmd_write(int fd, int addr, char **v, int n)
{
	u8 buf[MAXW];
	struct i2c_msg m;
	int i, r;

	if (n < 1 || n > MAXW)
		return 1;
	for (i = 0; i < n; i++) {
		long b = parse(v[i]);

		if (b < 0 || b > 255) {
			put("bad byte\n");
			return 1;
		}
		buf[i] = (u8)b;
	}
	m.addr = (u16)addr;
	m.flags = 0;
	m.len = (u16)n;
	m.buf = buf;
	r = xfer(fd, &m, 1);
	put(r < 0 ? "write failed: " : "write ok: ");
	putdec(r);
	put("\n");
	return r < 0;
}

static void usage(void)
{
	put("yb1-i2c <bus> probe [first] [last]\n"
	    "yb1-i2c <bus> r <addr> <reg> [n]\n"
	    "yb1-i2c <bus> d <addr> <reg> <n>\n"
	    "yb1-i2c <bus> w <addr> <byte>...\n"
	    "addresses are 7-bit; 0x prefix for hex\n");
}

int main(int argc, char **argv)
{
	char path[16] = "/dev/i2c-";
	long bus, addr, reg, n;
	int fd, rc;

	if (argc < 3) {
		usage();
		flushout();
		return 1;
	}
	bus = parse(argv[1]);
	if (bus < 0 || bus > 9) {
		put("bad bus\n");
		flushout();
		return 1;
	}
	path[9] = '0' + (char)bus;
	path[10] = 0;

	fd = (int)tr_syscall(SYS_open, (long)path, O_RDWR, 0, 0, 0, 0);
	if (fd < 0) {
		put("open ");
		put(path);
		put(" failed: ");
		putdec(fd);
		put("\n");
		flushout();
		return 1;
	}

	if (streq(argv[2], "probe")) {
		long f = argc > 3 ? parse(argv[3]) : 0x03;
		long l = argc > 4 ? parse(argv[4]) : 0x77;

		rc = cmd_probe(fd, (int)f, (int)l);
	} else if (streq(argv[2], "r") && argc >= 5) {
		addr = parse(argv[3]);
		reg = parse(argv[4]);
		n = argc > 5 ? parse(argv[5]) : 1;
		rc = cmd_read(fd, (int)addr, (int)reg, (int)n);
	} else if (streq(argv[2], "d") && argc >= 6) {
		addr = parse(argv[3]);
		reg = parse(argv[4]);
		n = parse(argv[5]);
		rc = cmd_dump(fd, (int)addr, (int)reg, (int)n);
	} else if (streq(argv[2], "w") && argc >= 5) {
		addr = parse(argv[3]);
		rc = cmd_write(fd, (int)addr, argv + 4, argc - 4);
	} else {
		usage();
		rc = 1;
	}

	tr_syscall(SYS_close, fd, 0, 0, 0, 0, 0);
	flushout();
	return rc;
}
