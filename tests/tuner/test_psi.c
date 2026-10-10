// SPDX-License-Identifier: GPL-2.0-only
/*
 * Host unit test for ird_psi.c (the tuner core's PID enumerator).
 *
 * Builds real MPEG-2 PSI (PAT / PMT / CAT) sections with correct CRC_32,
 * packetises them into 188-byte TS packets with pointer_field, continuity
 * counters and payload_unit_start, feeds them to ird_psi_feed() one packet at
 * a time, and checks what ird_psi_pids() / ird_psi_complete() report.  No
 * device and no broadcast content are needed: every input is synthesised, so
 * the hard parts (section reassembly across packets, version/CRC change, the
 * CA 0x09 and ARIB access-control 0xF6 descriptors) are exercised exactly.
 *
 * Validation against a real multiplex is the separate end-to-end step
 * (recisdb vs the reference receiver); this pins down the parser offline.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/types.h>
#include <linux/crc32.h>
#include "ird_psi.h"

static int fails;
#define CHECK(name, cond) do { \
	printf("  %s %s\n", (cond) ? "ok  " : "FAIL", name); \
	if (!(cond)) fails++; \
} while (0)

/* --- a tiny TS packetiser ------------------------------------------------- */

static struct ird_psi psi;
static u8 cc_of[8192];

/* Emit the section on `pid` as one or more TS packets straight into the
 * parser.  Returns 1 if any packet changed the PID list. */
static int feed_section(unsigned pid, const u8 *sec, int len)
{
	int off = 0, first = 1, changed = 0;

	while (off < len || first) {
		u8 p[188];
		int pl = 4, avail, chunk;

		memset(p, 0xff, sizeof(p));
		p[0] = 0x47;
		p[1] = (first ? 0x40 : 0x00) | ((pid >> 8) & 0x1f);
		p[2] = pid & 0xff;
		p[3] = 0x10 | (cc_of[pid] & 0x0f);
		cc_of[pid]++;
		if (first)
			p[pl++] = 0x00;		/* pointer_field = 0 */
		avail = 188 - pl;
		chunk = (len - off < avail) ? (len - off) : avail;
		memcpy(p + pl, sec + off, chunk);
		off += chunk;
		first = 0;
		changed |= ird_psi_feed(&psi, p);
	}
	return changed;
}

/* --- section builders ----------------------------------------------------- */

/* Set section_length and append CRC_32 so the parser accepts it.  n = bytes
 * written so far (header + payload, no CRC).  Returns the full length. */
static int finish(u8 *s, int n)
{
	int section_length = (n + 4) - 3;
	u32 crc;

	s[1] = (s[1] & 0xf0) | ((section_length >> 8) & 0x0f);
	s[2] = section_length & 0xff;
	crc = crc32_be(0xffffffffu, s, n);
	s[n] = crc >> 24; s[n + 1] = crc >> 16; s[n + 2] = crc >> 8; s[n + 3] = crc;
	return n + 4;
}

static u8 hdr_syntax(void) { return 0x80 | 0x30; }	/* syntax=1, reserved '11' */
static u8 ver_cni(int v)  { return 0xc0 | ((v & 0x1f) << 1) | 1; }  /* ver, current */

static int build_pat(u8 *s, unsigned tsid, int ver, unsigned nit_pid,
		     const unsigned *prog, const unsigned *pmt, int nprog)
{
	int n = 0, i;

	s[n++] = 0x00;				/* table_id = PAT */
	s[n++] = hdr_syntax(); s[n++] = 0;	/* section_length filled by finish */
	s[n++] = tsid >> 8; s[n++] = tsid & 0xff;
	s[n++] = ver_cni(ver);
	s[n++] = 0; s[n++] = 0;			/* section_number / last */
	if (nit_pid) {				/* program_number 0 -> network PID */
		s[n++] = 0; s[n++] = 0;
		s[n++] = 0xe0 | (nit_pid >> 8); s[n++] = nit_pid & 0xff;
	}
	for (i = 0; i < nprog; i++) {
		s[n++] = prog[i] >> 8; s[n++] = prog[i] & 0xff;
		s[n++] = 0xe0 | (pmt[i] >> 8); s[n++] = pmt[i] & 0xff;
	}
	return finish(s, n);
}

