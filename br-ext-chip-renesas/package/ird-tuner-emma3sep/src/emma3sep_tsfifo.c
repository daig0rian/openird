// SPDX-License-Identifier: GPL-2.0-only
/*
 * Renesas EMMA3SEP: drain the transport stream input FIFOs of the AV data
 * SRAM into a ring the main CPU owns, and hand the packets to a reader.
 *
 * Written from docs/specs/poe-br100-yb1-ts-fifo.md only (§3 the SRAM, §4 the
 * FIFOs and their slots, §7 how to take the packets out).  The XDMA registers
 * are the GPL kernel's (asm/emma/emma3sep.h, drivers/mtd/nand/nand_emma.c).
 *
 * What is read
 * ------------
 * The sub-CPU's data SRAM is visible to the main CPU at 0x10030000.  Each of
 * three FIFOs has a control block there holding a queue of its free 220-byte
 * slots; the TS input hardware fills a slot with a 27 MHz arrival time and one
 * whole 188-byte packet of any PID the demultiplexer's PID table names, still
 * scrambled if it was broadcast scrambled, and the sub-CPU hands the slot back
 * when it is done with it.  A FIFO has 6 to 8 slots, and they are NOT filled
 * in address order.
 *
 * How a packet is taken
 * ---------------------
 * A high-resolution timer in hard-IRQ context looks at every slot's arrival
 * time every 100 us; a slot whose time differs from the one last taken from
 * it holds a new packet.  The new ones are taken oldest first.  All but the
 * newest are complete, because a later packet has arrived behind them; the
 * newest may still be being written (the time precedes the packet), so it is
 * taken only once the next tick sees the same time.
 *
 * How the bytes are moved
 * -----------------------
 * The SRAM answers a 32-bit read in ~0.23 us, cached or not.  One FIFO -- the
 * busy one -- is therefore copied by the SoC's memory DMA (XDMA channel 2):
 * each tick starts one transfer of the address range that holds the ready
 * slots, straight into the ring, and the next tick checks each copy and
 * queues the elements in arrival order.  The other FIFOs are copied by the
 * CPU, before the transfer starts: while the XDMA reads the SRAM, a CPU read
 * of it is six times slower.
 *
 * The XDMA reads the SRAM at ~15 MB/s, so a range of all 7 slots takes about
 * a tick.  The FIFO fills its slots round and round, and the ready ones are
 * often the last and the first: such a range is copied as two transfers.  A
 * transfer still running at the next tick is waited for, briefly, rather
 * than leaving that FIFO for a whole tick: a slot comes round again in as
 * little as 230 to 350 us, and a tick missed is how packets were lost.
 *
 * This file only ever reads the SRAM.  The one register it writes is its own
 * XDMA channel.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/jiffies.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/gfp.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/bitops.h>
#include <linux/uaccess.h>
#include <asm/io.h>
#include <asm/addrspace.h>
#include <asm/mipsregs.h>

#include "emma3sep_av.h"

#define SRAM_PHYS	0x10030000ul
#define SRAM_LEN	0x10000ul
#define FIFO_CTL	0xec00		/* control block n at + 0x80 * n */
#define FIFO_CTL_QUEUE	0x14		/* 7 words: the free slots */
#define FIFO_N		EMMA3SEP_TSFIFO_N
#define SLOTS_MAX	16		/* seen: 6, 7 and 8 */
#define SLOT_ATS	0x0c		/* arrival time, 27 MHz */
#define SLOT_PKT	0x10
#define SLOT_STRIDE	0xdc		/* 220: slots are this far apart */
#define TS_BYTES	188
#define TS_WORDS	(TS_BYTES / 4)
#define ELEM		SLOT_STRIDE	/* ring element = arrival time + packet */

/*
 * How it drains.  All are taken when draining starts (at the first tune
 * after an open), so they can be changed at run time to compare
 * (/sys/module/emma3sep_tsfifo/parameters/).  The defaults lost no packet in
 * 2.2 million on the busiest terrestrial multiplexes (spec §7).
 */
static unsigned int period_us = 100;
module_param(period_us, uint, 0644);
MODULE_PARM_DESC(period_us, "tick in microseconds (default 100, at least 25)");
static int use_xdma = 1;
module_param(use_xdma, int, 0644);
MODULE_PARM_DESC(use_xdma, "copy the busy FIFO by XDMA (1, default) or by the CPU (0)");
static unsigned int wait_us = 40;
module_param(wait_us, uint, 0644);
MODULE_PARM_DESC(wait_us, "wait up to this long for a transfer still running at a tick (default 40, at most half a tick)");
static unsigned int split_span = 5;
module_param(split_span, uint, 0644);
MODULE_PARM_DESC(split_span, "copy a range of this many slots or more as two transfers when 2 or more in it are not ready (default 5, 0: never)");
static unsigned int cur_period_us = 100;	/* the ones in use */
static unsigned int cur_wait_us, cur_split_span;

