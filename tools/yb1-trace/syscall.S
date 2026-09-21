/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * long tr_syscall(long nr, long a, long b, long c, long d, long e, long f)
 *
 * MIPS o32: the syscall number goes in $v0, arguments 1-4 in $a0-$a3 and
 * arguments 5-7 on the stack at 16/20/24($sp).  Our own incoming arguments use
 * the same layout, so d/e/f must be loaded off the caller's frame *before*
 * $sp moves.
 *
 * o32 reports errors out of band: $a3 is non-zero and $v0 holds the positive
 * errno.  Fold that back into the negative return value the C side expects.
 */
	.set	noreorder
	.abicalls
	.text
	.globl	tr_syscall
	.hidden	tr_syscall
	.ent	tr_syscall
	.type	tr_syscall, @function
tr_syscall:
	lw	$t0, 16($sp)		/* d */
	lw	$t1, 20($sp)		/* e */
	lw	$t2, 24($sp)		/* f */
	move	$v0, $a0		/* syscall number */
	move	$a0, $a1
	move	$a1, $a2
	move	$a2, $a3
	move	$a3, $t0
	addiu	$sp, $sp, -32
	sw	$t1, 16($sp)
	sw	$t2, 20($sp)
	syscall
	addiu	$sp, $sp, 32
	beqz	$a3, 1f
	nop
	subu	$v0, $zero, $v0		/* error: return -errno */
1:
	jr	$ra
	nop
	.end	tr_syscall
	.size	tr_syscall, . - tr_syscall
	.section .note.GNU-stack, "", @progbits
