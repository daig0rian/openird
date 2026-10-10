// SPDX-License-Identifier: GPL-2.0-only
/*
 * OpenIRD tuner core: which PIDs make up a multiplex (see ird_psi.h).
 *
 * The rules (docs/specs/poe-br100-yb1-ts-fifo.md §6, from ARIB TR-B14):
 *   1. the fixed PIDs of the service information, carried whether or not
 *      anything is seen on them (some tables are sent rarely);
 *   2. PAT -> the NIT and the PMT of every program;
 *   3. PMT -> PCR, every elementary stream, and the PIDs named by CA
 *      descriptors (tag 0x09) and access control descriptors (tag 0xF6) in
 *      the program loop and the stream loops;
 *   4. CAT -> the PIDs named by the same two descriptors.
 * A table is read again whenever a section with another CRC_32 turns up,
 * which covers every version change.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/crc32.h>

#include "ird_psi.h"

#define PID_PAT		0x0000
#define PID_CAT		0x0001
#define PID_NULL	0x1fff
#define TID_PAT		0x00
#define TID_CAT		0x01
#define TID_PMT		0x02
#define TAG_CA		0x09
#define TAG_ACCESS	0xf6	/* ARIB access control descriptor: same layout */

/* TR-B14 vol. 4 table 5-4: PAT, CAT, NIT, SDT, EIT x3, TOT, SDTT x2, BIT, CDT */
static const u16 fixed_pids[] = {
	0x0000, 0x0001, 0x0010, 0x0011, 0x0012, 0x0026, 0x0027, 0x0014,
	0x0023, 0x0028, 0x0024, 0x0029,
};

static inline unsigned int be16(const u8 *p)
{
	return (p[0] << 8) | p[1];
}

static void sect_reset(struct ird_psi_sect *s)
{
	s->active = 0;
	s->cc = 0xff;
	s->have = 0;
}

void ird_psi_reset(struct ird_psi *p)
{
	int i;

	memset(p, 0, sizeof(*p));
	sect_reset(&p->sect_pat);
	sect_reset(&p->sect_cat);
	for (i = 0; i < IRD_PSI_MAX_PROG; i++)
		sect_reset(&p->sect_pmt[i]);
	p->nit_pid = PID_NULL;
}
EXPORT_SYMBOL_GPL(ird_psi_reset);

static struct ird_psi_sect *sect_of(struct ird_psi *p, unsigned int pid)
{
	int i;

	if (pid == PID_PAT)
		return &p->sect_pat;
	if (pid == PID_CAT)
		return &p->sect_cat;
	for (i = 0; i < p->npmt; i++)
		if (p->pmt_pid[i] == pid)
			return &p->sect_pmt[i];
	return NULL;
}

int ird_psi_wants(const struct ird_psi *p, unsigned int pid)
{
	return sect_of((struct ird_psi *)p, pid) != NULL;
}
EXPORT_SYMBOL_GPL(ird_psi_wants);

int ird_psi_complete(const struct ird_psi *p)
{
	int i;

	if (!p->pat_valid)
		return 0;
	for (i = 0; i < p->nprog; i++)
		if (!p->prog[i].valid)
			return 0;
	return 1;
}
EXPORT_SYMBOL_GPL(ird_psi_complete);

/* --- the tables ------------------------------------------------------------ */

static void prog_add(struct ird_psi_prog *g, unsigned int pid, int heavy)
{
	int i;

	if (pid >= PID_NULL)
		return;
	for (i = 0; i < g->npids; i++)
		if (g->pids[i] == pid) {
			g->heavy[i] &= heavy;	/* named both ways: the light side */
			return;
		}
	if (g->npids < IRD_PSI_MAX_ES) {
		g->pids[g->npids] = pid;
		g->heavy[g->npids++] = heavy;
	}
}

/* The PIDs named by the CA and access control descriptors of a loop. */
static void scan_descriptors(const u8 *d, int len, struct ird_psi_prog *g,
			     struct ird_psi *cat)
{
	while (len >= 2) {
		int l = d[1];

		if (2 + l > len)
			break;
		if ((d[0] == TAG_CA || d[0] == TAG_ACCESS) && l >= 4) {
			unsigned int pid = be16(d + 4) & 0x1fff;

			if (g)
				prog_add(g, pid, 0);
			else if (pid < PID_NULL && cat->ncat < IRD_PSI_MAX_CAT)
				cat->cat_pids[cat->ncat++] = pid;
		}
		d += 2 + l;
		len -= 2 + l;
	}
}