#define WAKE_PKTS	64		/* wake the reader for this many ... */
#define WAKE_TICKS	(10000 / cur_period_us)	/* ... or every 10 ms */

static void __iomem *sram;
static unsigned int fifos;		/* bit mask being drained */
static int xdma_fifo = -1;
static int running;

/* per FIFO, slots in address order */
static u32 slot_off[FIFO_N][SLOTS_MAX];
static int nslots[FIFO_N];
static u32 done_ats[FIFO_N][SLOTS_MAX];	/* arrival time last taken */
static u32 pend_ats[FIFO_N][SLOTS_MAX];	/* newest, seen once */
static u8 pend_ok[FIFO_N][SLOTS_MAX];
static u32 ref_ats[FIFO_N];		/* newest arrival taken */

/*
 * The ring: ring_n elements of ELEM bytes, physically contiguous, and
 * SLOTS_MAX spare ones after them so that a transfer never has to be cut where
 * the ring wraps.  Positions are counted in elements, virtually (wpos only
 * grows); element v lives at v % ring_n, or, when its transfer began before
 * the end, in the spare elements.
 *
 * The XDMA writes the slots into it in ADDRESS order; the reader must see
 * ARRIVAL order.  So what the reader gets is a queue of element numbers in
 * arrival order.  Each entry also says up to which virtual position the ring
 * is free once it has been read (free_to), so the writer learns what it may
 * reuse from how far the reader has got (qtail) alone.
 *
 * No line of the ring is ever dirty in the data cache: the DMA writes it, the
 * CPU writes its own copies through the uncached alias, and the reader only
 * reads.  An element is invalidated after it is written and before it is
 * queued.  That matters because a 220-byte element does not fill whole cache
 * lines -- neighbours share one -- and a dirty line written back would
 * overwrite what the DMA put in the neighbour.
 */
struct qent {
	u32 elem;		/* physical element */
	u32 free_to;		/* the ring below this is free once read */
};
static u8 *ring;
static int ring_order;
static u32 ring_n;
static struct qent *q;
static int q_order;
static u32 q_n;				/* a power of two, > ring_n */
static u32 wpos;			/* virtual write position */
static u32 qwr;				/* entries written; published as qhead */
static volatile u32 qhead, qtail;
static DECLARE_WAIT_QUEUE_HEAD(rd_waitq);
static int capturing;

/* the reader's: the rest of a packet it took part of */
static u8 carry[TS_BYTES];
static unsigned int carry_off = TS_BYTES;

static struct hrtimer timer;
static ktime_t period;
static u32 count_per_us;		/* CP0 Count rate */

/* counters; the timer callback is the only writer */
static u32 st_ticks, st_late, st_deferred, st_torn, st_overrun, st_sync;
static u32 st_pkts[FIFO_N], st_dma, st_dma_slow, st_tap_lost, st_max_ns;
/* how many new packets a tick found in a FIFO (the ready ones and the newest) */
static u32 st_seen[FIFO_N][SLOTS_MAX + 1];
static u32 st_span[SLOTS_MAX + 1];	/* slots a transfer spanned */
static u32 st_dma_wait, st_wait_dus;	/* waits that ended it, their 0.1 us */
static u32 st_split, st_split_dus, st_split_slow;	/* two-part transfers */

/* --- the tap: copies of chosen PIDs' packets for the PSI follower ---------- */

#define TAP_N		64		/* a power of two */
static unsigned long tap_pids[8192 / BITS_PER_LONG];
static u8 (*tap_buf)[TS_BYTES];
static volatile u32 tap_head, tap_tail;
static DECLARE_WAIT_QUEUE_HEAD(tap_waitq);

static void tap_put(const u32 *words)
{
	if (tap_head - tap_tail >= TAP_N) {
		st_tap_lost++;
		return;
	}
	memcpy(tap_buf[tap_head & (TAP_N - 1)], words, TS_BYTES);
	barrier();
	tap_head++;
	if (waitqueue_active(&tap_waitq))
		wake_up_interruptible(&tap_waitq);
}

void emma3sep_tsfifo_tap_clear(void)
{
	memset(tap_pids, 0, sizeof(tap_pids));
	tap_tail = tap_head;
}
EXPORT_SYMBOL_GPL(emma3sep_tsfifo_tap_clear);

void emma3sep_tsfifo_tap_pid(unsigned int pid)
{
	if (pid < 8192)
		set_bit(pid, tap_pids);
}
EXPORT_SYMBOL_GPL(emma3sep_tsfifo_tap_pid);

