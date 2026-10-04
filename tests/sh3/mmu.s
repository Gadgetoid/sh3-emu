	.include "common.inc"

	.equ	PTEH, 0xfffffff0
	.equ	PTEL, 0xfffffff4
	.equ	TEA, 0xfffffffc
	.equ	MMUCR, 0xffffffe0
	.equ	EXPEVT, 0xffffffd4
	.equ	FLAGS_RW, 0x16c
	.equ	FLAGS_RW_4K, 0x17c
	.equ	FLAGS_PRIVILEGED_READ, 0x10c
	.equ	FLAGS_CLEAN, 0x168
	.equ	FLAGS_INVALID, 0x06c
	.equ	FLAGS_SHARED, 0x16e

	.text
	.global	_start
_start:
	load	r1, vectors
	ldc	r1, vbr
	load	r1, 0x80480000
	load	r2, 0xcafef00d
	mov.l	r2, @r1
	load	r2, 0x12345678
	mov.l	r2, @(4,r1)
	load	r1, 0x80481000
	load	r2, 0x0badf00d
	mov.l	r2, @r1
	load	r10, misses
	mov	#0, r2
	mov.l	r2, @r10
	mov	#0, r9
	mov	#0, r12

	load	r1, PTEH
	mov	#0, r2
	mov.l	r2, @r1
	load	r1, MMUCR
	mov	#5, r2
	mov.l	r2, @r1

	load	r8, FLAGS_RW
	load	r1, 0x00480000
	mov.l	@r1, r2
	check	r2, 0xcafef00d, 1
	mov.l	@r10, r2
	check	r2, 1, 2
	load	r1, 0x00480004
	mov.l	@r1, r2
	check	r2, 0x12345678, 3
	mov.l	@r10, r2
	check	r2, 1, 4

	load	r1, 0x00480400
	mov.l	@r1, r2
	mov.l	@r10, r2
	check	r2, 2, 5

	load	r1, PTEH
	load	r2, 0x10000000
	mov.l	r2, @r1
	load	r1, PTEL
	load	r2, 0x00481000 + FLAGS_RW_4K
	mov.l	r2, @r1
	ldtlb
	load	r1, 0x10000000
	mov.l	@r1, r2
	check	r2, 0x0badf00d, 6
	mov.l	@r10, r2
	check	r2, 2, 7

	load	r1, PTEH
	mov	#5, r2
	mov.l	r2, @r1
	load	r1, 0x00480000
	mov.l	@r1, r2
	mov.l	@r10, r2
	check	r2, 3, 8
	load	r1, PTEH
	mov	#0, r2
	mov.l	r2, @r1

	load	r1, PTEH
	load	r2, 0x00490000
	mov.l	r2, @r1
	load	r1, PTEL
	load	r2, 0x00480000 + FLAGS_SHARED
	mov.l	r2, @r1
	ldtlb
	load	r1, PTEH
	mov	#7, r2
	mov.l	r2, @r1
	load	r1, 0x00490000
	mov.l	@r1, r2
	check	r2, 0xcafef00d, 9
	mov.l	@r10, r2
	check	r2, 3, 10
	load	r1, PTEH
	mov	#0, r2
	mov.l	r2, @r1

	load	r8, FLAGS_PRIVILEGED_READ
	mov	#2, r14
	load	r1, 0x004a0000
	mov.l	@r1, r2
	load	r1, 0x004a0000
p_store:
	mov.l	r2, @r1
	check	r9, 0x0c0, 11
	check	r11, p_store, 12
	check	r13, 0x004a0000, 13

	load	r8, FLAGS_CLEAN
	mov	#0, r9
	load	r1, 0x004b0000
	mov.l	@r1, r2
	check	r9, 0, 14
	mov.l	r2, @r1
	check	r9, 0x080, 15

	load	r8, FLAGS_INVALID
	mov	#0, r9
	load	r1, 0x004c0000
	mov.l	@r1, r2
	check	r9, 0x040, 16

	load	r1, MMUCR
	mov	#5, r2
	mov.l	r2, @r1
	load	r8, FLAGS_RW
	mov	#1, r12
	mov	#0, r9
	mov.l	@r10, r3
	load	r1, 0x10000000
	mov.l	@r1, r2
	mov.l	@r10, r4
	sub	r3, r4
	check	r4, 1, 17
	check	r9, 0x040, 19

	load	r1, 0xf2000080 + (0x00480000 & 0x1f000)
	load	r2, 0x00480000
	mov.l	r2, @r1
	load	r1, 0x00480000
	mov.l	@r1, r2
	load	r1, 0xf2000080 + (0x00480000 & 0x1f000)
	load	r2, 0x00480000
	mov.l	r2, @r1
	mov.l	@r10, r3
	load	r1, 0x00480000
	mov.l	@r1, r2
	mov.l	@r10, r4
	sub	r3, r4
	check	r4, 1, 18

	mov	#0, r4
	bra	finish
	nop

fail:
finish:
	load	r1, MMUCR
	mov	#4, r2
	mov.l	r2, @r1
	mov	#1, r3
	trapa	#0x13

	.align	10
vectors:
	.space	0x100
general:
	mov.l	expevt_address, r0
	mov.l	@r0, r9
	mov	r12, r0
	cmp/eq	#1, r0
	bf	general_skip
	mov	r9, r0
	cmp/eq	#0x40, r0
	bt	general_reload
	cmp/eq	#0x60, r0
	bf	general_skip
general_reload:
	mov.l	tlb_miss_address, r0
	jmp	@r0
	nop
general_skip:
	mov.l	tea_address, r0
	mov.l	@r0, r13
	stc	spc, r11
	stc	spc, r0
	add	r14, r0
	ldc	r0, spc
	rte
	nop
	.align	2
expevt_address:	.long	EXPEVT
tea_address:	.long	TEA
tlb_miss_address:	.long	tlb_miss

	.org	vectors - _start + 0x400
tlb_miss:
	mov.l	miss_counter, r0
	mov.l	@r0, r1
	add	#1, r1
	mov.l	r1, @r0
	mov.l	tea_miss, r0
	mov.l	@r0, r1
	mov.l	page_mask, r0
	and	r0, r1
	or	r8, r1
	mov.l	ptel_address, r0
	mov.l	r1, @r0
	ldtlb
	rte
	nop
	.align	2
miss_counter:	.long	misses
tea_miss:	.long	TEA
page_mask:	.long	0x1ffffc00
ptel_address:	.long	PTEL

	.data
	.space	0x2000
misses:	.long	0
