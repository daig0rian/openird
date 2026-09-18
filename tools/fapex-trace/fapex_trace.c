// SPDX-License-Identifier: GPL-2.0-only
/*
 * fapex-trace - LD_PRELOAD observer for the Fujitsu FAPex character devices.
 *
 * Stage-B observation tool (not shipped in any image).  Loaded into a vendor
 * process, it logs:
 *   - open/close of anything under /dev/ (to name file descriptors)
 *   - every ioctl of type 0xFF (FAPex) with the argument bytes: the input
 *     before the call for _IOC_WRITE, the output after it for _IOC_READ
 *   - for the I2C read/write command, the transmitted and received bytes
 *     that the argument structure points to
 *   - mmap of /dev/ files (register windows via /dev/mem)
 *
 * The ioctl numbering and the I2C argument layout follow the FAPex headers
 * published in Fujitsu's GPL release (FAPexK abst_lib.h / abst_libi2c.h).
 *
 * It does not link against any libc: the vendor processes use uClibc 0.9.31,
 * so this library talks to the kernel with raw syscalls and only imports
 * __errno_location from whatever libc the process has.  One log line is one
 * write() to /tmp/fapex-trace.log, so lines from threads do not interleave.
 */
#include <stdarg.h>

#define SYS_close		6
#define SYS_open		5
#define SYS_write		4
#define SYS_read		3
#define SYS_getpid		20
#define SYS_ioctl		54
#define SYS_openat		322
#define SYS_mmap2		192
#define SYS_gettid		224
#define SYS_clock_gettime	263
#define SYS_nanosleep		162
#define SYS_dup2		63

#define O_WRONLY	01
#define O_RDWR		02
#define O_CREAT		0100
#define O_APPEND	02000
#define O_LARGEFILE	0400000
#define O_CLOEXEC	02000000
#define AT_FDCWD	(-100)
#define CLOCK_MONOTONIC	1

#define IOC_WRITE	1U
#define IOC_READ	2U
#define FAPEX_TYPE	0xFF
#define FAPEX_I2C_RW	0x62	/* _IOR = read, _IOW = write; 20-byte argument */
#define FAPEX_I2C_OPEN_NR 0x60	/* NB_I2C + 0: OpenParam (24 bytes) */
#define FAPEX_I2C_OPEN_CMD 0x4018FF60UL	/* _IOW(0xFF, 0x60, 24) */

#define LOG_PATH	"/tmp/fapex-trace.log"
#define MAX_FD		256
#define PATH_LEN	40
#define DUMP_MAX	512

long tr_syscall(long nr, long a, long b, long c, long d, long e, long f)
	__attribute__((visibility("hidden")));
extern int *__errno_location(void);

static char fd_path[MAX_FD][PATH_LEN];

/* Remembered FAPex I2C OpenParam (24 B) per fd, for the proxy shim (§5.5). */
static unsigned char i2c_op[MAX_FD][24];
static int i2c_op_set[MAX_FD];

struct line {
	char s[4096];
	int n;
};

static int set_errno(long r)
{
	if (r < 0 && r > -4096) {
		*__errno_location() = (int)-r;
		return -1;
	}
	return (int)r;
}

static void put(struct line *l, const char *s)
{
	while (*s && l->n < (int)sizeof(l->s) - 2)
		l->s[l->n++] = *s++;
}

static void put_hex(struct line *l, unsigned long v, int digits)
{
	char t[9];
	int i;

	if (digits < 1 || digits > 8)
		digits = 8;
	t[digits] = 0;
	for (i = digits - 1; i >= 0; i--, v >>= 4)
		t[i] = "0123456789abcdef"[v & 15];
	put(l, t);
}

static void put_dec(struct line *l, long v)
{
	char t[12];
	int i = sizeof(t) - 1;
	unsigned long u = v < 0 ? -(unsigned long)v : (unsigned long)v;

	t[i] = 0;
	do {
		unsigned long q = 0, r = u;

		/* u / 10 without libgcc */
		while (r >= 10) {
			unsigned long k = 10, m = 1;

			while (r >= (k << 1) && (k << 1) > k) {
				k <<= 1;
				m <<= 1;
			}
			r -= k;
			q += m;
		}
		t[--i] = '0' + r;
		u = q;
	} while (u && i > 1);
	if (v < 0)
		t[--i] = '-';
	put(l, t + i);
}

