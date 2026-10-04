	.macro check register, expected, code
	mov.l	1f, r0
	cmp/eq	r0, \register
	bt	2f
	mov	#\code, r4
	bra	fail
	nop
	.align	2
1:	.long	\expected
2:
	.endm

	.macro load register, value
	mov.l	1f, \register
	bra	2f
	nop
	.align	2
1:	.long	\value
2:
	.endm
