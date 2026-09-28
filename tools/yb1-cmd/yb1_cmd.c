// SPDX-License-Identifier: GPL-2.0-only
/*
 * yb1-cmd - post commands to the POE-BR100-YB1 AV sub-CPU command stack
 * directly, with no vendor kernel module in the path.
 *
 * The procedure is the one observed in docs/specs/poe-br100-yb1-reception-
 * stack.md §66/§72.5/§80: the request words go into the data ring in shared
 * memory, an 8-word entry describes the command, and writing 0x00020000 to
 * the BHIF register +0x1b0 tells the sub-CPU to look.  When it is done, word 0
 * of the entry reads 0x00040002 and word 5 holds the status; the response
 * follows the request in the data ring.
 *
 * The sub-CPU must already be running (brought up by csd; §80), and the vendor
 * modules val_merge and csd removed so that nobody else posts commands.
 *
 * Usage:
 *   yb1-cmd [-v] <file>
 *
 * One command per line (blank lines and '#' comments ignored):
 *   <entry> <cmdseq> <reqaddr> <reqlen> <resplen> <status> [req ...] [= resp ...]
 *     entry    slot 0..31 of the entry table (physical 0x10400, 32 B each;
 *              the vendor path uses 0 for some of its commands too)
 *     cmdseq   (command ID << 16) | sequence number
 *     reqaddr  where the request goes (KSEG1 address in the data ring)
 *     status   the status expected back; resp the response words expected
 * tools/host/trace2cmd.py writes such a file from a yb1-ioctl "trace" log, so
 * a vendor call sequence can be replayed exactly (same entries, sequence
 * numbers and data-ring addresses).  Every command is checked against the
 * expected status and response; -v prints them all, otherwise only the ones
 * that differ.
 */

/* MIPS o32 syscall numbers. */
#define SYS_read	4003
#define SYS_write	4004
#define SYS_open	4005
#define SYS_close	4006
#define SYS_munmap	4091
#define SYS_nanosleep	4166
#define SYS_mmap2	4210

#define O_RDONLY	0x0000
#define O_RDWR		0x0002
#define O_SYNC		0x0010		/* MIPS: O_DSYNC, enough for /dev/mem */
#define PROT_READ	0x1
#define PROT_WRITE	0x2
#define MAP_SHARED	0x001
#define PAGE_SHIFT	12
#define STDOUT		1

#define SHM_PHYS	0x10000ul
#define SHM_LEN		0x10000ul
#define ENT_PHYS	0x10400ul
#define BHIF_PHYS	0x10000000ul
#define B_RESET		0x180
#define B_SHM		0x1ac
#define B_DOORBELL	0x1b0
#define PENDING		0x00020000ul
#define DONE		0x00040002ul
#define MAXW		32

typedef unsigned char u8;
typedef unsigned long u32;

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

/* --- the command stack -------------------------------------------------- */

static volatile u32 *shm, *bhif;

static volatile u32 *shm_at(unsigned long phys)
{
	return shm + (phys - SHM_PHYS) / 4;
}

static void pause_us(long us)
{
	long ts[2];

	ts[0] = 0;
	ts[1] = us * 1000;
	tr_syscall(SYS_nanosleep, (long)ts, 0, 0, 0, 0, 0);
}

static long map(unsigned long phys, unsigned long len, volatile u32 **out)
{
	long fd, r;

	fd = tr_syscall(SYS_open, (long)"/dev/mem", O_RDWR | O_SYNC, 0, 0, 0, 0);
	if (fd < 0)
		return fd;
	r = tr_syscall(SYS_mmap2, 0, (long)len, PROT_READ | PROT_WRITE, MAP_SHARED,
		       fd, (long)(phys >> PAGE_SHIFT));
	tr_syscall(SYS_close, fd, 0, 0, 0, 0, 0);
	if (r < 0 && r > -4096)
		return r;
	*out = (volatile u32 *)r;
	return 0;
}

/* Post one command and wait for it.  Returns the status, or -1000 on a
 * timeout.  The response words are copied to resp[]. */
static long post(unsigned long entry, unsigned long cmdseq, unsigned long req,
		 unsigned long reqlen, unsigned long resplen, const u32 *w,
		 u32 *resp)
{
	volatile u32 *e = shm_at(ENT_PHYS + 32 * entry);
	unsigned long rphys = req & 0x1ffffffful, i;
	int n;

	for (i = 0; i < reqlen / 4; i++)
		*shm_at(rphys + 4 * i) = w[i];
	for (i = 0; i < resplen / 4; i++)
		*shm_at(rphys + reqlen + 4 * i) = 0;
	e[1] = cmdseq;
	e[2] = req;
	e[3] = req + reqlen;
	e[4] = 0x7ffffffful;
	e[5] = 0;
	e[6] = reqlen + resplen;
	e[7] = 0;
	e[0] = PENDING;
	bhif[B_DOORBELL / 4] = PENDING;
	for (n = 0; n < 10000 && e[0] != DONE; n++)
		pause_us(200);
	if (e[0] != DONE)
		return -1000;
	for (i = 0; i < resplen / 4; i++)
		resp[i] = *shm_at(rphys + reqlen + 4 * i);
	return (long)e[5];
}

