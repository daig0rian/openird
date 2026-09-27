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
 *   yb1-mem <phys> <len> range [stride] [us]   one word per line, for windows
 *                                    that may hang the bus (address goes out
 *                                    before the load)
 *   yb1-mem <phys> <len> find <lo> <hi>        words holding an address in
 *                                    [lo, hi] (KSEG bits folded away)
 *   yb1-mem <phys> <len> raw         the region as raw bytes, for snapshots
 *   yb1-mem @<file> 0 <len> hashes   per packet: PID, cc, CRC-32 (compare two
 *                                    captures without moving the payload)
 *   yb1-mem 0x10030000 0x10000 fifo [ms]   the TS input FIFOs in the AV SRAM:
 *                                    arrival times and TS headers only
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
#define SYS_gettimeofday 4078

#define O_RDONLY	0x0000
#define O_SYNC		0x0010		/* MIPS: O_DSYNC, enough for /dev/mem */
#define PROT_READ	0x1
#define MAP_SHARED	0x001
#define MAP_PRIVATE	0x002
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
	unsigned pid_seen[64], pid_scr[64], npid = 0, k;
	unsigned pid_cnt[64], pid_cc[64], pid_ccerr[64], pid_miss[64];
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
		if (k == npid && npid < 64) {
			pid_seen[npid] = pid;
			pid_scr[npid] = 0;
			pid_cnt[npid] = 0;
			pid_cc[npid] = 16;		/* none seen yet */
			pid_ccerr[npid] = 0;
			pid_miss[npid] = 0;
			npid++;
		}
		if (k >= 64)
			continue;
		if (sc)
			pid_scr[k] |= 1u << sc;
		pid_cnt[k]++;
		/* continuity_counter: steps by one per packet that carries a
		 * payload (adaptation_field_control bit 0).  A repeat of the last
		 * value is an allowed duplicate; anything else is a gap. */
		if (p[3] & 0x10) {
			unsigned cc = p[3] & 15;

			if (pid_cc[k] < 16 && cc != ((pid_cc[k] + 1) & 15) &&
			    cc != pid_cc[k]) {
				pid_ccerr[k]++;
				pid_miss[k] += (cc - pid_cc[k] - 1) & 15;
			}
			pid_cc[k] = cc;
		}
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
	put("\nPIDs (scr = scrambled seen; cc gaps = continuity breaks,"
	    " lost = packets they skip):\n");
	for (k = 0; k < npid; k++) {
		put("  0x");
		puthex(pid_seen[k], 4);
		put("  packets ");
		putdec(pid_cnt[k]);
		put("  cc gaps ");
		putdec(pid_ccerr[k]);
		put("  lost ");
		putdec(pid_miss[k]);
		put(pid_scr[k] ? "  scr\n" : "\n");
	}
}

/* One line per TS packet: PID, continuity_counter and the CRC-32 of the whole
 * 188 bytes (the zlib/IEEE CRC-32, so a host can compute the same thing with
 * zlib.crc32).  For checking a capture against another receiver's capture of
 * the same broadcast packet by packet, without the payload leaving either
 * machine: only the hashes travel. */
static unsigned int crc_table[256];

static unsigned int crc32_ieee(const u8 *p, unsigned long n)
{
	unsigned int c = 0xffffffffu;

	while (n--)
		c = crc_table[(c ^ *p++) & 0xff] ^ (c >> 8);
	return c ^ 0xffffffffu;
}

