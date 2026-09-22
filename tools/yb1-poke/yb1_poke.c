// SPDX-License-Identifier: GPL-2.0-only
/*
 * yb1-poke - write 32-bit words into the POE-BR100-YB1 register windows.
 *
 * tools/yb1-mem only reads, on purpose.  This one writes, and that is the
 * point: comparing the SoC register windows between a vendor boot and a
 * no-vendor boot leaves a short list of words the vendor programs and we do
 * not (docs/specs/poe-br100-yb1-reception-stack.md §39), and the only way to
 * test whether those words are the gate is to put them there ourselves.
 *
 * Nothing here is vendor code.  The addresses come from the register-window
 * diff, i.e. from observation, and are supplied on the command line.
 *
 * Usage:
 *   yb1-poke <phys> read [n]              read n words (default 1)
 *   yb1-poke <phys> write <word> [...]    write words from <phys> upward
 *   yb1-poke <phys> setbits <mask>        read, OR in mask, write back
 *   yb1-poke <phys> clrbits <mask>        read, AND ~mask, write back
 *
 * Every write prints the word before and after, so a session is self
 * documenting.  Addresses must be 4-byte aligned.
 *
 * WARNING: this writes SoC peripheral registers.  A wrong write can hang the
 * bus or the board; a power cycle recovers it.  Do not use it on flash.
 */

/* MIPS o32 syscall numbers (arch/mips/kernel/syscalls/syscall_o32.tbl). */
#define SYS_write	4004
#define SYS_open	4005
#define SYS_close	4006
#define SYS_munmap	4091
#define SYS_mmap2	4210

#define O_RDWR		0x0002
#define O_SYNC		0x0010		/* MIPS: O_DSYNC, enough for /dev/mem */
#define PROT_READ	0x1
#define PROT_WRITE	0x2
#define MAP_SHARED	0x001

#define PAGE_SHIFT	12
#define PAGE_SIZE	(1UL << PAGE_SHIFT)
#define STDOUT		1
#define MAXW		32

typedef unsigned char u8;
typedef unsigned long u32;

long tr_syscall(long nr, long a, long b, long c, long d, long e, long f);

/* --- output (no libc) --------------------------------------------------- */

static char obuf[2048];
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
	int hex = 0, any = 0;

	if (!s || !*s)
		return -1;
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		hex = 1;
		s += 2;
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
		any = 1;
		s++;
	}
	*out = v;
	return any ? 0 : -1;
}

/* --- main --------------------------------------------------------------- */

static void usage(void)
{
	put("yb1-poke <phys> read [n]\n"
	    "yb1-poke <phys> write <word> [...]\n"
	    "yb1-poke <phys> setbits <mask>\n"
	    "yb1-poke <phys> clrbits <mask>\n"
	    "  32-bit accesses, address must be 4-byte aligned.\n"
	    "  Writes SoC registers: a bad write can hang the board (power cycle).\n");
	flushout();
}

int main(int argc, char **argv)
{
	static char dev[] = "/dev/mem";
	unsigned long phys, off, maplen, vals[MAXW];
	volatile u32 *m;
	long fd, r;
	int nv = 0, i;

	if (argc < 3 || parse(argv[1], &phys)) {
		usage();
		return 2;
	}
	if (phys & 3) {
		put("address must be 4-byte aligned\n");
		flushout();
		return 2;
	}

	off = phys & (PAGE_SIZE - 1);
	maplen = PAGE_SIZE * 2;		/* so a burst may cross a page */

	fd = tr_syscall(SYS_open, (long)dev, O_RDWR | O_SYNC, 0, 0, 0, 0);
	if (fd < 0) {
		put("open /dev/mem failed: ");
		putdec(fd);
		put("\n");
		flushout();
		return 1;
	}
	r = tr_syscall(SYS_mmap2, 0, (long)maplen, PROT_READ | PROT_WRITE,
		       MAP_SHARED, fd, (long)((phys - off) >> PAGE_SHIFT));
	if (r < 0 && r > -4096) {
		put("mmap failed: ");
		putdec(r);
		put("\n");
		flushout();
		tr_syscall(SYS_close, fd, 0, 0, 0, 0, 0);
		return 1;
	}
	m = (volatile u32 *)(r + off);

	if (streq(argv[2], "read")) {
		unsigned long n = 1;

		if (argc > 3 && parse(argv[3], &n))
			n = 1;
		if (n > MAXW)
			n = MAXW;
		for (i = 0; i < (int)n; i++) {
			put("  0x");
			puthex(phys + i * 4, 8);
			put(" = ");
			puthex(m[i], 8);
			put("\n");
		}
	} else if (streq(argv[2], "write")) {
		for (i = 3; i < argc && nv < MAXW; i++) {
			if (parse(argv[i], &vals[nv])) {
				put("bad word: ");
				put(argv[i]);
				put("\n");
				flushout();
				return 2;
			}
			nv++;
		}
		if (!nv) {
			usage();
			return 2;
		}
		for (i = 0; i < nv; i++) {
			u32 before = m[i];

			m[i] = (u32)vals[i];
			put("  0x");
			puthex(phys + i * 4, 8);
			put(" : ");
			puthex(before, 8);
			put(" -> ");
			puthex(m[i], 8);
			put("  (asked ");
			puthex(vals[i], 8);
			put(")\n");
		}
	} else if (streq(argv[2], "setbits") || streq(argv[2], "clrbits")) {
		unsigned long mask;
		u32 before, want;

		if (argc < 4 || parse(argv[3], &mask)) {
			usage();
			return 2;
		}
		before = m[0];
		want = streq(argv[2], "setbits") ? (before | (u32)mask)
						 : (before & ~(u32)mask);
		m[0] = want;
		put("  0x");
		puthex(phys, 8);
		put(" : ");
		puthex(before, 8);
		put(" -> ");
		puthex(m[0], 8);
		put("  (wanted ");
		puthex(want, 8);
		put(")\n");
	} else {
		usage();
		tr_syscall(SYS_munmap, r, (long)maplen, 0, 0, 0, 0);
		tr_syscall(SYS_close, fd, 0, 0, 0, 0, 0);
		return 2;
	}

	flushout();
	tr_syscall(SYS_munmap, r, (long)maplen, 0, 0, 0, 0);
	tr_syscall(SYS_close, fd, 0, 0, 0, 0, 0);
	return 0;
}
