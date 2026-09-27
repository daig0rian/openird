// SPDX-License-Identifier: GPL-2.0-only
/*
 * yb1-fifodrain - drain the TS input FIFOs of the POE-BR100-YB1 AV data SRAM
 * into a ring the main CPU owns, from a high-resolution timer, and hand the
 * packets to userland through a character device.
 *
 * What it reads (docs/specs/poe-br100-yb1-reception-stack.md §73, §77)
 * -------------------------------------------------------------------
 * The AV sub-CPU's data SRAM is visible to the main CPU at 0x10030000..
 * 0x1003ffff.  Three FIFO control blocks sit at SRAM offset 0xec00 + 0x80*n;
 * words 5..11 of each are a queue of the FIFO's free 220-byte slots.  A slot
 * holds a 27 MHz arrival time at +0x0c and one whole 188-byte TS packet at
 * +0x10.  The TS input hardware fills them with the packets of every PID the
 * demultiplexer's PID table holds for that FIFO's device (36 filters per
 * device), scrambled or not; the sub-CPU hands each slot back when it is done
 * with it.  FIFO 0 has 7 slots, FIFO 1 6, FIFO 2 8, and with many PIDs they
 * are NOT filled in address order.
 *
 * Userland cannot keep up with that (spec §73.10: 99.7 % with the CPU pinned,
 * lost in the 7-8 ms it is preempted).  An hrtimer callback in hard-IRQ
 * context can: it runs every period_us whatever the scheduler is doing.
 *
 * How a packet is taken
 * ---------------------
 * Every tick reads every slot's arrival time; a slot whose time differs from
 * the one we last took from it holds a new packet.  The new ones are taken
 * oldest first.  All but the newest are complete, because a later packet has
 * already arrived behind them; the newest may still be being written (the
 * arrival time precedes the packet in the slot), so it is taken only when the
 * next tick sees the same arrival time.
 *
 * How the bytes are moved
 * -----------------------
 * The SRAM answers every 32-bit read in ~0.23 us, cached or not (sramspeed.ko),
 * so a CPU copy costs ~15.6 us a packet (spec §74.3).  One FIFO (xdma_fifo) is
 * copied by the SoC's memory DMA instead (XDMA channel 2, see the XDMA
 * section): each tick starts one transfer over the address range holding the
 * ready slots into a staging buffer, and the next tick checks each copy and
 * puts the packets into the ring in arrival order.  The other enabled FIFOs
 * -- meant for the light PIDs -- are copied by the CPU in the same tick.
 * use_xdma=0 copies everything with the CPU.
 *
 * The module only ever reads the SRAM.  It never writes to it, never talks to
 * the sub-CPU, and does not touch the descrambler.  The only register it
 * writes is its own XDMA channel.  Which PIDs arrive is set up beforehand
 * with the demultiplexer ioctls (spec §43, §73.9, §77).
 *
 * Interfaces
 * ----------
 *   /dev/<misc "yb1fifo">  read(): whole 188-byte packets; within a PID in
 *                          arrival order.  open() starts from "now".
 *   /proc/yb1fifo          counters: packets, torn, overrun, continuity gaps
 *                          per PID, and the time the timer callback costs.
 *
 * Parameters: period_us (default 100), fifos (bit mask, default 1 = FIFO 0),
 * wake_pkts (wake the reader when this many wait, or every 10 ms; 64),
 * use_xdma (1 = XDMA for one FIFO, default; 0 = CPU copy only),
 * xdma_fifo (the FIFO the XDMA copies; default the highest enabled).
 *
 * Load it after the demultiplexer has been configured: the slot list is read
 * once, at load time, and loading fails if the chosen FIFOs are not set up.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/jiffies.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/gfp.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <asm/io.h>
#include <asm/mipsregs.h>

#define SRAM_PHYS	0x10030000ul
#define SRAM_LEN	0x10000ul
#define FIFO_CTL	0xec00
#define FIFO_N		3
#define SLOTS_MAX	16		/* measured: 7, 6 and 8 slots (spec §77) */
#define SLOT_ATS	0x0c
#define SLOT_PKT	0x10
#define SLOT_STRIDE	0xdc		/* 220: slots are this far apart */
#define TS_BYTES	188
#define TS_WORDS	(TS_BYTES / 4)
#define ELEM		SLOT_STRIDE	/* ring element = one slot image */
#define NPIDSTAT	16

