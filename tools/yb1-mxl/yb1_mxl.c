// SPDX-License-Identifier: GPL-2.0-only
/*
 * yb1-mxl - bring the POE-BR100-YB1 demodulator up from cold, without the
 * vendor application.
 *
 * Written from the written specification only:
 *   docs/specs/poe-br100-yb1-reception-stack.md  §29 (cold-start sequences)
 *   docs/specs/mbin-firmware-container.md        §2,§3,§5,§7 (container, transfer)
 * No vendor code was disassembled; the sequences in those sections come from
 * bus observation.
 *
 * The firmware itself is NOT shipped with OpenIRD.  It lives inside the vendor
 * application on the owner's own device, so this tool scans a file the owner
 * points it at (normally /app/bin/blur) for the MBIN container and streams it
 * to the chip at run time.
 *
 * Usage:
 *   yb1-mxl <bus> probe                     read the identity register
 *   yb1-mxl <bus> rd <reg>                  16-bit register read
 *   yb1-mxl <bus> rd32 <reg>                32-bit register read
 *   yb1-mxl <bus> wr <reg> <val>            16-bit register write
 *   yb1-mxl <bus> scan <file>               locate the MBIN, print its shape
 *   yb1-mxl <bus> load <file>               sequences A + B + C
 *   yb1-mxl <bus> tune <MHz>                sequence D (integer call sign)
 *   yb1-mxl <bus> bringup <file> [MHz]      load, then tune
 *   yb1-mxl <bus> status                    lock / quality registers
 *
 * <bus> is the number in /dev/i2c-N (bus 1 on this board).  Numbers are
 * decimal unless prefixed with 0x.
 *
 * Static, no libc, like the other stage-B/C tools here.
 */

/* MIPS o32 syscall numbers (arch/mips/kernel/syscalls/syscall_o32.tbl). */
#define SYS_read	4003
#define SYS_write	4004
#define SYS_open	4005
#define SYS_close	4006
#define SYS_lseek	4019
#define SYS_ioctl	4054
#define SYS_nanosleep	4166

#define O_RDONLY	0x0000
#define O_RDWR		0x0002
#define SEEK_SET	0

/* i2c-dev ioctls are plain numbers, not _IOC-encoded (include/uapi/linux/i2c-dev.h) */
#define I2C_RDWR	0x0707
#define I2C_M_RD	0x0001

#define STDOUT		1

/* Demodulator slave address and the two read-selector registers (spec §29). */
#define MXL_ADDR	0x60
#define SEL_R16		0xfffb
#define SEL_R32		0xfffd

/* Firmware download framing (mbin-firmware-container.md §7.1). */
#define DL_TAG		0xe0
#define DL_OP_ADDR	0x01
#define DL_OP_DATA	0x02
#define DL_OP_START	0x03
#define DL_MAX_PAYLOAD	44
#define DL_SEQ_MOD	15

/* MBIN container (mbin-firmware-container.md §2,§3). */
#define MBIN_HDR_LEN	16
#define MBIN_SEG_HDR	8
#define MBIN_MAX_SEG	32
#define MBIN_MAX_IMAGE	0x3fff0

#define SCAN_BUF	0x10000
#define CONT_BUF	(MBIN_HDR_LEN + MBIN_MAX_IMAGE)

typedef unsigned short u16;
typedef unsigned char u8;
typedef unsigned int u32;

long tr_syscall(long nr, long a, long b, long c, long d, long e, long f);

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

