// SPDX-License-Identifier: GPL-2.0-only
/*
 * yb1-trace - LD_PRELOAD observer for the POE-BR100-YB1 reception chardevs.
 *
 * Stage-B observation tool (not part of any distributed image).  Loaded into
 * the vendor application, it logs:
 *   - open/close of anything under /dev/ (to give file descriptors a name)
 *   - every ioctl on such a descriptor, decoded with the MIPS _IOC encoding,
 *     with the argument bytes: the input before the call for _IOC_WRITE, the
 *     output after it for _IOC_READ
 *   - the bytes behind a 4-byte ioctl argument when that word is a readable
 *     user address.  The board's /dev/io_controller passes its command and
 *     event buffers that way, so the argument itself is only a pointer
 *   - optionally read()/write() on those descriptors, when the runtime flag
 *     file exists (off by default: the TS path would flood the log)
 *
 * The interesting descriptor is /dev/fe_main: on this board the vendor
 * application drives tuning only through it, and the demodulator/tuner I2C
 * happens inside the kernel where userland cannot see it
 * (docs/specs/poe-br100-yb1-reception-stack.md sections 3 and 4).
 *
 * It does not link against any libc: the vendor processes use uClibc 0.9.31,
 * so this library talks to the kernel with raw syscalls and imports only
 * __errno_location, which that uClibc exports.  One log line is one write()
 * to the log, so lines from threads do not interleave.
 *
 * Nothing here decodes vendor code.  The ioctl numbering is read off the wire
 * with the architecture's own _IOC macros; the meaning of each command is to
 * be established by observation, not by reading the driver.
 */
#include <stdarg.h>

/* MIPS o32 syscall numbers (arch/mips/kernel/syscalls/syscall_o32.tbl). */
#define SYS_read		4003
#define SYS_write		4004
#define SYS_open		4005
#define SYS_close		4006
#define SYS_ioctl		4054
#define SYS_msync		4144
#define SYS_nanosleep		4166
#define SYS_gettid		4222
#define SYS_clock_gettime	4263
#define SYS_openat		4288

/* MIPS open flags differ from the asm-generic ones: O_APPEND is 0x0008, not
 * 02000 (arch/mips/include/uapi/asm/fcntl.h).  Getting this wrong would write
 * the log at offset 0 every time. */
#define O_WRONLY	0x0001
#define O_APPEND	0x0008
#define O_CREAT		0x0100
#define O_CLOEXEC	02000000
#define AT_FDCWD	(-100)
#define CLOCK_MONOTONIC	1
#define MS_ASYNC	0x0001

/* MIPS _IOC encoding (arch/mips/include/uapi/asm/ioctl.h): three direction
 * bits at the top, and NONE/READ/WRITE are 1/2/4 -- not the asm-generic
 * two-bit 0/2/1. */
#define IOC_NRBITS	8
#define IOC_TYPEBITS	8
#define IOC_SIZEBITS	13
#define IOC_NRSHIFT	0
#define IOC_TYPESHIFT	(IOC_NRSHIFT + IOC_NRBITS)
#define IOC_SIZESHIFT	(IOC_TYPESHIFT + IOC_TYPEBITS)
#define IOC_DIRSHIFT	(IOC_SIZESHIFT + IOC_SIZEBITS)
#define IOC_NONE	1U
#define IOC_READ	2U
#define IOC_WRITE	4U

/* Flags live on /app/setting (mtd8, yaffs2 rw) rather than /tmp: a tmpfs flag
 * is wiped by the reboot needed to pick it up, and the vendor application
 * cannot be restarted on its own -- killing it takes the board down with the
 * watchdog (docs/specs/poe-br100-yb1-reception-stack.md section 8). */
#define LOG_PATH	"/tmp/yb1-trace.log"
#define RW_FLAG_PATH	"/app/setting/openird-trace-rw"
#define NODEREF_FLAG_PATH "/app/setting/openird-trace-noderef"
#define MAX_FD		256
#define PATH_LEN	40
#define DUMP_MAX	256
#define DEREF_MAX	64

long tr_syscall(long nr, long a, long b, long c, long d, long e, long f)
	__attribute__((visibility("hidden")));
extern int *__errno_location(void);

static char fd_path[MAX_FD][PATH_LEN];

struct line {
	char s[2048];
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
		t[--i] = '0' + (char)(u % 10);	/* MIPS32 has divu; no libgcc */
		u /= 10;
	} while (u && i > 1);
	if (v < 0)
		t[--i] = '-';
	put(l, t + i);
}