static unsigned int period_us = 100;
module_param(period_us, uint, 0444);
MODULE_PARM_DESC(period_us, "polling period in microseconds (default 100)");
static unsigned int fifos = 1;
module_param(fifos, uint, 0444);
MODULE_PARM_DESC(fifos, "bit mask of FIFOs to drain (default 1 = FIFO 0)");
static unsigned int wake_pkts = 64;
module_param(wake_pkts, uint, 0444);
MODULE_PARM_DESC(wake_pkts, "wake the reader once this many packets wait "
		 "(and at least every 10 ms), default 64");
static unsigned int use_xdma = 1;
module_param(use_xdma, uint, 0444);
MODULE_PARM_DESC(use_xdma, "1 = XDMA channel 2 into the ring (default), "
		 "0 = CPU copy");

static void __iomem *sram;
/* per FIFO, slots in address order = the order the hardware fills them */
static u32 slot_off[FIFO_N][SLOTS_MAX];
static int nslots[FIFO_N];
static u32 done_ats[FIFO_N][SLOTS_MAX];	/* arrival time last taken */
static u32 pend_ats[FIFO_N][SLOTS_MAX];	/* newest, seen once */
static u8 pend_ok[FIFO_N][SLOTS_MAX];
static u32 ref_ats[FIFO_N];			/* newest arrival taken */
static int next_slot[FIFO_N];			/* written next, we expect */

/* The ring: physically contiguous, ELEM-byte slot images. */
static u8 *ring;
static int ring_order;
static u32 ring_n;
static u8 *valid;			/* element holds a good packet */
static volatile u32 head, tail;		/* element counters */
static DECLARE_WAIT_QUEUE_HEAD(fd_waitq);
static u32 wake_ticks;			/* 10 ms in ticks */

static struct hrtimer timer;
static ktime_t period;
static unsigned long start_jiffies;
static u32 count_per_us;		/* CP0 Count rate, measured at load */

/* Counters.  The callback is the only writer. */
static u32 st_ticks, st_late, st_deferred, st_torn, st_overrun, st_sync;
static u32 st_pkts[FIFO_N], st_resync;
static u32 st_ccgap, st_cclost, st_ccdup;
static u32 st_busy_us, st_busy_rem, st_max_ns;
static u32 st_idle_ticks, st_idle_us, st_idle_rem;	/* ticks that took nothing */
static u8 last_cc[8192];
static struct {
	u16 pid;
	u32 pkts, ccgap, cclost, scr[4], last_ats;
} pidstat[NPIDSTAT];
static int npidstat;

/* What each of the last four ticks went through, to explain a gap. */
#define F_FULL		1	/* all seven slots were read */
#define F_SPLIT		2	/* a batch was cut short */
#define F_BUSY		4	/* the previous transfer had not finished */
#define F_ALL7		8	/* all seven slots were new: we were behind */
#define F_TORN		16	/* a copy was dropped */
static u8 cur_flags, cur_seen, hist_flags[4], hist_seen[4];
#define NGAPLOG		16
static struct {
	u32 tick, dt_us;
	u16 pid;
	u8 from, to, flags[4], seen[4];
} gaplog[NGAPLOG];
static int ngaplog;

static void log_gap(unsigned int pid, unsigned int from, unsigned int to,
		    u32 dt)
{
	int j;

	if (ngaplog >= NGAPLOG)
		return;
	gaplog[ngaplog].tick = st_ticks;
	gaplog[ngaplog].pid = pid;
	gaplog[ngaplog].from = from;
	gaplog[ngaplog].to = to;
	gaplog[ngaplog].dt_us = dt / 27;
	gaplog[ngaplog].flags[0] = cur_flags;
	gaplog[ngaplog].seen[0] = cur_seen;
	for (j = 1; j < 4; j++) {		/* the three ticks before this one */
		gaplog[ngaplog].flags[j] = hist_flags[(st_ticks - j) & 3];
		gaplog[ngaplog].seen[j] = hist_seen[(st_ticks - j) & 3];
	}
	ngaplog++;
}

