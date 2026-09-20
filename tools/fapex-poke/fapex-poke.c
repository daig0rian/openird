// SPDX-License-Identifier: GPL-2.0-only
//
// fapex-poke: issue an arbitrary FAPex ioctl to any FAPex character device.
//
// The other tools in tools/ are each tied to one FAPex sub-device: fapex-i2c
// speaks I2C, fapex-gpio drives GPIOs, fapex-ts reads the TSD.  Bring-up needs
// more than those three.  In particular the vendor daemon's very first FAPex
// call is neither GPIO nor I2C: it is CLKPWR on /dev/fapi/fapi199 with
// nr=0x21 and an 8-byte argument (observed 06 00 00 00 00 00 00 00), and the
// demodulator does not answer on I2C until the Dione is fully up, which GPIO
// alone does not achieve (docs/specs/sb-tv02-wfpl-stage-b-fapex.md §5.5).
// This tool exists to test that kind of hypothesis without writing a new
// single-purpose program every time.
//
// It contains NO vendor code.  The only FAPex knowledge built in is the ioctl
// TYPE byte (0xFF) from the GPL headers abst_lib.h / abst_libi2c.h; everything
// else -- the command number, direction, size and payload -- comes from the
// command line, i.e. from the spec or from a bus observation the operator is
// reproducing.
//
// Two argument conventions are supported because the FAPex drivers use both:
// most commands take a pointer to a struct, but some encode _IOC_WRITE and a
// size while passing a small integer BY VALUE (SET_BUFFER_READSIZE is the
// documented example, §5.6).  --byval covers those.
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

#define FAPEX_DEV_TYPE 0xFF
#define MAX_ARG        512

static void usage(void)
{
	fprintf(stderr,
	  "usage: fapex-poke [opts] DEV NR DIR SIZE [BYTE...] [+ NR DIR SIZE [BYTE...]]...\n"
	  "  Groups separated by '+' run on the SAME open fd, in order: FAPex\n"
	  "  settings are tied to the handle (nr=0x21 on a fresh CLKPWR fd fails).\n"
	  "  DEV    /dev/fapi/fapiNNN\n"
	  "  NR     ioctl command number within type 0xFF (e.g. 0x21)\n"
	  "  DIR    -   no data      (_IO)\n"
	  "         w   write        (_IOW)\n"
	  "         r   read         (_IOR)\n"
	  "         rw  read+write   (_IOWR)\n"
	  "  SIZE   argument size in bytes (0 with DIR '-')\n"
	  "  BYTE.. input bytes for w/rw, hex or decimal; missing bytes are 0\n"
	  "opts:\n"
	  "  --byval        pass the argument by value, not by pointer\n"
	  "  --val N        with --byval, the integer to pass (default: first BYTE.. word)\n"
	  "  --repeat N     issue the ioctl N times (default 1)\n"
	  "  --delay-ms N   wait between repeats\n"
	  "  --open-rdonly  open the device O_RDONLY (default O_RDWR)\n"
	  "  --hold         keep the device open after the ioctl, until SIGINT/SIGTERM\n"
	  "                 (the vendor keeps CLKPWR open for the whole session; a\n"
	  "                  setting may not survive close())\n"
	  "  --quiet        only print the return value\n"
	  "examples:\n"
	  "  # the CLKPWR pair the vendor daemon issues on one fd (spec §5.5)\n"
	  "  fapex-poke --hold /dev/fapi/fapi199 0x00 - 0 + 0x21 w 8 0x06 0 0 0 0 0 0 0\n"
	  "  # read 4 bytes back from a status command\n"
	  "  fapex-poke /dev/fapi/fapi004 0x9e r 4\n");
}

static unsigned long ioc(unsigned dir, unsigned nr, unsigned size)
{
	/* _IOC layout is the asm-generic one this kernel uses; build the request
	 * here rather than with the _IOx macros so DIR and SIZE can be runtime
	 * values. */
	return ((unsigned long)dir << 30) | ((unsigned long)FAPEX_DEV_TYPE << 8) |
	       ((unsigned long)nr) | ((unsigned long)size << 16);
}

int main(int argc, char **argv)
{
	int i = 1, byval = 0, repeat = 1, delay_ms = 0, quiet = 0, oflags = O_RDWR;
	int hold = 0;
	unsigned long val = 0;
	int have_val = 0;

	setvbuf(stdout, NULL, _IOLBF, 0);
	setvbuf(stderr, NULL, _IOLBF, 0);

	for (; i < argc; i++) {
		if (!strcmp(argv[i], "--byval")) byval = 1;
		else if (!strcmp(argv[i], "--val") && i + 1 < argc) {
			val = strtoul(argv[++i], NULL, 0); have_val = 1;
		} else if (!strcmp(argv[i], "--repeat") && i + 1 < argc)
			repeat = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--delay-ms") && i + 1 < argc)
			delay_ms = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--open-rdonly")) oflags = O_RDONLY;
		else if (!strcmp(argv[i], "--hold")) hold = 1;
		else if (!strcmp(argv[i], "--quiet")) quiet = 1;
		else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			usage(); return 0;
		} else break;
	}
	if (argc - i < 4) { usage(); return 2; }

	const char *dev = argv[i++];
	int fd = open(dev, oflags);
	if (fd < 0) {
		fprintf(stderr, "open(%s): %s\n", dev, strerror(errno));
		return 1;
	}

	/* The remaining arguments are one or more command groups separated by "+".
	 * They all run on the SAME fd, in order, because FAPex settings are tied
	 * to the handle: the vendor daemon issues nr=0x00 and then nr=0x21 on one
	 * open CLKPWR fd, and nr=0x21 on a fresh fd fails (-6008). */
	int rc = 0;
	while (i < argc) {
		if (!strcmp(argv[i], "+")) { i++; continue; }
		if (argc - i < 3) { fprintf(stderr, "incomplete command group\n"); rc = 2; break; }

		unsigned nr    = (unsigned)strtoul(argv[i], NULL, 0);
		const char *ds = argv[i + 1];
		unsigned size  = (unsigned)strtoul(argv[i + 2], NULL, 0);
		i += 3;

		/* A trailing 'v' on DIR means "this group passes its argument BY
		 * VALUE" -- needed because one sequence can mix both conventions:
		 * CLKPWR takes nr=0x21 by pointer and nr=0x23 by value (§5.5). */
		int gbyval = byval;
		unsigned dir;
		if      (!strcmp(ds, "-"))   dir = 0;
		else if (!strcmp(ds, "w"))   dir = 1;
		else if (!strcmp(ds, "r"))   dir = 2;
		else if (!strcmp(ds, "rw"))  dir = 3;
		else if (!strcmp(ds, "wv"))  { dir = 1; gbyval = 1; }
		else if (!strcmp(ds, "rwv")) { dir = 3; gbyval = 1; }
		else { fprintf(stderr, "bad DIR '%s'\n", ds); rc = 2; break; }
		if (size > MAX_ARG) { fprintf(stderr, "SIZE too large\n"); rc = 2; break; }

		unsigned char buf[MAX_ARG];
		memset(buf, 0, sizeof buf);
		unsigned k = 0;
		while (i < argc && strcmp(argv[i], "+") != 0) {
			if (k < size) buf[k] = (unsigned char)strtoul(argv[i], NULL, 0);
			k++; i++;
		}
		unsigned long gval = val;
		if (gbyval && !have_val) {
			gval = 0;
			for (unsigned m = 0; m < 4 && m < size; m++)
				gval |= (unsigned long)buf[m] << (8 * m);
		}

		unsigned long req = ioc(dir, nr, size);
		if (!quiet)
			printf("fapex-poke: %s nr=0x%02x dir=%s size=%u req=0x%08lx%s\n",
			       dev, nr, ds, size, req, gbyval ? " (by value)" : "");

		for (int n = 0; n < repeat; n++) {
			long r;

			if (!quiet && (dir & 1) && size && !gbyval) {
				printf("  in :");
				for (unsigned m = 0; m < size; m++) printf(" %02x", buf[m]);
				printf("\n");
			}
			if (gbyval)
				r = ioctl(fd, req, gval);
			else if (size)
				r = ioctl(fd, req, buf);
			else
				r = ioctl(fd, req, 0);

			/* FAPex drivers report their own status in the return value, so
			 * print it as-is and do not treat non-zero as a fatal error. */
			printf("  ret=%ld", r);
			if (r < 0) printf(" errno=%d (%s)", errno, strerror(errno));
			printf("\n");
			if (!quiet && (dir & 2) && size && !gbyval) {
				printf("  out:");
				for (unsigned m = 0; m < size; m++) printf(" %02x", buf[m]);
				printf("\n");
			}
			if (r != 0) rc = 1;
			if (delay_ms > 0 && n + 1 < repeat) {
				struct timespec s = { delay_ms / 1000,
						      (long)(delay_ms % 1000) * 1000000L };
				nanosleep(&s, NULL);
			}
		}
	}
	if (hold) {
		/* Keep the fd open so a setting that lives only as long as the handle
		 * survives.  The vendor daemon keeps CLKPWR open for its whole run. */
		printf("holding %s open; pid %d; SIGTERM/SIGINT to release\n",
		       dev, (int)getpid());
		for (;;) {
			struct timespec s = { 3600, 0 };
			nanosleep(&s, NULL);
		}
	}
	close(fd);
	return rc;
}