struct timespec32 {
	long tv_sec;
	long tv_nsec;
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

static void nap(long ms)
{
	struct timespec32 ts;

	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (ms % 1000) * 1000000L;
	tr_syscall(SYS_nanosleep, (long)&ts, 0, 0, 0, 0, 0);
}

/* --- argument parsing --------------------------------------------------- */

static int streq(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

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

/* --- register access (spec §29 preamble) -------------------------------- */

static int i2c_fd;

static int xfer(struct i2c_msg *m, int n)
{
	struct i2c_rdwr_ioctl_data d;

	d.msgs = m;
	d.nmsgs = n;
	return (int)tr_syscall(SYS_ioctl, i2c_fd, I2C_RDWR, (long)&d, 0, 0, 0);
}

static int raw_write(const u8 *buf, int len)
{
	struct i2c_msg m;

	m.addr = MXL_ADDR;
	m.flags = 0;
	m.len = (u16)len;
	m.buf = (u8 *)buf;
	return xfer(&m, 1);
}

static int raw_read(u8 *buf, int len)
{
	struct i2c_msg m;

	m.addr = MXL_ADDR;
	m.flags = I2C_M_RD;
	m.len = (u16)len;
	m.buf = buf;
	return xfer(&m, 1);
}

/* Some writes in the cold-start sequence reset part of the chip, and it stops
 * acknowledging for a while (measured: writing 1 then 0 to 0x0003 takes it off
 * the bus).  Every register access therefore retries until the chip answers
 * again.  A NAKed transaction was not accepted, so retrying cannot double it.
 *
 * io_retries carries how many retries the last access needed, so the caller
 * can report where the chip went away and for how long.
 */
#define IO_TRIES	400		/* x IO_WAIT_MS = 4 s */
#define IO_WAIT_MS	10

static int io_retries;

/* write: one 4-byte transaction [reg_hi reg_lo val_hi val_lo] */
static int reg_write(int reg, int val)
{
	u8 b[4];
	int i;

	b[0] = (u8)(reg >> 8);
	b[1] = (u8)reg;
	b[2] = (u8)(val >> 8);
	b[3] = (u8)val;
	for (i = 0; i < IO_TRIES; i++) {
		if (raw_write(b, 4) >= 0) {
			io_retries = i;
			return 0;
		}
		nap(IO_WAIT_MS);
	}
	io_retries = i;
	return -1;
}

/* read: select through 0xfffb (2 bytes back) or 0xfffd (4 bytes back).  The
 * selector write and the data read are retried as one unit: a read that lost
 * its selector would return someone else's register. */
static long reg_read_n(int sel, int reg, int n)
{
	u8 b[4], v[4];
	int i, k;
	long out = 0;

	b[0] = (u8)(sel >> 8);
	b[1] = (u8)sel;
	b[2] = (u8)(reg >> 8);
	b[3] = (u8)reg;
	for (i = 0; i < IO_TRIES; i++) {
		if (raw_write(b, 4) >= 0 && raw_read(v, n) >= 0) {
			io_retries = i;
			for (k = 0; k < n; k++)
				out = (out << 8) | v[k];
			return out;
		}
		nap(IO_WAIT_MS);
	}
	io_retries = i;
	return -1;
}

static long reg_read(int reg)
{
	return reg_read_n(SEL_R16, reg, 2);
}

static long reg_read32(int reg)
{
	return reg_read_n(SEL_R32, reg, 4);
}

/* --- replayable operation tables (spec §29.1, §29.3, §29.4) ------------- */

#define OP_END	0
#define OP_W	1
#define OP_R	2
#define OP_R32	3
#define OP_FREQ	4		/* write the tuning word into 0x3029 */

struct op {
	u8 kind;
	u16 reg;
	u16 val;
};

/* §29.1 A: from the first touch to the start of the transfer. */
static const struct op seq_a[] = {
	{OP_R, 0x300c, 0}, {OP_R, 0x300d, 0},
	{OP_W, 0x300c, 0x0000}, {OP_W, 0x300d, 0x0003},
	{OP_R, 0x3002, 0}, {OP_R, 0x3003, 0},
	{OP_W, 0x3002, 0x0100}, {OP_W, 0x3003, 0x0008},
	{OP_W, 0x3002, 0x0000}, {OP_W, 0x3003, 0x0008},
	{OP_R, 0x3001, 0},
	{OP_W, 0x0000, 0x0000}, {OP_W, 0x000e, 0x0014},
	{OP_W, 0x0000, 0x0000}, {OP_R, 0x0001, 0},
	{OP_W, 0x0000, 0x0000}, {OP_W, 0x0001, 0x0005},
	{OP_W, 0x0000, 0x0000}, {OP_R, 0x0002, 0},
	{OP_W, 0x0000, 0x0000}, {OP_W, 0x0002, 0x0000},
	{OP_W, 0x0000, 0x0000}, {OP_W, 0x006d, 0x008a},
	{OP_W, 0x0000, 0x0000}, {OP_W, 0x0003, 0x0000},
	{OP_W, 0x0000, 0x0000}, {OP_W, 0x0003, 0x0001},
	{OP_W, 0x0000, 0x0000}, {OP_W, 0x0003, 0x0000},
	{OP_W, 0x0000, 0x0000}, {OP_W, 0x000b, 0x0001},
	{OP_W, 0x0000, 0x0000}, {OP_W, 0x0012, 0x0001},
	{OP_W, 0x0000, 0x0001}, {OP_R, 0x00d5, 0},
	{OP_W, 0x0000, 0x0001}, {OP_R, 0x00d5, 0},
	{OP_W, 0x0000, 0x0001}, {OP_R, 0x00d5, 0},
	{OP_R, 0x3002, 0}, {OP_R, 0x3003, 0},
	{OP_W, 0x3002, 0x0000}, {OP_W, 0x3003, 0x2008},
	{OP_R, 0x302a, 0}, {OP_R, 0x302b, 0},
	{OP_R, 0x300a, 0}, {OP_R, 0x300b, 0},
	{OP_END, 0, 0}
};

/* §29.3 C: after the transfer, up to the tuning sequence.  The identity read
 * of 0xa8cc that confirms the firmware is running is done separately. */
static const struct op seq_c[] = {
	{OP_R, 0x300a, 0}, {OP_R, 0x300b, 0},
	{OP_W, 0x300a, 0x0270}, {OP_W, 0x300b, 0x0000},
	{OP_R, 0x3002, 0},
	{OP_W, 0x3002, 0x0000}, {OP_W, 0x3003, 0x0f89},
	{OP_R, 0x3002, 0}, {OP_R, 0x3003, 0},
	{OP_W, 0x3002, 0x0000}, {OP_W, 0x3003, 0x0f89},
	{OP_W, 0x0012, 0x0000},
	{OP_R, 0x300a, 0}, {OP_R, 0x300b, 0},
	{OP_W, 0x300a, 0x0270}, {OP_W, 0x300b, 0x0000},
	{OP_R, 0x3002, 0}, {OP_R, 0x3003, 0},
	{OP_W, 0x3002, 0x0000}, {OP_W, 0x3003, 0x0f89},
	{OP_W, 0x0012, 0x0000},
	{OP_R, 0x302a, 0}, {OP_R, 0x302b, 0},
	{OP_R, 0x300a, 0}, {OP_R, 0x300b, 0},
	{OP_W, 0xa85c, 0x01da},
	{OP_W, 0xe034, 0x0000},
	{OP_R, 0xa85c, 0},
	{OP_W, 0xa85c, 0x0000},
	{OP_R, 0x302a, 0}, {OP_R, 0x302b, 0},
	{OP_R, 0x300a, 0}, {OP_R, 0x300b, 0},
	{OP_W, 0x000b, 0x0001},
	{OP_R, 0x0196, 0}, {OP_W, 0x0196, 0x0000},
	{OP_R, 0x1122, 0}, {OP_R, 0x11ad, 0},
	{OP_W, 0x11ad, 0x0000}, {OP_W, 0x1122, 0x0220},
	{OP_R, 0x300c, 0}, {OP_R, 0x300d, 0},
	{OP_W, 0x300c, 0x0000}, {OP_W, 0x300d, 0x0003},
	{OP_R, 0xa80a, 0},
	{OP_R, 0x301e, 0}, {OP_R, 0x301f, 0},
	{OP_W, 0x301e, 0x0000}, {OP_W, 0x301f, 0x0000},
	{OP_R, 0x3002, 0}, {OP_R, 0x3003, 0},
	{OP_W, 0x3002, 0x0000}, {OP_W, 0x3003, 0x0f89},
	{OP_W, 0x0012, 0x0000},
	{OP_END, 0, 0}
};

/* §29.4 D: tune.  OP_FREQ takes the computed word. */
static const struct op seq_d[] = {
	{OP_W, 0x3028, 0x0000}, {OP_W, 0x3029, 0x0000},
	{OP_W, 0x007c, 0x001f}, {OP_W, 0x0131, 0x0081},
	{OP_W, 0x0049, 0x001c}, {OP_W, 0x00db, 0x0017},
	{OP_R, 0x00ea, 0}, {OP_W, 0x00ea, 0x0000}, {OP_W, 0x00eb, 0x00d8},
	{OP_R, 0x00b6, 0}, {OP_W, 0x00b6, 0x0040},
	{OP_R, 0x015f, 0}, {OP_W, 0x015f, 0x0037},
	{OP_R, 0x0160, 0}, {OP_W, 0x0160, 0x0037},
	{OP_FREQ, 0x3029, 0},
	{OP_R, 0x3028, 0}, {OP_W, 0x3028, 0x0000},
	{OP_W, 0xe033, 0x0000},
	{OP_R, 0x101c, 0}, {OP_W, 0x101c, 0x0004},
	{OP_R, 0x101b, 0}, {OP_W, 0x101b, 0x0000}, {OP_W, 0x101b, 0x0001},
	{OP_END, 0, 0}
};

static void report_wait(int reg, int n)
{
	put("    waited ");
	putdec((long)io_retries * IO_WAIT_MS);
	put(" ms for the chip at operation ");
	putdec(n);
	put(" (register 0x");
	puthex((unsigned long)reg, 4);
	put(")\n");
	flushout();
}

/* Replay a table.  Return values are printed, never branched on: the spec
 * (§29.5) says the reads may carry side effects, so none are skipped. */
static int replay(const struct op *s, const char *name, int freq, int verbose)
{
	int n = 0;

	for (; s->kind != OP_END; s++, n++) {
		long v;

		switch (s->kind) {
		case OP_W:
			if (reg_write(s->reg, s->val) < 0)
				goto fail;
			if (io_retries)
				report_wait(s->reg, n);
			break;
		case OP_FREQ:
			if (reg_write(s->reg, freq) < 0)
				goto fail;
			if (verbose) {
				put("    0x");
				puthex(s->reg, 4);
				put(" <- 0x");
				puthex((unsigned long)freq, 4);
				put("  (tuning word)\n");
			}
			break;
		case OP_R:
			v = reg_read(s->reg);
			if (v < 0)
				goto fail;
			if (io_retries)
				report_wait(s->reg, n);
			if (verbose) {
				put("    0x");
				puthex(s->reg, 4);
				put(" -> 0x");
				puthex((unsigned long)v, 4);
				put("\n");
			}
			break;
		case OP_R32:
			v = reg_read32(s->reg);
			if (v < 0)
				goto fail;
			break;
		}
	}
	put("  ");
	put(name);
	put(": ");
	putdec(n);
	put(" operations ok\n");
	flushout();
	return 0;
fail:
	put("  ");
	put(name);
	put(": FAILED at operation ");
	putdec(n);
	put(" (register 0x");
	puthex(s->reg, 4);
	put(")\n");
	flushout();
	return -1;
}

/* --- MBIN container (mbin-firmware-container.md §2,§3,§5) --------------- */

static u8 scanbuf[SCAN_BUF + MBIN_HDR_LEN];
static u8 cont[CONT_BUF];

struct seg {
	u32 addr;
	u32 len;
	u32 off;		/* offset of the data inside cont[] */
};

static struct seg segs[MBIN_MAX_SEG];
static int nsegs;
static u32 entry_addr;
static u32 image_size;

static u32 be24(const u8 *p) { return ((u32)p[0] << 16) | (p[1] << 8) | p[2]; }
static u32 be32(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

/* Validate a container sitting at cont[0..]; fill segs[].  0 = good. */
static int mbin_parse(u32 avail)
{
	u32 p, sum = 0, i;
	int k;

	if (avail < MBIN_HDR_LEN)
		return -1;
	if (cont[0] != 'M' || cont[1] != '1' || cont[2] != MBIN_HDR_LEN)
		return -1;
	nsegs = cont[3];
	if (nsegs < 1 || nsegs > MBIN_MAX_SEG)
		return -1;
	entry_addr = be32(cont + 4);
	image_size = be24(cont + 8);
	if (image_size < 0x100 || image_size > MBIN_MAX_IMAGE)
		return -1;
	if (avail < MBIN_HDR_LEN + image_size)
		return -1;
	for (i = 0; i < image_size; i++)
		sum += cont[MBIN_HDR_LEN + i];
	if ((sum & 0xff) != cont[11])
		return -1;

	p = 0;
	for (k = 0; k < nsegs; k++) {
		const u8 *h = cont + MBIN_HDR_LEN + p;

		if (p + MBIN_SEG_HDR > image_size || h[0] != 'S')
			return -1;
		segs[k].len = be24(h + 1);
		segs[k].addr = be32(h + 4);
		segs[k].off = MBIN_HDR_LEN + p + MBIN_SEG_HDR;
		p += MBIN_SEG_HDR + ((segs[k].len + 3) & ~3u);
		if (p > image_size)
			return -1;
	}
	return p == image_size ? 0 : -1;
}

/* Scan a file for the one valid container (§5).  Returns its offset or -1. */
static long mbin_find(const char *path)
{
	int fd = (int)tr_syscall(SYS_open, (long)path, O_RDONLY, 0, 0, 0, 0);
	long base = 0, found = -1;
	int carry = 0;

	if (fd < 0) {
		put("cannot open ");
		put(path);
		put("\n");
		return -1;
	}
	for (;;) {
		long got = tr_syscall(SYS_read, fd, (long)(scanbuf + carry),
				      SCAN_BUF, 0, 0, 0);
		long have, i;

		if (got <= 0)
			break;
		have = carry + got;
		for (i = 0; i + 2 <= have; i++) {
			long off, n;

			if (scanbuf[i] != 'M' || scanbuf[i + 1] != '1')
				continue;
			off = base - carry + i;
			if (tr_syscall(SYS_lseek, fd, off, SEEK_SET, 0, 0, 0) < 0)
				continue;
			n = 0;
			for (;;) {
				long r = tr_syscall(SYS_read, fd,
						    (long)(cont + n),
						    CONT_BUF - n, 0, 0, 0);
				if (r <= 0)
					break;
				n += r;
				if (n >= (long)CONT_BUF)
					break;
			}
			if (mbin_parse((u32)n) == 0) {
				found = off;
				goto done;
			}
			/* put the cursor back and carry on scanning */
			tr_syscall(SYS_lseek, fd, base + got, SEEK_SET, 0, 0, 0);
		}
		base += got;
		carry = MBIN_HDR_LEN;
		for (i = 0; i < carry; i++)
			scanbuf[i] = scanbuf[have - carry + i];
	}
done:
	tr_syscall(SYS_close, fd, 0, 0, 0, 0, 0);
	return found;
}

static void mbin_show(long off)
{
	int k;

	put("  container at offset 0x");
	puthex((unsigned long)off, 8);
	put(", total ");
	putdec(MBIN_HDR_LEN + image_size);
	put(" bytes\n  entry 0x");
	puthex(entry_addr, 8);
	put(", ");
	putdec(nsegs);
	put(" segments\n");
	for (k = 0; k < nsegs; k++) {
		put("    seg[");
		putdec(k);
		put("] addr 0x");
		puthex(segs[k].addr, 8);
		put(" len ");
		putdec((long)segs[k].len);
		put("\n");
	}
	flushout();
}

/* --- firmware transfer (mbin-firmware-container.md §7) ------------------ */

static int dl_seq;

static int dl_cmd(int op, const u8 *payload, int len)
{
	u8 b[4 + DL_MAX_PAYLOAD];
	unsigned int sum = 0;
	int i;

	b[0] = (u8)(DL_TAG | dl_seq);
	b[1] = (u8)op;
	for (i = 0; i < len; i++) {
		b[4 + i] = payload[i];
		sum += payload[i];
	}
	b[2] = (u8)(sum & 0xff);
	b[3] = (u8)len;
	for (i = 0; i < IO_TRIES; i++) {
		if (raw_write(b, 4 + len) >= 0)
			break;
		nap(IO_WAIT_MS);
	}
	if (i == IO_TRIES)
		return -1;
	/* §7.3: the counter runs 0..14 over data blocks and is reset to 0 by
	 * every address/start command, which themselves carry 0. */
	if (op == DL_OP_DATA)
		dl_seq = (dl_seq + 1) % DL_SEQ_MOD;
	else
		dl_seq = 0;
	return 0;
}

static int dl_addr_cmd(int op, u32 addr)
{
	u8 a[4];

	a[0] = (u8)(addr >> 24);
	a[1] = (u8)(addr >> 16);
	a[2] = (u8)(addr >> 8);
	a[3] = (u8)addr;
	return dl_cmd(op, a, 4);
}

static int fw_download(void)
{
	int k, blocks = 0;
	u32 total = 0;

	dl_seq = 0;
	for (k = 0; k < nsegs; k++) {
		u32 done = 0;

		if (dl_addr_cmd(DL_OP_ADDR, segs[k].addr) < 0) {
			put("  address command failed for segment ");
			putdec(k);
			put("\n");
			return -1;
		}
		while (done < segs[k].len) {
			u32 n = segs[k].len - done;

			if (n > DL_MAX_PAYLOAD)
				n = DL_MAX_PAYLOAD;
			if (dl_cmd(DL_OP_DATA, cont + segs[k].off + done,
				   (int)n) < 0) {
				put("  data block failed at segment ");
				putdec(k);
				put(" offset ");
				putdec((long)done);
				put("\n");
				return -1;
			}
			done += n;
			total += n;
			blocks++;
		}
	}
	put("  transferred ");
	putdec((long)total);
	put(" bytes in ");
	putdec(blocks);
	put(" data blocks + ");
	putdec(nsegs);
	put(" address commands\n");
	flushout();

	/* §29.2: a 32-bit read sits either side of the start command. */
	reg_read32(0x0003);
	if (dl_addr_cmd(DL_OP_START, entry_addr) < 0) {
		put("  start command failed\n");
		return -1;
	}
	reg_read32(0x0003);
	return 0;
}

static int fw_alive(void)
{
	long v;
	int tries;

	for (tries = 0; tries < 50; tries++) {
		v = reg_read(0xa8cc);
		if (v == 0xabcd) {
			put("  firmware alive: 0xa8cc = 0xabcd");
			if (tries) {
				put(" (after ");
				putdec(tries * 10);
				put(" ms)");
			}
			put("\n");
			flushout();
			return 0;
		}
		nap(10);
	}
	put("  firmware did NOT come up: 0xa8cc = 0x");
	puthex((unsigned long)(v < 0 ? 0xffff : v), 4);
	put(" (want 0xabcd)\n");
	flushout();
	return -1;
}

/* --- commands ----------------------------------------------------------- */

static int cmd_load(const char *path)
{
	long off;

	put("scanning ");
	put(path);
	put(" for the firmware container\n");
	flushout();
	off = mbin_find(path);
	if (off < 0) {
		put("  no valid MBIN found\n");
		flushout();
		return 1;
	}
	mbin_show(off);

	put("A: cold-start sequence\n");
	flushout();
	if (replay(seq_a, "sequence A", 0, 0) < 0)
		return 1;
	put("B: firmware transfer\n");
	flushout();
	if (fw_download() < 0)
		return 1;
	if (fw_alive() < 0)
		return 1;
	put("C: post-transfer sequence\n");
	flushout();
	if (replay(seq_c, "sequence C", 0, 0) < 0)
		return 1;
	return 0;
}

/* §29.4: reg = (448 * X + 64) / 7 for terrestrial call sign X in MHz. */
static int tune_word(long mhz)
{
	return (int)((448 * mhz + 64) / 7);
}

static int cmd_tune(long mhz)
{
	int w = tune_word(mhz);

	put("D: tune to ");
	putdec(mhz);
	put(" MHz (word 0x");
	puthex((unsigned long)w, 4);
	put(")\n");
	flushout();
	return replay(seq_d, "sequence D", w, 1) < 0 ? 1 : 0;
}

static int cmd_status(void)
{
	static const u16 regs[] = {0xa8cc, 0xa80a, 0x112d, 0x0033, 0x0034,
				   0x3028, 0x3029};
	unsigned int i;

	for (i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
		long v = reg_read(regs[i]);

		put("  0x");
		puthex(regs[i], 4);
		put(" = ");
		if (v < 0) {
			put("read failed\n");
		} else {
			put("0x");
			puthex((unsigned long)v, 4);
			put("  (");
			putdec(v);
			put(")\n");
		}
	}
	flushout();
	return 0;
}

static void usage(void)
{
	put("yb1-mxl <bus> probe | rd <reg> | rd32 <reg> | wr <reg> <val>\n"
	    "        | scan <file> | load <file> | tune <MHz>\n"
	    "        | bringup <file> [MHz] | status\n");
	flushout();
}

int main(int argc, char **argv)
{
	static char dev[] = "/dev/i2c-0";   /* static: a local would need memcpy */
	long bus;
	int rc = 0;

	if (argc < 3) {
		usage();
		return 2;
	}
	bus = parse(argv[1]);
	if (bus < 0 || bus > 9) {
		put("bad bus\n");
		flushout();
		return 2;
	}
	dev[9] = (char)('0' + bus);
	i2c_fd = (int)tr_syscall(SYS_open, (long)dev, O_RDWR, 0, 0, 0, 0);
	if (i2c_fd < 0) {
		put("cannot open ");
		put(dev);
		put("\n");
		flushout();
		return 1;
	}

	if (streq(argv[2], "probe")) {
		long v = reg_read(0x3001);

		put("  0x3001 = ");
		if (v < 0) {
			put("no answer (chip held in reset, or wrong bus)\n");
			rc = 1;
		} else {
			put("0x");
			puthex((unsigned long)v, 4);
			put("\n");
		}
	} else if (streq(argv[2], "rd") && argc >= 4) {
		long v = reg_read((int)parse(argv[3]));

		put("  0x");
		puthex((unsigned long)v, 4);
		put("\n");
		rc = v < 0;
	} else if (streq(argv[2], "rd32") && argc >= 4) {
		long v = reg_read32((int)parse(argv[3]));

		put("  0x");
		puthex((unsigned long)v, 8);
		put("\n");
		rc = v < 0;
	} else if (streq(argv[2], "wr") && argc >= 5) {
		rc = reg_write((int)parse(argv[3]), (int)parse(argv[4])) < 0;
		put(rc ? "  write failed\n" : "  ok\n");
	} else if (streq(argv[2], "scan") && argc >= 4) {
		long off = mbin_find(argv[3]);

		if (off < 0) {
			put("  no valid MBIN found\n");
			rc = 1;
		} else {
			mbin_show(off);
		}
	} else if (streq(argv[2], "load") && argc >= 4) {
		rc = cmd_load(argv[3]);
	} else if (streq(argv[2], "tune") && argc >= 4) {
		rc = cmd_tune(parse(argv[3]));
	} else if (streq(argv[2], "bringup") && argc >= 4) {
		rc = cmd_load(argv[3]);
		if (!rc && argc >= 5)
			rc = cmd_tune(parse(argv[4]));
	} else if (streq(argv[2], "status")) {
		rc = cmd_status();
	} else {
		usage();
		rc = 2;
	}

	flushout();
	tr_syscall(SYS_close, i2c_fd, 0, 0, 0, 0, 0);
	return rc;
}