static void account(const u32 *pkt, u32 ats)
{
	u32 h = pkt[0];
	unsigned int pid = (h >> 8) & 0x1fff, cc = h & 15, i;

	if ((h >> 24) != 0x47) {
		st_sync++;
		return;
	}
	for (i = 0; i < npidstat; i++)
		if (pidstat[i].pid == pid)
			break;
	if (i == npidstat && npidstat < NPIDSTAT)
		pidstat[npidstat++].pid = pid;
	if (i < NPIDSTAT) {
		pidstat[i].pkts++;
		pidstat[i].scr[(h >> 6) & 3]++;
	}
	/* continuity_counter steps once per packet carrying payload. */
	if (!(h & 0x10))
		return;
	if (last_cc[pid] < 16 && cc != ((last_cc[pid] + 1) & 15)) {
		if (cc == last_cc[pid]) {
			st_ccdup++;
		} else {
			st_ccgap++;
			st_cclost += (cc - last_cc[pid] - 1) & 15;
			if (i < NPIDSTAT) {
				pidstat[i].ccgap++;
				pidstat[i].cclost += (cc - last_cc[pid] - 1) & 15;
				log_gap(pid, last_cc[pid], cc,
					ats - pidstat[i].last_ats);
			}
		}
	}
	last_cc[pid] = cc;
	if (i < NPIDSTAT)
		pidstat[i].last_ats = ats;
}

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

	/* Every slot, every tick, oldest first by arrival time.  The slots are
	 * NOT filled in address order in general: the list in the control
	 * block is a queue of free slots, the hardware takes the head and the
	 * sub-CPU puts a slot back when it is done with it, so with many PIDs
	 * the order drifts (spec §77; assuming address order lost packets).
	 * `resync' still counts where it differed from address order. */
	for (i = 0; i < ns; i++) {
		ats[i] = slot_ats(f, i);
		if (ats[i] != done_ats[f][i])
			cand[n++] = i;
	}
	for (i = 1; i < n; i++) {
		int c = cand[i];

		for (j = i; j > 0 && (s32)(ats[cand[j - 1]] - ref_ats[f]) >
				      (s32)(ats[c] - ref_ats[f]); j--)
			cand[j] = cand[j - 1];
		cand[j] = c;
	}
	if (n && cand[0] != next_slot[f])
		st_resync++;
	for (i = 1; i < n; i++)
		if (cand[i] != (cand[i - 1] + 1) % ns)
			st_resync++;
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
	if (n > cur_seen)
		cur_seen = n;
	if (n == ns)
		cur_flags |= F_ALL7;
	*seen = n;
	return r;
}

static void mark_taken(int f, int k, u32 ats)
{
	done_ats[f][k] = ats;
	pend_ok[f][k] = 0;
	next_slot[f] = (k + 1) % nslots[f];
}

/* A checked packet: count it; returns 1. */
static int got(int f, u32 ats, const u32 *pkt)
{
	account(pkt, ats);
	ref_ats[f] = ats;
	st_pkts[f]++;
	return 1;
}

/* --- CPU copy: 47 uncached loads, ~15.6 us a packet (spec §74.3) --------- */

static u32 scratch[ELEM / 4];

/*
 * Ring positions.  [tail, head) is published to the reader; [head, wr) has
 * been filled during the current tick and is published at its end.
 */
static u32 wr;

static int drain_cpu(int f)
{
	int rdy[SLOTS_MAX], n, i, j, took = 0, seen;
	u32 rdy_ats[SLOTS_MAX];

	n = ready_slots(f, rdy, rdy_ats, &seen);
	for (i = 0; i < n; i++) {
		void __iomem *slot = sram + slot_off[f][rdy[i]];
		int full = wr - tail >= ring_n;
		u32 *e = full ? scratch : (u32 *)(ring + (wr % ring_n) * ELEM);

		mark_taken(f, rdy[i], rdy_ats[i]);
		e[0] = rdy_ats[i];
		for (j = 0; j < TS_WORDS; j++)
			e[1 + j] = __raw_readl(slot + SLOT_PKT + 4 * j);
		if (__raw_readl(slot + SLOT_ATS) != rdy_ats[i]) {
			cur_flags |= F_TORN;
			st_torn++;		/* reused while we copied */
			continue;
		}
		took += got(f, rdy_ats[i], e + 1);
		if (full) {
			st_overrun++;		/* the reader's loss, not ours */
			continue;
		}
		valid[wr % ring_n] = 1;
		wr++;
	}
	return took;
}

/* --- XDMA: the SoC's memory DMA copies the slots into the ring -------------
 *
 * The SRAM costs a DMA engine the same ~0.23 us a word as the CPU
 * (xdmaspeed.ko: ~17 MB/s either way), but the CPU does not wait for it.
 * Registers and the channel setup are from the GPL kernel (emma3sep.h,
 * nand_emma.c: channel n at MEM_LIST0 + 0x20*n; LIST_ENABLE bit n starts it).
 * Measured on the device (spec §75): bit 27 must be set when the DESTINATION
 * is DRAM and bit 26 must not be set for the SRAM source -- nand_emma.c sets
 * them only on its memory side -- and MEM_LIST2 counts down to 0 when the
 * transfer is done.  Channel 2: NAND has 0, the vendor application 1
 * (platform.c).
 *
 * One transfer per tick, over consecutive slots; it lands in consecutive ring
 * elements.  A run that wraps from the last slot to the first, or from the
 * end of the ring to its start, is cut there and the rest waits a tick. */
#define XDMA_PHYS	0x10009000ul
#define X_ENABLE	0x088
#define XCH		2
#define X_LIST(n)	(0x100 + 0x20 * XCH + 4 * (n))
#define X_SETUP		((1u << 29) | (1u << 28) | (1u << 27))
#define STAGE_LEN	(SLOTS_MAX * ELEM)

static void __iomem *xdma;
static u8 *stage;		/* where a batch goes when the ring is full */
static int dma_busy, batch_f, batch_n, batch_k[SLOTS_MAX];
static u8 batch_safe[SLOTS_MAX];
static u32 batch_ats[SLOTS_MAX], batch_len;
static u32 st_dma, st_dma_slow, st_split, st_reused_after;

/*
 * Is the copy of a slot trustworthy without looking at the SRAM again?
 *
 * The transfer reads each slot from its arrival time on, so the arrival time
 * in the copy says the slot still held our packet when the DMA got there.
 * What it cannot say is whether the hardware began rewriting the slot while
 * the DMA was still reading its 188 bytes.  For that the slot must first be
 * freed and come to the head of the free queue again: at least N - 1 more
 * packets (N = the FIFO's slot count), and packets arrive no closer than
 * 32 us apart (measured: 863..865 ticks of 27 MHz is the smallest spacing,
 * spec §75).  With m packets already behind ours when the transfer starts,
 * and allowing one more to slip in before it does, the rewrite cannot begin
 * sooner than (N - 1 - m - 2) * 32 us after the start, while the DMA has
 * finished reading the slot at position p of the span by about
 * 2 + 13 * (p + 1) us (programming, then ~12.5 us per slot, measured).
 *
 * For a safe slot the SRAM is not read again at collection time: by then,
 * one tick later, the slot may well have been reused legitimately, and
 * reading it would throw away a good copy.  An unsafe one (we are behind) is
 * still checked against the SRAM and dropped if it moved.
 */
static int copy_is_safe(int ns, int m, int p)
{
	int left = ns - 1 - m - 2;

	return left > 0 && left * 32 > 2 + 13 * (p + 1);
}

/*
 * One transfer per tick copies the address range that holds this tick's
 * ready slots -- from the lowest to the highest, taking along any slot in
 * between (it is not used) -- into a staging buffer.  The next tick checks
 * each ready slot's copy and copies its packet into the ring in ARRIVAL
 * order, which is not address order (spec §77), so that packets of one PID
 * stay in sequence.  That second copy is from DRAM and cheap next to the
 * SRAM.
 */
static int batch_lo;				/* lowest slot of the span */

static int harvest(void)
{
	int i, j, took = 0;

	if (!dma_busy)
		return 0;
	if (__raw_readl(xdma + X_LIST(2)) != 0) {
		cur_flags |= F_BUSY;
		st_dma_slow++;			/* still running: next tick */
		return -1;
	}
	dma_busy = 0;
	dma_cache_inv((unsigned long)stage, batch_len);
	for (i = 0; i < batch_n; i++) {
		const u32 *c = (const u32 *)(stage + (batch_k[i] - batch_lo) * ELEM);
		int ok = c[0] == batch_ats[i], full;
		u32 *e;

		if (ok) {
			int moved = slot_ats(batch_f, batch_k[i]) != batch_ats[i];

			if (moved && batch_safe[i])
				st_reused_after++;	/* after the copy: fine */
			else if (moved)
				ok = 0;
		}
		if (!ok) {
			cur_flags |= F_TORN;
			st_torn++;
			continue;
		}
		full = wr - tail >= ring_n;
		e = full ? scratch : (u32 *)(ring + (wr % ring_n) * ELEM);
		for (j = 0; j < 1 + TS_WORDS; j++)
			e[j] = c[j];
		took += got(batch_f, batch_ats[i], e + 1);
		if (full) {
			st_overrun++;		/* the reader's loss, not ours */
			continue;
		}
		valid[wr % ring_n] = 1;
		wr++;
	}
	return took;
}

