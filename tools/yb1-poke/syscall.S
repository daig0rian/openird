/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Process entry and raw syscalls for a static, libc-free MIPS o32 binary.
 *
 * At entry $sp points at argc, with argv[] right behind it.  Unlike the
 * tracer this is not a shared object, so no .abicalls / GOT setup: the C is
 * built -mno-abicalls -fno-pic -G0.
 */
	.set	noreorder
	.text

	.globl	_start
	.globl	__start
	.ent	_start
_start:
__start:
	lw	$a0, 0($sp)		/* argc */
	addiu	$a1, $sp, 4		/* argv */
	jal	main
	nop
	move	$a0, $v0		/* exit(main(...)) */
	li	$v0, 4001		/* __NR_exit */
	syscall
	nop
	.end	_start

/*
 * long tr_syscall(long nr, long a, long b, long c, long d, long e, long f)
 * Arguments 5-7 come off the caller's frame, so read them before $sp moves.
 * o32 reports errors out of band in $a3; fold them into a negative return.
 */
	.globl	tr_syscall
	.ent	tr_syscall
	.type	tr_syscall, @function
tr_syscall:
	lw	$t0, 16($sp)		/* d */
	lw	$t1, 20($sp)		/* e */
	lw	$t2, 24($sp)		/* f */
	move	$v0, $a0
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
	subu	$v0, $zero, $v0
1:
	jr	$ra
	nop
	.end	tr_syscall
	.size	tr_syscall, . - tr_syscall
	.section .note.GNU-stack, "", @progbits