static void put_bytes(struct line *l, const char *label, const unsigned char *p,
		      unsigned long len)
{
	unsigned long i, n = len < DUMP_MAX ? len : DUMP_MAX;

	put(l, " ");
	put(l, label);
	put(l, "=");
	if (!p) {
		put(l, "NULL");
		return;
	}
	put(l, "[");
	put_dec(l, (long)len);
	put(l, "]");
	for (i = 0; i < n; i++) {
		put(l, i ? " " : ":");
		put_hex(l, p[i], 2);
	}
	if (n < len)
		put(l, "..");
}

/* A pointer is only safe to dereference if it is mapped.  The app hands the
 * driver real user pointers, so a plain read is fine; but guard the length so
 * a bogus value cannot make us walk off the end. */
static unsigned long clamp(unsigned long len)
{
	return len > DUMP_MAX ? DUMP_MAX : len;
}

static void emit(struct line *l)
{
	int fd;

	if (l->n && l->s[l->n - 1] != '\n')
		l->s[l->n++] = '\n';
	/* Open, write, close every line: the vendor daemon daemonizes and sweeps
	 * a range of fds closed, so a cached log fd would be clobbered.  O_APPEND
	 * keeps one write() atomic, so lines from threads still do not interleave. */
	fd = (int)tr_syscall(SYS_open, (long)LOG_PATH,
			     O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644,
			     0, 0, 0);
	if (fd >= 0) {
		tr_syscall(SYS_write, fd, (long)l->s, l->n, 0, 0, 0);
		tr_syscall(SYS_close, fd, 0, 0, 0, 0, 0);
	}
}

static void stamp(struct line *l)
{
	long ts[2] = { 0, 0 };

	tr_syscall(SYS_clock_gettime, CLOCK_MONOTONIC, (long)ts, 0, 0, 0, 0);
	put_dec(l, ts[0]);
	put(l, ".");
	put_dec(l, ts[1]);	/* nanoseconds; no divide (freestanding, no libgcc) */
	put(l, " [");
	put_dec(l, tr_syscall(SYS_gettid, 0, 0, 0, 0, 0, 0));
	put(l, "] ");
}

static const char *fd_name(int fd)
{
	if (fd >= 0 && fd < MAX_FD && fd_path[fd][0])
		return fd_path[fd];
	return "?";
}

static int is_dev(const char *p)
{
	return p && p[0] == '/' && p[1] == 'd' && p[2] == 'e' && p[3] == 'v'
	       && p[4] == '/';
}

/* Is this fd the Dione USB char dev (/dev/h57fmb*)?  Its ioctls tunnel the
 * tuner/demod I2C over USB; the driver is proprietary (no GPL), so we can only
 * log them black-box (§5.3). */
static int is_h57(int fd)
{
	const char *p;
	int i;

	if (fd < 0 || fd >= MAX_FD)
		return 0;
	p = fd_path[fd];
	for (i = 0; i < PATH_LEN - 6 && p[i]; i++)
		if (p[i] == 'h' && p[i + 1] == '5' && p[i + 2] == '7' &&
		    p[i + 3] == 'f' && p[i + 4] == 'm' && p[i + 5] == 'b')
			return 1;
	return 0;
}

static void remember_fd(int fd, const char *path)
{
	int i = 0;

	if (fd < 0 || fd >= MAX_FD || !is_dev(path))
		return;
	while (path[i] && i < PATH_LEN - 1) {
		fd_path[fd][i] = path[i];
		i++;
	}
	fd_path[fd][i] = 0;

	{
		struct line l; l.n = 0;

		stamp(&l);
		put(&l, "open ");
		put(&l, fd_path[fd]);
		put(&l, " -> fd ");
		put_dec(&l, fd);
		emit(&l);
	}
}

static void msleep(long ms)
{
	long ts[2];

	ts[0] = ms / 1000;
	ts[1] = (ms % 1000) * 1000000L;
	tr_syscall(SYS_nanosleep, (long)ts, 0, 0, 0, 0, 0);
}

/* Optional bring-up shim (not observation): when /tmp/fapex-h57-retry exists,
 * retry a Dione (h57fmb0) ioctl that fails with EPROTO or ENODEV.  Our USB path
 * enumerates fine but the vendor daemon queries the Dione ~10 ms after releasing
 * its reset, before the Dione firmware has booted, so the first FMB command
 * returns EPROTO (-71).  Worse, the Dione *re-enumerates itself* once its
 * firmware comes up (USB disconnect addr N -> reconnect addr N+1), which makes
 * the daemon's already-open h57fmb0 fd stale and the next FMB command returns
 * ENODEV (-19).  On ENODEV the shim re-opens the same device node and dup2()s it
 * onto the daemon's fd (transparently rebinding to the re-enumerated device),
 * then retries.  Default off, so the tracer stays a pure observer unless the
 * flag file is present. */
static int retry_enabled(void)
{
	static int cached = -1;

	if (cached < 0) {
		int fd = (int)tr_syscall(SYS_open, (long)"/tmp/fapex-h57-retry",
					 0 /*O_RDONLY*/, 0, 0, 0, 0);
		if (fd >= 0) {
			tr_syscall(SYS_close, fd, 0, 0, 0, 0, 0);
			cached = 1;
		} else {
			cached = 0;
		}
	}
	return cached;
}

/* When /tmp/fapex-h57-quiet exists, suppress the per-ioctl log lines for the
 * Dione (h57fmb0).  The retry/rebind shim and the passthrough still run; only
 * the file write is skipped.  This removes the ~1500 log I/Os of the firmware
 * transfer, whose overhead otherwise slows bootup enough to trip the daemon's
 * own bootup timeout before it reaches the demod init (§5.5).  FAPex I2C
 * (the demod register writes we actually want) is unaffected and still logged. */
static int h57_quiet(void)
{
	static int cached = -1;

	if (cached < 0) {
		int fd = (int)tr_syscall(SYS_open, (long)"/tmp/fapex-h57-quiet",
					 0 /*O_RDONLY*/, 0, 0, 0, 0);
		if (fd >= 0) {
			tr_syscall(SYS_close, fd, 0, 0, 0, 0, 0);
			cached = 1;
		} else {
			cached = 0;
		}
	}
	return cached;
}

/* Bring-up shim (not observation): when /tmp/fapex-i2c-proxy exists, run the
 * daemon's FAPex I2C read/write on a FRESH handle instead of the daemon's own
 * fd.  The vendor daemon's demod write blocks forever (D-state) on its own
 * fapi174 handle after the Dione re-enumerates, yet the identical transfer on a
 * freshly-opened handle ACKs (verified, even concurrently).  We re-open the same
 * node, re-apply the OpenParam the daemon used (captured at its I2C_OPEN), do the
 * transfer there, and return that result -- so the daemon gets past demod init
 * and on to the tuner init we want to observe.  Default off. */
static int proxy_enabled(void)
{
	static int cached = -1;

	if (cached < 0) {
		int fd = (int)tr_syscall(SYS_open, (long)"/tmp/fapex-i2c-proxy",
					 0 /*O_RDONLY*/, 0, 0, 0, 0);
		if (fd >= 0) {
			tr_syscall(SYS_close, fd, 0, 0, 0, 0, 0);
			cached = 1;
		} else {
			cached = 0;
		}
	}
	return cached;
}

/* Bring-up shim (not observation): the demod's I2C bus (SoC I2C0 / block0) is
 * gated by the Dione, and during the Dione's post-firmware re-enumeration that
 * bus is momentarily dead.  The vendor daemon writes the demod right after the
 * firmware load, i.e. inside that window, so the write never ACKs and blocks the
 * driver forever (D-state).  When /tmp/fapex-i2c-settle exists we sleep once,
 * just before the *first* FAPex I2C write to the demod (slave 0x20), to let the
 * Dione settle and the bus come alive.  The file's content is the delay in ms
 * (empty/0 -> 3000).  Default off, so the tracer stays a pure observer. */
static long settle_ms(void)
{
	static long cached = -2;

	if (cached == -2) {
		char buf[16];
		long n, i, v;
		int fd = (int)tr_syscall(SYS_open, (long)"/tmp/fapex-i2c-settle",
					 0 /*O_RDONLY*/, 0, 0, 0, 0);
		if (fd < 0) {
			cached = 0;
			return 0;
		}
		n = tr_syscall(SYS_read, fd, (long)buf, sizeof(buf) - 1, 0, 0, 0);
		tr_syscall(SYS_close, fd, 0, 0, 0, 0, 0);
		v = 0;
		for (i = 0; i < n && buf[i] >= '0' && buf[i] <= '9'; i++)
			v = v * 10 + (buf[i] - '0');
		cached = v ? v : 3000;
	}
	return cached;
}

/* --- interposed libc entry points --------------------------------------- */

int open(const char *path, int flags, ...)
{
	va_list ap;
	int mode = 0;
	long r;

	if (flags & O_CREAT) {
		va_start(ap, flags);
		mode = va_arg(ap, int);
		va_end(ap);
	}
	r = tr_syscall(SYS_open, (long)path, flags | O_LARGEFILE, mode, 0, 0, 0);
	if (r >= 0)
		remember_fd((int)r, path);
	return set_errno(r);
}

int open64(const char *path, int flags, ...)
{
	va_list ap;
	int mode = 0;

	if (flags & O_CREAT) {
		va_start(ap, flags);
		mode = va_arg(ap, int);
		va_end(ap);
	}
	return open(path, flags, mode);
}

int openat(int dirfd, const char *path, int flags, ...)
{
	va_list ap;
	int mode = 0;
	long r;

	if (flags & O_CREAT) {
		va_start(ap, flags);
		mode = va_arg(ap, int);
		va_end(ap);
	}
	r = tr_syscall(SYS_openat, dirfd, (long)path, flags | O_LARGEFILE, mode,
		       0, 0);
	if (r >= 0 && (dirfd == AT_FDCWD || path[0] == '/'))
		remember_fd((int)r, path);
	return set_errno(r);
}

int close(int fd)
{
	if (fd >= 0 && fd < MAX_FD)
		fd_path[fd][0] = 0;
	return set_errno(tr_syscall(SYS_close, fd, 0, 0, 0, 0, 0));
}

