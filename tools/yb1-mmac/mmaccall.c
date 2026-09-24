// SPDX-License-Identifier: GPL-2.0-only
/*
 * yb1-mmac / mmaccall - call one AV routine of the vendor's val_merge module
 * by the address /proc/kallsyms gives for its name, and report what happened.
 *
 * Why this exists
 * ---------------
 * The AV subsystem lives on a second CPU and is driven through a command
 * queue in shared memory.  The main CPU fills in an entry and then signals
 * the other side; that signal is the one part of the protocol that cannot be
 * observed from here, because this is a single-CPU machine and our loop only
 * runs while the driver is already asleep waiting for its answer (see
 * docs/specs/poe-br100-yb1-reception-stack.md, the sections on the command
 * stack).  A hand-built entry that is byte-for-byte what the driver writes
 * next is therefore never taken.
 *
 * The hardware has routines the driver never exposes as an ioctl -- among
 * them the one that points a PID filter at a ring of our own.  Those routines
 * are ordinary module-local symbols, and /proc/kallsyms names them.  So
 * instead of reproducing the signal, this module asks the driver to make the
 * call, which is what it knows how to do.
 *
 * This is observation, not translation: no vendor instruction sequence is
 * read, and nothing vendor is shipped.  What travels out of here is the
 * ANSWER -- what the hardware writes into a ring when it is asked -- which is
 * the fact the specification needs.  The address comes from the caller, read
 * out of /proc/kallsyms at run time, so nothing about a particular build is
 * baked in.
 *
 * Usage:
 *   FN=$(awk '$3 == "MMAC_DEMUX_SetPidFilterBuffer" { print $1 }' /proc/kallsyms)
 *   insmod mmaccall.ko fn=0x$FN a0=0x00100022 a1=0xad000000 a2=0x00020000
 *
 * The module never stays loaded: it makes the call from its init function and
 * then fails the load, so a session is a series of insmod lines and the
 * driver is never left with something of ours attached to it.
 *
 * The call is made with four register arguments.  On o32 a callee that takes
 * fewer simply ignores the rest, so this does not need the real arity; the
 * command the AV queue ends up carrying tells us how many words actually
 * travelled (its entry records the request length).
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>

static unsigned long fn;
static unsigned long a0, a1, a2, a3, a4, a5;
static int nargs = 4;
static int bufarg = -1;

static unsigned long scratch[32];

module_param(fn, ulong, 0);
MODULE_PARM_DESC(fn, "kernel address of the routine to call (from kallsyms)");
module_param(a0, ulong, 0);
module_param(a1, ulong, 0);
module_param(a2, ulong, 0);
module_param(a3, ulong, 0);
module_param(a4, ulong, 0);
module_param(a5, ulong, 0);
module_param(nargs, int, 0);
MODULE_PARM_DESC(nargs, "how many of a0..a5 to pass (1..6, default 4)");
module_param(bufarg, int, 0);
MODULE_PARM_DESC(bufarg,
		 "pass a zeroed scratch area as this argument (0..5) and "
		 "print what the call left in it; -1 means do not");

typedef long (*fn1_t)(unsigned long);
typedef long (*fn2_t)(unsigned long, unsigned long);
typedef long (*fn3_t)(unsigned long, unsigned long, unsigned long);
typedef long (*fn4_t)(unsigned long, unsigned long, unsigned long,
		      unsigned long);
typedef long (*fn5_t)(unsigned long, unsigned long, unsigned long,
		      unsigned long, unsigned long);
typedef long (*fn6_t)(unsigned long, unsigned long, unsigned long,
		      unsigned long, unsigned long, unsigned long);

static int __init mmaccall_init(void)
{
	long ret;

	if (!fn) {
		printk(KERN_INFO "mmaccall: no fn= given\n");
		return -EINVAL;
	}
	/* Several of these routines hand their result back through a pointer
	 * rather than in the return value, so offer one on request. */
	if (bufarg >= 0 && bufarg <= 5) {
		unsigned long p = (unsigned long)scratch;

		memset(scratch, 0, sizeof(scratch));
		switch (bufarg) {
		case 0: a0 = p; break;
		case 1: a1 = p; break;
		case 2: a2 = p; break;
		case 3: a3 = p; break;
		case 4: a4 = p; break;
		default: a5 = p; break;
		}
	}

	printk(KERN_INFO "mmaccall: calling %08lx(%08lx, %08lx, %08lx, %08lx,"
	       " %08lx, %08lx) with %d args\n", fn, a0, a1, a2, a3, a4, a5,
	       nargs);

	switch (nargs) {
	case 1:
		ret = ((fn1_t)fn)(a0);
		break;
	case 2:
		ret = ((fn2_t)fn)(a0, a1);
		break;
	case 3:
		ret = ((fn3_t)fn)(a0, a1, a2);
		break;
	case 4:
		ret = ((fn4_t)fn)(a0, a1, a2, a3);
		break;
	case 5:
		ret = ((fn5_t)fn)(a0, a1, a2, a3, a4);
		break;
	default:
		ret = ((fn6_t)fn)(a0, a1, a2, a3, a4, a5);
		break;
	}

	printk(KERN_INFO "mmaccall: returned %ld (0x%08lx)\n", ret,
	       (unsigned long)ret);
	if (bufarg >= 0 && bufarg <= 5)
		printk(KERN_INFO "mmaccall: scratch %08lx %08lx %08lx %08lx "
		       "%08lx %08lx %08lx %08lx\n",
		       scratch[0], scratch[1], scratch[2], scratch[3],
		       scratch[4], scratch[5], scratch[6], scratch[7]);

	/* Never stay loaded.  -EAGAIN makes insmod say so and unload us. */
	return -EAGAIN;
}

static void __exit mmaccall_exit(void)
{
}

module_init(mmaccall_init);
module_exit(mmaccall_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("call one AV routine by its kallsyms address, once");
