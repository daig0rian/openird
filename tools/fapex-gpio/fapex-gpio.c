// SPDX-License-Identifier: GPL-2.0-only
//
// fapex-gpio: stage-B bring-up helper for the FAPex GPIO character device.
//
// Milestone 1 observation tool.  It talks to the vendor FAPex GPIO chardev
// (major 244; the GPIO minor is fapi186 on SB-TV02-WFPL) using the *published*
// FAPEX ioctl ABI documented in the GPL headers abst_libgpio.h / abst_lib.h.
// It contains NO vendor code -- it is a clean user of the chardev contract,
// the same contract the vendor userland uses.  We use it to observe and bring
// the board up by hand (e.g. power the Dione H58 by driving its supply GPIOs)
// without running the vendor daemon, whose board init races our USB stack.
//
// ABI (GPL abst_lib.h / abst_libgpio.h):
//   FAPEX_DEV_TYPE          = 0xFF          ioctl type
//   FAPEX_IOCCOM_NB_GPIO    = 0x40          ioctl nr base for GPIO
//   OPEN     = _IOW(0xFF, 0x40, {u32 pin; u32 function})   -> 0x4008FF40
//   READBIT  = _IOR(0xFF, 0x41, int32_t)                   -> 0x8004FF41
//   WRITEBIT = _IOW(0xFF, 0x41, int32_t)                   -> 0x4004FF41
//   function: FAPEX_GPIO_OUT_0 = 0x0100 (output, drive 0)
//             FAPEX_GPIO_OUT_1 = 0x0101 (output, drive 1)
//             FAPEX_GPIO_IN_GENERIC_SW_FUNCTION = (OUT_1<<16)+0 = 0x01010000
//
// Semantics confirmed from fapex_fdlgpio_main.c:
//   OPEN binds one pin to the open file's context; a 2nd OPEN on the same fd is
//   a no-op.  Closing the fd frees the context (releases the pin).  So one fd
//   per pin, and hold the fd open to keep a rail asserted.  READBIT writes the
//   level (0/1) back through the int32 arg; WRITEBIT drives the int32 arg.
//
// Usage:
//   fapex-gpio DEV read  PIN...                 open each PIN as input, read, print
//   fapex-gpio DEV set   PIN LVL                open PIN as output LVL(0|1); exit
//                                               (closes fd -> releases pin)
//   fapex-gpio DEV write PIN LVL                open PIN out, then WRITEBIT LVL; exit
//   fapex-gpio DEV hold  [--delay-ms N] PIN:LVL...
//                                               open each PIN as output LVL in the
//                                               given order (sleeping N ms between,
//                                               default 0), print each, then block
//                                               holding every fd until SIGINT/SIGTERM
//                                               (on exit the fds close and the pins
//                                               are released).  Use this to keep a
//                                               power rail on while observing.
//
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/ioctl.h>

#define FAPEX_DEV_TYPE 0xFF
#define NB_GPIO        0x40

typedef struct { uint32_t pin; uint32_t function; } gpio_open_t;

#define GPIO_OPEN      _IOW(FAPEX_DEV_TYPE, NB_GPIO + 0, gpio_open_t)  /* 0x4008FF40 */
#define GPIO_READBIT   _IOR(FAPEX_DEV_TYPE, NB_GPIO + 1, int32_t)      /* 0x8004FF41 */
#define GPIO_WRITEBIT  _IOW(FAPEX_DEV_TYPE, NB_GPIO + 1, int32_t)      /* 0x4004FF41 */

#define FN_OUT0   0x0100u
#define FN_OUT1   0x0101u
#define FN_IN_SW  0x01010000u

#define MAX_HOLD  32

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

// open the chardev and bind `pin` to `fn`.  returns fd (>=0) or -1.
static int pin_open(const char *dev, uint32_t pin, uint32_t fn)
{
	int fd = open(dev, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open(%s): %s\n", dev, strerror(errno));
		return -1;
	}
	gpio_open_t p = { pin, fn };
	if (ioctl(fd, GPIO_OPEN, &p) != 0) {
		fprintf(stderr, "GPIO_OPEN pin=%u fn=0x%x: %s\n", pin, fn, strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

static int cmd_read(const char *dev, int argc, char **argv)
{
	int rc = 0;
	for (int i = 0; i < argc; i++) {
		uint32_t pin = (uint32_t)strtoul(argv[i], NULL, 0);
		int fd = pin_open(dev, pin, FN_IN_SW);
		if (fd < 0) { rc = 1; continue; }
		int32_t bit = -1;
		if (ioctl(fd, GPIO_READBIT, &bit) != 0) {
			fprintf(stderr, "READBIT pin=%u: %s\n", pin, strerror(errno));
			rc = 1;
		} else {
			printf("pin %u = %d\n", pin, bit);
		}
		close(fd);
	}
	return rc;
}

static int cmd_set(const char *dev, uint32_t pin, int lvl)
{
	int fd = pin_open(dev, pin, lvl ? FN_OUT1 : FN_OUT0);
	if (fd < 0) return 1;
	printf("pin %u set out %d (fd closes on exit -> pin released)\n", pin, lvl);
	close(fd);
	return 0;
}

static int cmd_write(const char *dev, uint32_t pin, int lvl)
{
	int fd = pin_open(dev, pin, lvl ? FN_OUT1 : FN_OUT0);
	if (fd < 0) return 1;
	int32_t bit = lvl ? 1 : 0;
	if (ioctl(fd, GPIO_WRITEBIT, &bit) != 0) {
		fprintf(stderr, "WRITEBIT pin=%u: %s\n", pin, strerror(errno));
		close(fd);
		return 1;
	}
	printf("pin %u write %d\n", pin, lvl);
	close(fd);
	return 0;
}

static int cmd_hold(const char *dev, int argc, char **argv)
{
	long delay_ms = 0;
	int fds[MAX_HOLD];
	int n = 0;

	for (int i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "--delay-ms") && i + 1 < argc) {
			delay_ms = strtol(argv[++i], NULL, 0);
			continue;
		}
		char *colon = strchr(argv[i], ':');
		if (!colon) { fprintf(stderr, "bad PIN:LVL '%s'\n", argv[i]); return 1; }
		*colon = 0;
		uint32_t pin = (uint32_t)strtoul(argv[i], NULL, 0);
		int lvl = atoi(colon + 1);
		if (n >= MAX_HOLD) { fprintf(stderr, "too many pins\n"); break; }
		if (n > 0 && delay_ms > 0)
			usleep((useconds_t)delay_ms * 1000);
		int fd = pin_open(dev, pin, lvl ? FN_OUT1 : FN_OUT0);
		if (fd < 0) {
			fprintf(stderr, "hold: aborting, releasing %d pin(s)\n", n);
			for (int k = 0; k < n; k++) close(fds[k]);
			return 1;
		}
		fds[n++] = fd;
		printf("held pin %u out %d\n", pin, lvl);
		fflush(stdout);
	}
	printf("holding %d pin(s); pid %d; SIGTERM/SIGINT to release\n", n, (int)getpid());
	fflush(stdout);

	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	while (!g_stop) pause();

	printf("releasing %d pin(s)\n", n);
	for (int k = 0; k < n; k++) close(fds[k]);
	return 0;
}

// pulse: drive PIN to !LVL for LOW_MS, then to LVL, then hold until signalled.
// Used to verify the output path (e.g. a reset line) with an observable effect
// while guaranteeing the pin ends at the safe level LVL (fd stays open).
static int cmd_pulse(const char *dev, uint32_t pin, int low_ms, int end_lvl)
{
	int fd = pin_open(dev, pin, end_lvl ? FN_OUT1 : FN_OUT0);
	if (fd < 0) return 1;
	int32_t lo = end_lvl ? 0 : 1, hi = end_lvl ? 1 : 0;
	if (ioctl(fd, GPIO_WRITEBIT, &lo) != 0)
		{ fprintf(stderr, "WRITEBIT lo: %s\n", strerror(errno)); close(fd); return 1; }
	printf("pin %u -> %d for %d ms\n", pin, (int)lo, low_ms); fflush(stdout);
	usleep((useconds_t)low_ms * 1000);
	if (ioctl(fd, GPIO_WRITEBIT, &hi) != 0)
		{ fprintf(stderr, "WRITEBIT hi: %s\n", strerror(errno)); close(fd); return 1; }
	printf("pin %u -> %d; holding (pid %d)\n", pin, (int)hi, (int)getpid());
	fflush(stdout);
	signal(SIGINT, on_sig); signal(SIGTERM, on_sig);
	while (!g_stop) pause();
	close(fd);
	return 0;
}

// seq: run a small power-up sequence and then hold every opened pin.
//   token PIN:LVL    open PIN as output LVL, keep the fd (held)
//   token w:PIN:LVL  WRITEBIT LVL on PIN's already-open fd (re-drive level)
//   token s:MS       sleep MS milliseconds
// Lets us assert a reset, ramp rails, then release the reset as a clean edge:
//   fapex-gpio DEV seq 74:0 77:1 78:1 81:1 75:1 41:1 42:1 s:300 w:74:1
static int cmd_seq(const char *dev, int argc, char **argv)
{
	uint32_t pins[MAX_HOLD];
	int fds[MAX_HOLD];
	int n = 0;

	for (int i = 0; i < argc; i++) {
		char *t = argv[i];
		if (t[0] == 's' && t[1] == ':') {
			usleep((useconds_t)strtol(t + 2, NULL, 0) * 1000);
			continue;
		}
		if (t[0] == 'w' && t[1] == ':') {
			uint32_t pin = (uint32_t)strtoul(t + 2, NULL, 0);
			char *c = strchr(t + 2, ':');
			if (!c) { fprintf(stderr, "bad w token '%s'\n", t); goto fail; }
			int lvl = atoi(c + 1);
			int fd = -1;
			for (int k = 0; k < n; k++) if (pins[k] == pin) fd = fds[k];
			if (fd < 0) { fprintf(stderr, "w:%u: pin not opened\n", pin); goto fail; }
			int32_t bit = lvl ? 1 : 0;
			if (ioctl(fd, GPIO_WRITEBIT, &bit) != 0) {
				fprintf(stderr, "WRITEBIT pin=%u: %s\n", pin, strerror(errno));
				goto fail;
			}
			printf("write pin %u %d\n", pin, lvl); fflush(stdout);
			continue;
		}
		// PIN:LVL open
		char *c = strchr(t, ':');
		if (!c) { fprintf(stderr, "bad token '%s'\n", t); goto fail; }
		*c = 0;
		uint32_t pin = (uint32_t)strtoul(t, NULL, 0);
		int lvl = atoi(c + 1);
		if (n >= MAX_HOLD) { fprintf(stderr, "too many pins\n"); goto fail; }
		int fd = pin_open(dev, pin, lvl ? FN_OUT1 : FN_OUT0);
		if (fd < 0) goto fail;
		pins[n] = pin; fds[n] = fd; n++;
		printf("open pin %u out %d\n", pin, lvl); fflush(stdout);
	}
	printf("holding %d pin(s); pid %d; SIGTERM/SIGINT to release\n", n, (int)getpid());
	fflush(stdout);
	signal(SIGINT, on_sig); signal(SIGTERM, on_sig);
	while (!g_stop) pause();
	for (int k = 0; k < n; k++) close(fds[k]);
	return 0;
fail:
	for (int k = 0; k < n; k++) close(fds[k]);
	return 1;
}

static void usage(void)
{
	fprintf(stderr,
	  "usage:\n"
	  "  fapex-gpio DEV read  PIN...\n"
	  "  fapex-gpio DEV set   PIN LVL\n"
	  "  fapex-gpio DEV write PIN LVL\n"
	  "  fapex-gpio DEV pulse PIN LOW_MS END_LVL\n"
	  "  fapex-gpio DEV hold  [--delay-ms N] PIN:LVL...\n"
	  "  fapex-gpio DEV seq   TOKEN...   (PIN:LVL | w:PIN:LVL | s:MS)\n");
}

int main(int argc, char **argv)
{
	fprintf(stderr, "fapex-gpio: OPEN=0x%lx READBIT=0x%lx WRITEBIT=0x%lx\n",
		(unsigned long)GPIO_OPEN, (unsigned long)GPIO_READBIT,
		(unsigned long)GPIO_WRITEBIT);
	if (argc < 3) { usage(); return 2; }
	const char *dev = argv[1];
	const char *cmd = argv[2];

	if (!strcmp(cmd, "read"))
		return cmd_read(dev, argc - 3, argv + 3);
	if (!strcmp(cmd, "set") && argc == 5)
		return cmd_set(dev, (uint32_t)strtoul(argv[3], NULL, 0), atoi(argv[4]));
	if (!strcmp(cmd, "write") && argc == 5)
		return cmd_write(dev, (uint32_t)strtoul(argv[3], NULL, 0), atoi(argv[4]));
	if (!strcmp(cmd, "pulse") && argc == 6)
		return cmd_pulse(dev, (uint32_t)strtoul(argv[3], NULL, 0), atoi(argv[4]), atoi(argv[5]));
	if (!strcmp(cmd, "hold"))
		return cmd_hold(dev, argc - 3, argv + 3);
	if (!strcmp(cmd, "seq"))
		return cmd_seq(dev, argc - 3, argv + 3);

	usage();
	return 2;
}
