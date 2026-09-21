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
 *   yb1-ioctl <device> <cmd> [byte ...]      one request, then close
 *   yb1-ioctl <device> @<script>             several requests on ONE open fd
 *
 *   <device>  e.g. /dev/val_demux
 *   <cmd>     the request as the tracer prints it, e.g. 0x4028c32f
 *   [byte]    payload bytes, most significant first, zero padded to the size
 *             encoded in <cmd>.  Omit for an all-zero argument.
 *
 * The script form exists because this driver keeps per-open state.  The vendor
 * application holds /dev/val_demux open for its whole life; a tool that opens
 * and closes around every request gets a fresh context each time, so a filter
 * set up by one call is gone by the next -- every call still returns 0, but
 * nothing ever flows.  Measured: polling the vendor's own active buffers from
 * a fresh fd reports offset 0 while the vendor sees them advancing.
 *
 * Script lines are whitespace separated:
 *   <cmd> [word ...]   issue that request, payload given as 32-bit words
 *   load $N <phys>     read one 32-bit word of physical memory into $N
 *   hold <seconds>     keep the fd open and wait (so the capture can run)
 * Blank lines and lines starting with '#' are ignored.  Anywhere a word is
 * expected, "$N" (N = 0..7) substitutes a stored value.
 *
 * "load" exists because handles are decided at run time: an allocate returns
 * its handle by writing it to the address the caller passed, not into the
 * ioctl buffer, so the next request cannot be written literally in advance.
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
#define SYS_read	4003
#define SYS_write	4004
#define SYS_open	4005
#define SYS_close	4006
#define SYS_ioctl	4054
#define SYS_munmap	4091
#define SYS_nanosleep	4166
#define SYS_mmap2	4210

#define O_RDONLY	0x0000
#define O_RDWR		0x0002
#define O_SYNC		0x0010		/* MIPS: O_DSYNC, enough for /dev/mem */
#define PROT_READ	0x1
#define MAP_SHARED	0x001
#define PAGE_SHIFT	12
#define PAGE_SIZE	(1UL << PAGE_SHIFT)

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

