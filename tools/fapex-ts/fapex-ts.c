// SPDX-License-Identifier: GPL-2.0-only
//
// fapex-ts: stage-B / Milestone-1 observation tool for the FAPex TSD
// (transport-stream demux) character device on SB-TV02-WFPL (Fujitsu MB86H61).
//
// It talks to the vendor FAPex TSD chardev (major 244; TSD0 common = fapi000,
// its per-channel sub nodes = fapi004..fapi035) using ONLY the *published*
// FAPEX ioctl ABI from the GPL headers abst_lib.h / abst_libtsd.h and the GPL
// sample fapex_pvr_demo (tsd.c / main.c).  It contains NO vendor code -- it is a
// clean user of the chardev contract, the same one the vendor userland uses.
//
// Purpose (spec docs/specs/sb-tv02-wfpl-stage-b-fapex.md §5.6):
//   The demod (Toshiba TC90532) emits a raw MPEG2-TS (scrambled, pre-Multi2).
//   Two possible datapaths deliver it to the SoC TSD:
//     Path H : demod -> SoC TSD hardware TS input channel (CH_A/B/C)   [Dione-free]
//     Path D : demod -> Dione(MB86H58, THROUGH) -> USB -> TSD packet-RAM (linear-auto)
//   This tool reads the TSD demux output so we can (a) PROBE which HW input
//   channel/mode carries the demod TS (decides Path H), and (b) CAPTURE the raw
//   TS to a file for validation (0x47 sync, PID, scramble bits).
//
// TSD ABI (GPL abst_lib.h / abst_libtsd.h):
//   FAPEX_DEV_TYPE = 0xFF   NB_TSD = 0x90   NB_SUBID_TSD = 0xFF-2 = 0xFD
//   OPEN         = _IOW(0xFF, 0x90+0,  OpenParam[20])
//   GET_STATUS   = _IOR(0xFF, 0x90+3,  Status)
//   SET_INPUT    = _IOW(0xFF, 0x90+4,  InputParam[20]  == OpenParam)
//   SET_PID_FLT  = _IOW(0xFF, 0x90+5,  PidParam[20])
//   SET_READSIZE = _IOW(0xFF, 0x90+10, uint32)
//   GET_READINFO = _IOR(0xFF, 0x90+12, ReadInfo[24])
//   GET_LEVEL    = _IOR(0xFF, 0x90+14, uint32)
//   RESET_BUFFER = _IOW(0xFF, 0x90+21, uint32)
//   OPEN_SUBID   = _IOW(0xFF, 0xFD,    OpenSubId[8])
//   common: OPEN=_IO(0xFF,0) START=_IO(0xFF,1) STOP=_IO(0xFF,2) CLOSE_SUBID=_IO(0xFF,6)
//
// Read model (GPL): after START, poll GET_READINFO -> {fillSize, readOffset}.
// The demux ring is mmap()'d on the sub fd; read fillSize bytes starting at
// mmap[readOffset] (clamped to the linear tail to handle wrap), write them out,
// then SET_READSIZE(consumed) to advance the driver read pointer.
//
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <signal.h>


#define FAPEX_DEV_TYPE 0xFF
#define NB_TSD         0x90
#define NB_SUBID_TSD   (0xFF - 2)   /* 0xFD */

/* common commands */
#define TSD_START       _IO (FAPEX_DEV_TYPE, 0x01)
#define TSD_STOP        _IO (FAPEX_DEV_TYPE, 0x02)
#define TSD_CLOSE_SUBID _IO (FAPEX_DEV_TYPE, 0x06)

typedef struct {          /* FAPEX_TSD_OpenParamStrT / InputParamStrT (20B) */
	uint32_t useStc;
	uint32_t tsSelect;        /* CH_A=0 CH_B=1 CH_C=2 LINEAR_AUTO=0x29 */
	uint32_t syncByteDistance;/* 0=default, else 188/204 */
	uint32_t iflags;          /* SP mode + AM_SYNC etc. */
	uint32_t oflags;
} tsd_open_t;

typedef struct {          /* FAPEX_TSD_OpenSubIdParamStrT (8B) */
	int32_t sub_id;           /* in: desired minor; out: descriptor */
	int32_t channelNo;        /* 0..31 */
} tsd_subid_t;

typedef struct {          /* FAPEX_TSD_PidParamStrT (20B) */
	uint32_t pid;
	uint32_t flags;
	uint32_t bufferSize;
	uint32_t bufferThresholdH;
	uint32_t bufferThresholdL;
} tsd_pid_t;

typedef struct {          /* FAPEX_TSD_ReadInfoStrT (24B) */
	uint32_t fillSize;
	uint32_t readOffset;
	uint32_t sectionOffset;
	uint32_t sectionLength;
	int32_t  sectionNo;
	uint32_t bufferStatus;
} tsd_readinfo_t;

#define TSD_OPEN         _IOW(FAPEX_DEV_TYPE, NB_TSD + 0,  tsd_open_t)
#define TSD_SET_INPUT    _IOW(FAPEX_DEV_TYPE, NB_TSD + 4,  tsd_open_t)
#define TSD_SET_PID_FLT  _IOW(FAPEX_DEV_TYPE, NB_TSD + 5,  tsd_pid_t)
#define TSD_SET_READSIZE _IOW(FAPEX_DEV_TYPE, NB_TSD + 10, uint32_t)
#define TSD_GET_READINFO _IOR(FAPEX_DEV_TYPE, NB_TSD + 12, tsd_readinfo_t)
#define TSD_GET_LEVEL    _IOR(FAPEX_DEV_TYPE, NB_TSD + 14, uint32_t)
#define TSD_RESET_BUFFER _IOW(FAPEX_DEV_TYPE, NB_TSD + 21, uint32_t)
#define TSD_OPEN_SUBID   _IOW(FAPEX_DEV_TYPE, NB_SUBID_TSD, tsd_subid_t)

/* tsSelect */
#define IN_CH_A       0x00
#define IN_CH_B       0x01
#define IN_CH_C       0x02
#define IN_LINEAR_AUTO 0x29
/* iflags */
#define IF_AM_SYNC    0x0020
#define IF_PACKET_ST  0x0010
#define IF_EN         0x0008
#define IF_INV_CLK    0x0004
#define IF_SP_MODE1   0x0001  /* serial   with    packet_start */
#define IF_SP_MODE2   0x0002  /* parallel without packet_start */
#define IF_SP_MODE3   0x0003  /* parallel with    packet_start */
/* PID filter flags */
#define FLT_RM_TS_HEADER 0x00001000
#define FLT_SWAPRD_SWAP  0x00000002

/* --- options --- */
/* Defaults are the values the vendor daemon actually passes, captured with
 * fapex-trace while the demodulator was locked on a real channel and TS was
 * flowing (docs/specs/sb-tv02-wfpl-stage-b-fapex.md §5.6, "ベンダ daemon の TSD
 * 設定").  They replace the reference-board guesses this tool started with --
 * notably iflags, which was 3 (parallel+PS) here but is 1 (serial+PS) on this
 * board, and flags, where the vendor sets a bit (0x04) that the GPL headers do
 * not name. */
static uint32_t g_tsSelect = IN_CH_A;       /* observed: tsSelect=0 */
static uint32_t g_iflags   = IF_SP_MODE1;   /* observed: iflags=1 (serial+PS) */
static uint32_t g_sync     = 0;             /* observed: syncByteDistance=0 */
static int32_t  g_chan     = 0;             /* observed: channelNo=0 */
static int32_t  g_subid    = -1;            /* default: parse from SUBDEV path
                                             * (observed: sub_id == minor) */
static uint32_t g_pid      = 0x0000;        /* observed: pid=0 (PAT) */
static uint32_t g_flags    = 0x00000004;    /* observed: flags=0x04 (unnamed in
                                             * the GPL headers; RM_TS_HEADER is
                                             * NOT set, so packets stay 188B) */
static uint32_t g_bufkb    = 3760;          /* observed: bufferSize=0x3ac000 */
static uint32_t g_thh      = 0xb798;        /* observed: thH for the CH_A channel.
                                             * Must be well below bufferSize; the
                                             * old code used the whole buffer. */
static int      g_setinput = 0;             /* the vendor issues OPEN only, never
                                             * SET_INPUT.  Opt in to compare. */
static int      g_stage_tmo = 10;           /* per-ioctl watchdog, seconds */
static int      g_secs     = 0;             /* 0 = default per cmd */
static long long g_bytes   = 0;             /* 0 = no limit */
static const char *g_out   = NULL;

static long now_ms(void)
{
	struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)(ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
}

/* --- stage log + per-call watchdog -----------------------------------------
 * On 2026-09-20 a scan run produced nothing but its banner and never returned,
 * and there was no way to tell which ioctl it was sitting in.  Two fixes:
 * every step announces itself BEFORE it runs (and the line is flushed), and a
 * SIGALRM without SA_RESTART breaks a blocked call so the tool reports instead
 * of hanging.  stdout/stderr are line buffered from main(), so redirecting to a
 * file keeps the progress visible. */
static volatile sig_atomic_t g_alarmed;

static void on_alarm(int sig) { (void)sig; g_alarmed = 1; }

static void stage(const char *what)
{
	fprintf(stderr, "[stage] %s\n", what);
	fflush(stderr);
}

static void wd_arm(void)
{
	static int installed;

	if (!installed) {
		struct sigaction sa;

		memset(&sa, 0, sizeof sa);
		sa.sa_handler = on_alarm;	/* deliberately no SA_RESTART */
		sigaction(SIGALRM, &sa, NULL);
		installed = 1;
	}
	g_alarmed = 0;
	if (g_stage_tmo > 0)
		alarm((unsigned)g_stage_tmo);
}

static void wd_off(void) { alarm(0); }

static void wd_report(const char *what)
{
	if (g_alarmed) {
		fprintf(stderr, "[stage] %s TIMED OUT after %ds -- the driver is "
			"blocking here\n", what, g_stage_tmo);
		fflush(stderr);
	}
}

static int xopen(const char *path, int flags, const char *what)
{
	int fd;

	stage(what);
	wd_arm();
	fd = open(path, flags);
	wd_off();
	wd_report(what);
	if (fd < 0) {
		fprintf(stderr, "open(%s): %s\n", path, strerror(errno));
		fflush(stderr);
	}
	return fd;
}

/* FAPex ioctls report their own status in the return value, so the caller
 * decides what non-zero means; this only adds the log and the watchdog. */
static int xioctl(int fd, unsigned long req, void *arg, const char *what)
{
	int r;

	stage(what);
	wd_arm();
	r = ioctl(fd, req, arg);
	wd_off();
	wd_report(what);
	return r;
}

/* NB: the ioctls whose argument is passed BY VALUE despite the _IOW encoding
 * (SET_BUFFER_READSIZE, §5.6) are called directly in the drain loop -- logging
 * every iteration there would drown the stage log. */

/* Parse trailing integer of "/dev/fapi/fapi004" -> 4 */
static int minor_from_path(const char *p)
{
	const char *q = p + strlen(p);
	while (q > p && (q[-1] >= '0' && q[-1] <= '9')) q--;
	return (*q) ? atoi(q) : -1;
}

/* Open TSD block-common node, TSD_OPEN + SET_INPUT with current input config. */
static int tsd_open_block(const char *dev)
{
	int fd = xopen(dev, O_RDWR | O_SYNC, "open TSD common node");
	if (fd < 0) return -1;
	tsd_open_t op = { 0 /*useStc*/, g_tsSelect, g_sync, g_iflags, 0 };
	if (xioctl(fd, TSD_OPEN, &op, "TSD_OPEN") != 0) {
		fprintf(stderr, "TSD_OPEN(sel=%u iflags=0x%x sync=%u): %s\n",
			g_tsSelect, g_iflags, g_sync, strerror(errno));
		close(fd); return -1;
	}
	/* The vendor daemon issues OPEN only -- SET_INPUT never appears in its
	 * trace -- so this is off by default (§5.6).  --setinput turns it back on
	 * for comparison. */
	if (g_setinput && xioctl(fd, TSD_SET_INPUT, &op, "TSD_SET_INPUT") != 0) {
		fprintf(stderr, "TSD_SET_INPUT: %s\n", strerror(errno));
		close(fd); return -1;
	}
	return fd;
}

/* Open a demux channel on the block: open sub node, OPEN_SUBID, SET_PID_FILTER,
 * mmap the ring.  Returns sub fd and fills the map pointer and length. */
static int tsd_open_channel(const char *subdev, uint8_t **map, size_t *maplen)
{
	int sub_minor = (g_subid >= 0) ? g_subid : minor_from_path(subdev);
	int fd = xopen(subdev, O_RDWR | O_SYNC, "open TSD channel node");
	if (fd < 0) return -1;

	tsd_subid_t si = { sub_minor, g_chan };
	if (xioctl(fd, TSD_OPEN_SUBID, &si, "TSD_OPEN_SUBID") != 0) {
		fprintf(stderr, "TSD_OPEN_SUBID(sub=%d ch=%d): %s\n",
			sub_minor, g_chan, strerror(errno));
		close(fd); return -1;
	}

	/* thH is a fill threshold, not the buffer size: the vendor uses 0xb798
	 * against a 0x3ac000 buffer (§5.6).  Setting it to the whole buffer, as
	 * this tool used to, asks the driver to hold everything back. */
	uint32_t bufsz = g_bufkb * 1024;
	uint32_t thh = g_thh < bufsz ? g_thh : bufsz / 8;
	tsd_pid_t pf = { g_pid, g_flags, bufsz, thh, 0 };
	if (xioctl(fd, TSD_SET_PID_FLT, &pf, "TSD_SET_PID_FILTER") != 0) {
		fprintf(stderr, "TSD_SET_PID_FILTER(pid=0x%x flags=0x%x buf=%u thH=%u): %s\n",
			g_pid, g_flags, bufsz, thh, strerror(errno));
		close(fd); return -1;
	}

	stage("mmap ring");
	uint8_t *m = mmap(NULL, bufsz, PROT_READ, MAP_SHARED, fd, 0);
	if (m == MAP_FAILED) {
		fprintf(stderr, "mmap(%u): %s\n", bufsz, strerror(errno));
		close(fd); return -1;
	}
	*map = m; *maplen = bufsz;
	return fd;
}

/* Drain the ring for up to secs seconds (0 = until byte limit / forever),
 * writing raw bytes to outfd (if >=0).  Returns bytes drained. */
static long long drain(int fd, uint8_t *map, size_t maplen, int outfd,
                       int secs, long long bytelimit,
                       long long *out_bytes, unsigned *out_pkts, int *out_locked)
{
	long long total = 0;
	unsigned pkts = 0; int locked = 0;
	long t0 = now_ms();
	long tdiag = t0;
	uint8_t carry = 0; /* previous last byte, for sync check across reads */
	(void)carry;

	for (;;) {
		if (secs > 0 && now_ms() - t0 >= (long)secs * 1000) break;
		if (bytelimit > 0 && total >= bytelimit) break;

		/* FAPex ioctls return their own status as the ioctl() return value
		 * (not 0/-1+errno).  An empty ring reports a non-zero "no data" code,
		 * so do NOT treat non-zero as fatal: rely on fillSize (memset to 0
		 * beforehand, so a failed copy_to_user simply reads as empty). */
		tsd_readinfo_t ri;
		memset(&ri, 0, sizeof ri);
		ioctl(fd, TSD_GET_READINFO, &ri);

		/* Once a second, say what the driver reports even when nothing is
		 * arriving.  A bare "0 bytes" cannot distinguish "no TS on this input"
		 * from "TS arrives but no packet matches the PID filter"; bufferStatus
		 * and the buffer level can.  The vendor was seen with
		 * bufferStatus=0x20 while TS was flowing (§5.6). */
		if (now_ms() - tdiag >= 1000) {
			uint32_t lvl = 0;
			tdiag = now_ms();
			ioctl(fd, TSD_GET_LEVEL, &lvl);
			fprintf(stderr, "[diag] fill=%u roff=0x%x status=0x%x level=%u "
				"secOff=%u secLen=%u secNo=%d\n",
				ri.fillSize, ri.readOffset, ri.bufferStatus, lvl,
				ri.sectionOffset, ri.sectionLength, ri.sectionNo);
		}
		if (ri.fillSize == 0) {
			struct timespec s = { 0, 20 * 1000000L }; /* 20ms */
			nanosleep(&s, NULL);
			continue;
		}
		uint32_t off = ri.readOffset % maplen;
		uint32_t n   = ri.fillSize;
		if (off + n > maplen) n = maplen - off;  /* linear tail; rest next round */

		/* sync analysis: count 0x47 at 188 spacing within this chunk */
		for (uint32_t i = 0; i + 188 <= n; i += 188) {
			if (map[off + i] == 0x47) { pkts++; if (pkts >= 3) locked = 1; }
			else { pkts = 0; }
		}
		if (outfd >= 0) {
			ssize_t w = write(outfd, map + off, n);
			if (w < 0) { fprintf(stderr, "write out: %s\n", strerror(errno)); break; }
		}
		total += n;
		/* BY VALUE, despite the _IOW encoding: that is how the vendor calls
		 * it (§5.6).  Passing a pointer here left the read pointer where it
		 * was, so the ring never drained. */
		if (ioctl(fd, TSD_SET_READSIZE, (unsigned long)n) != 0) {
			fprintf(stderr, "SET_READSIZE(%u): %s\n", n, strerror(errno));
			break;
		}
		if (ri.bufferStatus & 0x0080 /*OVERFLOW-ish*/)
			fprintf(stderr, "[warn] buffer status=0x%x\n", ri.bufferStatus);
	}
	if (out_bytes) *out_bytes = total;
	if (out_pkts)  *out_pkts  = pkts;
	if (out_locked)*out_locked= locked;
	return total;
}

/* Find the first 0x47/188 lock inside a captured buffer already in a file is
 * overkill here; drain() already tracks lock live. */

static int do_capture(const char *dev, const char *subdev)
{
	int bfd = tsd_open_block(dev);
	if (bfd < 0) return 1;
	uint8_t *map = NULL; size_t maplen = 0;
	int sfd = tsd_open_channel(subdev, &map, &maplen);
	if (sfd < 0) { close(bfd); return 1; }

	int outfd = 1; /* stdout */
	if (g_out) {
		outfd = open(g_out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (outfd < 0) { fprintf(stderr, "open(%s): %s\n", g_out, strerror(errno));
			munmap(map, maplen); close(sfd); close(bfd); return 1; }
	}
	if (xioctl(sfd, TSD_START, NULL, "TSD_START") != 0)
		fprintf(stderr, "START: %s (continuing)\n", strerror(errno));

	long long bytes = 0; unsigned pkts = 0; int locked = 0;
	drain(sfd, map, maplen, outfd, g_secs, g_bytes, &bytes, &pkts, &locked);

	ioctl(sfd, TSD_STOP, NULL);
	fprintf(stderr, "capture: %lld bytes, sync-lock=%s (input=%u iflags=0x%x pid=0x%x)\n",
		bytes, locked ? "YES" : "no", g_tsSelect, g_iflags, g_pid);
	if (g_out) close(outfd);
	munmap(map, maplen); ioctl(sfd, TSD_CLOSE_SUBID, NULL); close(sfd); close(bfd);
	return locked ? 0 : 2;
}

/* PROBE: try current input config for a few seconds, report sync lock. */
static int do_probe(const char *dev, const char *subdev)
{
	int bfd = tsd_open_block(dev);
	if (bfd < 0) return 1;
	uint8_t *map = NULL; size_t maplen = 0;
	int sfd = tsd_open_channel(subdev, &map, &maplen);
	if (sfd < 0) { close(bfd); return 1; }
	if (xioctl(sfd, TSD_START, NULL, "TSD_START") != 0)
		fprintf(stderr, "START: %s (continuing)\n", strerror(errno));

	int secs = g_secs > 0 ? g_secs : 5;
	long long bytes = 0; unsigned pkts = 0; int locked = 0;
	drain(sfd, map, maplen, -1, secs, 0, &bytes, &pkts, &locked);

	ioctl(sfd, TSD_STOP, NULL);
	printf("probe input=%u iflags=0x%x sync=%u pid=0x%x: %lld bytes in %ds, TS-sync=%s\n",
		g_tsSelect, g_iflags, g_sync, g_pid, bytes, secs, locked ? "LOCKED" : "none");
	munmap(map, maplen); ioctl(sfd, TSD_CLOSE_SUBID, NULL); close(sfd); close(bfd);
	return locked ? 0 : 2;
}

/* SCAN: sweep input CH_A/B/C x {serial, par, parps} (+AM_SYNC) to find which HW
 * input carries the demod TS.  Decides Path H.  Demod must already be tuned+locked. */
static int do_scan(const char *dev, const char *subdev)
{
	const uint32_t sels[]  = { IN_CH_A, IN_CH_B, IN_CH_C };
	const char    *seln[]  = { "A", "B", "C" };
	const uint32_t modes[] = { IF_SP_MODE1, IF_SP_MODE2, IF_SP_MODE3 };
	const char    *moden[] = { "serial", "par", "parps" };
	int hit = 0;
	int secs = g_secs > 0 ? g_secs : 2;
	printf("scan: sweeping HW TS input CH x mode (%ds each), pid=0x%x\n", secs, g_pid);
	for (int s = 0; s < 3; s++) {
		for (int m = 0; m < 3; m++) {
			for (int am = 0; am < 2; am++) {
				g_tsSelect = sels[s];
				g_iflags = modes[m] | (am ? IF_AM_SYNC : 0);
				int bfd = tsd_open_block(dev);
				if (bfd < 0) continue;
				uint8_t *map = NULL; size_t maplen = 0;
				int sfd = tsd_open_channel(subdev, &map, &maplen);
				if (sfd < 0) { close(bfd); continue; }
				xioctl(sfd, TSD_START, NULL, "TSD_START");
				long long bytes = 0; unsigned pkts = 0; int locked = 0;
				drain(sfd, map, maplen, -1, secs, 0, &bytes, &pkts, &locked);
				ioctl(sfd, TSD_STOP, NULL);
				printf("  CH_%s %-6s %-6s : %8lld B  %s\n",
					seln[s], moden[m], am ? "amsync" : "-",
					bytes, locked ? "<== TS-SYNC LOCKED" : "");
				if (locked) hit++;
				munmap(map, maplen); ioctl(sfd, TSD_CLOSE_SUBID, NULL);
				close(sfd); close(bfd);
			}
		}
	}
	printf("scan done: %d combo(s) locked. Locked combo => demod is wired to that HW CH (Path H).\n", hit);
	return hit ? 0 : 2;
}

static void usage(void)
{
	fprintf(stderr,
	  "usage: fapex-ts [opts] CMD TSDCOMMON TSDSUB\n"
	  "  CMD       = probe | capture | scan\n"
	  "  TSDCOMMON = /dev/fapi/fapi000  (TSD0 block-common node; block N = minor N)\n"
	  "  TSDSUB    = /dev/fapi/fapi004  (a TSD sub/channel node; TSD0 subs = 4..35)\n"
	  "opts:\n"
	  "  --input A|B|C        HW TS input channel (tsSelect); default A\n"
	  "  --linauto            packet-RAM linear-auto input (Path D); tsSelect=0x29\n"
	  "  --mode serial|par|parps   SP mode 1/2/3; default serial (observed value)\n"
	  "  --amsync             set AM_SYNC (auto sync)\n"
	  "  --invclk             invert TS input clock\n"
	  "  --sync N             syncByteDistance (0=default,188,204)\n"
	  "  --chan N             demux channelNo (default 0)\n"
	  "  --subid N            OPEN_SUBID minor (default: parse from TSDSUB path)\n"
	  "  --pid 0xN            PID filter (default 0x0000 = PAT, always clear)\n"
	  "  --rmhdr              remove TS header (default keep raw 188B packets)\n"
	  "  --swap               enable read byte-swap\n"
	  "  --bufkb N            ring buffer KB (default 3760 = 0x3ac000, observed)\n"
	  "  --thh N              fill threshold thH (default 0xb798, observed)\n"
	  "  --fltflags 0xN       raw PID-filter flags (default 0x04, observed)\n"
	  "  --setinput           also issue SET_INPUT (the vendor issues OPEN only)\n"
	  "  --stage-timeout N    per-ioctl watchdog seconds, 0=off (default 10)\n"
	  "  --secs N             duration (probe 5, scan 2 per combo, capture until limit)\n"
	  "  --bytes N            capture byte limit (0 = unlimited)\n"
	  "  -o FILE              capture output file (default stdout)\n"
	  "notes:\n"
	  "  Defaults match what the vendor daemon was observed to pass while a real\n"
	  "  channel was locked and TS was flowing (spec §5.6), so 'probe' with no\n"
	  "  options is the first thing to try.  'scan' sweeps CH_A/B/C x\n"
	  "  {serial,par,parps}(+amsync) and is only the fallback.\n"
	  "  The demod must be tuned and locked first.  On the observation rig use the\n"
	  "  vendor factory shell: forceinit / dpower 1 1 / sfreq 1 <Hz>.  Use sfreq,\n"
	  "  NOT settuner: settuner also opens the Dione TS streamer, which then fights\n"
	  "  this tool for the TSD.\n"
	  "  Every step is announced on stderr as [stage] before it runs, so if a call\n"
	  "  blocks the last line names it.\n");
}

int main(int argc, char **argv)
{
	int i = 1;

	/* Line buffered, so progress is visible when stdout is a file.  The 2026-09-20
	 * run looked like a hang partly because the per-combination lines were still
	 * sitting in the stdio buffer. */
	setvbuf(stdout, NULL, _IOLBF, 0);
	setvbuf(stderr, NULL, _IOLBF, 0);

	for (; i < argc; i++) {
		if (!strcmp(argv[i], "--input") && i + 1 < argc) {
			char c = argv[++i][0];
			g_tsSelect = (c == 'B' || c == 'b') ? IN_CH_B :
			             (c == 'C' || c == 'c') ? IN_CH_C : IN_CH_A;
		} else if (!strcmp(argv[i], "--linauto")) {
			g_tsSelect = IN_LINEAR_AUTO;
		} else if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
			const char *m = argv[++i];
			g_iflags = (g_iflags & ~0x3u) |
			           (!strcmp(m, "serial") ? IF_SP_MODE1 :
			            !strcmp(m, "par")    ? IF_SP_MODE2 : IF_SP_MODE3);
		} else if (!strcmp(argv[i], "--amsync")) {
			g_iflags |= IF_AM_SYNC;
		} else if (!strcmp(argv[i], "--invclk")) {
			g_iflags |= IF_INV_CLK;
		} else if (!strcmp(argv[i], "--sync") && i + 1 < argc) {
			g_sync = (uint32_t)strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "--chan") && i + 1 < argc) {
			g_chan = (int32_t)strtol(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "--subid") && i + 1 < argc) {
			g_subid = (int32_t)strtol(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "--pid") && i + 1 < argc) {
			g_pid = (uint32_t)strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "--rmhdr")) {
			g_flags |= FLT_RM_TS_HEADER;
		} else if (!strcmp(argv[i], "--swap")) {
			g_flags |= FLT_SWAPRD_SWAP;
		} else if (!strcmp(argv[i], "--bufkb") && i + 1 < argc) {
			g_bufkb = (uint32_t)strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "--thh") && i + 1 < argc) {
			g_thh = (uint32_t)strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "--fltflags") && i + 1 < argc) {
			g_flags = (uint32_t)strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "--setinput")) {
			g_setinput = 1;
		} else if (!strcmp(argv[i], "--stage-timeout") && i + 1 < argc) {
			g_stage_tmo = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--secs") && i + 1 < argc) {
			g_secs = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--bytes") && i + 1 < argc) {
			g_bytes = strtoll(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "-o") && i + 1 < argc) {
			g_out = argv[++i];
		} else break;
	}
	fprintf(stderr, "fapex-ts: OPEN=0x%lx SET_INPUT=0x%lx SET_PID=0x%lx "
		"OPEN_SUBID=0x%lx GET_READINFO=0x%lx SET_READSIZE=0x%lx\n",
		(unsigned long)TSD_OPEN, (unsigned long)TSD_SET_INPUT,
		(unsigned long)TSD_SET_PID_FLT, (unsigned long)TSD_OPEN_SUBID,
		(unsigned long)TSD_GET_READINFO, (unsigned long)TSD_SET_READSIZE);

	if (argc - i < 3) { usage(); return 2; }
	const char *cmd    = argv[i];
	const char *dev    = argv[i + 1];
	const char *subdev = argv[i + 2];

	if (!strcmp(cmd, "probe"))   return do_probe(dev, subdev);
	if (!strcmp(cmd, "capture")) return do_capture(dev, subdev);
	if (!strcmp(cmd, "scan"))    return do_scan(dev, subdev);
	usage();
	return 2;
}