int emma3sep_tsfifo_tap_read(u8 *pkt, unsigned int timeout_ms)
{
	long r = wait_event_interruptible_timeout(tap_waitq, tap_head != tap_tail,
						  msecs_to_jiffies(timeout_ms));

	if (r < 0)
		return r;
	if (tap_head == tap_tail)
		return 0;
	memcpy(pkt, tap_buf[tap_tail & (TAP_N - 1)], TS_BYTES);
	barrier();
	tap_tail++;
	return 1;
}
EXPORT_SYMBOL_GPL(emma3sep_tsfifo_tap_read);

/* --- which slots hold packets ---------------------------------------------- */

static inline u32 slot_ats(int f, int k)
{
	return __raw_readl(sram + slot_off[f][k] + SLOT_ATS);
}

/* Which slots of FIFO f hold packets ready to take, oldest first.  *seen is
 * how many new packets there were in all (the ready ones plus a newest one
 * held back). */
static int ready_slots(int f, int *rdy, u32 *rdy_ats, int *seen)
{
	int cand[SLOTS_MAX], n = 0, i, j, r = 0, ns = nslots[f];
	u32 ats[SLOTS_MAX];

	for (i = 0; i < ns; i++) {
		ats[i] = slot_ats(f, i);
		if (ats[i] != done_ats[f][i])
			cand[n++] = i;
	}
	/* by arrival time; the 32-bit counter wraps, so compare differences */
	for (i = 1; i < n; i++) {
		int c = cand[i];

		for (j = i; j > 0 && (s32)(ats[cand[j - 1]] - ref_ats[f]) >
				      (s32)(ats[c] - ref_ats[f]); j--)
			cand[j] = cand[j - 1];
		cand[j] = c;
	}
	for (i = 0; i < n; i++) {
		int k = cand[i];

		if (i == n - 1 && !(pend_ok[f][k] && pend_ats[f][k] == ats[k])) {
			pend_ats[f][k] = ats[k];	/* newest: wait one tick */
			pend_ok[f][k] = 1;
			st_deferred++;
			continue;
		}
		rdy[r] = k;
		rdy_ats[r++] = ats[k];
	}
	*seen = n;
	st_seen[f][n]++;
	return r;
}

static void mark_taken(int f, int k, u32 ats)
{
	done_ats[f][k] = ats;
	pend_ok[f][k] = 0;
}

/* A checked packet (h = its first word): count it; returns 1. */
static int got(int f, u32 ats, u32 h)
{
	if ((h >> 24) != 0x47)
		st_sync++;
	ref_ats[f] = ats;
	st_pkts[f]++;
	return 1;
}

/* --- the ring -------------------------------------------------------------- */

/* The physical element where n more may be written at wpos, or -1 if the
 * reader has not freed that much yet (no reader: the ring fills and stays
 * full, and every packet is an overrun). */
static int ring_room(u32 n)
{
	u32 free_to = q[(qtail - 1) & (q_n - 1)].free_to;

	if (wpos + n - free_to > ring_n)
		return -1;
	return wpos % ring_n;
}

static void queue_elem(u32 elem, u32 free_to)
{
	struct qent *e = &q[qwr & (q_n - 1)];

	e->elem = elem;
	e->free_to = free_to;
	qwr++;
}

/* --- CPU copy: 47 loads a packet ------------------------------------------- */

static u32 scratch[ELEM / 4];		/* where a packet goes on overrun */
static int dma_busy;			/* an XDMA transfer has been started */
static int batch_p;			/* its first element, -1 = stage */
static u32 batch_v;			/* its virtual position */

static void drain_cpu(int f)
{
	int rdy[SLOTS_MAX], n, i, j, seen;
	u32 rdy_ats[SLOTS_MAX];

	n = ready_slots(f, rdy, rdy_ats, &seen);
	for (i = 0; i < n; i++) {
		void __iomem *slot = sram + slot_off[f][rdy[i]];
		int p = ring_room(1);
		u32 *e = p < 0 ? scratch :
			 (u32 *)KSEG1ADDR(ring + p * ELEM);	/* uncached */
		u32 h = __raw_readl(slot + SLOT_PKT);

		mark_taken(f, rdy[i], rdy_ats[i]);
		e[0] = rdy_ats[i];
		e[1] = h;
		for (j = 1; j < TS_WORDS; j++)
			e[1 + j] = __raw_readl(slot + SLOT_PKT + 4 * j);
		if (slot_ats(f, rdy[i]) != rdy_ats[i]) {
			st_torn++;		/* reused while we copied */
			continue;
		}
		got(f, rdy_ats[i], h);
		if (test_bit((h >> 8) & 0x1fff, tap_pids))
			tap_put(e + 1);
		if (p < 0) {
			st_overrun++;		/* the reader's loss, not ours */
			continue;
		}
		dma_cache_inv((unsigned long)(ring + p * ELEM), ELEM);
		wpos++;
		/* A transfer still in flight lies before this element, and its
		 * elements are queued only when it is collected: reading this
		 * one must not free them. */
		queue_elem(p, dma_busy && batch_p >= 0 ? batch_v : wpos);
	}
}

/* --- XDMA: the SoC's memory DMA copies the slots into the ring -------------
 *
 * Channel n is at MEM_LIST0 + 0x20 * n; LIST_ENABLE bit n starts it and
 * MEM_LIST2 counts down to 0 when it is done.  From the SRAM to DRAM the
 * setup word is: increment source, increment destination, destination is
 * DRAM (spec §7.4).  Channel 2: the NAND driver has 0 and the vendor
 * application 1.
 */
#define XDMA_PHYS	0x10009000ul
#define X_ENABLE	0x088
#define XCH		2
#define X_LIST(n)	(0x100 + 0x20 * XCH + 4 * (n))
#define X_SETUP		((1u << 29) | (1u << 28) | (1u << 27))
#define STAGE_LEN	(SLOTS_MAX * ELEM)

static void __iomem *xdma;
static u8 *stage;		/* where a transfer goes when the ring is full */
static int batch_f, batch_n, batch_k[SLOTS_MAX], batch_e[SLOTS_MAX];
static u8 batch_safe[SLOTS_MAX];
static u32 batch_ats[SLOTS_MAX], batch_len, batch_span;

/*
 * Is the copy of a slot trustworthy without looking at the SRAM again?
 *
 * The transfer reads each slot from its arrival time on, so the arrival time
 * in the copy says the slot still held our packet when the DMA got there.
 * What it cannot say is whether the hardware began rewriting the slot while
 * the DMA was still reading its 188 bytes.  For that the slot must first be
 * freed and come to the head of the free queue again: at least N - 1 more
 * packets (N = the FIFO's slot count), and packets arrive no closer than
 * 32 us apart.  With m packets already behind ours when the transfer starts,
 * and allowing one more to slip in before it does, the rewrite cannot begin
 * sooner than (N - 1 - m - 2) * 32 us after the start, while the DMA has
 * finished reading the slot at position p of the span by about
 * 2 + 13 * (p + 1) us, plus however long the tick spent between the scan
 * and the start (late_us).  (Spec §7.4.)
 *
 * For a safe slot the SRAM is not read again at collection time: by then,
 * one tick later, the slot may well have been reused legitimately.  An
 * unsafe one (we are behind) is checked against the SRAM and dropped if it
 * moved.
 */
static int copy_is_safe(int ns, int m, int p, u32 late_us)
{
	int left = ns - 1 - m - 2;

	return left > 0 && left * 32 > late_us + 2 + 13 * (p + 1);
}

/* Collect the last transfer: check each copy and queue the elements in
 * ARRIVAL order.  A transfer still running is waited for up to wait_us (it
 * is nearly always nearly done); returns -1 if it still is. */
static int harvest(void)
{
	int i;
	u8 *base;

	if (!dma_busy)
		return 0;
	if (__raw_readl(xdma + X_LIST(2)) != 0) {
		u32 c0 = read_c0_count(), lim = cur_wait_us * count_per_us;

		do {
			if (read_c0_count() - c0 >= lim) {
				st_dma_slow++;	/* still running: next tick */
				return -1;
			}
		} while (__raw_readl(xdma + X_LIST(2)) != 0);
		st_dma_wait++;
		st_wait_dus += (read_c0_count() - c0) * 10 / count_per_us;
	}
	dma_busy = 0;
	base = batch_p < 0 ? stage : ring + batch_p * ELEM;
	dma_cache_inv((unsigned long)base, batch_len);
	for (i = 0; i < batch_n; i++) {
		int k = batch_e[i];
		const u32 *c = (const u32 *)(base + k * ELEM);
		int ok = c[0] == batch_ats[i];

		if (ok && !batch_safe[i] &&
		    slot_ats(batch_f, batch_k[i]) != batch_ats[i])
			ok = 0;			/* we were behind, and it moved */
		if (!ok) {
			st_torn++;
			continue;
		}
		got(batch_f, batch_ats[i], c[1]);
		if (batch_p < 0)
			st_overrun++;		/* the reader's loss, not ours */
		else
			/* the last one frees the span, the others what is
			 * before it */
			queue_elem(batch_p + k, i == batch_n - 1 ?
				   batch_v + batch_span : batch_v);
	}
	return 0;
}

/* Start one transfer of the slots lo..hi of FIFO f to dst. */
static void xdma_go(int f, int lo, int hi, u8 *dst)
{
	__raw_writel(SRAM_PHYS + slot_off[f][lo] + SLOT_ATS, xdma + X_LIST(0));
	__raw_writel(virt_to_phys(dst), xdma + X_LIST(1));
	__raw_writel((hi - lo) * ELEM + 4 + TS_BYTES, xdma + X_LIST(2));
	__raw_writel(X_SETUP, xdma + X_LIST(3));
	__raw_writel(0, xdma + X_LIST(4));
	__raw_writel(1u << XCH, xdma + X_ENABLE);
}

/*
 * One transfer copies the address range that holds this tick's ready slots,
 * taking along any slot in between (it is not queued).  The FIFO fills its
 * slots round and round, so the ready ones are often the last and the first,
 * and the range is then the whole FIFO: about 100 us of transfer for two
 * packets, longer than a tick.  So a long range with slots in between is
 * copied as two: the shorter part first, waiting for it (some 13 us a slot),
 * then the other, which the next tick collects.  The elements go where they
 * would have gone anyway, side by side in address order.
 */
static void start_batch(int f, const int *rdy, const u32 *rdy_ats, int n,
			int seen, u32 late_us)
{
	int i, k, lo = rdy[0], hi = rdy[0], nseg = 1, a = 0, b = 1, drop_b = 0;
	int seg_lo[2] = { 0 }, seg_hi[2] = { 0 }, seg_n[2] = { 0 };
	int seg_off[2] = { 0 };
	u8 in[SLOTS_MAX] = { 0 };
	u8 *dst;

	for (i = 0; i < n; i++) {
		in[rdy[i]] = 1;
		if (rdy[i] < lo)
			lo = rdy[i];
		if (rdy[i] > hi)
			hi = rdy[i];
	}
	seg_lo[0] = lo;
	seg_hi[0] = hi;
	if (cur_split_span && hi - lo + 1 >= cur_split_span) {
		int run = 0, best = 0, end = 0;

		for (k = lo + 1; k < hi; k++) {	/* the widest gap */
			run = in[k] ? 0 : run + 1;
			if (run > best) {
				best = run;
				end = k;
			}
		}
		if (best >= 2) {
			seg_hi[0] = end - best;
			seg_lo[1] = end + 1;
			seg_hi[1] = hi;
			nseg = 2;
		}
	}
	seg_n[0] = seg_hi[0] - seg_lo[0] + 1;
	seg_off[0] = 0;
	batch_span = seg_n[0];
	if (nseg == 2) {
		seg_n[1] = seg_hi[1] - seg_lo[1] + 1;
		seg_off[1] = seg_n[0];
		batch_span += seg_n[1];
		if (seg_n[1] < seg_n[0]) {
			a = 1;
			b = 0;
		}
	}
	st_span[hi - lo + 1]++;
	batch_p = ring_room(batch_span);
	batch_v = wpos;
	if (batch_p >= 0) {
		wpos += batch_span;
		dst = ring + batch_p * ELEM;
	} else {
		dst = stage;			/* only to count what is lost */
	}
	batch_len = (batch_span - 1) * ELEM + 4 + TS_BYTES;

	if (nseg == 1) {
		xdma_go(f, lo, hi, dst);
	} else {
		u32 c0, lim;

		xdma_go(f, seg_lo[a], seg_hi[a], dst + seg_off[a] * ELEM);
		c0 = read_c0_count();
		lim = (20 + 20 * seg_n[a]) * count_per_us;
		while (__raw_readl(xdma + X_LIST(2)) != 0)
			if (read_c0_count() - c0 > lim) {
				drop_b = 1;	/* not now: they stay for the next tick */
				st_split_slow++;
				break;
			}
		if (!drop_b)
			xdma_go(f, seg_lo[b], seg_hi[b], dst + seg_off[b] * ELEM);
		st_split++;
		st_split_dus += (read_c0_count() - c0) * 10 / count_per_us;
	}
	dma_busy = 1;
	st_dma++;

	batch_f = f;
	batch_n = 0;
	for (i = 0; i < n; i++) {
		int s = nseg == 2 && rdy[i] >= seg_lo[1];
		int p;			/* how many slots the DMA reads up to it */

		if (drop_b && s == b)
			continue;
		if (nseg == 1)
			p = rdy[i] - lo;
		else if (s == a)
			p = rdy[i] - seg_lo[a];
		else
			p = seg_n[a] + rdy[i] - seg_lo[b];
		batch_k[batch_n] = rdy[i];
		batch_e[batch_n] = seg_off[s] + rdy[i] - seg_lo[s];
		batch_ats[batch_n] = rdy_ats[i];
		/* rdy[] is oldest first, so seen - 1 - i packets are behind it */
		batch_safe[batch_n] = copy_is_safe(nslots[f], seen - 1 - i, p,
						   late_us);
		mark_taken(f, rdy[i], rdy_ats[i]);
		batch_n++;
	}
}

/* --- the tick -------------------------------------------------------------- */

static enum hrtimer_restart tick(struct hrtimer *t)
{
	int x_rdy[SLOTS_MAX], x_n = 0, x_seen = 0, f;
	u32 x_rdy_ats[SLOTS_MAX], c0 = read_c0_count(), x_c0 = 0, ns;
	u64 missed;

	missed = hrtimer_forward_now(t, period);
	if (missed > 1)
		st_late += (u32)(missed - 1);
	st_ticks++;

	/* 1. collect the last transfer, find what the XDMA FIFO has now */
	if (xdma_fifo >= 0 && harvest() == 0) {
		x_n = ready_slots(xdma_fifo, x_rdy, x_rdy_ats, &x_seen);
		x_c0 = read_c0_count();
	}
	/* 2. the CPU-copied FIFOs, while the SRAM port is ours alone */
	for (f = 0; f < FIFO_N; f++)
		if ((fifos & (1u << f)) && f != xdma_fifo)
			drain_cpu(f);
	/* 3. start this tick's transfer */
	if (x_n)
		start_batch(xdma_fifo, x_rdy, x_rdy_ats, x_n, x_seen,
			    (read_c0_count() - x_c0) / count_per_us + 1);
	/* 4. publish what is queued */
	barrier();
	qhead = qwr;
	/* Waking the reader for every packet would cost a context switch per
	 * tick; batch it, but never hold packets back for more than 10 ms --
	 * and only when it is actually waiting. */
	if (qhead != qtail && waitqueue_active(&rd_waitq) &&
	    (qhead - qtail >= WAKE_PKTS || st_ticks % WAKE_TICKS == 0))
		wake_up_interruptible(&rd_waitq);

	ns = (read_c0_count() - c0) * 1000 / count_per_us;
	if (ns > st_max_ns)
		st_max_ns = ns;
	return HRTIMER_RESTART;
}

/* --- the reader ------------------------------------------------------------ */

void emma3sep_tsfifo_capture(int on)
{
	if (on) {
		qtail = qhead;			/* start from now */
		carry_off = TS_BYTES;
	}
	capturing = on;
	if (!on)
		wake_up_interruptible(&rd_waitq);
}
EXPORT_SYMBOL_GPL(emma3sep_tsfifo_capture);

static const u8 *elem_packet(u32 qpos)
{
	const struct qent *e = &q[qpos & (q_n - 1)];

	return ring + e->elem * ELEM + 4;
}

ssize_t emma3sep_tsfifo_read(char __user *buf, size_t count, int nonblock)
{
	size_t done = 0, n;
	int r;

	if (carry_off < TS_BYTES) {		/* the rest of a packet first */
		n = min(count, (size_t)(TS_BYTES - carry_off));
		if (copy_to_user(buf, carry + carry_off, n))
			return -EFAULT;
		carry_off += n;
		done = n;
	}
	if (done == count)
		return done;
	if (qhead == qtail) {
		if (done)
			return done;
		if (nonblock)
			return capturing ? -EAGAIN : 0;
		r = wait_event_interruptible(rd_waitq,
					     qhead != qtail || !capturing);
		if (r)
			return -EINTR;
		if (qhead == qtail)
			return 0;		/* capture was stopped */
	}
	while (count - done >= TS_BYTES && qtail != qhead) {
		barrier();			/* the entry, after qhead */
		if (copy_to_user(buf + done, elem_packet(qtail), TS_BYTES))
			return done ? (ssize_t)done : -EFAULT;
		done += TS_BYTES;
		barrier();			/* read: let the element go */
		qtail++;
	}
	if (done < count && count - done < TS_BYTES && qtail != qhead) {
		/* room for part of a packet only: keep the rest for next time */
		n = count - done;
		memcpy(carry, elem_packet(qtail), TS_BYTES);
		barrier();
		qtail++;
		if (copy_to_user(buf + done, carry, n))
			return done ? (ssize_t)done : -EFAULT;
		carry_off = n;
		done += n;
	}
	return done;
}
EXPORT_SYMBOL_GPL(emma3sep_tsfifo_read);

/* --- slots ----------------------------------------------------------------- */

/* Returns 1 if o is a slot of FIFO f we had not seen yet. */
static int add_slot(int f, u32 o)
{
	int k;

	if (!o || (o & 3) || o > SRAM_LEN - SLOT_PKT - TS_BYTES)
		return 0;
	for (k = 0; k < nslots[f]; k++)
		if (slot_off[f][k] == o)
			return 0;
	if (nslots[f] >= SLOTS_MAX)
		return 0;
	slot_off[f][nslots[f]++] = o;
	return 1;
}

int emma3sep_tsfifo_slots(int fifo)
{
	return fifo >= 0 && fifo < FIFO_N ? nslots[fifo] : 0;
}
EXPORT_SYMBOL_GPL(emma3sep_tsfifo_slots);