static int streq(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

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

/* --- one request on an already-open fd ---------------------------------- */

static u8 argbuf[MAXARG];

static long do_one(long fd, unsigned long cmd, const unsigned long *w, int nw)
{
	unsigned long size, dir, nr;
	long r;
	int i;

	dir = (cmd >> IOC_DIRSHIFT) & ((1U << IOC_DIRBITS) - 1);
	size = (cmd >> IOC_SIZESHIFT) & ((1U << IOC_SIZEBITS) - 1);
	nr = (cmd >> IOC_NRSHIFT) & 0xff;
	if (size > MAXARG)
		size = MAXARG;
	for (i = 0; i < (int)size; i++)
		argbuf[i] = 0;
	for (i = 0; i < nw && (unsigned long)(i * 4 + 4) <= size; i++) {
		argbuf[i * 4 + 0] = (u8)(w[i] >> 24);
		argbuf[i * 4 + 1] = (u8)(w[i] >> 16);
		argbuf[i * 4 + 2] = (u8)(w[i] >> 8);
		argbuf[i * 4 + 3] = (u8)w[i];
	}
	r = tr_syscall(SYS_ioctl, fd, (long)cmd, (long)argbuf, 0, 0, 0);

	put("nr=0x");
	puthex(nr, 2);
	put(dir & IOC_READ ? " R " : " W ");
	put("ret=");
	putdec(r);
	put("  buf:");
	for (i = 0; i < 4 && (unsigned long)(i * 4 + 4) <= size; i++) {
		put(" ");
		puthex(((unsigned long)argbuf[i * 4] << 24) |
		       ((unsigned long)argbuf[i * 4 + 1] << 16) |
		       ((unsigned long)argbuf[i * 4 + 2] << 8) |
		       (unsigned long)argbuf[i * 4 + 3], 8);
	}
	put("\n");
	flushout();
	return r;
}

/* --- script mode: several requests, one fd ------------------------------ */

static char script[8192];
static unsigned long var[8];

/* One 32-bit word of physical memory, through /dev/mem.  Used to pick up a
 * handle an allocate wrote to shared memory. */
static int read_phys(unsigned long phys, unsigned long *out)
{
	unsigned long off = phys & (PAGE_SIZE - 1);
	const volatile unsigned char *m;
	long fd, r;

	fd = tr_syscall(SYS_open, (long)"/dev/mem", O_RDONLY | O_SYNC, 0, 0, 0, 0);
	if (fd < 0)
		return -1;
	r = tr_syscall(SYS_mmap2, 0, (long)PAGE_SIZE, PROT_READ, MAP_SHARED,
		       fd, (long)((phys - off) >> PAGE_SHIFT));
	if (r < 0 && r > -4096) {
		tr_syscall(SYS_close, fd, 0, 0, 0, 0, 0);
		return -1;
	}
	m = (const volatile unsigned char *)r + off;
	*out = ((unsigned long)m[0] << 24) | ((unsigned long)m[1] << 16) |
	       ((unsigned long)m[2] << 8) | (unsigned long)m[3];
	tr_syscall(SYS_munmap, r, (long)PAGE_SIZE, 0, 0, 0, 0);
	tr_syscall(SYS_close, fd, 0, 0, 0, 0, 0);
	return 0;
}

/* Like parse(), but "$N" yields the stored value. */
static int parse_arg(const char *s, unsigned long *out)
{
	if (s[0] == '$' && s[1] >= '0' && s[1] <= '7' && s[2] == 0) {
		*out = var[s[1] - '0'];
		return 0;
	}
	return parse(s, out);
}

static int is_space(char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* The vendor drives TWO nodes at once -- /dev/val_demux and /dev/val_demux1 --
 * and the driver keeps its state per open, so the script has to hold both fds
 * open and switch between them.  Re-opening a node would throw its state away,
 * so paths are opened once and remembered.
 */
#define MAXDEV	4
#define MAXPATH	40

static char dev_path[MAXDEV][MAXPATH];
static long dev_fd[MAXDEV];
static int dev_n;

static void copy_path(char *dst, const char *src)
{
	int i = 0;

	while (src[i] && i < MAXPATH - 1) {
		dst[i] = src[i];
		i++;
	}
	dst[i] = 0;
}

static long dev_get(const char *path)
{
	int i;
	long fd;

	for (i = 0; i < dev_n; i++)
		if (streq(dev_path[i], path))
			return dev_fd[i];
	if (dev_n >= MAXDEV)
		return -1;
	fd = tr_syscall(SYS_open, (long)path, O_RDWR, 0, 0, 0, 0);
	if (fd < 0)
		return fd;
	copy_path(dev_path[dev_n], path);
	dev_fd[dev_n] = fd;
	dev_n++;
	return fd;
}

static int run_script(long fd, const char *dev0, const char *path)
{
	long sf, n, got = 0, cur = fd;
	char *p, *end;

	copy_path(dev_path[0], dev0);
	dev_fd[0] = fd;
	dev_n = 1;

	sf = tr_syscall(SYS_open, (long)path, O_RDONLY, 0, 0, 0, 0);
	if (sf < 0) {
		put("open script failed: ");
		putdec(sf);
		put("\n");
		return 1;
	}
	while (got < (long)sizeof(script) - 1) {
		n = tr_syscall(SYS_read, sf, (long)(script + got),
			       (long)(sizeof(script) - 1 - got), 0, 0, 0);
		if (n <= 0)
			break;
		got += n;
	}
	script[got] = 0;
	tr_syscall(SYS_close, sf, 0, 0, 0, 0, 0);

	p = script;
	end = script + got;
	while (p < end) {
		char *line = p;
		unsigned long w[8], cmd;
		int nw = 0;
		char *tok;

		while (p < end && *p != '\n')
			p++;
		if (p < end)
			*p++ = 0;
		while (is_space(*line))
			line++;
		if (!*line || *line == '#')
			continue;

		/* first token */
		tok = line;
		while (*line && !is_space(*line))
			line++;
		if (*line)
			*line++ = 0;

		if (streq(tok, "load")) {
			char *a, *b;
			unsigned long phys;
			int idx;

			while (is_space(*line))
				line++;
			a = line;
			while (*line && !is_space(*line))
				line++;
			if (*line)
				*line++ = 0;
			while (is_space(*line))
				line++;
			b = line;
			while (*line && !is_space(*line))
				line++;
			if (*line)
				*line++ = 0;
			if (a[0] != '$' || a[1] < '0' || a[1] > '7' ||
			    parse_arg(b, &phys)) {
				put("bad load line\n");
				flushout();
				continue;
			}
			idx = a[1] - '0';
			if (read_phys(phys, &var[idx])) {
				put("load: cannot read 0x");
				puthex(phys, 8);
				put("\n");
			} else {
				put("$");
				putdec(idx);
				put(" = 0x");
				puthex(var[idx], 8);
				put("\n");
			}
			flushout();
			continue;
		}
		if (streq(tok, "dev")) {
			long d;

			while (is_space(*line))
				line++;
			tok = line;
			while (*line && !is_space(*line))
				line++;
			if (*line)
				*line++ = 0;
			d = dev_get(tok);
			if (d < 0) {
				put("dev ");
				put(tok);
				put(": open failed ");
				putdec(d);
				put("\n");
			} else {
				cur = d;
				put("-- now on ");
				put(tok);
				put("\n");
			}
			flushout();
			continue;
		}
		if (streq(tok, "hold")) {
			long ts[2];

			while (is_space(*line))
				line++;
			if (parse_arg(line, &w[0]))
				w[0] = 1;
			ts[0] = (long)w[0];
			ts[1] = 0;
			put("hold ");
			putdec((long)w[0]);
			put("s\n");
			flushout();
			tr_syscall(SYS_nanosleep, (long)ts, 0, 0, 0, 0, 0);
			continue;
		}
		if (parse_arg(tok, &cmd)) {
			put("bad cmd in script: ");
			put(tok);
			put("\n");
			continue;
		}
		while (nw < 8) {
			char *a;

			while (is_space(*line))
				line++;
			if (!*line)
				break;
			a = line;
			while (*line && !is_space(*line))
				line++;
			if (*line)
				*line++ = 0;
			if (parse_arg(a, &w[nw]))
				break;
			nw++;
		}
		do_one(cur, cmd, w, nw);
	}
	return 0;
}

static void usage(void)
{
	put("yb1-ioctl <device> <cmd> [byte ...]   one request, then close\n"
	    "yb1-ioctl <device> @<script>          several on ONE open fd\n"
	    "  <cmd>     request as the tracer prints it, e.g. 0x4028c32f\n"
	    "  [byte]    payload, most significant first, zero padded\n"
	    "Script lines: '<cmd> [word ...]', 'load $N <phys>', 'hold <secs>',\n"
	    "  or 'dev <path>' to switch node (both stay open).\n"
	    "The driver keeps per-open state, so a filter set up by one call is\n"
	    "gone by the next unless the fd stays open -- hence the script form.\n");
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
	if (argv[2][0] == '@') {
		long r;

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
		r = run_script(fd, argv[1], argv[2] + 1);
		for (i = 0; i < dev_n; i++)
			tr_syscall(SYS_close, dev_fd[i], 0, 0, 0, 0, 0);
		flushout();
		return (int)r;
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