/* a CA (0x09) or ARIB access-control (0xf6) descriptor naming a PID */
static int ca_desc(u8 *d, u8 tag, unsigned sysid, unsigned pid)
{
	d[0] = tag; d[1] = 4;
	d[2] = sysid >> 8; d[3] = sysid & 0xff;
	d[4] = 0xe0 | (pid >> 8); d[5] = pid & 0xff;
	return 6;
}

struct es { u8 type; unsigned pid; u8 ca_tag; unsigned ca_pid; };

static int build_pmt(u8 *s, unsigned prognum, int ver, unsigned pcr,
		     u8 pi_ca_tag, unsigned pi_ca_pid,
		     const struct es *es, int nes)
{
	int n = 0, i, pil_pos, pil = 0;

	s[n++] = 0x02;				/* table_id = PMT */
	s[n++] = hdr_syntax(); s[n++] = 0;
	s[n++] = prognum >> 8; s[n++] = prognum & 0xff;
	s[n++] = ver_cni(ver);
	s[n++] = 0; s[n++] = 0;
	s[n++] = 0xe0 | (pcr >> 8); s[n++] = pcr & 0xff;	/* PCR_PID */
	pil_pos = n;
	s[n++] = 0xf0; s[n++] = 0;		/* program_info_length (filled below) */
	if (pi_ca_tag) {
		pil = ca_desc(s + n, pi_ca_tag, 0x0005, pi_ca_pid);
		n += pil;
	}
	s[pil_pos] = 0xf0 | (pil >> 8); s[pil_pos + 1] = pil & 0xff;
	for (i = 0; i < nes; i++) {
		int esil_pos, esil = 0;

		s[n++] = es[i].type;
		s[n++] = 0xe0 | (es[i].pid >> 8); s[n++] = es[i].pid & 0xff;
		esil_pos = n;
		s[n++] = 0xf0; s[n++] = 0;
		if (es[i].ca_tag) {
			esil = ca_desc(s + n, es[i].ca_tag, 0x0005, es[i].ca_pid);
			n += esil;
		}
		s[esil_pos] = 0xf0 | (esil >> 8); s[esil_pos + 1] = esil & 0xff;
	}
	return finish(s, n);
}

static int build_cat(u8 *s, int ver, u8 tag, unsigned emm_pid)
{
	int n = 0;

	s[n++] = 0x01;				/* table_id = CAT */
	s[n++] = hdr_syntax(); s[n++] = 0;
	s[n++] = 0xff; s[n++] = 0xff;		/* reserved */
	s[n++] = ver_cni(ver);
	s[n++] = 0; s[n++] = 0;
	n += ca_desc(s + n, tag, 0x0005, emm_pid);
	return finish(s, n);
}

/* --- assertions ----------------------------------------------------------- */

static u16 light[2 * IRD_PSI_MAX_ES * IRD_PSI_MAX_PROG];
static u16 heavy[2 * IRD_PSI_MAX_ES * IRD_PSI_MAX_PROG];
static int nlight, nheavy;

static void snapshot(void)
{
	ird_psi_pids(&psi, light, &nlight, heavy, &nheavy, (int)(sizeof(light) / 2));
}

static int has(const u16 *v, int n, unsigned pid)
{
	int i;
	for (i = 0; i < n; i++)
		if (v[i] == pid)
			return 1;
	return 0;
}

#define IN_LIGHT(pid) has(light, nlight, pid)
#define IN_HEAVY(pid) has(heavy, nheavy, pid)
#define IN_ANY(pid)   (IN_LIGHT(pid) || IN_HEAVY(pid))

/* --- scenarios ------------------------------------------------------------ */

static void reset(void)
{
	memset(cc_of, 0, sizeof(cc_of));
	ird_psi_reset(&psi);
}