static void start_batch(int f, const int *rdy, const u32 *rdy_ats, int n,
			int seen)
{
	int i, lo = rdy[0], hi = rdy[0];

	for (i = 1; i < n; i++) {
		if (rdy[i] < lo)
			lo = rdy[i];
		if (rdy[i] > hi)
			hi = rdy[i];
	}
	if (hi - lo > 0)
		cur_flags |= F_SPLIT;		/* more than one slot: a span */
	batch_f = f;
	batch_lo = lo;
	batch_n = n;
	for (i = 0; i < n; i++) {
		batch_k[i] = rdy[i];
		batch_ats[i] = rdy_ats[i];
		/* rdy[] is oldest first, so seen - 1 - i packets are behind it */
		batch_safe[i] = copy_is_safe(nslots[f], seen - 1 - i, rdy[i] - lo);
		mark_taken(f, rdy[i], rdy_ats[i]);
	}
	batch_len = (hi - lo) * ELEM + 4 + TS_BYTES;
	st_split += hi - lo + 1 - n;		/* slots copied for nothing */
	__raw_writel(SRAM_PHYS + slot_off[f][lo] + SLOT_ATS, xdma + X_LIST(0));
	__raw_writel(virt_to_phys(stage), xdma + X_LIST(1));
	__raw_writel(batch_len, xdma + X_LIST(2));
	__raw_writel(X_SETUP, xdma + X_LIST(3));
	__raw_writel(0, xdma + X_LIST(4));
	__raw_writel(1u << XCH, xdma + X_ENABLE);
	dma_busy = 1;
	st_dma++;
}

/* The FIFO the XDMA channel serves; the others are copied by the CPU in the
 * same tick (meant for the light PIDs: the device's PID table holds 36 per
 * FIFO, spec §77, so a full multiplex needs two). */
static int xdma_fifo = -1;
module_param(xdma_fifo, int, 0444);
MODULE_PARM_DESC(xdma_fifo, "FIFO copied by XDMA (default: the highest "
		 "enabled); the other enabled FIFOs are copied by the CPU");

static int drain_xdma(void)
{
	int took, n, seen, rdy[SLOTS_MAX];
	u32 rdy_ats[SLOTS_MAX];

	took = harvest();
	if (took < 0)
		return 0;		/* still running: it owns its slots' copies */
	n = ready_slots(xdma_fifo, rdy, rdy_ats, &seen);
	if (n)
		start_batch(xdma_fifo, rdy, rdy_ats, n, seen);
	return took;
}

/* --- the tick -------------------------------------------------------------- */

static void add_us(u32 cyc, u32 *us, u32 *rem)
{
	*rem += cyc;
	*us += *rem / count_per_us;
	*rem %= count_per_us;
}

static enum hrtimer_restart tick(struct hrtimer *t)
{
	u32 c0 = read_c0_count(), cyc;
	u64 missed;
	int f, took = 0;

	missed = hrtimer_forward_now(t, period);
	if (missed > 1)
		st_late += (u32)(missed - 1);
	st_ticks++;
	cur_flags = 0;
	cur_seen = 0;
	if (use_xdma)
		took = drain_xdma();
	for (f = 0; f < FIFO_N; f++)
		if ((fifos & (1u << f)) && !(use_xdma && f == xdma_fifo))
			took += drain_cpu(f);
	/* publish what is filled */
	barrier();
	head = wr;
	/* Waking the reader for every packet would cost a context switch per
	 * tick; batch it, but never hold packets back for more than 10 ms. */
	if (head != tail && (head - tail >= wake_pkts ||
			     st_ticks % wake_ticks == 0))
		wake_up_interruptible(&fd_waitq);

	hist_flags[st_ticks & 3] = cur_flags;
	hist_seen[st_ticks & 3] = cur_seen;
	cyc = read_c0_count() - c0;
	if (cyc * 1000 / count_per_us > st_max_ns)
		st_max_ns = cyc * 1000 / count_per_us;
	add_us(cyc, &st_busy_us, &st_busy_rem);
	if (!took) {			/* a tick that took nothing = fixed cost */
		st_idle_ticks++;
		add_us(cyc, &st_idle_us, &st_idle_rem);
	}
	return HRTIMER_RESTART;
}

