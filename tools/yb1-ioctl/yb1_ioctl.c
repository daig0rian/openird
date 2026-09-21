// SPDX-License-Identifier: GPL-2.0-only
/*
 * yb1-ioctl - issue exactly one ioctl to a POE-BR100-YB1 vendor device.
 *
 * Stage-B observation tool (not part of any distributed image).
 *
 * Everything else in tools/ only reads.  This one writes, and that is the
 * point: the AV sub-CPU command stack is only exercised when somebody drives
 * it, and the vendor application drives it in bursts of ~21 commands per
 * action, which cannot be told apart.  With the device idle the vendor makes
 * no API calls at all (measured: 0 in 5 s), so a single ioctl from here lands
 * alone and its entry in the command stack can be read back unambiguously.
 *
 * Usage:
 *   yb1-ioctl <device> <cmd> [byte ...]
 *
 *   <device>  e.g. /dev/val_demux
 *   <cmd>     the request as the tracer prints it, e.g. 0x4028c32f
 *   [byte]    payload bytes, most significant first, zero padded to the size
 *             encoded in <cmd>.  Omit for an all-zero argument.
 *
 * The argument size, direction and type come out of <cmd> itself (MIPS _IOC
 * encoding: SIZEBITS=13, DIRBITS=3, NONE/READ/WRITE = 1/2/4 -- note that this
 * differs from the asm-generic encoding used on x86).  The buffer is printed
 * before and after, so a read-back is visible.
 *
 * Start with a request the vendor itself issues constantly (val_demux
 * nr=0x2f is a poll): if that behaves, step up from there.
 */

/* MIPS o32 syscall numbers. */
#define SYS_write	4004
#define SYS_open	4005
#define SYS_close	4006
#define SYS_ioctl	4054

#define O_RDWR		0x0002

/* MIPS _IOC encoding (asm-mips/ioctl.h), which is NOT asm-generic's. */
#define IOC_NRBITS	8
#define IOC_TYPEBITS	8
#define IOC_SIZEBITS	13
#define IOC_DIRBITS	3
#define IOC_NRSHIFT	0
#define IOC_TYPESHIFT	(IOC_NRSHIFT + IOC_NRBITS)
#define IOC_SIZESHIFT	(IOC_TYPESHIFT + IOC_TYPEBITS)
#define IOC_DIRSHIFT	(IOC_SIZESHIFT + IOC_SIZEBITS)
#define IOC_NONE	1U
#define IOC_READ	2U
#define IOC_WRITE	4U

#define MAXARG		256		/* _IOC size field caps at 8191, but
					 * every vendor request seen is <= 64 */
#define STDOUT		1

typedef unsigned char u8;

long tr_syscall(long nr, long a, long b, long c, long d, long e, long f);

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
	unsigned long u;

	if (v < 0) {
		put("-");
		u = (unsigned long)-v;
	} else {
		u = (unsigned long)v;
	}
	t[i] = 0;
	do {
		t[--i] = '0' + (char)(u % 10);
		u /= 10;
	} while (u && i > 0);
	put(t + i);
}

/* --- arguments ---------------------------------------------------------- */

static int parse(const char *s, unsigned long *out)
{
	unsigned long v = 0;
	int hex = 0;

	if (!s || !*s)
		return -1;
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		hex = 1;
		s += 2;
		if (!*s)
			return -1;
	}
	for (; *s; s++) {
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
	}
	*out = v;
	return 0;
}

static void dump(const char *label, const u8 *p, unsigned long n)
{
	unsigned long i;

	put(label);
	for (i = 0; i < n; i++) {
		put(" ");
		puthex(p[i], 2);
	}
	put("\n");
}

static void usage(void)
{
	put("yb1-ioctl <device> <cmd> [byte ...]\n"
	    "  <device>  e.g. /dev/val_demux\n"
	    "  <cmd>     request as the tracer prints it, e.g. 0x4028c32f\n"
	    "  [byte]    payload, most significant first, zero padded\n"
	    "Size, direction and type are decoded from <cmd> (MIPS _IOC).\n"
	    "Issues the request ONCE and prints the buffer before and after.\n");
}

int main(int argc, char **argv)
{
	static u8 buf[MAXARG];
	unsigned long cmd, size, dir, type, nr, v;
	long fd, r;
	int i;

	if (argc < 3) {
		usage();
		flushout();
		return 1;
	}
	if (parse(argv[2], &cmd)) {
		put("bad cmd\n");
		flushout();
		return 1;
	}
	dir = (cmd >> IOC_DIRSHIFT) & ((1U << IOC_DIRBITS) - 1);
	size = (cmd >> IOC_SIZESHIFT) & ((1U << IOC_SIZEBITS) - 1);
	type = (cmd >> IOC_TYPESHIFT) & 0xff;
	nr = (cmd >> IOC_NRSHIFT) & 0xff;

	if (size > MAXARG) {
		put("size too large: ");
		putdec((long)size);
		put("\n");
		flushout();
		return 1;
	}
	for (i = 3; i < argc && (unsigned long)(i - 3) < size; i++) {
		if (parse(argv[i], &v) || v > 0xff) {
			put("bad byte: ");
			put(argv[i]);
			put("\n");
			flushout();
			return 1;
		}
		buf[i - 3] = (u8)v;
	}

	put("cmd 0x");
	puthex(cmd, 8);
	put("  dir=");
	put(dir & IOC_READ ? "R" : "");
	put(dir & IOC_WRITE ? "W" : "");
	put(dir == IOC_NONE ? "NONE" : "");
	put(" type=0x");
	puthex(type, 2);
	put(" nr=0x");
	puthex(nr, 2);
	put(" size=");
	putdec((long)size);
	put("\n");

	fd = tr_syscall(SYS_open, (long)argv[1], O_RDWR, 0, 0, 0, 0);
	if (fd < 0) {
		put("open ");
		put(argv[1]);
		put(" failed: ");
		putdec(fd);
		put("\n");
		flushout();
		return 1;
	}

	if (size)
		dump("in :", buf, size);
	r = tr_syscall(SYS_ioctl, fd, (long)cmd, (long)buf, 0, 0, 0);
	if (size)
		dump("out:", buf, size);
	put("ret = ");
	putdec(r);
	put("\n");

	tr_syscall(SYS_close, fd, 0, 0, 0, 0, 0);
	flushout();
	return r < 0 ? 1 : 0;
}