/* --- main --------------------------------------------------------------- */

static char text[131072];

static int is_space(char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

int main(int argc, char **argv)
{
	unsigned long nok = 0, nbad = 0, ntimeout = 0;
	int verbose = 0, a = 1;
	long fd, got = 0, n;
	char *p, *end;

	if (argc > a && streq(argv[a], "-v")) {
		verbose = 1;
		a++;
	}
	if (argc != a + 1) {
		put("usage: yb1-cmd [-v] <file>   (see the source for the format)\n");
		flushout();
		return 1;
	}
	if (map(SHM_PHYS, SHM_LEN, &shm) || map(BHIF_PHYS, 0x1000, &bhif)) {
		put("cannot map /dev/mem\n");
		flushout();
		return 1;
	}
	if (bhif[B_RESET / 4] != 0 || bhif[B_SHM / 4] != 0xa0010000ul) {
		put("the sub-CPU is not up (+0x180 = 0x");
		puthex(bhif[B_RESET / 4], 8);
		put(", +0x1ac = 0x");
		puthex(bhif[B_SHM / 4], 8);
		put("): bring it up with csd first\n");
		flushout();
		return 1;
	}
	fd = tr_syscall(SYS_open, (long)argv[a], O_RDONLY, 0, 0, 0, 0);
	if (fd < 0) {
		put("cannot open the command file\n");
		flushout();
		return 1;
	}
	while (got < (long)sizeof(text) - 1) {
		n = tr_syscall(SYS_read, fd, (long)(text + got),
			       (long)(sizeof(text) - 1 - got), 0, 0, 0);
		if (n <= 0)
			break;
		got += n;
	}
	text[got] = 0;
	tr_syscall(SYS_close, fd, 0, 0, 0, 0, 0);

	p = text;
	end = text + got;
	while (p < end) {
		char *line = p, *tok;
		unsigned long f[6], w[MAXW], x[MAXW];
		u32 resp[MAXW];
		int nf = 0, nw = 0, nx = 0, after = 0, i, bad;
		long st;

		while (p < end && *p != '\n')
			p++;
		if (p < end)
			*p++ = 0;
		while (is_space(*line))
			line++;
		if (!*line || *line == '#')
			continue;
		while (*line) {
			unsigned long v;

			tok = line;
			while (*line && !is_space(*line))
				line++;
			if (*line)
				*line++ = 0;
			while (is_space(*line))
				line++;
			if (streq(tok, "=")) {
				after = 1;
				continue;
			}
			if (parse(tok, &v))
				break;
			if (nf < 6)
				f[nf++] = v;
			else if (!after && nw < MAXW)
				w[nw++] = v;
			else if (after && nx < MAXW)
				x[nx++] = v;
		}
		if (nf < 6 || f[0] > 31 || f[3] > 4 * MAXW ||
		    f[4] > 4 * MAXW) {
			put("bad line\n");
			flushout();
			continue;
		}
		st = post(f[0], f[1], f[2], f[3], f[4], w, resp);
		if (st == -1000) {
			ntimeout++;
			put("TIMEOUT cmd 0x");
			puthex(f[1] >> 16, 4);
			put(" seq 0x");
			puthex(f[1] & 0xffff, 4);
			put(" at entry ");
			putdec((long)f[0]);
			put(" -- stopping\n");
			break;
		}
		bad = st != (long)f[5];
		for (i = 0; i < nx && i < (int)(f[4] / 4); i++)
			if (resp[i] != x[i])
				bad = 1;
		if (bad)
			nbad++;
		else
			nok++;
		if (bad || verbose) {
			put(bad ? "DIFF " : "ok   ");
			put("cmd 0x");
			puthex(f[1] >> 16, 4);
			put(" seq 0x");
			puthex(f[1] & 0xffff, 4);
			put(" status ");
			putdec(st);
			put(" (want ");
			putdec((long)f[5]);
			put(") resp");
			for (i = 0; i < (int)(f[4] / 4); i++) {
				put(" ");
				puthex(resp[i], 8);
			}
			if (bad && nx) {
				put(" (want");
				for (i = 0; i < nx; i++) {
					put(" ");
					puthex(x[i], 8);
				}
				put(")");
			}
			put("\n");
		}
		flushout();
	}
	put("commands ok ");
	putdec((long)nok);
	put(", different ");
	putdec((long)nbad);
	put(", timed out ");
	putdec((long)ntimeout);
	put("\n");
	flushout();
	return nbad || ntimeout;
}
