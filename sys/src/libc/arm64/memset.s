TEXT memset(SB), $-4
	MOVBU	c+8(FP), R1
	MOVWU	n+16(FP), R2

	ADD	R0, R2, R3
	BIC	$7, R2, R4
	CBZ	R4, _loop1
	ADD	R0, R4, R4

	ORR	R1<<8, R1
	ORR	R1<<16, R1
	ORR	R1<<32, R1

_loop8:
	MOV	R1, (R0)8!
	CMP	R4, R0
	BNE	_loop8

_loop1:
	CMP	R3, R0
	BEQ	_done

	MOVBU	R1, (R0)1!
	B	_loop1

_done:
	RETURN