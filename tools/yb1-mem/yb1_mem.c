// SPDX-License-Identifier: GPL-2.0-only
/*
 * yb1-mem - look at the POE-BR100-YB1 AV memory region from userland.
 *
 * Stage-B observation tool (not part of any distributed image).
 *
 * The board's AV subsystem keeps its buffers above the RAM the kernel manages:
 * /proc/iomem says System RAM ends at 0x08ffffff, and the vendor application
 * mmaps /dev/mem at 0x09000000 and beyond.  The demux ioctls pass KSEG1
 * addresses (0xa9......) into that same region, i.e. physical 0x09......
 *
 * read() on /dev/mem is refused there -- 2.6.27's MIPS valid_phys_addr_range()
 * allows RAM only -- but mmap() is not, which is how the vendor gets at it.
 * So map it and look.
 *
 * Usage:
 *   yb1-mem <phys> <len> scan        find MPEG-TS sync (0x47 every 188 bytes)
 *   yb1-mem <phys> <len> dump [n]    hex dump the first n bytes (default 128)
 *   yb1-mem <phys> <len> nonzero     report where the region is not all zero
 *
 * Numbers are decimal unless prefixed with 0x.  Reading is passive: nothing is
 * written, so the vendor application is not disturbed.
 */

/* MIPS o32 syscall numbers (arch/mips/kernel/syscalls/syscall_o32.tbl). */
#define SYS_write	4004
#define SYS_open	4005
#define SYS_close	4006
#define SYS_mmap2	4210
#define SYS_munmap	4091

#define O_RDONLY	0x0000
#define O_SYNC		0x0010		/* MIPS: O_DSYNC, enough for /dev/mem */
#define PROT_READ	0x1
#define MAP_SHARED	0x001
#define PAGE_SHIFT	12
#define PAGE_SIZE	(1UL << PAGE_SHIFT)

#define TS_LEN		188
#define STDOUT		1

typedef unsigned char u8;

long tr_syscall(long nr, long a, long b, long c, long d, long e, long f);

/* --- output (no libc) --------------------------------------------------- */

static char obuf[8192];
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

static void putdec(unsigned long u)
{
	char t[12];
	int i = sizeof(t) - 1;

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

static long parse(const char *s)
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
	return (long)v;
}

/* --- commands ----------------------------------------------------------- */

/* One TS packet header: sync, transport_error, payload_unit_start, priority,
 * 13-bit PID, scrambling control, adaptation field control, continuity. */
static void put_ts_header(const u8 *p)
{
	unsigned pid = ((p[1] & 0x1f) << 8) | p[2];

	put(" pid=0x");
	puthex(pid, 4);
	put(" tei=");
	putdec((p[1] >> 7) & 1);
	put(" pusi=");
	putdec((p[1] >> 6) & 1);
	put(" scram=");
	putdec((p[3] >> 6) & 3);
	put(" afc=");
	putdec((p[3] >> 4) & 3);
	put(" cc=");
	putdec(p[3] & 0x0f);
}

static void cmd_scan(const u8 *m, unsigned long len)
{
	unsigned long i, runs = 0, first = 0;
	int found = 0;

	for (i = 0; i + 3 * TS_LEN < len; i++) {
		if (m[i] != 0x47 || m[i + TS_LEN] != 0x47 ||
		    m[i + 2 * TS_LEN] != 0x47 || m[i + 3 * TS_LEN] != 0x47)
			continue;
		runs++;
		if (!found) {
			found = 1;
			first = i;
		}
	}
	put("TS sync (0x47 x4 at 188B) : ");
	putdec(runs);
	put(" hits\n");
	if (!found) {
		put("  (no MPEG-TS pattern here)\n");
		return;
	}
	put("  first at +0x");
	puthex(first, 8);
	put("\n");
	for (i = 0; i < 6 && first + i * TS_LEN + 4 < len; i++) {
		const u8 *p = m + first + i * TS_LEN;

		put("  pkt ");
		putdec(i);
		put(":");
		put_ts_header(p);
		put("\n");
	}
}

/* Walk the packets from the first sync and count PIDs and scrambling.
 *
 * transport_scrambling_control (bits 7-6 of byte 3) is the answer to the
 * question that matters here: if the audio/video PIDs carry a non-zero value,
 * this buffer sits *before* the descrambler, which is where OpenIRD wants to
 * tap (see the scope note in the spec). */
