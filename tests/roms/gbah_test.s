@ GBA Handoff test ROM - CC0 / public domain (own work, no third-party code).
@ Mode 3 pattern that depends on frame counter and held keys, a square-wave
@ tone whose pitch changes every frame, and an SRAM byte written every frame.
@ Build: see build.sh
	.arm
	.section .text
	.global _start
_start:
	b	init
	.space	156			@ Nintendo logo area (left zeroed: mGBA does not validate it)
	.ascii	"GBAHTEST\0\0\0\0"	@ 0xA0 title (12 bytes)
	.ascii	"AGHT"			@ 0xAC game code
	.ascii	"00"			@ 0xB0 maker
	.byte	0x96			@ 0xB2 fixed
	.byte	0, 0			@ unit code, device type
	.space	7			@ reserved
	.byte	0			@ 0xBC version
	.byte	0			@ 0xBD header checksum (patched by build.sh)
	.byte	0, 0
init:
	mov	r0, #0x04000000
	ldr	r1, =0x0403		@ DISPCNT: mode 3, BG2 on
	strh	r1, [r0]
	mov	r1, #0x80
	strh	r1, [r0, #0x84]		@ SOUNDCNT_X: master enable
	ldr	r1, =0xFF77
	strh	r1, [r0, #0x80]		@ SOUNDCNT_L
	mov	r1, #2
	strh	r1, [r0, #0x82]		@ SOUNDCNT_H: 100% PSG
	ldr	r1, =0xF080
	strh	r1, [r0, #0x62]		@ SOUND1CNT_H: vol 15, duty 50%
	mov	r4, #0			@ r4 = frame counter
	mov	r10, #0
main:
1:	ldrh	r1, [r0, #6]		@ wait for VCOUNT < 160
	cmp	r1, #160
	bge	1b
2:	ldrh	r1, [r0, #6]		@ wait for VBlank (VCOUNT >= 160)
	cmp	r1, #160
	blt	2b
	add	r1, r0, #0x130
	ldrh	r5, [r1]		@ KEYINPUT (active low)
	mvn	r5, r5
	bic	r5, r5, #0xFC00		@ r5 = pressed keys (10 bits)
	add	r4, r4, #1
	and	r2, r4, #0xFF		@ tone: freq = 0x400 + (frame&0xFF)*4, trigger
	mov	r2, r2, lsl #2
	orr	r2, r2, #0x400
	orr	r2, r2, #0x8000
	strh	r2, [r0, #0x64]
	mov	r1, #0x0E000000
.ifdef LATE
	@ LATE variant: no "SRAM_V113" hint, so the save type stays AUTODETECT
	@ until the first SRAM access. At ROM loop 20 write one byte (0xA5) at
	@ SRAM[0x100], never touched again; from then on it is read every frame
	@ and mixed into the picture, so losing the save changes the frame hash.
	cmp	r4, #20
	moveq	r2, #0xA5
	streqb	r2, [r1, #0x100]
	ldrcsb	r10, [r1, #0x100]
.else
	strb	r4, [r1]		@ SRAM[0]=frame, SRAM[1]=keys&0xFF
	strb	r5, [r1, #1]
	mov	r6, r5, lsr #8		@ SRAM[2]=keys>>8 (L/R are bits 8-9)
	strb	r6, [r1, #2]
.endif
	mov	r6, #0x06000000
	mov	r7, #0			@ y
yl:	mov	r8, #0			@ x
xl:	add	r2, r8, r4
	add	r3, r7, r5, lsl #3
.ifdef LATE
	add	r3, r3, r10, lsl #1
.endif
	eor	r2, r2, r3
	mov	r2, r2, lsl #17
	mov	r2, r2, lsr #17
	strh	r2, [r6], #2
	add	r8, r8, #1
	cmp	r8, #240
	blt	xl
	add	r7, r7, #1
	cmp	r7, #160
	blt	yl
	b	main
	.pool
.ifndef LATE
	.ascii	"SRAM_V113\0\0\0"	@ makes mGBA detect 32 KiB SRAM
.endif