static void test_basic(void)
{
	u8 pat[256], pmt[256], cat[64];
	unsigned prog[] = {1}, pmtp[] = {0x1000};
	struct es es[] = {
		{0x02, 0x0100, 0, 0},			/* video */
		{0x0f, 0x0110, 0, 0},			/* audio */
		{0x06, 0x0130, 0x09, 0x0901},		/* data, with its own ECM */
	};
	int lpat, lpmt, lcat;

	printf("[psi] basic single-program multiplex\n");
	reset();
	lpat = build_pat(pat, 0x7fe0, 0, 0x0010, prog, pmtp, 1);
	lpmt = build_pmt(pmt, 1, 0, 0x01f0, 0x09, 0x0900, es, 3);
	lcat = build_cat(cat, 0, 0xf6, 0x0902);		/* EMM via access-control desc */

	feed_section(0x0000, pat, lpat);
	snapshot();
	CHECK("after PAT only: not complete", !ird_psi_complete(&psi));
	CHECK("after PAT: PMT PID is listed (light)", IN_LIGHT(0x1000));
	CHECK("after PAT: no ES yet", !IN_ANY(0x0100));

	feed_section(0x1000, pmt, lpmt);
	feed_section(0x0001, cat, lcat);
	snapshot();
	CHECK("complete after PAT + PMT", ird_psi_complete(&psi));
	CHECK("fixed PID PAT present", IN_LIGHT(0x0000));
	CHECK("fixed PID CAT present", IN_LIGHT(0x0001));
	CHECK("NIT (0x0010) present", IN_ANY(0x0010));
	CHECK("PMT PID is light", IN_LIGHT(0x1000));
	CHECK("video is heavy", IN_HEAVY(0x0100));
	CHECK("audio is heavy", IN_HEAVY(0x0110));
	CHECK("PCR is heavy", IN_HEAVY(0x01f0));
	CHECK("program ECM (0x0900) is light", IN_LIGHT(0x0900));
	CHECK("per-ES ECM (0x0901) is light", IN_LIGHT(0x0901));
	CHECK("EMM via 0xf6 (0x0902) is light", IN_LIGHT(0x0902));
	CHECK("video not also classed light", !IN_LIGHT(0x0100));
	CHECK("ECM not also classed heavy", !IN_HEAVY(0x0900));
}

static void test_multi_program(void)
{
	u8 pat[256], hd[256], seg[256], data[256];
	unsigned prog[] = {101, 102, 103}, pmtp[] = {0x1000, 0x1001, 0x1002};
	struct es hdes[]  = {{0x02, 0x0100, 0, 0}, {0x0f, 0x0110, 0, 0}};
	struct es seges[] = {{0x02, 0x1100, 0, 0}, {0x0f, 0x1110, 0, 0}};	/* 1seg */
	struct es dates[] = {{0x0d, 0x1200, 0, 0}};				/* data */

	printf("[psi] multi-program multiplex (HD + 1seg + data)\n");
	reset();
	feed_section(0x0000, pat, build_pat(pat, 0x7fe0, 0, 0x0010, prog, pmtp, 3));
	snapshot();
	CHECK("3 PMT PIDs listed after PAT",
	      IN_LIGHT(0x1000) && IN_LIGHT(0x1001) && IN_LIGHT(0x1002));
	CHECK("not complete until all PMTs read", !ird_psi_complete(&psi));

	feed_section(0x1000, hd,   build_pmt(hd,   101, 0, 0x01f0, 0, 0, hdes, 2));
	feed_section(0x1001, seg,  build_pmt(seg,  102, 0, 0x11f0, 0, 0, seges, 2));
	feed_section(0x1002, data, build_pmt(data, 103, 0, 0x12f0, 0, 0, dates, 1));
	snapshot();
	CHECK("complete after all 3 PMTs", ird_psi_complete(&psi));
	CHECK("HD video/audio/pcr", IN_HEAVY(0x0100) && IN_HEAVY(0x0110) && IN_HEAVY(0x01f0));
	CHECK("1seg video/audio/pcr", IN_HEAVY(0x1100) && IN_HEAVY(0x1110) && IN_HEAVY(0x11f0));
	CHECK("data es/pcr", IN_HEAVY(0x1200) && IN_HEAVY(0x12f0));
}