/* --- character device ---------------------------------------------------- */

static int fd_open(struct inode *inode, struct file *file)
{
	tail = head;			/* start from now */
	return 0;
}

static ssize_t fd_read(struct file *file, char __user *buf, size_t count,
		       loff_t *ppos)
{
	size_t want = count / TS_BYTES, done = 0;
	int r;

	if (!want)
		return -EINVAL;
again:
	r = wait_event_interruptible(fd_waitq, head != tail);
	if (r)
		return r;
	while (done < want && tail != head) {
		u32 e = tail % ring_n;

		barrier();		/* valid[] and the element after head */
		if (valid[e]) {
			if (copy_to_user(buf + done * TS_BYTES,
					 ring + e * ELEM + 4, TS_BYTES))
				return done ? (ssize_t)(done * TS_BYTES) : -EFAULT;
			done++;
		}
		barrier();
		tail++;
	}
	if (!done)
		goto again;	/* only dropped elements: 0 would read as EOF */
	return done * TS_BYTES;
}

static const struct file_operations fd_fops = {
	.owner	= THIS_MODULE,
	.open	= fd_open,
	.read	= fd_read,
};

static struct miscdevice fd_dev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "yb1fifo",
	.fops	= &fd_fops,
};

/* --- /proc/yb1fifo ------------------------------------------------------- */

static int proc_read(char *page, char **start, off_t off, int count,
		     int *eof, void *data)
{
	int len = 0, f, k, i;

	if (off > 0) {
		*eof = 1;
		return 0;
	}
	len += sprintf(page + len, "period_us %u fifos 0x%x ring %u x %u B\n",
		       period_us, fifos, ring_n, ELEM);
	for (f = 0; f < FIFO_N; f++) {
		if (!(fifos & (1u << f)))
			continue;
		len += sprintf(page + len, "fifo %d: %d slots", f, nslots[f]);
		for (k = 0; k < nslots[f]; k++)
			len += sprintf(page + len, " %04x", slot_off[f][k]);
		len += sprintf(page + len, "  packets %u\n", st_pkts[f]);
	}
	len += sprintf(page + len,
		       "elapsed_ms %u ticks %u late %u busy_us %u max_tick_ns %u\n",
		       jiffies_to_msecs(jiffies - start_jiffies), st_ticks,
		       st_late, st_busy_us, st_max_ns);
	len += sprintf(page + len, "idle_ticks %u idle_us %u\n",
		       st_idle_ticks, st_idle_us);
	len += sprintf(page + len,
		       "deferred %u torn %u overrun %u sync_err %u resync %u\n",
		       st_deferred, st_torn, st_overrun, st_sync, st_resync);
	len += sprintf(page + len, "cc_gaps %u cc_lost %u cc_dup %u\n",
		       st_ccgap, st_cclost, st_ccdup);
	len += sprintf(page + len, "ring head %u tail %u\n", head, tail);
	len += sprintf(page + len, "copy %s", use_xdma ? "xdma" : "cpu");
	if (use_xdma)
		len += sprintf(page + len, " ch%d for FIFO %d (others by cpu) "
			       "transfers %u still_running %u idle_slots_copied %u "
			       "reused_after_copy %u", XCH, xdma_fifo, st_dma,
			       st_dma_slow, st_split, st_reused_after);
	len += sprintf(page + len, "\n");
	for (i = 0; i < ngaplog; i++)
		len += sprintf(page + len, "gap tick %u pid 0x%04x cc %u->%u "
			       "dt %u us  ticks now..-3: flags %02x %02x %02x %02x "
			       "seen %u %u %u %u\n", gaplog[i].tick, gaplog[i].pid,
			       gaplog[i].from, gaplog[i].to, gaplog[i].dt_us,
			       gaplog[i].flags[0], gaplog[i].flags[1],
			       gaplog[i].flags[2], gaplog[i].flags[3],
			       gaplog[i].seen[0], gaplog[i].seen[1],
			       gaplog[i].seen[2], gaplog[i].seen[3]);
	for (i = 0; i < npidstat; i++)
		len += sprintf(page + len,
			       "pid 0x%04x packets %u cc_gaps %u cc_lost %u "
			       "scr none/rsv/even/odd %u/%u/%u/%u\n",
			       pidstat[i].pid, pidstat[i].pkts,
			       pidstat[i].ccgap, pidstat[i].cclost,
			       pidstat[i].scr[0], pidstat[i].scr[1],
			       pidstat[i].scr[2], pidstat[i].scr[3]);
	*eof = 1;
	return len;
}