/*
 * The seven-word list in the control block is a queue the sub-CPU rotates
 * while we read it; whatever slot is being filled or handled at the moment
 * is missing from it -- and a FIFO can have more slots than the list has
 * words.  Read it for at least 100 ms and until nothing new has turned up
 * for 50 ms (1 s at most), keep the union, then check that the slots are
 * evenly spaced: one not seen would leave a gap.
 */
int emma3sep_tsfifo_find_slots(unsigned int mask)
{
	int f, k, r, quiet = 0;

	if (running)
		return -EBUSY;
	for (f = 0; f < FIFO_N; f++)
		if (mask & (1u << f))
			nslots[f] = 0;
	for (r = 0; r < 500; r++) {
		int fresh = 0, w;

		for (w = 0; w < 200; w++)
			for (f = 0; f < FIFO_N; f++) {
				if (!(mask & (1u << f)))
					continue;
				for (k = 0; k < 7; k++)
					fresh |= add_slot(f, __raw_readl(sram +
						FIFO_CTL + f * 0x80 +
						FIFO_CTL_QUEUE + 4 * k));
			}
		quiet = fresh ? 0 : quiet + 1;
		if (r >= 50 && quiet >= 25)
			break;
		msleep(2);
	}
	for (f = 0; f < FIFO_N; f++) {
		if (!(mask & (1u << f)))
			continue;
		for (k = 1; k < nslots[f]; k++) {	/* address order */
			u32 o = slot_off[f][k];
			int j;

			for (j = k; j > 0 && slot_off[f][j - 1] > o; j--)
				slot_off[f][j] = slot_off[f][j - 1];
			slot_off[f][j] = o;
		}
		for (k = 1; k < nslots[f]; k++)
			if (slot_off[f][k] - slot_off[f][k - 1] != SLOT_STRIDE)
				break;
		if (nslots[f] < 3 || k < nslots[f]) {
			printk(KERN_ERR "emma3sep-tsfifo: FIFO %d: %d slots seen, "
			       "not a run of %u-byte slots\n", f, nslots[f],
			       SLOT_STRIDE);
			nslots[f] = 0;
			return -ENODEV;
		}
	}
	return 0;
}
EXPORT_SYMBOL_GPL(emma3sep_tsfifo_find_slots);

/* --- start / stop ---------------------------------------------------------- */