static void cmd_hashes(const u8 *m, unsigned long len)
{
	unsigned long i;
	unsigned int k, j;

	for (k = 0; k < 256; k++) {
		unsigned int c = k;

		for (j = 0; j < 8; j++)
			c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
		crc_table[k] = c;
	}
	for (i = 0; i + TS_LEN <= len; i += TS_LEN) {
		const u8 *p = m + i;

		if (p[0] != 0x47) {
			put("desync at +0x");
			puthex(i, 8);
			put("\n");
			break;
		}
		puthex(((p[1] & 0x1f) << 8) | p[2], 4);
		put(" ");
		puthex(p[3] & 15, 1);
		put(" ");
		puthex(crc32_ieee(p, TS_LEN), 8);
		put("\n");
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
#define CHURN_MAX	16384		/* words held in the snapshot buffer */

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
		if (shown < 512) {
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

/* Watch n words in a tight loop and record EVERY change, in order.
 *
 * churn only compares the first and last sample, so a word that goes
 * 0 -> 1 -> 0 inside the window is invisible to it -- and a shared-memory
 * command queue is full of exactly that kind of word.  This loop reads the
 * words as fast as it can and appends (word, new value) whenever one moves,
 * which is the only way to see the order the other CPU works in.
 *
 * Single CPU, so this only runs while the process that issues the command is
 * asleep waiting for its answer -- which is precisely the interval of
 * interest.  The mapping is uncached (O_SYNC), so every read is a real one. */
#define SPIN_WORDS	64
#define SPIN_EVENTS	3000

static unsigned int spin_last[SPIN_WORDS];
static unsigned short spin_ev_idx[SPIN_EVENTS];
static unsigned int spin_ev_val[SPIN_EVENTS];
static unsigned int spin_ev_us[SPIN_EVENTS];

static unsigned long now_us(void)
{
	unsigned long tv[2];

	tr_syscall(SYS_gettimeofday, (long)tv, 0, 0, 0, 0, 0);
	return tv[0] * 1000000ul + tv[1];
}

/*
 * Where did a region's content come from?  Search files for it by exact match
 * and print POSITIONS only -- never the bytes: the region may hold vendor code
 * (docs/clean-room.md).  First the whole region, then each 512-byte block on
 * its own, so that a region put together from pieces, or patched in places,
 * still shows where its pieces come from.  A block of one repeated byte is
 * only counted, not searched.
 */
#define LOC_MAX		0x10000
#define LOC_BLK		512

static u8 loc_buf[LOC_MAX];

static long loc_find(const u8 *h, unsigned long hn, const u8 *n,
		     unsigned long nn)
{
	unsigned long i, j;

	for (i = 0; i + nn <= hn; i++) {
		if (h[i] != n[0])
			continue;
		for (j = 1; j < nn && h[i + j] == n[j]; j++)
			;
		if (j == nn)
			return (long)i;
	}
	return -1;
}

static int loc_uniform(const u8 *b, unsigned long n)
{
	unsigned long i;

	for (i = 1; i < n; i++)
		if (b[i] != b[0])
			return 0;
	return 1;
}

#define LOC_PIECE	16

static void loc_span(const u8 *h, unsigned long hn, unsigned long s,
		     unsigned long e)
{
	unsigned long o, found = 0, shown = 0, total = 0, base = 0;
	int base_ok = 1;
	long r = loc_find(h, hn, loc_buf + s, e - s);

	put("  content span ");
	if (r >= 0) {
		put("at file offset 0x");
		puthex((unsigned long)r, 8);
		put("\n");
		return;
	}
	put("not found; its 16-byte pieces (filler skipped):");
	for (o = s; o < e; o += LOC_PIECE) {
		unsigned long q = o + LOC_PIECE <= e ? o : e - LOC_PIECE;
		long p;

		if (e - s < LOC_PIECE)
			q = s;		/* too short to cut: take it whole */
		if (loc_uniform(loc_buf + q, e - s < LOC_PIECE ? e - s : LOC_PIECE))
			continue;
		total++;
		p = loc_find(h, hn, loc_buf + q,
			     e - s < LOC_PIECE ? e - s : LOC_PIECE);
		if (p < 0)
			continue;
		if (!found)
			base = (unsigned long)p - q;
		else if ((unsigned long)p - q != base)
			base_ok = 0;
		found++;
		if (shown++ < 24) {
			put(" +0x");
			puthex(q, 4);
			put("@0x");
			puthex((unsigned long)p, 8);
		}
	}
	put("\n  pieces found ");
	putdec(found);
	put(" of ");
	putdec(total);
	put(" with content\n");
	if (found && found == total && base_ok) {
		/* Every piece sits at the same distance: compare word by word,
		 * printing where the region has content and whether the file
		 * has the same word there -- still positions only. */
		unsigned long w, same = 0, diff = 0, notcopied = 0;

		put("  all at file offset = region offset + 0x");
		puthex(base, 8);
		put("; non-zero words:");
		for (w = s & ~3ul; w + 4 <= e; w += 4) {
			const u8 *a = loc_buf + w, *c;
			int nz = a[0] | a[1] | a[2] | a[3], eq;

			if (base + w + 4 > hn)
				break;
			c = h + base + w;
			eq = a[0] == c[0] && a[1] == c[1] && a[2] == c[2] &&
			     a[3] == c[3];
			if (!nz) {
				if (c[0] | c[1] | c[2] | c[3])
					notcopied++;
				continue;
			}
			put(" +0x");
			puthex(w, 4);
			put(eq ? "=" : "!");
			if (eq)
				same++;
			else
				diff++;
		}
		put("\n  words equal to the file ");
		putdec(same);
		put(", different ");
		putdec(diff);
		put("; zero here but not in the file ");
		putdec(notcopied);
		put("\n");
	}
}

static void cmd_locate(const u8 *m, unsigned long len, int nf, char **files)
{
	unsigned long i, b, nblk, s, e, uniform = 0;
	int f, fill = -1;

	if (len > LOC_MAX)
		len = LOC_MAX;
	for (i = 0; i < len; i++)	/* one pass over the (uncached) window */
		loc_buf[i] = m[i];
	nblk = (len + LOC_BLK - 1) / LOC_BLK;
	/* The shape, once: which blocks are filler, and where the rest is. */
	for (b = 0; b < nblk; b++) {
		unsigned long o = b * LOC_BLK;
		unsigned long n = len - o < LOC_BLK ? len - o : LOC_BLK;

		if (loc_uniform(loc_buf + o, n)) {
			uniform++;
			if (fill < 0)
				fill = loc_buf[o];
		}
	}
	s = 0;
	e = len;
	if (fill >= 0) {
		while (s < len && loc_buf[s] == fill)
			s++;
		while (e > s && loc_buf[e - 1] == fill)
			e--;
	}
	put("blocks ");
	putdec(nblk);
	put(", uniform ");
	putdec(uniform);
	if (fill >= 0) {
		put(" (fill 0x");
		puthex((unsigned long)fill, 2);
		put(")");
	}
	put("; content span +0x");
	puthex(s, 4);
	put("..+0x");
	puthex(e, 4);
	put(" (");
	putdec(e - s);
	put(" bytes)\n");
	flushout();
	for (f = 0; f < nf; f++) {
		long fd, r;
		long long size = 0;
		const u8 *h;
		unsigned long found = 0;

		put(files[f]);
		fd = tr_syscall(SYS_open, (long)files[f], O_RDONLY, 0, 0, 0, 0);
		if (fd < 0) {
			put(": open failed\n");
			continue;
		}
		r = tr_syscall(SYS__llseek, fd, 0, 0, (long)&size, 2 /* SEEK_END */, 0);
		if (r < 0 || size <= 0) {
			put(": empty\n");
			tr_syscall(SYS_close, fd, 0, 0, 0, 0, 0);
			continue;
		}
		r = tr_syscall(SYS_mmap2, 0, (long)size, PROT_READ, MAP_PRIVATE,
			       fd, 0);
		if (r < 0 && r > -4096) {
			put(": mmap failed\n");
			tr_syscall(SYS_close, fd, 0, 0, 0, 0, 0);
			continue;
		}
		h = (const u8 *)r;
		put(": ");
		putdec((unsigned long)size);
		put(" bytes; whole region ");
		r = loc_find(h, (unsigned long)size, loc_buf, len);
		if (r >= 0) {
			put("at file offset 0x");
			puthex((unsigned long)r, 8);
			put("\n");
		} else {
			put("not found\n");
			for (b = 0; b < nblk; b++) {
				unsigned long o = b * LOC_BLK;
				unsigned long n = len - o < LOC_BLK ? len - o : LOC_BLK;
				long p;

				if (loc_uniform(loc_buf + o, n))
					continue;
				p = loc_find(h, (unsigned long)size, loc_buf + o, n);
				if (p < 0)
					continue;
				found++;
				put("  block +0x");
				puthex(o, 4);
				put(" at file offset 0x");
				puthex((unsigned long)p, 8);
				put("\n");
			}
			put("  blocks found ");
			putdec(found);
			put(" of ");
			putdec(nblk - uniform);
			put("\n");
			if (e > s)
				loc_span(h, (unsigned long)size, s, e);
		}
		tr_syscall(SYS_munmap, (long)h, (long)size, 0, 0, 0, 0);
		tr_syscall(SYS_close, fd, 0, 0, 0, 0, 0);
		flushout();
	}
}

/*
 * How much CPU is left for others: count passes of a loop that touches
 * nothing but the stack for ms milliseconds.  Run it with and without the
 * load under test; the ratio is what the load took, interrupt time included.
 * (2.6.27 samples CPU time on the scheduler tick, which cannot see time spent
 * in hard-IRQ context and aliases with work woken by a timer.)
 */
static void cmd_burn(unsigned long ms)
{
	unsigned long t0 = now_us(), t1, deadline = t0 + ms * 1000ul, n = 0;
	volatile unsigned int x = 0;
	unsigned int i;

	do {
		for (i = 0; i < 4096; i++)
			x += i;
		n++;
	} while ((t1 = now_us()) < deadline);
	put("burn ");
	putdec(n);
	put(" passes in ");
	putdec((t1 - t0) / 1000);
	put(" ms = ");
	putdec(n * 1000 / ((t1 - t0) / 1000));
	put(" per second\n");
}

static void cmd_spin(const u8 *m, unsigned long len, unsigned long n,
		     unsigned long ms)
{
	const volatile unsigned int *w = (const volatile unsigned int *)m;
	unsigned long i, ev = 0, passes = 0, t0, deadline;

	if (((unsigned long)m & 3) != 0) {
		put("address is not 4-byte aligned\n");
		return;
	}
	if (n > len / 4)
		n = len / 4;
	if (n > SPIN_WORDS)
		n = SPIN_WORDS;
	for (i = 0; i < n; i++)
		spin_last[i] = w[i];

	t0 = now_us();
	deadline = t0 + ms * 1000ul;
	while (ev < SPIN_EVENTS) {
		for (i = 0; i < n; i++) {
			unsigned int v = w[i];

			if (v == spin_last[i])
				continue;
			spin_last[i] = v;
			spin_ev_idx[ev] = (unsigned short)i;
			spin_ev_val[ev] = v;
			spin_ev_us[ev] = (unsigned int)(now_us() - t0);
			if (++ev >= SPIN_EVENTS)
				break;
		}
		passes++;
		if ((passes & 0xff) == 0 && now_us() >= deadline)
			break;
	}

	put("spin ");
	putdec(passes);
	put(" passes over ");
	putdec(n);
	put(" words, ");
	putdec(ev);
	put(" changes\n");
	for (i = 0; i < ev; i++) {
		put("  ");
		putdec(spin_ev_us[i]);
		put(" us  +0x");
		puthex((unsigned long)spin_ev_idx[i] * 4, 4);
		put(" = ");
		puthex(spin_ev_val[i], 8);
		put("\n");
	}
}

/* Walk an unknown register window one word at a time, one line per word.
 *
 * Some windows stop the board when read (0x11000000 does, spec §54.1), and
 * the only useful record of such a read is the address it died on.  So the
 * address goes out -- written, not buffered -- BEFORE the load is issued, and
 * the value follows on the same line.  A line with an address and no value is
 * the one that hung.  With us > 0 the tool also sleeps between printing the
 * address and loading it, which lets telnetd push the line onto the wire
 * first; without that the last lines can still be sitting in the pty when the
 * bus locks up. */
static void sleep_us(unsigned long us)
{
	long ts[2];

	ts[0] = (long)(us / 1000000ul);
	ts[1] = (long)((us % 1000000ul) * 1000ul);
	tr_syscall(SYS_nanosleep, (long)ts, 0, 0, 0, 0, 0);
}

static void cmd_range(const u8 *m, unsigned long phys, unsigned long len,
		      unsigned long stride, unsigned long us)
{
	unsigned long i;

	if (((unsigned long)m & 3) != 0 || (stride & 3) != 0 || stride == 0) {
		put("address and stride must be multiples of 4\n");
		return;
	}
	for (i = 0; i + 4 <= len; i += stride) {
		put("0x");
		puthex(phys + i, 8);
		put(" ");
		flushout();
		if (us)
			sleep_us(us);
		puthex(*(const volatile unsigned int *)(m + i), 8);
		put("\n");
		flushout();
	}
}

/* Report every word whose value, with the KSEG bits folded away, lies in
 * [lo, hi].  The AV side hands out KSEG1 (0xa.......) and physical forms of
 * the same address interchangeably, so both must match one query. */
static void cmd_find(const u8 *m, unsigned long phys, unsigned long len,
		     unsigned long lo, unsigned long hi)
{
	const volatile unsigned int *w = (const volatile unsigned int *)m;
	unsigned long i, n = len / 4, hits = 0;

	if (((unsigned long)m & 3) != 0) {
		put("address is not 4-byte aligned\n");
		return;
	}
	for (i = 0; i < n; i++) {
		unsigned int v = w[i];
		unsigned long p = v & 0x1ffffffful;

		if (p < lo || p > hi)
			continue;
		if (hits++ < 256) {
			put("  0x");
			puthex(phys + i * 4, 8);
			put(" = ");
			puthex(v, 8);
			put("\n");
		}
	}
	put("find [0x");
	puthex(lo, 8);
	put(", 0x");
	puthex(hi, 8);
	put("] : ");
	putdec(hits);
	put(" hits in ");
	putdec(n);
	put(" words\n");
}

/* The region as raw big-endian bytes on stdout, read with 32-bit loads (the
 * register windows want word accesses).  For snapshots: redirect to a file in
 * tmpfs, compress, and diff on the host. */
static unsigned int rawbuf[1024];

static void cmd_raw(const u8 *m, unsigned long len)
{
	const volatile unsigned int *w = (const volatile unsigned int *)m;
	unsigned long i, n = len / 4, k = 0;

	for (i = 0; i < n; i++) {
		rawbuf[k++] = w[i];
		if (k == sizeof(rawbuf) / sizeof(rawbuf[0]) || i + 1 == n) {
			tr_syscall(SYS_write, STDOUT, (long)rawbuf, (long)(k * 4),
				   0, 0, 0);
			k = 0;
		}
	}
}

/* The TS input FIFOs in the AV data SRAM (spec §73.6).
 *
 * Three control blocks, 0x80 apart from SRAM offset 0xec00; words 5..11 of
 * each hold the SRAM offsets of its 7 slots.  A slot carries a 27 MHz arrival
 * time at +0x0c and a whole 188-byte TS packet from +0x10.
 *
 * Only the arrival time and the 4-byte TS header are ever read here -- never
 * the payload.  Packets of scrambled services may pass through these slots,
 * and the question asked of them is only the header's
 * transport_scrambling_control, the same treatment as the "stats" command.
 *
 * ms == 0 prints the slots once.  ms > 0 watches all slots in a tight loop,
 * logs every new packet (a slot whose arrival time changed), then reports per
 * FIFO and PID: packets seen, the scrambling_control histogram, and the rate.
 * The loop can miss packets when the CPU is taken away, so the rate is given
 * twice: from the packets seen, and from the continuity_counter steps between
 * them (which also counts the ones that were overwritten before we looked). */
#define FIFO_CTL	0xec00
#define FIFO_N		3
#define FIFO_SLOTS	7
#define FIFO_EVENTS	20000
#define TICKS_PER_MS	27000u		/* measured: 27.000 MHz, spec §73.6 */

static unsigned int fev_ats[FIFO_EVENTS];
static unsigned int fev_hdr[FIFO_EVENTS];
static unsigned char fev_fifo[FIFO_EVENTS];

static void put_hdr_word(unsigned int h)
{
	put(" pid=0x");
	puthex((h >> 8) & 0x1fff, 4);
	put(" pusi=");
	putdec((h >> 22) & 1);
	put(" scr=");
	putdec((h >> 6) & 3);
	put(" afc=");
	putdec((h >> 4) & 3);
	put(" cc=");
	putdec(h & 15);
}

static void cmd_fifo(const u8 *m, unsigned long phys, unsigned long len,
		     unsigned long ms, int dump)
{
	const volatile unsigned int *w = (const volatile unsigned int *)m;
	unsigned int slot[FIFO_N][FIFO_SLOTS], last[FIFO_N][FIFO_SLOTS];
	unsigned long f, k, i, j, ev = 0, passes = 0, t0, deadline, r;

	if (phys != 0x10030000ul || len < 0x10000ul) {
		put("fifo wants the AV SRAM window: 0x10030000 0x10000\n");
		return;
	}
	/* The 7-word list is a queue the other CPU rotates while we read it, so
	 * one read can show a slot twice and miss another.  Read it many times
	 * and keep the union. */
	for (f = 0; f < FIFO_N; f++)
		for (k = 0; k < FIFO_SLOTS; k++)
			slot[f][k] = 0;
	for (r = 0; r < 2000; r++)
		for (f = 0; f < FIFO_N; f++)
			for (k = 0; k < FIFO_SLOTS; k++) {
				unsigned int o = w[(FIFO_CTL + f * 0x80 + 0x14) / 4 + k];
				unsigned long q;

				if (o >= 0x10000 - 0x10 - 4 || (o & 3) != 0 || o == 0)
					continue;
				for (q = 0; q < FIFO_SLOTS && slot[f][q] && slot[f][q] != o; q++)
					;
				if (q < FIFO_SLOTS && !slot[f][q])
					slot[f][q] = o;
			}
	for (f = 0; f < FIFO_N; f++)
		for (k = 0; k < FIFO_SLOTS; k++)
			last[f][k] = slot[f][k] ? w[(slot[f][k] + 0x0c) / 4] : 0;

	if (ms == 0) {
		for (f = 0; f < FIFO_N; f++)
			for (k = 0; k < FIFO_SLOTS; k++) {
				put("fifo ");
				putdec(f);
				put(" slot +0x");
				puthex(slot[f][k], 4);
				if (!slot[f][k]) {
					put(" (not set up)\n");
					continue;
				}
				put(" t=");
				puthex(w[(slot[f][k] + 0x0c) / 4], 8);
				put(" hdr=");
				puthex(w[(slot[f][k] + 0x10) / 4], 8);
				put_hdr_word(w[(slot[f][k] + 0x10) / 4]);
				put("\n");
			}
		return;
	}

	t0 = now_us();
	deadline = t0 + ms * 1000ul;
	while (ev < FIFO_EVENTS) {
		for (f = 0; f < FIFO_N; f++)
			for (k = 0; k < FIFO_SLOTS; k++) {
				unsigned int a, h, a2;

				if (!slot[f][k])
					continue;
				a = w[(slot[f][k] + 0x0c) / 4];
				if (a == last[f][k])
					continue;
				h = w[(slot[f][k] + 0x10) / 4];
				a2 = w[(slot[f][k] + 0x0c) / 4];
				last[f][k] = a2;
				if (a2 != a || ev >= FIFO_EVENTS)
					continue;	/* rewritten under us */
				fev_ats[ev] = a;
				fev_hdr[ev] = h;
				fev_fifo[ev] = (unsigned char)f;
				ev++;
			}
		passes++;
		if ((passes & 0xff) == 0 && now_us() >= deadline)
			break;
	}

	put("fifo watch ");
	putdec(now_us() - t0);
	put(" us, ");
	putdec(passes);
	put(" passes, ");
	putdec(ev);
	put(" new packets\n");

	/* Arrival order: detection order is nearly sorted already, so an
	 * insertion sort on the arrival time (relative to the first) is cheap. */
	for (i = 1; i < ev; i++) {
		unsigned int a = fev_ats[i], h = fev_hdr[i];
		unsigned char ff = fev_fifo[i];

		for (j = i; j > 0 &&
		     (int)(fev_ats[j - 1] - fev_ats[0]) > (int)(a - fev_ats[0]); j--) {
			fev_ats[j] = fev_ats[j - 1];
			fev_hdr[j] = fev_hdr[j - 1];
			fev_fifo[j] = fev_fifo[j - 1];
		}
		fev_ats[j] = a;
		fev_hdr[j] = h;
		fev_fifo[j] = ff;
	}

	/* "dump": every event as "fifo time header" -- header only, no payload --
	 * for analysis on the host. */
	for (i = 0; i < ev && (dump || i < 24); i++) {
		if (dump) {
			putdec(fev_fifo[i]);
			put(" ");
			puthex(fev_ats[i], 8);
			put(" ");
			puthex(fev_hdr[i], 8);
			put("\n");
			continue;
		}
		put("  fifo ");
		putdec(fev_fifo[i]);
		put(" t=");
		puthex(fev_ats[i], 8);
		put_hdr_word(fev_hdr[i]);
		put("\n");
	}

	/* One summary line per (fifo, pid), in order of first appearance. */
	{
	unsigned int key[32], first_ix[32];
	unsigned long nk = 0, q;

	for (i = 0; i < ev; i++) {
		unsigned int kv = ((unsigned int)fev_fifo[i] << 16) |
				  ((fev_hdr[i] >> 8) & 0x1fff);

		for (q = 0; q < nk; q++)
			if (key[q] == kv)
				break;
		if (q == nk && nk < 32) {
			key[nk] = kv;
			first_ix[nk] = i;
			nk++;
		}
	}
	for (q = 0; q < nk; q++) {
		unsigned int pid = key[q] & 0x1fff, f0 = key[q] >> 16;
		unsigned long seen = 0, steps = 0, zero = 0, span, spanms;
		unsigned long scr[4] = { 0, 0, 0, 0 };
		unsigned int first, lastt, cc = 16;

		i = first_ix[q];
		first = lastt = fev_ats[i];
		for (j = i; j < ev; j++) {
			unsigned int h = fev_hdr[j];

			if (fev_fifo[j] != f0 || ((h >> 8) & 0x1fff) != pid)
				continue;
			seen++;
			scr[(h >> 6) & 3]++;
			lastt = fev_ats[j];
			if (cc < 16) {
				unsigned int d = ((h & 15) - cc) & 15;

				if (d == 0)
					zero++;
				steps += d;
			}
			cc = h & 15;
		}
		span = lastt - first;
		spanms = span / TICKS_PER_MS;
		put("fifo ");
		putdec(f0);
		put(" pid 0x");
		puthex(pid, 4);
		put(" : seen ");
		putdec(seen);
		put("  scr none/rsv/even/odd = ");
		putdec(scr[0]);
		put("/");
		putdec(scr[1]);
		put("/");
		putdec(scr[2]);
		put("/");
		putdec(scr[3]);
		put("  span ");
		putdec(spanms);
		put(" ms");
		if (spanms) {
			put("  seen-rate ");
			putdec(seen * 1000ul / spanms);
			put("/s  cc-rate ");
			putdec(steps * 1000ul / spanms);
			put("/s (cc 0-steps ");
			putdec(zero);
			put(")");
		}
		put("\n");
	}
	}
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
	put("yb1-mem [@dev] <addr> <len> scan|stats|dump|words|churn|spin|burn|locate|kread|nonzero|range|find|raw [n] [ms]\n"
	    "  @dev    : map this device instead of /dev/mem (offset is its own)\n"
	    "  kread   : <addr> is a KERNEL VIRTUAL address, read via /dev/kmem\n"
	    "            (the only way to reach a module's variables; see kallsyms)\n"
	    "  scan    : find MPEG-TS sync (0x47 every 188 bytes)\n"
	    "  stats   : walk the packets, count PIDs and scrambling_control\n"
	    "  dump    : hex dump the first n bytes (default 128)\n"
	    "  words   : dump n 32-bit words -- use this for SoC registers\n"
	    "  churn   : which of n words change over ms milliseconds (default 64, 500)\n"
	    "  spin    : tight loop over n words for ms, log EVERY change in order\n"
	    "  burn    : [ms] count passes of a CPU-only loop (addr/len ignored):\n"
	    "            what a load leaves for others, IRQ time included\n"
	    "  locate  : <file>... where the region (or each 512-byte block of it)\n"
	    "            occurs in the files: positions only, never the bytes\n"
	    "  nonzero : how much of the region is not zero\n"
	    "  range   : one word per line, address flushed BEFORE the load\n"
	    "            [stride] [us]  (default 4, 0; us = pause before each load)\n"
	    "  find    : words whose value & 0x1fffffff is in [lo, hi]  <lo> <hi>\n"
	    "  raw     : the region as raw bytes on stdout (32-bit loads)\n"
	    "  hashes  : per TS packet 'pid cc crc32' (zlib CRC-32 of 188 bytes)\n"
	    "  fifo    : AV SRAM TS-input FIFOs (0x10030000 0x10000) [ms] [dump]:\n"
	    "            headers and arrival times only; ms > 0 watches and counts,\n"
	    "            'dump' lists every packet seen as 'fifo time header'\n"
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

	/* burn maps nothing: <addr> <len> are ignored. */
	if (streq(argv[3], "burn")) {
		cmd_burn(argc > 4 ? parse_or(argv[4], 10000) : 10000);
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

	/* hashes and raw write nothing but their data, so no banner. */
	if (streq(argv[3], "hashes")) {
		cmd_hashes(m, len);
		flushout();
		tr_syscall(SYS_munmap, r, (long)maplen, 0, 0, 0, 0);
		tr_syscall(SYS_close, fd, 0, 0, 0, 0, 0);
		return 0;
	}
	if (streq(argv[3], "raw")) {
		cmd_raw(m, len);
		tr_syscall(SYS_munmap, r, (long)maplen, 0, 0, 0, 0);
		tr_syscall(SYS_close, fd, 0, 0, 0, 0, 0);
		return 0;
	}

	put(dev);
	put(" off 0x");
	puthex(phys, 8);
	put("  len ");
	putdec(len);
	put("\n");
	flushout();

	if (streq(argv[3], "range"))
		cmd_range(m, phys, len, argc > 4 ? parse_or(argv[4], 4) : 4,
			  argc > 5 ? parse_or(argv[5], 0) : 0);
	else if (streq(argv[3], "find"))
		cmd_find(m, phys, len, argc > 4 ? parse_or(argv[4], 0) : 0,
			 argc > 5 ? parse_or(argv[5], 0) : 0);
	else if (streq(argv[3], "fifo"))
		cmd_fifo(m, phys, len, argc > 4 ? parse_or(argv[4], 0) : 0,
			 argc > 5 && streq(argv[5], "dump"));
	else if (streq(argv[3], "scan"))
		cmd_scan(m, len);
	else if (streq(argv[3], "dump"))
		cmd_dump(m, len, argc > 4 ? parse_or(argv[4], 128) : 128);
	else if (streq(argv[3], "words"))
		cmd_words(m, len, argc > 4 ? parse_or(argv[4], 16) : 16);
	else if (streq(argv[3], "churn"))
		cmd_churn(m, len, argc > 4 ? parse_or(argv[4], 64) : 64,
			  argc > 5 ? parse_or(argv[5], 500) : 500);
	else if (streq(argv[3], "spin"))
		cmd_spin(m, len, argc > 4 ? parse_or(argv[4], 8) : 8,
			 argc > 5 ? parse_or(argv[5], 500) : 500);
	else if (streq(argv[3], "stats"))
		cmd_stats(m, len);
	else if (streq(argv[3], "locate"))
		cmd_locate(m, len, argc - 4, argv + 4);
	else if (streq(argv[3], "nonzero"))
		cmd_nonzero(m, len);
	else
		usage();

	tr_syscall(SYS_munmap, r, (long)maplen, 0, 0, 0, 0);
	tr_syscall(SYS_close, fd, 0, 0, 0, 0, 0);
	flushout();
	return 0;
}
