// SPDX-License-Identifier: GPL-2.0-only
// h57cmd - drive the Fujitsu MB86H58 "Dione" through its USB tunnel
// /dev/h57fmb0, for stage-B observation on SB-TV02-WFPL.
//
// Purpose (owner's device, research only):
//   - bring Dione up ourselves (load the owner's transcoder firmware from
//     /slot/firm and replay the captured init), without the vendor application,
//   - so we can check two things and ONLY these two things:
//       (1) whether Dione's pass-through TS output is left scrambled (= the raw
//           TS OpenIRD wants; descramble stays an upper-layer job with the card),
//       (2) whether the B-CAS card answers as a transparent APDU reader.
//
// It does NOT descramble, and does NOT touch the CAS key path (ECM->Ks),
// dione key erase, or any secure-key command.  Those are permanently out of
// scope (CLAUDE.md 1-2).  This tool only issues the plain USB-tunnel ioctls we
// observed and writes the firmware blob; the owner's firmware and key material
// are never distributed.
//
// The h57fmb0 ioctls are type 0xae (observed via the LD_PRELOAD tracer):
//   nr=0x08 R[12]  GET_INFO  -> {u16 vid; u16 pid; u32 index}
//   nr=0x04 R[46]  version string
//   nr=0x02 R[12]  reg read  {u32 flag; u32 addr; u32 *val}
//   nr=0x03 W[12]  reg write {u32 flag; u32 addr; u32 *val}   (16-bit reg value)
//   nr=0x01 RW[12] command   {u32 cmd_sub; u32 arg; u32 *val}
//   nr=0x11 R[20] / nr=0x12 W[20]  transfer descriptor (5 u32)
//   write(fd, buf, n)              firmware, 512-byte chunks
//
// Usage:
//   h57cmd load  FILE            write FILE to h57fmb0 in 512-byte chunks
//   h57cmd info                  GET_INFO (VID:PID index)
//   h57cmd ver                   version string
//   h57cmd rd    ADDR            read 16-bit reg
//   h57cmd wr    ADDR VAL        write 16-bit reg (flag=1)
//   h57cmd cmd   W0 W1 VAL       command ioctl (nr=0x01)
//   h57cmd @FILE                 run a script (one command per line; blank/# ok)
// ADDR/VAL/W0/W1 accept 0x.. hex.  Every line is echoed with its result.
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define H57_TYPE 0xae
#define IOC_R 2u
#define IOC_W 1u
#define IOC_RW 3u

static unsigned long ioc(unsigned dir, unsigned nr, unsigned size)
{
	return ((unsigned long)dir << 30) | ((unsigned long)size << 16) |
	       ((unsigned long)H57_TYPE << 8) | (unsigned long)nr;
}

struct reg12 {          /* nr=0x02/0x03/0x01 argument */
	uint32_t flag;
	uint32_t addr;
	uint32_t *val;
};

static int do_info(int fd)
{
	unsigned char b[12];
	memset(b, 0, sizeof b);
	long r = ioctl(fd, ioc(IOC_R, 0x08, 12), b);
	printf("info: ret=%ld VID=0x%04x PID=0x%04x index=%u\n", r,
	       b[0] | b[1] << 8, b[4] | b[5] << 8, b[8] | b[9] << 8 | b[10] << 16 | b[11] << 24);
	return r < 0;
}

static int do_ver(int fd)
{
	unsigned char b[46];
	memset(b, 0, sizeof b);
	long r = ioctl(fd, ioc(IOC_R, 0x04, 46), b);
	int i;
	printf("ver: ret=%ld str=\"", r);
	for (i = 0; i < 20 && b[i]; i++) putchar(b[i] >= 0x20 && b[i] < 0x7f ? b[i] : '.');
	printf("\" raw=");
	for (i = 0; i < (int)sizeof b; i++) printf("%02x ", b[i]);
	printf("\n");
	return r < 0;
}

static int do_rd(int fd, uint32_t addr)
{
	uint32_t val = 0;
	struct reg12 a = { 1, addr, &val };
	long r = ioctl(fd, ioc(IOC_R, 0x02, 12), &a);
	printf("rd  0x%06x = 0x%04x (ret=%ld)\n", addr, val & 0xffff, r);
	return r < 0;
}

static int do_wr(int fd, uint32_t addr, uint32_t val)
{
	uint32_t v = val & 0xffff;
	struct reg12 a = { 1, addr, &v };
	long r = ioctl(fd, ioc(IOC_W, 0x03, 12), &a);
	printf("wr  0x%06x = 0x%04x (ret=%ld)\n", addr, v, r);
	return r < 0;
}

