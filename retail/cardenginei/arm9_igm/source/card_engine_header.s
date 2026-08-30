@---------------------------------------------------------------------------------
	.section ".init"
@---------------------------------------------------------------------------------
	.global _start
	.global igmText
	.global sharedAddr
	.global waitSysCyclesLoc
	.global scfgExtBak
	.global scfgClkBak
	.global igmPal
	.global codeJump
	.global codeJump1
	.global codeJumpWord
	.global changeMpu
	.global revertMpu
	.align	4
	.arm

#define ICACHE_SIZE	0x2000
#define DCACHE_SIZE	0x1000
#define CACHE_LINE_SIZE	32

@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

#Text is placed here
@
@ !!! MIRROR OF sizeof(struct IgmText) IN retail/common/include/igm_text.h !!!
@
@ Nothing checks this. Growing IgmText in C without growing this reserves too little here, so every
@ symbol after it -- sharedAddr, the palette, and the entry point the cardengine jumps to -- lands
@ short of where the C side computes it, and opening the in-game menu jumps into the middle of the
@ text block. That is exactly what happened when the RetroAchievements entry was added: 0xF40 became
@ 0xF68 in the header and stayed 0xF40 here, and Contra 4 died on the frame the menu opened.
@
@ 0xF7C since the Achievements page: raMenu grew from one label to two, which is twenty bytes. The
@ C side has a static_assert on the same number, so the two disagree only if both are edited wrong
@ -- which is the most this arrangement can offer, because nothing links these files.
@
@ The .align 4 below is a power of two -- 16 bytes -- and matches IGM_TEXT_SIZE_ALIGNED's & ~0xF.
@
igmText:
.space 0xF7C
.align 4

sharedAddr:
.word 0
waitSysCyclesLoc:
.word 0
scfgExtBak:
.word 0
scfgClkBak:
.hword 0
igmPal:
.hword 0xFFFF @ White
.hword 0xDEF7 @ Light gray
.hword 0xCE73 @ Darker gray
.hword 0xF355 @ Light blue
.hword 0x801B @ Red
.hword 0x8360 @ Lime
.hword 0x0000 @ Black (BG)
.hword 0x0010 @ Red (Exception BG)
.align 4

card_engine_start:

@---------------------------------------------------------------------------------
igm_arm9:
@---------------------------------------------------------------------------------
	stmfd   sp!, {r2-r11,lr}

	bl		inGameMenu

	ldmfd   sp!, {r2-r11,pc}

@---------------------------------------------------------------------------------
codeJump: codeJump1: .word codeJump+4
@---------------------------------------------------------------------------------
	ldr pc, codeJumpWord
codeJumpWord:
.word	0

@---------------------------------------------------------------------------------
changeMpu: .word changeMpu+4
@---------------------------------------------------------------------------------
	stmfd   sp!, {r0-r2,lr}

	ldr r1, =mpuBits
	ldr r2, =0x027FF017

	mrc	p15,0,r0,c6,c0,0
	str r0, [r1]
	mov	r0, #0x35
	mcr	p15,0,r0,c6,c0,0

	mrc	p15,0,r0,c6,c2,0
	str r0, [r1, #4]
	cmp r0, r2
	beq changeMpu3
	mov	r0, #0
	mcr	p15,0,r0,c6,c2,0

changeMpu3:
	mrc	p15,0,r0,c6,c3,0
	str r0, [r1, #8]
	mov	r0, #0x08000000
	add	r0, r0, #0x35
	mcr	p15,0,r0,c6,c3,0

	ldmfd   sp!, {r0-r2,pc}

@---------------------------------------------------------------------------------
revertMpu: .word revertMpu+4
@---------------------------------------------------------------------------------
	stmfd   sp!, {r0-r1,lr}

	ldr r1, =mpuBits

	ldr r0, [r1]
	mcr	p15,0,r0,c6,c0,0
	ldr r0, [r1, #4]
	mcr	p15,0,r0,c6,c2,0
	ldr r0, [r1, #8]
	mcr	p15,0,r0,c6,c3,0

	ldmfd   sp!, {r0-r1,pc}
.pool

@ MPU region backups
mpuBits:
.word	0 @ Region 0
.word	0 @ Region 2
.word	0 @ Region 3

card_engine_end:
