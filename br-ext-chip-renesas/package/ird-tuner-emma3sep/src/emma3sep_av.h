/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Renesas EMMA3SEP: the pieces of the AV subsystem a tuner driver uses.
 *
 *   emma3sep_av_*      commands to the firmware of the AV sub-CPU
 *                      (docs/specs/poe-br100-yb1-av-command.md)
 *   emma3sep_tsfifo_*  the transport stream input FIFOs in the sub-CPU's data
 *                      SRAM, drained into a ring the main CPU owns
 *                      (docs/specs/poe-br100-yb1-ts-fifo.md)
 *
 * Neither touches the descrambler.  The sub-CPU has to be running already;
 * starting it is left to the owner's own csd.ko (av-command spec §0/§1).
 */
#ifndef _IRD_EMMA3SEP_AV_H
#define _IRD_EMMA3SEP_AV_H

#include <linux/types.h>

/* --- commands ---------------------------------------------------------------- */

#define EMMA3SEP_DEMUX_FILTERS	36	/* PID filters per demultiplexer device */

/* Is the sub-CPU out of reset and pointing at the shared memory? */
int emma3sep_av_up(void);

/* Is another poster (the vendor's csd.ko) still attached? */
int emma3sep_av_shared(void);

/*
 * Find where the command ring stands.  *fresh = 1 when no command has been
 * posted since the firmware started.  Call before the first command, and
 * with nobody else posting (the vendor modules unloaded).
 */
int emma3sep_av_attach(int *fresh);

/*
 * Post one command and wait for it.  req/resp are 32-bit words.  Returns the
 * firmware's status (0 = success, > 0 = refused) or -errno.  Not reentrant:
 * the caller serialises.
 */
int emma3sep_av_cmd(u16 cmd, const u32 *req, int nreq, u32 *resp, int nresp);

/* The demultiplexer as far as raw TS needs it (av-command spec §5.1). */
int emma3sep_demux_init(u32 dev[3]);	/* Initialise, open 3 devices, inputs */
int emma3sep_demux_filter_alloc(u32 dev, u32 *filter);
int emma3sep_demux_filter_pid(u32 filter, u16 pid);

/* --- TS input FIFOs ---------------------------------------------------------- */

#define EMMA3SEP_TSFIFO_N	3

int emma3sep_tsfifo_init(void);
void emma3sep_tsfifo_exit(void);

/* Learn the slots of the FIFOs in mask.  Packets must be flowing into them
 * (ts-fifo spec §4.2).  Sleeps up to a second.  The drainer must be stopped. */
int emma3sep_tsfifo_find_slots(unsigned int mask);
int emma3sep_tsfifo_slots(int fifo);		/* how many are known */

/* Start / stop draining.  The FIFO xdma_fifo is copied by the SoC's memory
 * DMA, the others in mask by the CPU (ts-fifo spec §7). */
int emma3sep_tsfifo_start(unsigned int mask, int xdma_fifo);
void emma3sep_tsfifo_stop(void);
/* The stream is a heavy one (CS, ~23,000 packets a second) or not: picks the
 * drainer's settings for it (ts-fifo spec §7.1), running or not; a running
 * drainer takes them from its next tick. */
void emma3sep_tsfifo_heavy(int heavy);

/* The reader's side (one reader). */
void emma3sep_tsfifo_capture(int on);	/* on: drop what is queued, allow reads */
ssize_t emma3sep_tsfifo_read(char __user *buf, size_t count, int nonblock);

/* A copy of every packet of the chosen PIDs that arrives through a CPU-copied
 * FIFO, for whoever follows the program-specific information. */
void emma3sep_tsfifo_tap_clear(void);
void emma3sep_tsfifo_tap_pid(unsigned int pid);
/* 1 = a packet (188 bytes), 0 = none within timeout_ms, < 0 = interrupted */
int emma3sep_tsfifo_tap_read(u8 *pkt, unsigned int timeout_ms);

int emma3sep_tsfifo_show(char *page);	/* counters, one page at most */

#endif /* _IRD_EMMA3SEP_AV_H */
