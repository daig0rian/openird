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
 *   yb1-mem <phys> <len> words [n]   dump n 32-bit words (for device registers)
 *   yb1-mem <phys> <len> churn [n] [ms]   which words move over ms
 *   yb1-mem <phys> <len> nonzero     report where the region is not all zero
 *
 * "words" exists because the SoC register windows the vendor application maps
 * (BHIF at 0x10000000, XDMA at 0x10009000, timer at 0x10110000) are peripheral
 * registers, and byte-wide loads to those are not guaranteed to read back what
 * a 32-bit load would.  Use "words" there and "dump" for memory.
 *
 * Numbers are decimal unless prefixed with 0x.  Reading is passive: nothing is
 * written, so the vendor application is not disturbed.
 */

/* MIPS o32 syscall numbers (arch/mips/kernel/syscalls/syscall_o32.tbl). */
#define SYS_write	4004
#define SYS_open	4005
#define SYS_close	4006
#define SYS_mmap2	4210
#define SYS_read	4003
#define SYS_munmap	4091
#define SYS__llseek	4140
#define SYS_nanosleep	4166

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

/* Returns 0 and stores the value, or -1 if the string is not a number.
 * The value is unsigned: kernel virtual addresses (0xc0......) are ordinary
 * inputs here, so "negative means error" would reject half the address space. */
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

