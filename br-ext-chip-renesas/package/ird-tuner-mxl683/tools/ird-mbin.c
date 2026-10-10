// SPDX-License-Identifier: GPL-2.0-only
/*
 * ird-mbin - find the MXL683 firmware container (MBIN) inside a file of the
 * owner's own device and write it out, so that the tuner driver can load it
 * into the demodulator at run time.
 *
 * The firmware is not part of OpenIRD and is not distributed with it.  On the
 * POE-BR100-YB1 it sits, unpacked, inside the vendor application
 * (/app/bin/blur on the vendor system).  Its position there changes with the
 * vendor firmware version, so this scans for it and checks what it finds:
 * header shape, checksum, and segments that add up
 * (docs/specs/mbin-firmware-container.md §2-§5).
 *
 *   ird-mbin <file>            say where the container is
 *   ird-mbin -o <out> <file>   write it to <out>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HDR_LEN		16
#define SEG_HDR		8
#define MAX_SEG		32

static unsigned long be24(const unsigned char *p)
{
	return ((unsigned long)p[0] << 16) | (p[1] << 8) | p[2];
}

/* The length of the container at p (avail bytes left), or 0 if it is none. */
static unsigned long mbin_len(const unsigned char *p, unsigned long avail)
{
	unsigned long size, pos = 0, i;
	unsigned char sum = 0;
	int k;

	if (avail < HDR_LEN || p[0] != 'M' || p[1] != '1' || p[2] != HDR_LEN ||
	    !p[3] || p[3] > MAX_SEG)
		return 0;
	size = be24(p + 8);
	if (size < 0x100 || HDR_LEN + size > avail)
		return 0;
	for (i = 0; i < size; i++)
		sum += p[HDR_LEN + i];
	if (sum != p[11])
		return 0;
	for (k = 0; k < p[3]; k++) {
		const unsigned char *h = p + HDR_LEN + pos;

		if (pos + SEG_HDR > size || h[0] != 'S')
			return 0;
		/* data, then padding to a 4-byte boundary */
		pos += SEG_HDR + ((be24(h + 1) + 3) & ~3ul);
		if (pos > size)
			return 0;
	}
	return pos == size ? HDR_LEN + size : 0;
}

int main(int argc, char **argv)
{
	const char *out = NULL, *in;
	unsigned char *buf;
	unsigned long len, off, n = 0;
	FILE *f;

	if (argc == 4 && !strcmp(argv[1], "-o")) {
		out = argv[2];
		in = argv[3];
	} else if (argc == 2) {
		in = argv[1];
	} else {
		fprintf(stderr, "usage: ird-mbin [-o <out>] <file>\n");
		return 2;
	}
	f = fopen(in, "rb");
	if (!f) {
		perror(in);
		return 1;
	}
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	rewind(f);
	buf = malloc(len ? len : 1);
	if (!buf || fread(buf, 1, len, f) != len) {
		fprintf(stderr, "ird-mbin: cannot read %s\n", in);
		return 1;
	}
	fclose(f);

	for (off = 0; off + HDR_LEN <= len; off++) {
		if (buf[off] != 'M' || buf[off + 1] != '1')
			continue;
		n = mbin_len(buf + off, len - off);
		if (n)
			break;
	}
	if (!n) {
		fprintf(stderr, "ird-mbin: no firmware container in %s\n", in);
		return 1;
	}
	fprintf(stderr, "ird-mbin: container at offset 0x%lx, %lu bytes, %d segments\n",
		off, n, buf[off + 3]);
	if (out) {
		f = fopen(out, "wb");
		if (!f || fwrite(buf + off, 1, n, f) != n || fclose(f)) {
			perror(out);
			return 1;
		}
	}
	return 0;
}
