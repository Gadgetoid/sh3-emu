	.include "common.inc"

	.text
	.global	_start
_start:
	load	r1, vectors
	ldc	r1, vbr
	mov	#0, r8

	mov	#0x12, r3
	mov	#0, r14
	trapa	#5
t1_after:
	check	r9, 0x160, 1
	check	r12, 0x14, 2
	check	r10, t1_after, 3
	check	r11, 0x40000000, 4
	check	r3, 0x12, 5
	stc	sr, r1
	check	r1, 0x40000001, 6

	mov	#2, r14
t2_at:
	.word	0xfffd
	check	r9, 0x180, 7
	check	r10, t2_at, 8

	mov	#4, r14
t3_at:
	bra	t3_target
	bra	t3_target
t3_target:
	check	r9, 0x1a0, 9
	check	r10, t3_at, 10

	mov	#2, r14
	load	r1, 0x80480001
t4_at:
	mov.w	@r1, r2
	check	r9, 0x0e0, 11
	check	r10, t4_at, 12
	check	r13, 0x80480001, 13
	mov	#2, r14
	load	r1, 0x80480002
t5_at:
	mov.l	r2, @r1
	check	r9, 0x100, 14
	check	r13, 0x80480002, 15

	mov	#2, r14
	mov	#1, r8
	load	r1, user_code - 0x80000000
	ldc	r1, spc
	mov	#0, r1
	ldc	r1, ssr
	rte
	nop
user_code:
	load	r1, 0x80480000
user_load:
	mov.l	@r1, r2
	check	r9, 0x0e0, 16
	check	r10, user_load - 0x80000000, 17
	check	r11, 0, 18
	mov	#0, r14
	trapa	#1
	check	r9, 0x160, 19
	stc	sr, r1
	check	r1, 0x40000001, 20
	mov	#0, r8

	load	r1, 0x11111111
	ldc	r1, r2_bank
	stc	r2_bank, r3
	check	r3, 0x11111111, 21
	mov	#0x22, r2
	load	r1, 0x60000000
	ldc	r1, sr
	check	r2, 0x11111111, 22
	stc	r2_bank, r3
	check	r3, 0x22, 23
	load	r1, 0x40000000
	ldc	r1, sr
	check	r2, 0x22, 24

	load	r1, 0xffffffff
	ldc	r1, sr
	stc	sr, r1
	check	r1, 0x700003f3, 25
	load	r1, 0x40000000
	ldc	r1, sr

	load	r1, 0x40000000
	mov	r1, r5
	ldc	r1, gbr
	stc	gbr, r6
	check	r6, 0x40000000, 26

	mov	#0, r4
	bra	finish
	nop

fail:
finish:
	mov	#1, r3
	trapa	#0x13

	.align	8
vectors:
	.space	0x100
general:
	mov.l	expevt_address, r0
	mov.l	@r0, r9
	mov.l	tra_address, r0
	mov.l	@r0, r12
	mov.l	tea_address, r0
	mov.l	@r0, r13
	stc	spc, r10
	stc	ssr, r11
	mov	#0x55, r3
	stc	spc, r0
	add	r14, r0
	ldc	r0, spc
	mov	r8, r0
	cmp/eq	#1, r0
	bf	general_return
	stc	ssr, r0
	mov.l	md_bit, r1
	or	r1, r0
	ldc	r0, ssr
	mov	#0, r8
general_return:
	rte
	nop
	.align	2
expevt_address:	.long	0xffffffd4
tra_address:	.long	0xffffffd0
tea_address:	.long	0xfffffffc
md_bit:		.long	0x40000000

	.data
	.long	0