/* For optional count arguments, where a bad string just means "use default". */
static unsigned long parse_or(const char *s, unsigned long dflt)
{
	unsigned long v;

	return parse(s, &v) == 0 ? v : dflt;
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

/* 32-bit reads, four per line, offsets relative to the start address. */
static void cmd_words(const u8 *m, unsigned long len, unsigned long n)
{
	const volatile unsigned int *w = (const volatile unsigned int *)m;
	unsigned long i;

	if (((unsigned long)m & 3) != 0) {
		put("address is not 4-byte aligned\n");
		return;
	}
	if (n > len / 4)
		n = len / 4;
	for (i = 0; i < n; i++) {
		if ((i & 3) == 0) {
			if (i)
				put("\n");
			puthex(i * 4, 6);
			put(":");
		}
		put(" ");
		puthex(w[i], 8);
	}
	put("\n");
}

/* Which 32-bit words in this window move?
 *
 * The AV subsystem is driven through shared memory (a command stack posted by
 * the main CPU, plus state the sub-CPU writes back), so the areas that matter
 * are the ones that change while something is happening.  Snapshot, wait, read
 * again, and report only what moved -- the output stays small enough to sweep
 * a lot of windows over a slow link. */
#define CHURN_MAX	4096		/* words held in the snapshot buffer */

static unsigned int snap[CHURN_MAX];

static void cmd_churn(const u8 *m, unsigned long len, unsigned long n,
		      unsigned long ms)
{
	const volatile unsigned int *w = (const volatile unsigned int *)m;
	long ts[2];
	unsigned long i, changed = 0, shown = 0;

	if (((unsigned long)m & 3) != 0) {
		put("address is not 4-byte aligned\n");
		return;
	}
	if (n > len / 4)
		n = len / 4;
	if (n > CHURN_MAX)
		n = CHURN_MAX;
	for (i = 0; i < n; i++)
		snap[i] = w[i];

	ts[0] = (long)(ms / 1000);
	ts[1] = (long)((ms % 1000) * 1000000ul);
	tr_syscall(SYS_nanosleep, (long)ts, 0, 0, 0, 0, 0);

	for (i = 0; i < n; i++) {
		unsigned int now = w[i];

		if (now == snap[i])
			continue;
		changed++;
		if (shown < 16) {
			shown++;
			put("  +0x");
			puthex(i * 4, 5);
			put(": ");
			puthex(snap[i], 8);
			put(" -> ");
			puthex(now, 8);
			put("\n");
		}
	}
	put("churn ");
	putdec(changed);
	put(" / ");
	putdec(n);
	put(" words in ");
	putdec(ms);
	put(" ms\n");
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

/* Read a KERNEL VIRTUAL address through /dev/kmem.
 *
 * The other commands mmap physical memory, which cannot reach a module's
 * variables: module text and data are vmalloc'd, so they have no physical
 * address a userland mmap could name.  /dev/kmem's read path does handle
 * vmalloc (it goes through vread(), which walks the vmlist and zero-fills
 * anything unmapped), so reading is both possible and safe there -- but it
 * only works through read(), not mmap().  Hence a separate command.
 *
 * Addresses come from /proc/kallsyms.  This reads; it never writes. */
static u8 kbuf[4096];

static void cmd_kread(const char *dev, unsigned long addr, unsigned long len)
{
	long long res = 0;
	unsigned long got = 0, i;
	long fd, r;

	if (len > sizeof(kbuf))
		len = sizeof(kbuf);
	fd = tr_syscall(SYS_open, (long)dev, O_RDONLY, 0, 0, 0, 0);
	if (fd < 0) {
		put("open ");
		put(dev);
		put(" failed: ");
		putdec((unsigned long)-fd);
		put("\n");
		return;
	}
	/* _llseek takes the offset split into two words; kernel addresses are
	 * above 2GB so the low word alone would look negative to lseek(2). */
	r = tr_syscall(SYS__llseek, fd, 0, (long)addr, (long)&res, 0 /* SEEK_SET */, 0);
	if (r < 0) {
		put("llseek failed: ");
		putdec((unsigned long)-r);
		put("\n");
		tr_syscall(SYS_close, fd, 0, 0, 0, 0, 0);
		return;
	}
	while (got < len) {
		r = tr_syscall(SYS_read, fd, (long)(kbuf + got),
			       (long)(len - got), 0, 0, 0);
		if (r <= 0)
			break;
		got += (unsigned long)r;
	}
	tr_syscall(SYS_close, fd, 0, 0, 0, 0, 0);

	put(dev);
	put(" 0x");
	puthex(addr, 8);
	put("  read ");
	putdec(got);
	put(" of ");
	putdec(len);
	if (r < 0) {
		put("  (read failed: ");
		putdec((unsigned long)-r);
		put(")");
	}
	put("\n");
	for (i = 0; i + 4 <= got; i += 4) {
		if ((i & 15) == 0) {
			if (i)
				put("\n");
			puthex(addr + i, 8);
			put(":");
		}
		put(" ");
		puthex(((unsigned long)kbuf[i] << 24) |
		       ((unsigned long)kbuf[i + 1] << 16) |
		       ((unsigned long)kbuf[i + 2] << 8) |
		       (unsigned long)kbuf[i + 3], 8);
	}
	put("\n");
}

static void usage(void)
{
	put("yb1-mem [@dev] <addr> <len> scan|stats|dump|words|churn|kread|nonzero [n] [ms]\n"
	    "  @dev    : map this device instead of /dev/mem (offset is its own)\n"
	    "  kread   : <addr> is a KERNEL VIRTUAL address, read via /dev/kmem\n"
	    "            (the only way to reach a module's variables; see kallsyms)\n"
	    "  scan    : find MPEG-TS sync (0x47 every 188 bytes)\n"
	    "  stats   : walk the packets, count PIDs and scrambling_control\n"
	    "  dump    : hex dump the first n bytes (default 128)\n"
	    "  words   : dump n 32-bit words -- use this for SoC registers\n"
	    "  churn   : which of n words change over ms milliseconds (default 64, 500)\n"
	    "  nonzero : how much of the region is not zero\n"
	    "0x prefix for hex.  Reads only; nothing is written.\n");
}

int main(int argc, char **argv)
{
	unsigned long phys, len, off, maplen;
	const char *dev = "/dev/mem";
	long fd, r;
	const u8 *m;

	/* "@/dev/mem2 ..." maps that device instead; the offset is then the
	 * device's own offset, not a physical address. */
	if (argc > 1 && argv[1][0] == '@') {
		dev = argv[1] + 1;
		argv++;
		argc--;
	}
	if (argc < 4) {
		usage();
		flushout();
		return 1;
	}
	if (parse(argv[1], &phys) || parse(argv[2], &len) || len == 0) {
		put("bad address/len\n");
		flushout();
		return 1;
	}

	/* kread reaches kernel virtual addresses, which mmap cannot name. */
	if (streq(argv[3], "kread")) {
		cmd_kread(dev[0] == '/' && dev[1] == 'd' && dev[2] == 'e' &&
			  dev[3] == 'v' && dev[4] == '/' && dev[5] == 'm' &&
			  dev[6] == 'e' && dev[7] == 'm' && dev[8] == 0
			  ? "/dev/kmem" : dev, phys, len);
		flushout();
		return 0;
	}

	/* mmap needs a page-aligned offset; keep the slack and skip it after. */
	off = phys & (PAGE_SIZE - 1);
	maplen = (len + off + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

	fd = tr_syscall(SYS_open, (long)dev, O_RDONLY | O_SYNC, 0, 0, 0, 0);
	if (fd < 0) {
		put("open ");
		put(dev);
		put(" failed: ");
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

	put(dev);
	put(" off 0x");
	puthex(phys, 8);
	put("  len ");
	putdec(len);
	put("\n");

	if (streq(argv[3], "scan"))
		cmd_scan(m, len);
	else if (streq(argv[3], "dump"))
		cmd_dump(m, len, argc > 4 ? parse_or(argv[4], 128) : 128);
	else if (streq(argv[3], "words"))
		cmd_words(m, len, argc > 4 ? parse_or(argv[4], 16) : 16);
	else if (streq(argv[3], "churn"))
		cmd_churn(m, len, argc > 4 ? parse_or(argv[4], 64) : 64,
			  argc > 5 ? parse_or(argv[5], 500) : 500);
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