/* b: a whole section, len bytes with the CRC_32; crc: that CRC_32 */
static int take_pat(struct ird_psi *p, const u8 *b, int len, u32 crc)
{
	struct ird_psi_prog *old = p->scratch;
	int nold = p->nprog, i, j;

	if (b[0] != TID_PAT)
		return 0;
	if (p->pat_valid && p->pat_crc == crc)
		return 0;
	memcpy(old, p->prog, sizeof(p->prog));
	p->nprog = 0;
	p->npmt = 0;
	p->nit_pid = PID_NULL;
	for (i = 8; i + 4 <= len - 4; i += 4) {
		unsigned int number = be16(b + i), pid = be16(b + i + 2) & 0x1fff;
		struct ird_psi_prog *g;

		if (!number) {
			p->nit_pid = pid;
			continue;
		}
		if (pid >= PID_NULL || pid <= PID_CAT ||
		    p->nprog == IRD_PSI_MAX_PROG)
			continue;
		g = &p->prog[p->nprog++];
		memset(g, 0, sizeof(*g));
		g->number = number;
		g->pmt_pid = pid;
		/* a program that stays keeps what its PMT said */
		for (j = 0; j < nold; j++)
			if (old[j].number == number && old[j].pmt_pid == pid)
				*g = old[j];
		for (j = 0; j < p->npmt && p->pmt_pid[j] != pid; j++)
			;
		if (j == p->npmt)
			p->pmt_pid[p->npmt++] = pid;
	}
	for (i = 0; i < IRD_PSI_MAX_PROG; i++)
		sect_reset(&p->sect_pmt[i]);
	p->pat_valid = 1;
	p->pat_crc = crc;
	return 1;
}

static int take_pmt(struct ird_psi *p, unsigned int pid, const u8 *b, int len,
		    u32 crc)
{
	struct ird_psi_prog *g = NULL;
	int i, end = len - 4, pil;

	if (b[0] != TID_PMT || len < 16)
		return 0;
	for (i = 0; i < p->nprog; i++)
		if (p->prog[i].pmt_pid == pid && p->prog[i].number == be16(b + 3))
			g = &p->prog[i];
	if (!g || (g->valid && g->crc == crc))
		return 0;
	g->npids = 0;
	prog_add(g, be16(b + 8) & 0x1fff, 1);		/* PCR */
	pil = be16(b + 10) & 0x0fff;
	if (12 + pil > end)
		return 0;
	scan_descriptors(b + 12, pil, g, NULL);
	for (i = 12 + pil; i + 5 <= end; ) {
		int esl = be16(b + i + 3) & 0x0fff;

		if (i + 5 + esl > end)
			break;
		prog_add(g, be16(b + i + 1) & 0x1fff, 1);
		scan_descriptors(b + i + 5, esl, g, NULL);
		i += 5 + esl;
	}
	g->valid = 1;
	g->crc = crc;
	return 1;
}

static int take_cat(struct ird_psi *p, const u8 *b, int len, u32 crc)
{
	if (b[0] != TID_CAT)
		return 0;
	if (p->cat_valid && p->cat_crc == crc)
		return 0;
	p->ncat = 0;
	scan_descriptors(b + 8, len - 4 - 8, NULL, p);
	p->cat_valid = 1;
	p->cat_crc = crc;
	return 1;
}

/* --- sections out of packets ------------------------------------------------ */

static int sect_len(const struct ird_psi_sect *s)
{
	return 3 + (((s->buf[1] & 0x0f) << 8) | s->buf[2]);
}

static int sect_whole(const struct ird_psi_sect *s)
{
	return s->active && s->have >= 3 && s->have >= sect_len(s);
}

/* Give the section up to n more bytes; returns how many it took. */
static int sect_add(struct ird_psi_sect *s, const u8 *d, int n)
{
	int took = 0;

	while (took < n) {
		int k;

		if (s->have < 3) {
			s->buf[s->have++] = d[took++];
			continue;
		}
		k = sect_len(s);
		if (k > IRD_PSI_SECT_MAX || k < 12) {	/* not a PSI section */
			s->active = 0;
			return n;
		}
		if (s->have >= k)
			break;
		k = min(n - took, k - (int)s->have);
		memcpy(s->buf + s->have, d + took, k);
		s->have += k;
		took += k;
	}
	return took;
}

static int sect_done(struct ird_psi *p, unsigned int pid,
		     const struct ird_psi_sect *s)
{
	const u8 *b = s->buf;
	int len = sect_len(s);
	u32 crc;

	/* long form, applicable now, and whole: the CRC over all of it is 0 */
	if (!(b[1] & 0x80) || !(b[5] & 1) || crc32_be(~0, b, len))
		return 0;
	crc = (be16(b + len - 4) << 16) | be16(b + len - 2);
	if (pid == PID_PAT)
		return take_pat(p, b, len, crc);
	if (pid == PID_CAT)
		return take_cat(p, b, len, crc);
	return take_pmt(p, pid, b, len, crc);
}

int ird_psi_feed(struct ird_psi *p, const u8 *pkt)
{
	unsigned int pid = be16(pkt + 1) & 0x1fff, cc = pkt[3] & 15;
	struct ird_psi_sect *s = sect_of(p, pid);
	const u8 *d;
	int n, off = 4, changed = 0;

	if (!s || pkt[0] != 0x47)
		return 0;
	if (pkt[1] & 0x80) {			/* transport_error_indicator */
		sect_reset(s);
		return 0;
	}
	if (!(pkt[3] & 0x10) || (pkt[3] & 0xc0))	/* no payload / scrambled */
		return 0;
	if (pkt[3] & 0x20)
		off += 1 + pkt[4];
	if (off >= 188)
		return 0;
	if (s->cc != 0xff) {
		if (cc == s->cc)
			return 0;		/* the same packet again */
		if (cc != ((s->cc + 1) & 15))
			s->active = 0;		/* lost some: start over */
	}
	s->cc = cc;
	d = pkt + off;
	n = 188 - off;

	if (!(pkt[1] & 0x40)) {			/* a section goes on */
		if (s->active) {
			sect_add(s, d, n);
			if (sect_whole(s)) {
				changed |= sect_done(p, pid, s);
				s->active = 0;
			}
		}
		return changed;
	}

	/* payload_unit_start: pointer_field, the end of the section before,
	 * then one or more new sections */
	{
		int ptr = d[0];

		d++;
		n--;
		if (ptr > n) {
			s->active = 0;
			return 0;
		}
		if (s->active) {
			sect_add(s, d, ptr);
			if (sect_whole(s))
				changed |= sect_done(p, pid, s);
		}
		d += ptr;
		n -= ptr;
	}
	s->active = 0;
	while (n > 0 && d[0] != 0xff) {
		int k;

		s->active = 1;
		s->have = 0;
		k = sect_add(s, d, n);
		d += k;
		n -= k;
		if (!sect_whole(s))
			break;			/* garbage, or goes on next time */
		changed |= sect_done(p, pid, s);
		s->active = 0;
		/* sect_done() may have replaced the PMT list under us */
		s = sect_of(p, pid);
		if (!s)
			break;
	}
	return changed;
}
EXPORT_SYMBOL_GPL(ird_psi_feed);

/* --- the list ---------------------------------------------------------------- */

struct pidlist {
	u16 *light, *heavy;
	int nlight, nheavy, max;
};

static int in_list(const u16 *v, int n, unsigned int pid)
{
	int i;

	for (i = 0; i < n; i++)
		if (v[i] == pid)
			return 1;
	return 0;
}

static void pidlist_add(struct pidlist *l, unsigned int pid, int heavy)
{
	if (pid >= PID_NULL || in_list(l->light, l->nlight, pid))
		return;
	if (heavy) {
		if (!in_list(l->heavy, l->nheavy, pid) && l->nheavy < l->max)
			l->heavy[l->nheavy++] = pid;
	} else if (l->nlight < l->max) {
		l->light[l->nlight++] = pid;
	}
}

void ird_psi_pids(const struct ird_psi *p, u16 *light, int *nlight,
		  u16 *heavy, int *nheavy, int max)
{
	struct pidlist l = { light, heavy, 0, 0, max };
	int i, j, pass;

	for (i = 0; i < ARRAY_SIZE(fixed_pids); i++)
		pidlist_add(&l, fixed_pids[i], 0);
	pidlist_add(&l, p->nit_pid, 0);
	for (i = 0; i < p->npmt; i++)
		pidlist_add(&l, p->pmt_pid[i], 0);
	for (i = 0; i < p->ncat; i++)
		pidlist_add(&l, p->cat_pids[i], 0);
	/* the light ones of every program first, so that a PID named both
	 * ways ends up there */
	for (pass = 0; pass < 2; pass++)
		for (i = 0; i < p->nprog; i++)
			for (j = 0; j < p->prog[i].npids; j++)
				if (p->prog[i].heavy[j] == pass)
					pidlist_add(&l, p->prog[i].pids[j], pass);
	*nlight = l.nlight;
	*nheavy = l.nheavy;
}
EXPORT_SYMBOL_GPL(ird_psi_pids);

MODULE_LICENSE("GPL");