static void test_spanning(void)
{
	u8 pat[256], pmt[1024];
	unsigned prog[] = {1}, pmtp[] = {0x1000};
	struct es es[40];
	/* 34 ES (+PCR = 35 PIDs, under IRD_PSI_MAX_ES) makes the section
	 * 12 + 34*5 + 4 = 186 B, past one 184-byte TS payload, so it spans. */
	int i, nes = 34;

	printf("[psi] PMT spanning multiple TS packets\n");
	reset();
	for (i = 0; i < nes; i++) {
		es[i].type = (i & 1) ? 0x0f : 0x02;
		es[i].pid = 0x0200 + i;
		es[i].ca_tag = 0;
		es[i].ca_pid = 0;
	}
	feed_section(0x0000, pat, build_pat(pat, 1, 0, 0, prog, pmtp, 1));
	i = build_pmt(pmt, 1, 0, 0x01f0, 0, 0, es, nes);
	CHECK("PMT section is larger than one TS payload", i > 183);
	feed_section(0x1000, pmt, i);
	snapshot();
	CHECK("complete with a spanning PMT", ird_psi_complete(&psi));
	CHECK("first ES of spanning PMT present", IN_HEAVY(0x0200));
	CHECK("last ES of spanning PMT present", IN_HEAVY(0x0200 + nes - 1));
}

static void test_version_change(void)
{
	u8 pat[256], pmt[256];
	unsigned prog[] = {1}, pmtp[] = {0x1000};
	struct es es2[] = {{0x02, 0x0100, 0, 0}, {0x0f, 0x0110, 0, 0}};
	struct es es1[] = {{0x02, 0x0100, 0, 0}};
	int changed;

	printf("[psi] version/CRC change re-enumerates\n");
	reset();
	feed_section(0x0000, pat, build_pat(pat, 1, 0, 0, prog, pmtp, 1));
	feed_section(0x1000, pmt, build_pmt(pmt, 1, 0, 0x01f0, 0, 0, es2, 2));
	snapshot();
	CHECK("v0: audio present", IN_HEAVY(0x0110));

	/* same section again = same CRC = no change */
	changed = feed_section(0x1000, pmt, build_pmt(pmt, 1, 0, 0x01f0, 0, 0, es2, 2));
	CHECK("identical PMT reports no change", !changed);

	/* new version, audio dropped */
	changed = feed_section(0x1000, pmt, build_pmt(pmt, 1, 1, 0x01f0, 0, 0, es1, 1));
	CHECK("new PMT version reports a change", changed);
	snapshot();
	CHECK("v1: audio gone", !IN_ANY(0x0110));
	CHECK("v1: video still there", IN_HEAVY(0x0100));
}

static void test_robustness(void)
{
	u8 pat[256], pmt[256], junk[188];
	unsigned prog[] = {1}, pmtp[] = {0x1000};
	struct es es[] = {{0x02, 0x0100, 0, 0}};

	printf("[psi] ignores scrambled / error / unwanted packets\n");
	reset();
	/* a packet on a PID we do not want is ignored */
	memset(junk, 0, sizeof(junk));
	junk[0] = 0x47; junk[1] = 0x41; junk[2] = 0x23; junk[3] = 0x10;
	CHECK("unwanted PID -> no change", !ird_psi_feed(&psi, junk));

	feed_section(0x0000, pat, build_pat(pat, 1, 0, 0, prog, pmtp, 1));
	/* a scrambled PMT packet (transport_scrambling_control set) is skipped */
	{
		u8 p[188];
		int lpmt = build_pmt(pmt, 1, 0, 0x01f0, 0, 0, es, 1);
		memset(p, 0xff, sizeof(p));
		p[0] = 0x47; p[1] = 0x40 | 0x10; p[2] = 0x00;	/* PID 0x1000 */
		p[3] = 0xd0;					/* scrambled + payload */
		p[4] = 0x00;
		memcpy(p + 5, pmt, lpmt < 183 ? lpmt : 183);
		CHECK("scrambled PMT -> no change", !ird_psi_feed(&psi, p));
		CHECK("not complete from a scrambled PMT", !ird_psi_complete(&psi));
	}
	/* the clean PMT is parsed */
	feed_section(0x1000, pmt, build_pmt(pmt, 1, 0, 0x01f0, 0, 0, es, 1));
	CHECK("clean PMT after a scrambled one is parsed", ird_psi_complete(&psi));
}

int main(void)
{
	test_basic();
	test_multi_program();
	test_spanning();
	test_version_change();
	test_robustness();
	printf(fails ? "\nFAIL: %d check(s) failed\n" : "\nok: all checks passed\n", fails);
	return fails ? 1 : 0;
}