static void cmd_stats(const u8 *m, unsigned long len)
{
	unsigned long i, start = 0, pkts = 0, bad = 0;
	unsigned long scram[4] = { 0, 0, 0, 0 };
	unsigned pid_seen[24], pid_scr[24], npid = 0, k;
	int found = 0;

	for (i = 0; i + 3 * TS_LEN < len; i++) {
		if (m[i] == 0x47 && m[i + TS_LEN] == 0x47 &&
		    m[i + 2 * TS_LEN] == 0x47) {
			found = 1;
			start = i;
			break;
		}
	}
	if (!found) {
		put("no TS here\n");
		return;
	}
	for (i = start; i + TS_LEN <= len; i += TS_LEN) {
		const u8 *p = m + i;
		unsigned pid, sc;

		if (p[0] != 0x47) {
			bad++;
			continue;
		}
		pkts++;
		pid = ((p[1] & 0x1f) << 8) | p[2];
		sc = (p[3] >> 6) & 3;
		scram[sc]++;
		for (k = 0; k < npid; k++)
			if (pid_seen[k] == pid)
				break;
		if (k == npid && npid < 24) {
			pid_seen[npid] = pid;
			pid_scr[npid] = 0;
			npid++;
		}
		if (k < 24 && sc)
			pid_scr[k] |= 1u << sc;
	}
	put("packets ");
	putdec(pkts);
	put("  desync ");
	putdec(bad);
	put("\nscrambling_control: none=");
	putdec(scram[0]);
	put(" reserved=");
	putdec(scram[1]);
	put(" even=");
	putdec(scram[2]);
	put(" odd=");
	putdec(scram[3]);
	put("\nPIDs (scr = scrambled seen):\n");
	for (k = 0; k < npid; k++) {
		put("  0x");
		puthex(pid_seen[k], 4);
		put(pid_scr[k] ? "  scr\n" : "\n");
	}
}

static void cmd_dump(const u8 *m, unsigned long len, unsigned long n)
{
	unsigned long i;

	if (n > len)
		n = len;
	for (i = 0; i < n; i++) {
		if ((i & 15) == 0) {
			if (i)
				put("\n");
			puthex(i, 6);
			put(":");
		}
		put(" ");
		puthex(m[i], 2);
	}
	put("\n");
}

static void cmd_nonzero(const u8 *m, unsigned long len)
{
	unsigned long i, nz = 0, firstnz = 0;
	int found = 0;

	for (i = 0; i < len; i++) {
		if (!m[i])
			continue;
		nz++;
		if (!found) {
			found = 1;
			firstnz = i;
		}
	}
	put("nonzero bytes : ");
	putdec(nz);
	put(" / ");
	putdec(len);
	if (found) {
		put("   first at +0x");
		puthex(firstnz, 8);
	}
	put("\n");
}

static void usage(void)
{
	put("yb1-mem <phys> <len> scan|stats|dump|nonzero [n]\n"
	    "  scan    : find MPEG-TS sync (0x47 every 188 bytes)\n"
	    "  stats   : walk the packets, count PIDs and scrambling_control\n"
	    "  dump    : hex dump the first n bytes (default 128)\n"
	    "  nonzero : how much of the region is not zero\n"
	    "0x prefix for hex.  Reads only; nothing is written.\n");
}

int main(int argc, char **argv)
{
	unsigned long phys, len, off, maplen;
	long fd, r;
	const u8 *m;

	if (argc < 4) {
		usage();
		flushout();
		return 1;
	}
	phys = (unsigned long)parse(argv[1]);
	len = (unsigned long)parse(argv[2]);
	if ((long)phys < 0 || (long)len <= 0) {
		put("bad phys/len\n");
		flushout();
		return 1;
	}

	/* mmap needs a page-aligned offset; keep the slack and skip it after. */
	off = phys & (PAGE_SIZE - 1);
	maplen = (len + off + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

	fd = tr_syscall(SYS_open, (long)"/dev/mem", O_RDONLY | O_SYNC, 0, 0, 0, 0);
	if (fd < 0) {
		put("open /dev/mem failed: ");
		putdec((unsigned long)-fd);
		put("\n");
		flushout();
		return 1;
	}
	/* mmap2 takes the offset in pages, not bytes. */
	r = tr_syscall(SYS_mmap2, 0, (long)maplen, PROT_READ, MAP_SHARED,
		       fd, (long)((phys - off) >> PAGE_SHIFT));
	if (r < 0 && r > -4096) {
		put("mmap failed: ");
		putdec((unsigned long)-r);
		put("\n");
		tr_syscall(SYS_close, fd, 0, 0, 0, 0, 0);
		flushout();
		return 1;
	}
	m = (const u8 *)r + off;

	put("phys 0x");
	puthex(phys, 8);
	put("  len ");
	putdec(len);
	put("\n");

	if (streq(argv[3], "scan"))
		cmd_scan(m, len);
	else if (streq(argv[3], "dump"))
		cmd_dump(m, len, argc > 4 ? (unsigned long)parse(argv[4]) : 128);
	else if (streq(argv[3], "stats"))
		cmd_stats(m, len);
	else if (streq(argv[3], "nonzero"))
		cmd_nonzero(m, len);
	else
		usage();

	tr_syscall(SYS_munmap, r, (long)maplen, 0, 0, 0, 0);
	tr_syscall(SYS_close, fd, 0, 0, 0, 0, 0);
	flushout();
	return 0;
}