/* --- load / unload -------------------------------------------------------- */

/* Returns 1 if o is a slot of FIFO f we had not seen yet. */
static int add_slot(int f, u32 o)
{
	int q;

	if (!o || (o & 3) || o > SRAM_LEN - SLOT_PKT - TS_BYTES)
		return 0;
	for (q = 0; q < nslots[f]; q++)
		if (slot_off[f][q] == o)
			return 0;
	if (nslots[f] >= SLOTS_MAX)
		return 0;
	slot_off[f][nslots[f]++] = o;
	return 1;
}

/* A transfer still in flight must land before its buffer goes back. */
static void xdma_quiesce(void)
{
	int i;

	if (!xdma)
		return;
	for (i = 0; dma_busy && i < 1000; i++) {
		if (__raw_readl(xdma + X_LIST(2)) == 0)
			dma_busy = 0;
		else
			udelay(10);
	}
	if (dma_busy)
		printk(KERN_ERR "yb1fifo: XDMA channel %d did not finish\n", XCH);
}

static int __init fifodrain_init(void)
{
	int f, k, r, q, err;
	u32 c0, ns;
	ktime_t t0;

	if (period_us < 20 || period_us > 100000 || !(fifos & 7))
		return -EINVAL;

	sram = ioremap_nocache(SRAM_PHYS, SRAM_LEN);
	if (!sram)
		return -ENOMEM;

	/* The seven-word slot list is a queue the sub-CPU rotates while we
	 * read it; whatever slot is being filled or handled at the moment is
	 * missing from it -- and a FIFO can have more slots than the list has
	 * words (FIFO 0 has 7, FIFO 1 6, FIFO 2 8: spec §77).  Read it for at
	 * least 100 ms and until nothing new has turned up for 50 ms (1 s at
	 * most), keep the union, then check the slots are evenly spaced. */
	for (r = 0, q = 0; r < 500; r++) {
		int fresh = 0, w;

		for (w = 0; w < 200; w++)
			for (f = 0; f < FIFO_N; f++)
				for (k = 0; k < 7; k++)
					fresh |= add_slot(f, __raw_readl(sram + FIFO_CTL +
							  f * 0x80 + 0x14 + 4 * k));
		q = fresh ? 0 : q + 1;		/* rounds with nothing new */
		if (r >= 50 && q >= 25)
			break;
		msleep(2);
	}
	for (f = 0; f < FIFO_N; f++) {
		if (!(fifos & (1u << f)))
			continue;
		if (nslots[f] < 2) {
			printk(KERN_ERR "yb1fifo: FIFO %d has %d slots set up -- "
			       "configure the demultiplexer first\n", f, nslots[f]);
			err = -ENODEV;
			goto fail_map;
		}
		/* address order is the order they are filled in */
		for (k = 1; k < nslots[f]; k++) {
			u32 o = slot_off[f][k];

			for (q = k; q > 0 && slot_off[f][q - 1] > o; q--)
				slot_off[f][q] = slot_off[f][q - 1];
			slot_off[f][q] = o;
		}
		for (k = 1; k < nslots[f]; k++)
			if (slot_off[f][k] - slot_off[f][k - 1] != SLOT_STRIDE) {
				printk(KERN_ERR "yb1fifo: FIFO %d slots %04x %04x are "
				       "not %u apart -- one not seen?\n", f,
				       slot_off[f][k - 1], slot_off[f][k], SLOT_STRIDE);
				err = -ENODEV;
				goto fail_map;
			}
	}
	if (use_xdma) {
		if (xdma_fifo < 0)
			for (f = 0; f < FIFO_N; f++)
				if (fifos & (1u << f))
					xdma_fifo = f;	/* the highest enabled */
		if (xdma_fifo >= FIFO_N || !(fifos & (1u << xdma_fifo))) {
			err = -EINVAL;
			goto fail_map;
		}
	}

	/* CP0 Count runs at a fixed fraction of the CPU clock; measure it */
	c0 = read_c0_count();
	t0 = ktime_get();
	msleep(100);
	ns = (u32)ktime_to_ns(ktime_sub(ktime_get(), t0));
	count_per_us = (read_c0_count() - c0) / (ns / 1000);
	if (!count_per_us)
		count_per_us = 1;

	/* the ring: as large a physically contiguous block as we can get */
	for (ring_order = 8; ring_order >= 5; ring_order--) {
		ring = (u8 *)__get_free_pages(GFP_KERNEL | __GFP_NOWARN,
					      ring_order);
		if (ring)
			break;
	}
	if (!ring) {
		err = -ENOMEM;
		goto fail_map;
	}
	ring_n = (PAGE_SIZE << ring_order) / ELEM;
	valid = kzalloc(ring_n, GFP_KERNEL);
	if (!valid) {
		err = -ENOMEM;
		goto fail_ring;
	}
	/* no dirty line may later be written back over what the DMA put there */
	dma_cache_wback_inv((unsigned long)ring, PAGE_SIZE << ring_order);

	if (use_xdma) {
		xdma = ioremap_nocache(XDMA_PHYS, 0x1000);
		stage = kmalloc(STAGE_LEN, GFP_KERNEL);
		if (!xdma || !stage) {
			err = -ENOMEM;
			goto fail_xdma;
		}
		if (__raw_readl(xdma + X_ENABLE) & (1u << XCH)) {
			printk(KERN_ERR "yb1fifo: XDMA channel %d is busy\n", XCH);
			err = -EBUSY;
			goto fail_xdma;
		}
		dma_cache_wback_inv((unsigned long)stage, STAGE_LEN);
	}
	memset(last_cc, 0xff, sizeof(last_cc));

	err = misc_register(&fd_dev);
	if (err)
		goto fail_xdma;
	if (!create_proc_read_entry("yb1fifo", 0444, NULL, proc_read, NULL)) {
		err = -ENOMEM;
		goto fail_misc;
	}

	/* What is in the slots now is old: start after the newest.  Done last,
	 * right before the timer starts -- the FIFO keeps moving while we set
	 * up (the Count calibration alone sleeps 100 ms), and a stale starting
	 * point costs the first ticks a lap of the FIFO (seen on the device). */
	for (f = 0; f < FIFO_N; f++) {
		if (!(fifos & (1u << f)))
			continue;
		for (k = 0; k < nslots[f]; k++) {
			done_ats[f][k] = slot_ats(f, k);
			if (k == 0 || (s32)(done_ats[f][k] - ref_ats[f]) > 0) {
				ref_ats[f] = done_ats[f][k];
				next_slot[f] = (k + 1) % nslots[f];
			}
		}
	}
	start_jiffies = jiffies;
	wake_ticks = 10000 / period_us;
	if (!wake_ticks)
		wake_ticks = 1;
	period = ktime_set(0, period_us * 1000);
	hrtimer_init(&timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	timer.function = tick;
	timer.cb_mode = HRTIMER_CB_IRQSAFE_UNLOCKED;	/* hard IRQ, may wake */
	hrtimer_start(&timer, period, HRTIMER_MODE_REL);
	printk(KERN_INFO "yb1fifo: draining FIFO mask 0x%x every %u us by %s, "
	       "ring %u x %u B, CP0 Count %u/us, misc minor %d\n", fifos,
	       period_us, use_xdma ? "XDMA" : "CPU", ring_n, ELEM,
	       count_per_us, fd_dev.minor);
	return 0;

fail_misc:
	misc_deregister(&fd_dev);
fail_xdma:
	kfree(stage);
	if (xdma)
		iounmap(xdma);
	kfree(valid);
fail_ring:
	free_pages((unsigned long)ring, ring_order);
fail_map:
	iounmap(sram);
	return err;
}

static void __exit fifodrain_exit(void)
{
	hrtimer_cancel(&timer);
	xdma_quiesce();
	remove_proc_entry("yb1fifo", NULL);
	misc_deregister(&fd_dev);
	if (!dma_busy) {		/* else leak rather than let DMA scribble */
		free_pages((unsigned long)ring, ring_order);
		kfree(stage);
	}
	kfree(valid);
	if (xdma)
		iounmap(xdma);
	iounmap(sram);
}

module_init(fifodrain_init);
module_exit(fifodrain_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("POE-BR100-YB1: drain the AV SRAM TS input FIFOs");