int emma3sep_tsfifo_start(unsigned int mask, int by_xdma)
{
	int f, k;

	if (running)
		return -EBUSY;
	if (!(mask & 7) || (by_xdma >= 0 && !(mask & (1u << by_xdma))))
		return -EINVAL;
	for (f = 0; f < FIFO_N; f++)
		if ((mask & (1u << f)) && !nslots[f])
			return -ENODEV;
	if (__raw_readl(xdma + X_ENABLE) & (1u << XCH))
		return -EBUSY;			/* somebody else's transfer */

	/* What is in the slots now is old: start after the newest. */
	for (f = 0; f < FIFO_N; f++)
		for (k = 0; k < nslots[f]; k++) {
			done_ats[f][k] = slot_ats(f, k);
			pend_ok[f][k] = 0;
			if (k == 0 || (s32)(done_ats[f][k] - ref_ats[f]) > 0)
				ref_ats[f] = done_ats[f][k];
		}
	fifos = mask;
	xdma_fifo = use_xdma ? by_xdma : -1;
	dma_busy = 0;
	cur_period_us = max(period_us, 25u);
	cur_wait_us = min(wait_us, cur_period_us / 2);
	cur_split_span = split_span;
	period = ktime_set(0, cur_period_us * 1000);
	hrtimer_init(&timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	timer.function = tick;
	timer.cb_mode = HRTIMER_CB_IRQSAFE_UNLOCKED;	/* hard IRQ, may wake */
	running = 1;
	hrtimer_start(&timer, period, HRTIMER_MODE_REL);
	return 0;
}
EXPORT_SYMBOL_GPL(emma3sep_tsfifo_start);

void emma3sep_tsfifo_stop(void)
{
	int i;

	if (!running)
		return;
	hrtimer_cancel(&timer);
	running = 0;
	/* a transfer still in flight must land before anyone reuses the ring */
	for (i = 0; dma_busy && i < 1000; i++) {
		if (__raw_readl(xdma + X_LIST(2)) == 0)
			dma_busy = 0;
		else
			udelay(10);
	}
	if (dma_busy)
		printk(KERN_ERR "emma3sep-tsfifo: XDMA channel %d did not finish\n",
		       XCH);
	/* nothing half-collected survives a restart */
	qhead = qwr;
}
EXPORT_SYMBOL_GPL(emma3sep_tsfifo_stop);

int emma3sep_tsfifo_show(char *page)
{
	int len = 0, f, k;

	len += sprintf(page + len, "drainer %s period_us %u ring %u x %u B\n",
		       running ? "running" : "stopped", cur_period_us, ring_n, ELEM);
	for (f = 0; f < FIFO_N; f++) {
		if (!nslots[f])
			continue;
		len += sprintf(page + len, "fifo %d%s: %d slots", f,
			       !(fifos & (1u << f)) ? " (idle)" :
			       f == xdma_fifo ? " (xdma)" : " (cpu)", nslots[f]);
		for (k = 0; k < nslots[f]; k++)
			len += sprintf(page + len, " %04x", slot_off[f][k]);
		len += sprintf(page + len, "  packets %u\n", st_pkts[f]);
		len += sprintf(page + len, "fifo %d new packets a tick (0..%d):", f,
			       nslots[f]);
		for (k = 0; k <= nslots[f]; k++)
			len += sprintf(page + len, " %u", st_seen[f][k]);
		len += sprintf(page + len, "\n");
	}
	len += sprintf(page + len, "ticks %u late %u max_tick_ns %u\n",
		       st_ticks, st_late, st_max_ns);
	len += sprintf(page + len, "deferred %u torn %u overrun %u sync_err %u "
		       "tap_lost %u\n", st_deferred, st_torn, st_overrun,
		       st_sync, st_tap_lost);
	len += sprintf(page + len, "xdma transfers %u still_running %u "
		       "waited %u for %u.%u us (wait_us %u)\n", st_dma, st_dma_slow,
		       st_dma_wait, st_wait_dus / 10, st_wait_dus % 10, cur_wait_us);
	len += sprintf(page + len, "xdma split %u for %u.%u us, not finished %u "
		       "(split_span %u)\n", st_split, st_split_dus / 10,
		       st_split_dus % 10, st_split_slow, cur_split_span);
	len += sprintf(page + len, "xdma slots a transfer spanned (0..%d):",
		       SLOTS_MAX);
	for (k = 0; k <= SLOTS_MAX; k++)
		len += sprintf(page + len, " %u", st_span[k]);
	len += sprintf(page + len, "\n");
	len += sprintf(page + len, "queue head %u tail %u capturing %d\n",
		       qhead, qtail, capturing);
	return len;
}
EXPORT_SYMBOL_GPL(emma3sep_tsfifo_show);

int emma3sep_tsfifo_init(void)
{
	ktime_t t0;
	u32 c0, us;

	sram = ioremap_nocache(SRAM_PHYS, SRAM_LEN);
	xdma = ioremap_nocache(XDMA_PHYS, 0x1000);
	if (!sram || !xdma)
		return -ENOMEM;

	/* CP0 Count runs at a fixed fraction of the CPU clock; measure it */
	c0 = read_c0_count();
	t0 = ktime_get();
	msleep(50);
	us = (u32)ktime_to_ns(ktime_sub(ktime_get(), t0)) / 1000;
	count_per_us = (read_c0_count() - c0) / us;
	if (!count_per_us)
		count_per_us = 1;

	/* the ring: as large a physically contiguous block as we can get */
	for (ring_order = 10; ring_order >= 6; ring_order--) {
		ring = (u8 *)__get_free_pages(GFP_KERNEL | __GFP_NOWARN,
					      ring_order);
		if (ring)
			break;
	}
	if (!ring)
		return -ENOMEM;
	ring_n = (PAGE_SIZE << ring_order) / ELEM - SLOTS_MAX;
	for (q_n = 1; q_n <= ring_n; q_n <<= 1)
		;
	q_order = get_order(q_n * sizeof(*q));
	q = (struct qent *)__get_free_pages(GFP_KERNEL | __GFP_ZERO, q_order);
	stage = kmalloc(STAGE_LEN, GFP_KERNEL);
	tap_buf = kmalloc(TAP_N * TS_BYTES, GFP_KERNEL);
	if (!q || !stage || !tap_buf)
		return -ENOMEM;
	/* no line of the ring may be dirty (see "The ring") */
	dma_cache_wback_inv((unsigned long)ring, PAGE_SIZE << ring_order);
	dma_cache_wback_inv((unsigned long)stage, STAGE_LEN);
	return 0;
}
EXPORT_SYMBOL_GPL(emma3sep_tsfifo_init);

void emma3sep_tsfifo_exit(void)
{
	emma3sep_tsfifo_stop();
	if (!dma_busy) {		/* else leak rather than let DMA scribble */
		if (ring)
			free_pages((unsigned long)ring, ring_order);
		kfree(stage);
	}
	if (q)
		free_pages((unsigned long)q, q_order);
	kfree(tap_buf);
	if (xdma)
		iounmap(xdma);
	if (sram)
		iounmap(sram);
}
EXPORT_SYMBOL_GPL(emma3sep_tsfifo_exit);
