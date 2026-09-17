/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * long tr_syscall(long nr, long a, long b, long c, long d, long e, long f)
 * ARM EABI: syscall number in r7, arguments in r0-r5.
 */
	.syntax unified
	.arm
	.text
	.global	tr_syscall
	.hidden	tr_syscall
	.type	tr_syscall, %function
tr_syscall:
	mov	ip, sp
	push	{r4, r5, r6, r7}
	mov	r7, r0
	mov	r0, r1
	mov	r1, r2
	mov	r2, r3
	ldm	ip, {r3, r4, r5}
	svc	#0
	pop	{r4, r5, r6, r7}
	bx	lr
	.size	tr_syscall, . - tr_syscall
	.section .note.GNU-stack, "", %progbits