/* Is [p, p+n) mapped?  Not every ioctl passes its argument by pointer: some
 * encode a direction and a size but hand the driver a small integer by value.
 * Dereferencing that would kill the vendor application, so ask the kernel
 * before touching it.
 *
 * msync(MS_ASYNC) is the probe: on a mapped range it is a no-op returning 0,
 * on a range with an unmapped page it returns -ENOMEM, and either way it
 * copies nothing and changes nothing. */
static int readable(const void *p, unsigned long n)
{
	unsigned long a = (unsigned long)p;
	unsigned long start, end;
	long r;

	if (!p || !n)
		return 0;
	start = a & ~0xfffUL;
	end = (a + n + 0xfffUL) & ~0xfffUL;
	r = tr_syscall(SYS_msync, (long)start, (long)(end - start), MS_ASYNC,
		       0, 0, 0);
	if (r == -38)		/* -ENOSYS */
		return a >= 0x1000;
	return r == 0;
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
	if (!readable(p, n)) {
		put(l, "<byvalue:0x");
		put_hex(l, (unsigned long)p, 8);
		put(l, ">");
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

static void emit(struct line *l)
{
	int fd;

	if (l->n && l->s[l->n - 1] != '\n')
		l->s[l->n++] = '\n';
	/* Open, write and close for every line: the vendor application sweeps a
	 * range of descriptors closed as it starts up, so a cached log fd would
	 * be clobbered.  O_APPEND keeps one write() atomic, so lines from
	 * different threads still do not interleave. */
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
	put_dec(l, ts[1]);	/* nanoseconds, unscaled */
	put(l, " [");
	put_dec(l, tr_syscall(SYS_gettid, 0, 0, 0, 0, 0, 0));
	put(l, "] ");
}

static const char *fd_name(int fd)
{
	if (fd >= 0 && fd < MAX_FD && fd_path[fd][0])
		return fd_path[fd];
	return 0;
}

static int is_dev(const char *p)
{
	return p && p[0] == '/' && p[1] == 'd' && p[2] == 'e' && p[3] == 'v'
	       && p[4] == '/';
}

static int flag_set(const char *path)
{
	long fd = tr_syscall(SYS_open, (long)path, O_CLOEXEC, 0, 0, 0, 0);

	if (fd < 0)
		return 0;
	tr_syscall(SYS_close, fd, 0, 0, 0, 0, 0);
	return 1;
}

/* read()/write() logging is opt-in: the TS path would otherwise flood the log.
 * The answer is cached per process, so the flag has to be in place before the
 * vendor application starts. */
static int rw_enabled(void)
{
	static int cached = -1;

	if (cached < 0)
		cached = flag_set(RW_FLAG_PATH);
	return cached;
}

static int deref_enabled(void)
{
	static int cached = -1;

	if (cached < 0)
		cached = !flag_set(NODEREF_FLAG_PATH);
	return cached;
}

/* A 4-byte ioctl argument is often just a small integer, but /dev/io_controller
 * uses it to hand the driver the address of a command or event buffer.  Read
 * the word big-endian (this is a BE board), and dump what it points at when the
 * kernel says that address is mapped.  readable() keeps a plain integer like
 * 0x00000014 from ever being dereferenced. */
static void put_deref(struct line *l, const char *label, const unsigned char *p)
{
	unsigned long v;

	if (!deref_enabled() || !readable(p, 4))
		return;
	v = ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
	    ((unsigned long)p[2] << 8) | p[3];
	if (!readable((const void *)v, DEREF_MAX))
		return;
	put(l, " ");
	put(l, label);
	put(l, "@0x");
	put_hex(l, v, 8);
	put_bytes(l, "", (const unsigned char *)v, DEREF_MAX);
}

static void remember_fd(int fd, const char *path)
{
	struct line l;
	int i = 0;

	if (fd < 0 || fd >= MAX_FD || !is_dev(path))
		return;
	while (path[i] && i < PATH_LEN - 1) {
		fd_path[fd][i] = path[i];
		i++;
	}
	fd_path[fd][i] = 0;

	l.n = 0;
	stamp(&l);
	put(&l, "open ");
	put(&l, fd_path[fd]);
	put(&l, " -> fd ");
	put_dec(&l, fd);
	emit(&l);
}

/* --- interposed entry points -------------------------------------------- */

int open(const char *path, int flags, ...)
{
	va_list ap;
	long mode = 0;
	long r;

	if (flags & O_CREAT) {
		va_start(ap, flags);
		mode = va_arg(ap, int);
		va_end(ap);
	}
	r = tr_syscall(SYS_open, (long)path, flags, mode, 0, 0, 0);
	if (r >= 0)
		remember_fd((int)r, path);
	return set_errno(r);
}

int open64(const char *path, int flags, ...)
{
	va_list ap;
	long mode = 0;
	long r;

	if (flags & O_CREAT) {
		va_start(ap, flags);
		mode = va_arg(ap, int);
		va_end(ap);
	}
	r = tr_syscall(SYS_open, (long)path, flags, mode, 0, 0, 0);
	if (r >= 0)
		remember_fd((int)r, path);
	return set_errno(r);
}

int openat(int dirfd, const char *path, int flags, ...)
{
	va_list ap;
	long mode = 0;
	long r;

	if (flags & O_CREAT) {
		va_start(ap, flags);
		mode = va_arg(ap, int);
		va_end(ap);
	}
	r = tr_syscall(SYS_openat, dirfd, (long)path, flags, mode, 0, 0);
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

int ioctl(int fd, unsigned long req, ...)
{
	const char *name = fd_name(fd);
	unsigned int dir = (unsigned int)(req >> IOC_DIRSHIFT) & 7U;
	unsigned int size = (unsigned int)(req >> IOC_SIZESHIFT) & 0x1fffU;
	struct line l;
	va_list ap;
	void *arg;
	long r;

	va_start(ap, req);
	arg = va_arg(ap, void *);
	va_end(ap);

	if (!name)
		return set_errno(tr_syscall(SYS_ioctl, fd, (long)req,
					    (long)arg, 0, 0, 0));

	l.n = 0;
	stamp(&l);
	put(&l, "ioctl ");
	put(&l, name);
	put(&l, " cmd=0x");
	put_hex(&l, req, 8);
	put(&l, " dir=");
	put(&l, dir & IOC_WRITE ? (dir & IOC_READ ? "WR" : "W ") :
	       (dir & IOC_READ ? "R " : "N "));
	put(&l, " type=0x");
	put_hex(&l, (req >> IOC_TYPESHIFT) & 0xff, 2);
	put(&l, " nr=0x");
	put_hex(&l, (req >> IOC_NRSHIFT) & 0xff, 2);
	put(&l, " size=");
	put_dec(&l, size);
	if (!size) {
		/* No size encoded: the argument is whatever the driver makes of
		 * the third word, so record it raw rather than dereference it. */
		put(&l, " arg=0x");
		put_hex(&l, (unsigned long)arg, 8);
	} else if (dir & IOC_WRITE) {
		put_bytes(&l, "in", (const unsigned char *)arg, size);
		if (size == 4)
			put_deref(&l, "in*", (const unsigned char *)arg);
	}

	r = tr_syscall(SYS_ioctl, fd, (long)req, (long)arg, 0, 0, 0);

	if (size && (dir & IOC_READ)) {
		put_bytes(&l, "out", (const unsigned char *)arg, size);
		if (size == 4)
			put_deref(&l, "out*", (const unsigned char *)arg);
	}
	put(&l, " = ");
	put_dec(&l, r);
	emit(&l);
	return set_errno(r);
}

long read(int fd, void *buf, unsigned long count)
{
	long r = tr_syscall(SYS_read, fd, (long)buf, (long)count, 0, 0, 0);
	const char *name;
	struct line l;

	name = fd_name(fd);
	if (name && rw_enabled()) {
		l.n = 0;
		stamp(&l);
		put(&l, "read ");
		put(&l, name);
		put(&l, " count=");
		put_dec(&l, (long)count);
		put(&l, " = ");
		put_dec(&l, r);
		if (r > 0)
			put_bytes(&l, "data", (const unsigned char *)buf,
				  (unsigned long)r);
		emit(&l);
	}
	return set_errno(r);
}

long write(int fd, const void *buf, unsigned long count)
{
	const char *name = fd_name(fd);
	struct line l;
	long r;

	if (name && rw_enabled()) {
		l.n = 0;
		stamp(&l);
		put(&l, "write ");
		put(&l, name);
		put(&l, " count=");
		put_dec(&l, (long)count);
		put_bytes(&l, "data", (const unsigned char *)buf, count);
		r = tr_syscall(SYS_write, fd, (long)buf, (long)count, 0, 0, 0);
		put(&l, " = ");
		put_dec(&l, r);
		emit(&l);
		return set_errno(r);
	}
	return set_errno(tr_syscall(SYS_write, fd, (long)buf, (long)count,
				    0, 0, 0));
}
