@---------------------------------------------------------------------------------
@ Entry point for cardenginei_arm9_ra, the RetroAchievements code that does not fit
@ in the ARM9 cardengine's 12K window. Loaded into DSi WRAM and called once per
@ frame; see docs/retroachievements.md.
@
@ The layout is the colour LUT's, deliberately, because the loader recognises it:
@ the first word must be a branch, so `*(u16*)(location + 2)` reads 0xEA00 and an
@ unloaded window -- which holds whatever was there before -- can be told apart from
@ real code. The flags word at +4 is there for the launcher to write configuration
@ into before the binary is copied across, the same way the colour LUT receives its.
@---------------------------------------------------------------------------------
	.section ".init"
@---------------------------------------------------------------------------------
	.align	4
	.arm

card_engine_start:

	b ra_wram_tick

.global raWramFlags
raWramFlags:
.word 0

.pool

card_engine_end:
