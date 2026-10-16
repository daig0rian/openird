// SPDX-License-Identifier: GPL-2.0-only
/*
 * ird-rec - tune an OpenIRD tuner and read its transport stream.
 *
 * A small client of the px4_drv-compatible character device, for checking a
 * tuner on the device itself: it does what recpt1 does (set the channel,
 * start streaming, read), writes the stream to a file or throws it away, and
 * reports what it saw -- packets, PIDs, continuity errors, how many packets
 * are scrambled.  The stream is written as it comes; nothing is descrambled.
 *
 *   ird-rec [-d <device>] [-p] <channel> <seconds> [<outfile>|-]
 *
 *   <channel>  UHF physical channel, 13..62; or, as recpt1 names them,
 *              BS<1..23 odd>_<stream 0..7> (BS15_0) or CS<2..24 even> (CS2)
 *   <seconds>  how long to read (0: tune and report the signal only)
 *   <outfile>  where the stream goes; "-" is stdout; none: thrown away
 *   -p         list every PID seen
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#include <sys/ioctl.h>

#include "ird_ptx_ioctl.h"

#define TS	188

static unsigned long count[8192], cc_err[8192], scrambled[8192];
static signed char last_cc[8192];

/* what recpt1 makes of PTX_GET_CNR for ISDB-T */
static double cnr_db(int raw)
{
	double p;

	if (raw <= 0)
		return 0;
	p = log10(5505024 / (double)raw) * 10;
	return 0.000024 * p * p * p * p - 0.0016 * p * p * p + 0.0398 * p * p +
	       0.5491 * p + 3.0965;
}

/* ISDB-S: the TC90522's register 0xbc, by the mainline Linux tc90522.c */
static double cnr_db_s(int raw)
{
	double p;

	if (raw <= 3000)
		return 0;
	p = sqrt(raw - 3000) / 64;
	return -1.6346 * p * p * p * p * p + 14.341 * p * p * p * p -
	       50.259 * p * p * p + 88.977 * p * p - 89.565 * p + 58.857;
}

/* <channel> -> the px4_drv numbering (ird_ptx_ioctl.h); 1 = satellite */
static int parse_channel(const char *s, struct ptx_freq *f)
{
	char *end;
	long n;

	f->slot = 0;
	if (!strncmp(s, "BS", 2)) {
		n = strtol(s + 2, &end, 10);
		if (n < 1 || n > 23 || !(n & 1))
			return -1;
		if (*end == '_')
			f->slot = strtol(end + 1, &end, 10);
		if (*end || f->slot < 0 || f->slot > 7)
			return -1;
		f->freq_no = (n - 1) / 2;
		return 1;
	}
	if (!strncmp(s, "CS", 2)) {
		n = strtol(s + 2, &end, 10);
		if (*end || n < 2 || n > 24 || (n & 1))
			return -1;
		f->freq_no = 12 + (n - 2) / 2;
		return 1;
	}
	n = strtol(s, &end, 10);
	if (*end || n < 13 || n > 62)
		return -1;
	/* UHF 13..62 is freq_no 63..112 (the PT1 numbering px4_drv kept) */
	f->freq_no = n + 50;
	return 0;
}

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/irdvideo0", *out = NULL;
	static unsigned char buf[TS * 256];
	unsigned long total = 0, bad_sync = 0, cc_total = 0, scr_total = 0, npid = 0;
	int list = 0, fd, ofd = -1, sat, secs, raw = 0, opt, have = 0, i;
	const char *ch;
	struct ptx_freq freq;
	double t0, t1;

	while ((opt = getopt(argc, argv, "d:p")) != -1) {
		if (opt == 'd')
			dev = optarg;
		else if (opt == 'p')
			list = 1;
		else
			goto usage;
	}
	if (argc - optind < 2)
		goto usage;
	ch = argv[optind];
	secs = atoi(argv[optind + 1]);
	if (argc - optind > 2)
		out = argv[optind + 2];
	sat = parse_channel(ch, &freq);
	if (sat < 0)
		goto usage;

	fd = open(dev, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "ird-rec: %s: %s\n", dev, strerror(errno));
		return 1;
	}
	t0 = now();
	if (ioctl(fd, PTX_SET_CHANNEL, &freq) < 0) {
		fprintf(stderr, "ird-rec: channel %s: %s\n", ch,
			errno == EAGAIN ? "no lock" : strerror(errno));
		return 1;
	}
	t1 = now();
	if (ioctl(fd, PTX_GET_CNR, &raw) < 0)
		fprintf(stderr, "ird-rec: PTX_GET_CNR: %s\n", strerror(errno));
	fprintf(stderr, "ird-rec: channel %s locked in %.2f s, C/N %.1f dB\n",
		ch, t1 - t0, sat ? cnr_db_s(raw) : cnr_db(raw));
	if (secs <= 0)
		return 0;

	if (out && !strcmp(out, "-")) {
		ofd = 1;
	} else if (out) {
		ofd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (ofd < 0) {
			fprintf(stderr, "ird-rec: %s: %s\n", out, strerror(errno));
			return 1;
		}
	}
	memset(last_cc, -1, sizeof(last_cc));
	if (ioctl(fd, PTX_START_STREAMING) < 0) {
		fprintf(stderr, "ird-rec: PTX_START_STREAMING: %s\n", strerror(errno));
		return 1;
	}
	t0 = now();
	while (now() - t0 < secs) {
		int n = read(fd, buf + have, sizeof(buf) - have), whole;

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			fprintf(stderr, "ird-rec: read: %s\n",
				n ? strerror(errno) : "end of stream");
			break;
		}
		if (ofd >= 0 && write(ofd, buf + have, n) != n) {
			fprintf(stderr, "ird-rec: write: %s\n", strerror(errno));
			break;
		}
		have += n;
		whole = have / TS * TS;
		for (i = 0; i < whole; i += TS) {
			const unsigned char *p = buf + i;
			unsigned int pid = ((p[1] & 0x1f) << 8) | p[2];
			int cc = p[3] & 15;

			total++;
			if (p[0] != 0x47) {
				bad_sync++;
				continue;
			}
			count[pid]++;
			if (p[3] & 0xc0)
				scrambled[pid]++;
			/* the counter steps on packets that carry payload; a
			 * repeat of the same value is a legal duplicate */
			if ((p[3] & 0x10) && pid != 0x1fff) {
				if (last_cc[pid] >= 0 && cc != last_cc[pid] &&
				    cc != ((last_cc[pid] + 1) & 15))
					cc_err[pid]++;
				last_cc[pid] = cc;
			}
		}
		have -= whole;
		memmove(buf, buf + whole, have);
	}
	t1 = now();
	ioctl(fd, PTX_STOP_STREAMING);
	if (ioctl(fd, PTX_GET_CNR, &raw) == 0)
		fprintf(stderr, "ird-rec: C/N %.1f dB at the end\n",
			sat ? cnr_db_s(raw) : cnr_db(raw));
	close(fd);

	for (i = 0; i < 8192; i++) {
		if (!count[i])
			continue;
		npid++;
		cc_total += cc_err[i];
		scr_total += scrambled[i];
		if (list)
			fprintf(stderr, "  pid 0x%04x %8lu packets%s%s\n", i, count[i],
				scrambled[i] ? "  scrambled" : "",
				cc_err[i] ? "  CONTINUITY ERRORS" : "");
	}
	fprintf(stderr, "ird-rec: %lu packets in %.1f s (%.0f kbit/s), %lu PIDs, "
		"%lu scrambled, %lu continuity errors, %lu sync errors\n",
		total, t1 - t0, total * TS * 8 / (t1 - t0) / 1000, npid,
		scr_total, cc_total, bad_sync);
	return cc_total || bad_sync ? 3 : 0;
usage:
	fprintf(stderr, "usage: ird-rec [-d <device>] [-p] <channel> <seconds> "
		"[<outfile>|-]\n"
		"  <channel>: 13..62 (UHF), BS1_0..BS23_7, CS2..CS24\n");
	return 2;
}