static int do_cmd(int fd, uint32_t w0, uint32_t w1, uint32_t val)
{
	uint32_t v = val;
	struct reg12 a = { w0, w1, &v };
	long r = ioctl(fd, ioc(IOC_RW, 0x01, 12), &a);
	printf("cmd w0=0x%08x w1=0x%08x val=0x%08x -> ret=%ld val=0x%08x\n",
	       w0, w1, val, r, v);
	return r < 0;
}

/* The firmware write() must be armed first: the driver hands us a 20-byte
 * transfer descriptor via nr=0x11 (the DRAM target it prepared) and we echo it
 * back with nr=0x12; then the write() stream is accepted.  Skipping this makes
 * write() time out after a couple of chunks. */
static int do_desc(int fd)
{
	unsigned char d[20];
	long r1, r2;
	int i;
	memset(d, 0, sizeof d);
	r1 = ioctl(fd, ioc(IOC_R, 0x11, 20), d);
	r2 = ioctl(fd, ioc(IOC_W, 0x12, 20), d);
	printf("desc: 0x11 ret=%ld 0x12 ret=%ld d=", r1, r2);
	for (i = 0; i < 20; i++) printf("%02x ", d[i]);
	printf("\n");
	return (r1 < 0 || r2 < 0);
}

static int do_load(int fd, const char *path)
{
	int in = open(path, O_RDONLY);
	static unsigned char buf[512];
	long total = 0, chunks = 0;
	ssize_t n;
	if (in < 0) {
		fprintf(stderr, "load: open(%s): %s\n", path, strerror(errno));
		return 1;
	}
	while ((n = read(in, buf, sizeof buf)) > 0) {
		ssize_t w = write(fd, buf, (size_t)n);
		if (w < 0) {
			fprintf(stderr, "load: write at %ld: %s\n", total, strerror(errno));
			close(in);
			return 1;
		}
		total += w;
		chunks++;
	}
	close(in);
	printf("load %s: %ld bytes in %ld chunks\n", path, total, chunks);
	return 0;
}

/* loadfw = arm the transfer (desc) then stream the firmware. */
static int do_loadfw(int fd, const char *path)
{
	if (do_desc(fd)) return 1;
	return do_load(fd, path);
}

static uint32_t hx(const char *s) { return (uint32_t)strtoul(s, NULL, 0); }

static int run_one(int fd, int argc, char **argv)
{
	if (!strcmp(argv[0], "info")) return do_info(fd);
	if (!strcmp(argv[0], "ver")) return do_ver(fd);
	if (!strcmp(argv[0], "load") && argc >= 2) return do_load(fd, argv[1]);
	if (!strcmp(argv[0], "loadfw") && argc >= 2) return do_loadfw(fd, argv[1]);
	if (!strcmp(argv[0], "desc")) return do_desc(fd);
	if (!strcmp(argv[0], "rd") && argc >= 2) return do_rd(fd, hx(argv[1]));
	if (!strcmp(argv[0], "wr") && argc >= 3) return do_wr(fd, hx(argv[1]), hx(argv[2]));
	if (!strcmp(argv[0], "cmd") && argc >= 4) return do_cmd(fd, hx(argv[1]), hx(argv[2]), hx(argv[3]));
	fprintf(stderr, "unknown/short: %s\n", argv[0]);
	return 2;
}

static int run_script(int fd, const char *path)
{
	FILE *f = strcmp(path, "-") ? fopen(path, "r") : stdin;
	char line[256];
	int rc = 0;
	if (!f) { fprintf(stderr, "script open %s: %s\n", path, strerror(errno)); return 1; }
	while (fgets(line, sizeof line, f)) {
		char *av[8]; int ac = 0;
		char *p = strtok(line, " \t\r\n");
		while (p && ac < 8) { av[ac++] = p; p = strtok(NULL, " \t\r\n"); }
		if (ac == 0 || av[0][0] == '#') continue;
		if (run_one(fd, ac, av)) rc = 1;
	}
	if (f != stdin) fclose(f);
	return rc;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: h57cmd (load FILE|info|ver|rd A|wr A V|cmd W0 W1 V|@script)\n");
		return 2;
	}
	int fd = open("/dev/h57fmb0", O_RDWR);
	if (fd < 0) { fprintf(stderr, "open /dev/h57fmb0: %s\n", strerror(errno)); return 1; }
	int rc;
	if (argv[1][0] == '@') rc = run_script(fd, argv[1] + 1);
	else rc = run_one(fd, argc - 1, argv + 1);
	close(fd);
	return rc;
}