int ioctl(int fd, unsigned long cmd, ...)
{
	va_list ap;
	void *arg;
	unsigned type = (cmd >> 8) & 0xff;
	unsigned nr = cmd & 0xff;
	unsigned size = (cmd >> 16) & 0x3fff;
	unsigned dir = (cmd >> 30) & 3;
	long r;
	struct line l; l.n = 0;

	va_start(ap, cmd);
	arg = va_arg(ap, void *);
	va_end(ap);

	if (type != FAPEX_TYPE) {
		/* Dione (h57fmb0): black-box log the proprietary USB-tunnel ioctls
		 * (tuner/demod I2C rides here, §5.3).  We don't know the struct
		 * layout, so dump the top-level arg by _IOC_SIZE, both sides. */
		if (is_h57(fd)) {
			stamp(&l);
			put(&l, "ioctl ");
			put(&l, fd_name(fd));
			put(&l, " type=0x");
			put_hex(&l, type, 2);
			put(&l, " nr=0x");
			put_hex(&l, nr, 2);
			put(&l, " dir=");
			put(&l, dir == IOC_WRITE ? "W" : dir == IOC_READ ? "R" :
				dir == (IOC_READ | IOC_WRITE) ? "RW" : "-");
			put(&l, " size=");
			put_dec(&l, size);
			if ((dir & IOC_WRITE) && arg && size)
				put_bytes(&l, "in", arg, size);
			r = tr_syscall(SYS_ioctl, fd, cmd, (long)arg, 0, 0, 0);
			if ((r == -71 || r == -19) && retry_enabled()) {
				int tries = 0;
				int rebinds = 0;

				while ((r == -71 || r == -19) && tries < 40) {
					msleep(300);
					if (r == -19 && fd >= 0 && fd < MAX_FD &&
					    fd_path[fd][0]) {
						/* device re-enumerated: rebind
						 * the stale fd to the current
						 * node (open + dup2). */
						int nfd = (int)tr_syscall(
							SYS_open,
							(long)fd_path[fd],
							O_RDWR | O_LARGEFILE,
							0, 0, 0, 0);
						if (nfd >= 0) {
							if (nfd != fd) {
								tr_syscall(SYS_dup2,
									nfd, fd,
									0, 0, 0, 0);
								tr_syscall(SYS_close,
									nfd, 0, 0,
									0, 0, 0);
							}
							rebinds++;
						}
					}
					r = tr_syscall(SYS_ioctl, fd, cmd,
						       (long)arg, 0, 0, 0);
					tries++;
				}
				put(&l, " [retried ");
				put_dec(&l, tries);
				put(&l, " rebind ");
				put_dec(&l, rebinds);
				put(&l, "]");
			}
			if (r >= 0 && (dir & IOC_READ) && arg && size)
				put_bytes(&l, "out", arg, size);
			put(&l, " = ");
			put_dec(&l, r < 0 ? r : (long)(int)r);
			if (!h57_quiet())
				emit(&l);
			return set_errno(r);
		}
		return set_errno(tr_syscall(SYS_ioctl, fd, cmd, (long)arg,
					    0, 0, 0));
	}

	stamp(&l);
	put(&l, "ioctl ");
	put(&l, fd_name(fd));
	put(&l, " nr=0x");
	put_hex(&l, nr, 2);
	put(&l, " dir=");
	put(&l, dir == IOC_WRITE ? "W" : dir == IOC_READ ? "R" :
		dir == (IOC_READ | IOC_WRITE) ? "RW" : "-");
	put(&l, " size=");
	put_dec(&l, size);

	/* Remember the FAPex I2C OpenParam per fd, so the proxy shim can re-apply
	 * it on a fresh handle (§5.5). */
	if (nr == FAPEX_I2C_OPEN_NR && (dir & IOC_WRITE) && arg && size >= 24 &&
	    fd >= 0 && fd < MAX_FD) {
		const unsigned char *s = arg;
		int k;
		for (k = 0; k < 24; k++)
			i2c_op[fd][k] = s[k];
		i2c_op_set[fd] = 1;
	}

	/* input side, before the call */
	if ((dir & IOC_WRITE) && arg)
		put_bytes(&l, "in", arg, size);
	if (nr == FAPEX_I2C_RW && arg) {
		/* FAPEX_I2C_RWParamStrT: u8 slave; pad; u8* tx; u32 txlen;
		 * u8* rx; u32 rxlen  (ARM EABI, 20 bytes) */
		unsigned char *a = arg;
		unsigned char slave = a[0];
		unsigned char **txp = (unsigned char **)(a + 4);
		unsigned int txlen = *(unsigned int *)(a + 8);

		put(&l, " i2c_slave=0x");
		put_hex(&l, slave, 2);
		put_bytes(&l, "tx", *txp, clamp(txlen));
	}

	/* Bring-up shim: settle once before the first demod (slave 0x20) write, to
	 * give the Dione time to finish re-enumerating before the demod is touched.
	 * NB (§5.5): this alone does NOT unblock the vendor daemon's demod write --
	 * that write hangs forever in the kernel on the daemon's own fapi174 handle
	 * even after a long settle, while the identical write on a FRESH fapex-i2c
	 * handle ACKs (even concurrently, while the daemon is hung).  The shim is
	 * kept as a general "wait before first demod write" knob. */
	{
		static int settled = 0;
		long sm;

		if (!settled && nr == FAPEX_I2C_RW && (dir & IOC_WRITE) && arg &&
		    ((unsigned char *)arg)[0] == 0x20 && (sm = settle_ms()) > 0) {
			long slept = 0;

			/* sleep in fixed 100 ms chunks: msleep(100) folds its
			 * /1000 and %1000 at compile time, so we avoid a runtime
			 * integer divide (no libgcc in this freestanding .so). */
			while (slept < sm) {
				msleep(100);
				slept += 100;
			}
			settled = 1;
			put(&l, " [settle ");
			put_dec(&l, sm);
			put(&l, "ms]");
		}
	}

	/* I2C writes can block forever on a slave that never ACKs (e.g. a demod
	 * held in reset / without its clock).  Emit the input side now so the
	 * register bytes are captured even if the call never returns. */
	if (nr == FAPEX_I2C_RW && (dir & IOC_WRITE) && arg) {
		put(&l, " [pre-call]");
		emit(&l);
		l.n = 0;
		stamp(&l);
		put(&l, "ioctl ");
		put(&l, fd_name(fd));
		put(&l, " nr=0x");
		put_hex(&l, nr, 2);
	}

	/* proxy diagnostic (separate line so it survives a subsequent hang) */
	if (nr == FAPEX_I2C_RW) {
		struct line d; d.n = 0;
		stamp(&d);
		put(&d, "[proxydiag] en=");
		put_dec(&d, proxy_enabled());
		put(&d, " fd=");
		put_dec(&d, fd);
		put(&d, " opset=");
		put_dec(&d, (fd >= 0 && fd < MAX_FD) ? i2c_op_set[fd] : -1);
		put(&d, " path=");
		put(&d, (fd >= 0 && fd < MAX_FD && fd_path[fd][0]) ? fd_path[fd] : "?");
		emit(&d);
	}

	/* Proxy shim: run an I2C RW on a fresh handle (open + re-apply OpenParam)
	 * instead of the daemon's own (blocking) fd. */
	if (nr == FAPEX_I2C_RW && proxy_enabled() && fd >= 0 && fd < MAX_FD &&
	    i2c_op_set[fd] && fd_path[fd][0]) {
		int nfd = (int)tr_syscall(SYS_open, (long)fd_path[fd],
					  O_RDWR | O_LARGEFILE, 0, 0, 0, 0);
		struct line d; d.n = 0; stamp(&d);
		put(&d, "[proxy] nfd="); put_dec(&d, nfd);
		if (nfd >= 0) {
			long orc = tr_syscall(SYS_ioctl, nfd, FAPEX_I2C_OPEN_CMD,
					      (long)i2c_op[fd], 0, 0, 0);
			put(&d, " i2copen="); put_dec(&d, orc);
			emit(&d);
			if (orc >= 0) {
				r = tr_syscall(SYS_ioctl, nfd, cmd, (long)arg,
					       0, 0, 0);
				tr_syscall(SYS_close, nfd, 0, 0, 0, 0, 0);
				put(&l, " [proxy]");
				goto after_call;
			}
			tr_syscall(SYS_close, nfd, 0, 0, 0, 0, 0);
		} else {
			emit(&d);
		}
		/* proxy setup failed: fall through to the normal call */
	}

	r = tr_syscall(SYS_ioctl, fd, cmd, (long)arg, 0, 0, 0);
after_call:

	/* output side, after the call */
	if (r >= 0 && (dir & IOC_READ) && arg && nr != FAPEX_I2C_RW)
		put_bytes(&l, "out", arg, size);
	if (r >= 0 && nr == FAPEX_I2C_RW && arg) {
		unsigned char *a = arg;
		unsigned char **rxp = (unsigned char **)(a + 12);
		unsigned int rxlen = *(unsigned int *)(a + 16);

		put_bytes(&l, "rx", *rxp, clamp(rxlen));
	}
	put(&l, " = ");
	put_dec(&l, r < 0 ? r : (long)(int)r);
	emit(&l);
	return set_errno(r);
}

void *mmap(void *addr, unsigned long len, int prot, int flags, int fd,
	   unsigned long off)
{
	long r = tr_syscall(SYS_mmap2, (long)addr, len, prot, flags, fd,
			    off >> 12);

	if (r >= 0 || r < -4096) {
		if (fd >= 0 && fd < MAX_FD && fd_path[fd][0]) {
			struct line l; l.n = 0;

			stamp(&l);
			put(&l, "mmap ");
			put(&l, fd_name(fd));
			put(&l, " len=");
			put_dec(&l, (long)len);
			put(&l, " off=0x");
			put_hex(&l, off, 8);
			put(&l, " -> 0x");
			put_hex(&l, (unsigned long)r, 8);
			emit(&l);
		}
		return (void *)r;
	}
	*__errno_location() = (int)-r;
	return (void *)-1;
}

void *mmap64(void *addr, unsigned long len, int prot, int flags, int fd,
	     unsigned long long off)
{
	return mmap(addr, len, prot, flags, fd, (unsigned long)off);
}
