/*
    RetroAchievements support for nds-bootstrap -- on-screen text overlay (DSi WRAM).

    Composites text over the running game, on its **own sprites** where it can and on a borrowed
    background layer where it cannot.

    Two paths, and the order matters. Objects first: an object at a given priority is drawn above
    every background at that priority, so it needs no layer at all, and what it does need -- a
    disabled OAM entry and a range of object VRAM nobody references -- it *finds* rather than
    borrows. Confirmed on hardware to cost the running game nothing observable: no scenery, no
    sprites, no tiles. See spriteShow().

    Backgrounds second, as the fallback for a game with 2D object mapping, with objects switched
    off, or with nothing free. That path works and was measured to work, at the price of one
    background layer for three seconds.

    Three lessons from hardware are built into both, and every one of them cost a run.

    A game's VRAM layout is not fixed. One game moved its BG2 character base onto the block the
    overlay had chosen at boot, so holding that block meant overwriting its tiles. Nothing here may
    assume a spot stays free: every claim is re-surveyed each frame and given back on eviction.

    Being drawn is not being seen. The DS puts the lower-numbered background in front when two share
    a priority, so on any layer above an enabled priority-0 one the overlay is drawn perfectly and
    covered completely. Free-and-hidden looked exactly like drawn-and-lost, and took five hypotheses
    to separate. See chooseLayer().

    And "unused right now" is not "not wanted". A game builds its object list from index 0 up each
    frame and leaves the rest disabled, so the earliest free OAM entries are the next ones it will
    need -- taking those deleted its bullets. See chooseSprites().

    The rule the whole file holds to: a notification that corrupts the game is worse than no
    notification, and one that is correct and never appears is not a notification.

    It lived in the ARM9 cardengine until sprites needed writing, and the window that had already
    refused a four-byte diagnostic field was never going to hold OAM negotiation. Moving it here
    gave the cardengine **1,228 bytes free** where it had 60, and cost nothing: this binary is
    called once a frame from the same VBlank hook, so the overlay runs exactly as often as before.

    Two things came free with the move. The trigger count is now this frame's rather than last
    frame's, so a notification is raised on the frame the achievement fires. And the rendered strip
    is a neighbour's pointer instead of an address crossing a binary boundary, so the range check
    that guarded it is gone -- there is nothing left to distrust.

    This file is part of nds-bootstrap and is licensed under the GPL-3.0,
    the same terms as the rest of the project.
*/

#include "ra_overlay.h"
#include "locations.h"
#include "ra_wifi.h"   /* raSessionBlock -- the launcher's overlay switch */

#if RA_READER_ENABLED

#define SUB_DISPCNT  (*(vu32*)0x04001000)
/*
    The sub engine's master brightness. Read at show() time for one reason: a real unlock fires when a
    stage ends, which is exactly when a game fades the screen -- and a fade applies to our glyphs too.
    That would explain a notification that reports drawn, is never denied or evicted, and is not seen,
    while the demo timer's identical call *is* seen because it fires on an ordinary frame.
*/
#define SUB_MASTER_BRIGHT (*(vu16*)0x0400106C)

#define SUB_BGCNT(i) (*(vu16*)(0x04001008 + (i) * 2))
/*
    The window registers, read only to be reported. A layer the game has excluded from every active
    window is a layer that is enabled and drawn and shows nowhere -- which is one of the two mechanisms
    left that could explain the notification going missing. See raSnapshot.overlayWindow.
*/
#define SUB_WININ    (*(vu16*)0x04001048)
#define SUB_WINOUT   (*(vu16*)0x0400104A)
/* Scroll is per layer, four bytes apart -- not always BG0's. */
#define SUB_BGHOFS(i) (*(vu16*)(0x04001010 + (i) * 4))
#define SUB_BGVOFS(i) (*(vu16*)(0x04001012 + (i) * 4))

/* Standard sub-engine BG palette: 16 banks of 16 in 4bpp mode. */
#define SUB_BG_PALETTE ((vu16*)0x05000400)

/* Sub BG VRAM: bank C, 128K at 0x06200000, so eight 16K character base blocks. */
#define SUB_BG_VRAM 0x06200000
#define CHAR_BLOCKS 8

/*
    ...but only the first four can be *borrowed*, and this is a bug fix rather than a restriction.

    BGCNT holds the character base in bits 2-5 -- four bits, 16K units, so any of the eight -- and the
    screen base in bits 8-12: **five bits, 2K units**, which reaches 62K and no further. The offsets
    that would extend both (DISPCNT bits 24-29) are main-engine only; the sub engine has none. So a map
    placed inside block 4 or above needs a screen base of 32 or more, which does not fit the field: it
    truncates, and the map lands inside a low block the game may well be using. Two kilobytes of tilemap
    written over somebody's tiles.

    The survey still examines all eight, because a block the *game* is using has to be detected wherever
    it is. Only the choice is narrowed. That makes `denied` a little more likely and silent corruption
    impossible, which is the same trade the enable-bit fix made and it goes the same way.
*/
#define CHOOSABLE_BLOCKS 4

/*
    Is the sub engine actually dimming or whitening the screen right now?

    Bits 0-4 are the blend factor and bits 14-15 are the mode: 0 and 3 mean *no effect at all*, 1 blends
    toward white and 2 toward black. So the factor field on its own says nothing -- a game may leave a
    stale factor of 16 sitting there with the mode off and the screen perfectly normal.

    This is a correction, and it matters because the earlier reading was over-read. `overlayState` bit 5
    came back set on Contra 4 and was reported as "confirmed: the notification landed inside a fade".
    All it ever proved was that bits 0-4 were non-zero, which is a much weaker claim than the one made
    from it. There was no room in the window to publish the register itself, so bit 5 now consults the mode
    and the raw value stays live at 0x0400106C for anyone who wants it.

    A function rather than a macro, because this window is measured in single bytes and there are two call
    sites: expanded at both, this overflowed it. It reads the register itself rather than taking it as a
    parameter -- both callers want it live, and the two forms measure the same.
*/
static bool brightActive(void) {
	const u16 b = SUB_MASTER_BRIGHT;
	const u16 mode = b & 0xC000;
	return (mode == 0x4000 || mode == 0x8000) && (b & 0x1F) != 0;
}

/*
    Is the sub engine showing its backgrounds at all right now?

    Two ways it can be told not to, neither of which the fade gate had ever looked at, and neither of
    which any counter here could have detected:

      bit 7        forced blank. The engine outputs white and displays nothing -- not a layer, not a
                   sprite. Games set it while rebuilding a scene, which is exactly when an achievement
                   for finishing that scene fires.
      bits 16-17   the display mode. 0 is off, and no background appears in it however its own
                   registers read.

    And this *was* the bug, confirmed on hardware rather than reasoned about. Through the whole of stage 1
    on Contra 4 the pulse probe was visible, reading the placeholder arm9_ra renders at init; at the
    stage-clear transition it went blank for a stretch; at the start of stage 2 it came back reading
    **"Welcome to the jungle"**, the real name of the achievement that had fired during the blank. `shows`
    43 against 10,339 ticks, which is exactly 10,339 / 240, with nothing denied or evicted.

    So the notification had always been drawn correctly, into a screen that was not being displayed.
*/
static bool screenHidden(void) {
	const u32 d = SUB_DISPCNT;

	return (d & (1u << 7)) != 0 || ((d >> 16) & 3) == 0;
}

#define OVERLAY_PAL_BANK 15

/*
    The glyphs are drawn in two colour indices -- RA_TEXT_INK and RA_TEXT_SHADOW, which ra_text.c
    writes as nibbles -- so exactly two palette entries have to be borrowed. An earlier
    version saved and whitened all sixteen in the bank, which cost nothing to write and
    everything to a game using those colours: on Final Fantasy III's title screen
    fourteen entries the overlay never needed went white for the three seconds a
    notification was up, and came back when it hid. A transient graphical fault tied to
    the toaster appearing, for no benefit.

    Two entries the game may be using is one more than before, and it is the price of the drop
    shadow. It was worth paying: white glyphs on a game's own artwork are legible only where the
    artwork happens to be dark, which on a photograph of a three-second toaster is exactly the
    complaint that started this. The blast radius is still bounded to what the design needs rather
    than fifteen times it, and both entries are saved and put back on hide.
*/
#define OVERLAY_PAL_ENTRY  (OVERLAY_PAL_BANK * 16 + RA_TEXT_INK)
#define OVERLAY_PAL_SHADOW (OVERLAY_PAL_BANK * 16 + RA_TEXT_SHADOW)

/* The two colours themselves: white text over black, which is the pairing that survives any
   background the game puts underneath it. */
#define OVERLAY_INK_COLOUR    0x7FFF
#define OVERLAY_SHADOW_COLOUR 0x0000

/*
    Which character row of the sub screen the strip sits on, counting from the top.

    Row 1 rather than the middle of the screen, because a notification belongs out of the way and
    the way is wherever the game is being played. Not row 0: flush against the top edge reads as a
    rendering fault on a screen with any bezel at all, and one row of clearance costs eight pixels.
    The horizontal half of the same decision lives in ra_text.c, which right-aligns the text against
    RA_TEXT_MARGIN -- together they put the message in the top right corner.
*/
#define OVERLAY_ROW 1

/*
    Frames the notification stays up, and it is three seconds again rather than whatever the tick rate
    happened to be. This counter advances once per call, and the reader now runs from the game's VBlank
    handler -- so a call is a frame. It was not: on the VCOUNT hook Contra 4 cleared the Y-trigger every
    frame and the reader ran on about 8% of them, which stretched this to half a minute.
*/
#define OVERLAY_SHOW_FRAMES 180

/*
    How long a notification will wait for the screen to stop fading before giving up and showing anyway.

    Ticks, and a tick is a frame again: the reader runs from the game's VBlank handler now. It was 600 and
    described as ten seconds while the reader was on the VCOUNT hook, which was wrong by an order of
    magnitude -- Contra 4 cleared the Y-trigger every frame, the reader ran on about 8% of them, and 600
    of those was minutes rather than seconds. A notification owed for minutes is one that never arrives.

    90 stays, now meaning what it says: a second and a half, which is longer than any transition and short
    enough that a game leaving the screen dimmed -- a dark room, a pause menu -- cannot swallow the
    notification. Waiting longer buys nothing; if the screen is still dimmed after 90 frames it is not a
    fade, and no amount of patience fixes that.
*/
#define OVERLAY_FADE_WAIT_TICKS 90

/*
    Was: raise the overlay on a timer, because when this was written there was no achievement logic to
    trigger it. There is now -- rcheevos delivers RC_RUNTIME_EVENT_ACHIEVEMENT_TRIGGERED and the count
    reaches here as raSnapshot.rcTriggered -- so the timer is off and the notification means something.

    Kept rather than deleted, at 0 so it compiles out, and overridable from the command line -- which
    is what it is for. Contra 4 froze `ticks` at 19, so the reader dies about three tenths of a second
    into the game and nothing can ever unlock; a build with -DOVERLAY_DEMO_INTERVAL=60 answers "is
    ra_tick still being called" by pulsing once a second on screen, with no RAM viewer and nothing to
    interpret. It also exercises the borrow-and-return negotiation without spending an achievement.
*/
#ifndef OVERLAY_DEMO_INTERVAL
#define OVERLAY_DEMO_INTERVAL 0
#endif


/*
    Guarded by a magic value rather than a bool: the cardengine's .bss was never zeroed, so a plain flag
    started as whatever happened to be in RAM.

    Kept after the move even though this binary's .bss *is* cleared, by ra_startup(). It costs four bytes
    of a 256K window and it is the only thing that would notice if that ever stopped being true.
*/
#define STATE_MAGIC 0x5241564C  /* "RAVL" */

static u32  stateMagic;
static u32  framesLeft;   /* non-zero while visible */
#if OVERLAY_DEMO_INTERVAL
static u16  demoCounter;
#endif
static u32  lastUnlocks;
/*
    A notification owed but not yet raised, because the screen was being faded when it was earned.

    Measured, not guessed: a real unlock in Contra 4 read `shows 1` with denied, evicted and
    deniedNoLayer all zero -- borrowed, held, drew, nobody took it back -- and was not seen, while the
    demo timer's identical call in the same game was. overlayState bit 5 came back set, so the sub
    engine's master brightness was non-zero: the glyphs were drawn into a screen on its way to black and
    spent their 180 frames inside the transition.

    Which is exactly when an achievement completes. A stage ends, the game fades, and that is the moment
    rcheevos fires.
*/
static u8   pending;        /* a notification is owed */
/*
    Ticks it has waited for the fade to end. A byte, because the bound is 90 -- so it needs no clamp
    before it is published and it fits alongside `pending` in seven bits.
*/
static u8   pendingFrames;
static int  block;        /* character base block currently borrowed */
static int  layer;        /* background layer currently borrowed */
static u16  savedBgCnt;
static u16  savedHofs;
static u16  savedVofs;
static u16  savedPaletteEntry;
static u16  savedPaletteShadow;
static bool savedDispcntBg;
/*
    The object path's state. `usingSprites` is what hide() and the per-frame hold branch on: the two
    paths take different things and must give back exactly what they took.
*/
static u8   usingSprites;
/*
    The strip that is on screen, kept so the tiles can be written again next frame.

    Saved rather than taken from the tick's argument: a second unlock while the first is still up
    changes what the tick is handed, and the notification that is *showing* is the one whose pixels
    have to stay correct. Nothing here changes which notification that is.
*/
static const void* shownText;
/*
    The last word spriteBlit() wrote, so the tail probe has something to compare against.

    Kept rather than recomputed because the sprite blit is a *gather* -- it rearranges the strip's
    background layout into per-object order -- so the word at the end of our object VRAM is not the
    word at the end of the strip, and working out which one it is means redoing the index arithmetic
    the blit already did.
*/
static u32  spriteLastWord;
static u8   spriteOam[8];
static u8   spriteSlot;
static u32  spriteBase;
static u32  spriteTile;
static u16  savedObjPaletteEntry;
static u16  savedObjPaletteShadow;

/* Read into the snapshot, so the negotiation is observable rather than guessed at. */
u32 raOverlayShows;
u32 raOverlayDenied;   /* wanted to show, found nothing free */
u32 raOverlayEvicted;  /* the game reclaimed the block mid-notification */
/*
    How many frames the probe found our pixels overwritten and put them back.

    Reported because it is the only way to know whether re-asserting them was needed at all. Zero on
    a game that leaves object VRAM alone -- Contra 4 is expected to read zero -- and climbing on the
    game whose notification looked wrong, which is what turns "this should help" into a reading.
*/
u32 raOverlayRedrawn;
/*
    Of those denials, the ones where no background layer was switched off -- as opposed
    to no VRAM block being spare. The two have different answers, and lumping them
    together left the obvious hypothesis untestable: on Final Fantasy III the
    notification went missing across map transitions, where the screen fades and the
    game plausibly has every sub layer enabled to do it. If that is right, this counter
    accounts for nearly all of them.
*/
u32 raOverlayDeniedNoLayer;
/* SUB_DISPCNT bit 30 as of the last show(); see raSnapshot.overlayExtPal. */
/* The sub engine's state and our choice out of it, as of the last show(). See raSnapshot. */
u8  raOverlayState;
/*
    The sub engine as the game had it, at the moment of the last show(). Whole registers rather than
    picked bits: picking the bit is what went wrong with the fade. See raSnapshot.overlayDispcnt.
*/
u32 raOverlayDispcnt;
/*
    Three readings the background path takes anyway, published because a photograph cannot recover
    them. See raSnapshot.overlaySavedInk for what each one decides.
*/
/*
    The nine VRAM bank registers, which this overlay has never once looked at.

    Every "is this block free?" question it has ever asked was answered by *inference*: read the four
    BGCNTs, work out which 16K blocks they reference, call the rest spare. That reasoning has been
    wrong on hardware three times, and it was always reasoning about the wrong thing. Which memory the
    sub engine can even see is not a deduction -- it is nine bytes of I/O space, and bit 7 of each says
    whether the bank is mapped at all.

    A bank with its enable bit clear is not "probably unused". The game **cannot address it**: it is
    not in any memory map the game's code can reach. Nothing can overwrite what we put there, and
    nothing of the game's is lost by our taking it. That is a different class of answer from anything
    surveyBlocks() can produce.

    Which matters most for bank I: 16K, the smallest of the nine, and it maps to **sub-engine object
    VRAM** at MST 2. The eight sprites this overlay draws need about a kilobyte. If a game leaves I
    disabled, the notification can have tile memory that was never anyone's.

    0x04000247 is skipped because it is not a VRAM bank -- it is WRAMCNT, sitting in the middle of the
    range between G and H.

    Two readings, because they answer different questions:

      raOverlayVramCnt    the nine registers as of the last show(), the same moment overlayDispcnt is
                          taken: what the layout was when we decided.

      raOverlayVramEverOn a bit per bank, accumulated **every frame**: has this bank ever been enabled
                          this session? A bank that was off when we looked may be the game's a frame
                          later -- that is the exact mistake surveyBlocks() made with layer enable
                          bits and paid for on Contra 4. A bank that has never once been on for the
                          whole session is a far stronger claim, and it is the one worth acting on.
*/
#define VRAM_CNT_BASE 0x04000240
#define VRAM_BANKS    9

/* A..G are 0x240..0x246; 0x247 is WRAMCNT; H and I are 0x248 and 0x249. */
static const u8 vramCntOffset[VRAM_BANKS] = { 0, 1, 2, 3, 4, 5, 6, 8, 9 };

u8  raOverlayVramCnt[VRAM_BANKS];
u16 raOverlayVramEverOn;

static void vramAccumulate(void) {
	const vu8* const cnt = (const vu8*)VRAM_CNT_BASE;
	int i;

	for (i = 0; i < VRAM_BANKS; i++) {
		if (cnt[vramCntOffset[i]] & 0x80) {
			raOverlayVramEverOn |= (u16)(1u << i);
		}
	}
}

static void vramSnapshot(void) {
	const vu8* const cnt = (const vu8*)VRAM_CNT_BASE;
	int i;

	for (i = 0; i < VRAM_BANKS; i++) {
		raOverlayVramCnt[i] = cnt[vramCntOffset[i]];
	}
}

u16 raOverlaySavedInk;
u16 raOverlaySavedShadow;
u16 raOverlayBgCnt;
u32 raOverlayWindow;
/*
    Which path drew, and what it took. 0xFF in raOverlaySpriteOam means the background path -- so the two
    are never ambiguous the way "layer 0, block 0" would be if the object path reported through
    overlayState's layer and block bits.
*/
u8  raOverlaySpriteOam = 0xFF;
u8  raOverlaySpriteSlot = 0xFF;

/*
    The three shapes a background can have, which is the distinction this survey used to be blind to.
    Text was the only one it knew, and the other two put different meanings on the very same BGCNT
    fields -- so reading them as text did not make the answer imprecise, it made it wrong.
*/
#define LAYER_TEXT     0
#define LAYER_AFFINE   1
#define LAYER_EXTENDED 2

/*
    What the BG mode makes of each layer.

    BG0 and BG1 are always text here. Only BG2 and BG3 change shape with the mode:

    | mode | BG2      | BG3      |
    |------|----------|----------|
    | 0    | text     | text     |
    | 1    | text     | affine   |
    | 2    | affine   | affine   |
    | 3    | text     | extended |
    | 4    | affine   | extended |
    | 5    | extended | extended |

    Modes 6 and 7 do not exist on this engine -- libnds refuses the large bitmap on the sub display
    outright ("Sub Display has no large Bitmaps") and 7 is not a mode at all -- and they are handled
    before this is ever called. See raOverlaySurvey().
*/
static int layerKind(u32 mode, int layer) {
	if (layer < 2) {
		return LAYER_TEXT;
	}
	if (layer == 2) {
		switch (mode) {
			case 2: case 4: return LAYER_AFFINE;
			case 5:         return LAYER_EXTENDED;
			default:        return LAYER_TEXT;
		}
	}
	switch (mode) {
		case 0:         return LAYER_TEXT;
		case 1: case 2: return LAYER_AFFINE;
		default:        return LAYER_EXTENDED;
	}
}

/*
    Mark every 16K block the byte range [start, start + bytes) touches.

    Written as an intersection rather than as arithmetic on a start block so that a base pointing
    outside the sub engine's 128K simply matches nothing. Both base fields can express that: the
    character base is four bits of 16K units, which reaches 240K, and a bitmap's base is five bits of
    16K units, which reaches 496K. There is no block of ours to protect out there, and guessing how the
    hardware folds such an address back would be exactly the kind of unmeasured assumption that has
    cost this project runs before.
*/
static void markRange(bool* used, u32 start, u32 bytes) {
	const u32 end = start + bytes;
	int b;

	if (bytes == 0) {
		return;
	}
	for (b = 0; b < CHAR_BLOCKS; b++) {
		const u32 lo = (u32)b * 0x4000;

		if (start < lo + 0x4000 && end > lo) {
			used[b] = true;
		}
	}
}

/*
    Which 16K blocks of sub BG VRAM the game is using, for tiles, maps or bitmaps.

    Split from the registers it used to read so that the host suite can put a whole BG configuration in
    front of it and check the answer -- which is the only way the mode handling below gets tested at
    all, because reaching it on hardware means finding a game that uses a non-text background on the
    sub engine and then earning an achievement in it.

    `dispcnt` and the four `bgcnt` entries are the sub engine's, and `used` has CHAR_BLOCKS entries.
*/
void raOverlaySurvey(u32 dispcnt, const u16* bgcnt, bool* used) {
	/*
	    Text map entries are two bytes, affine map entries are one, and an extended-affine map is
	    affine-shaped with two-byte entries. Sizes 0-3 are 32x32/64x32/32x64/64x64 tiles for text and
	    16x16/32x32/64x64/128x128 tiles for both affine kinds.

	    A bitmap is measured in pixels instead -- 128x128/256x256/512x256/512x512 -- and its depth
	    comes from bit 2, which for every tiled kind is part of the character base.
	*/
	static const u32 textMap[4]      = {  2048,  4096,  4096,  8192 };
	static const u32 affineMap[4]    = {   256,  1024,  4096, 16384 };
	static const u32 extAffineMap[4] = {   512,  2048,  8192, 32768 };
	static const u32 bitmapPixels[4] = { 128*128, 256*256, 512*256, 512*512 };

	const u32 mode = dispcnt & 7;
	int i;

	/*
	    A mode this engine does not have says nothing that can be acted on, so the survey answers the
	    only safe way it can: everything is spoken for, and the notification is denied. That is the same
	    trade the rest of this function makes -- a notification that does not appear is a missing
	    feature, a corrupted game is a bug.
	*/
	for (i = 0; i < CHAR_BLOCKS; i++) {
		used[i] = (mode > 5);
	}
	if (mode > 5) {
		return;
	}

	for (i = 0; i < 4; i++) {
		const u16 cnt = bgcnt[i];
		const u32 size = (u32)(cnt >> 14) & 3;
		const u32 screenBase = (u32)(cnt >> 8) & 0x1F;
		int kind;

		/*
		    **No layer is skipped, including the one about to be borrowed**, and that exception is
		    what put garbage on Ketsui's screen for three seconds.

		    It used to `continue` past the borrowed layer, on the reasoning that the overlay is taking
		    that layer so its VRAM is going spare. The layer is taken. Its **VRAM is not** -- the game
		    still owns the tiles and the tilemap sitting there, and chooseLayer() promises exactly that
		    in as many words: "the overlay points its character and screen bases at a block of its own,
		    so the game's tiles and tilemap for this layer sit there untouched throughout". The skip
		    made that promise impossible to keep, because the block the game's own layer was using was
		    the one block the survey could not see.

		    Hardware, read out of the snapshot on a real unlock: `overlayState` 0x6A -- layer 1, block
		    1 -- with `overlayDispcnt` 0x00010700 showing BG0, BG1 and BG2 all enabled. So BG1 was
		    borrowed and block 1 was chosen, and block 1 is what BG1 was using. The overlay wrote its
		    glyphs and a full 32x32 tilemap into a live layer's character block while the game kept
		    writing into it too. Garbage where the notification should be, in the place and for the
		    duration the notification would have had, which is exactly what was reported.

		    It also contradicted the paragraph immediately below, which already says a block referenced
		    by *any* layer counts as in use, enabled or not. This was the one layer exempted from the
		    rule the rest of the function had already learned the hard way.
		*/
		/*
		    A layer that is off right now was skipped here, on the reasoning that its VRAM is not in
		    use. Hardware says that reasoning is wrong, and expensively so.

		    Deferring the notification past a fade made it land in the middle of stage 2 of Contra 4 and
		    glitch the whole screen for 180 frames. The survey had sampled the registers at a moment when
		    the game had layers off, concluded their blocks were free, taken one, and then the game turned
		    the layer back on -- with its character base full of our glyphs.

		    And there is direct evidence the game does this constantly rather than occasionally:
		    raSnapshot.rearmDispstat clamped at 255, which counts how often it had to put the Y-trigger
		    back into the very register this survey trusts. A game that rewrites SUB_DISPCNT every frame
		    is a game whose enable bits say nothing about what it owns.

		    So a block referenced by *any* layer counts as in use, enabled or not. Strictly more
		    conservative, and the trade is the right way round: this makes `denied` more likely and
		    corruption less, and a notification that does not appear is a missing feature while a
		    corrupted game is a bug.
		*/

		kind = layerKind(mode, i);

		if (kind == LAYER_EXTENDED && (cnt & 0x0080)) {
			/*
			    A bitmap, and three fields change meaning at once. The base is the screen-base field
			    in **16K** units rather than 2K -- libnds says so in as many words, offering the
			    bitmap's offset as `mapBase * 16KB` while asserting the tile base is unused. Bit 2
			    selects 16-bit direct colour over 8-bit paletted instead of carrying the low bit of a
			    character base. And there is no character base to mark at all, because there are no
			    tiles: reading one here is how the old survey could mark a block nobody was using
			    while missing the several the bitmap actually covers.
			*/
			markRange(used, screenBase * 0x4000,
			          bitmapPixels[size] * ((cnt & 0x0004) ? 2u : 1u));
			continue;
		}

		/*
		    Tiled: character base in 16K units, map at the screen base in 2K units.

		    Only one block is marked for the tiles, which is unchanged and remains a heuristic rather
		    than a reading. How far tile data actually reaches is not in any register -- it depends on
		    how many distinct tiles the map references -- and the worst case is 64K of 256-colour
		    tiles, which would deny almost every notification. See the note in docs.
		*/
		markRange(used, (u32)((cnt >> 2) & 0xF) * 0x4000, 0x4000);
		markRange(used, screenBase * 0x800,
		          kind == LAYER_TEXT   ? textMap[size] :
		          kind == LAYER_AFFINE ? affineMap[size] : extAffineMap[size]);
	}
}

/*
    Read from the live registers every time, because what they hold changes as the game switches
    scenes -- which is the whole reason a block chosen at boot is not safe to keep.
*/
static void surveyBlocks(bool* used) {
	u16 cnt[4];
	int i;

	for (i = 0; i < 4; i++) {
		cnt[i] = SUB_BGCNT(i);
	}
	raOverlaySurvey(SUB_DISPCNT, cnt, used);
}

/*
    Tiles at the start of the borrowed block, map 4K in: one block covers both.

    4K rather than the 2K it used to be, because the tiles grew. Sixty-five of them are needed now --
    one blank for clearing the map, then RA_TEXT_TILES of text -- which is 2,080 bytes and would have
    run 32 bytes into a map sitting at 2K. The whole 16K block is ours while borrowed, so moving the
    map costs nothing; leaving it would have put the last tile of the message underneath the tilemap
    that displays it.
*/
static vu32* tilesOf(int b)   { return (vu32*)(SUB_BG_VRAM + b * 0x4000); }
static vu16* mapOf(int b)     { return (vu16*)(SUB_BG_VRAM + b * 0x4000 + 0x1000); }
static u16   bgCntFor(int b)  { return (u16)((b << 2) | (((b * 8) + 2) << 8)); }

/*
    A layer the game currently has switched off. Taking one it is using would displace
    its graphics, which is exactly the mistake that corrupted them before.
*/
static int chooseLayer(void) {
	int i;

	for (i = 0; i < 4; i++) {
		if (!(SUB_DISPCNT & (1u << (8 + i)))) {
			/*
			    Free, and reachable: every enabled layer before it holds priority 1 or worse, which the
			    overlay's priority 0 beats. This is the ordinary case and it disturbs nothing.
			*/
			return i;
		}
		if ((SUB_BGCNT(i) & 3) == 0) {
			/*
			    Enabled, at priority 0, and therefore in front of every layer after it -- so no later
			    layer can be seen and there is no priority left to outrank it with. **Take this one.**

			    Which is a deliberate change of policy, and the reason is that the alternative delivers
			    nothing. Refusing here is honest and was measured to be useless: at the start of stage 2
			    on Contra 4 the game's BG0 is on at priority 0, and that is exactly when the stage-1
			    achievement's notification is due. A notification that is correct and never appears is not
			    a notification.

			    What it costs is bounded and reversible, and both halves matter. **The layer's VRAM is
			    never touched** -- the overlay points its character and screen bases at a block of its own,
			    so the game's tiles and tilemap for this layer sit there untouched throughout. And every
			    register taken is saved and put back by hide(): BGCNT, both scroll registers, and the
			    enable bit. So the layer's content is intact the whole time and simply does not display
			    for the three seconds the notification is up.

			    Measured on hardware and it works: `overlayState` 0x48 on a real unlock -- layer 0, block 1,
			    deferred and released -- with the achievement's name on screen, stable, no flicker. The
			    prediction that displacing an enabled layer would bring back the "who writes last" race
			    against the game's own code did not come true.

			    Sprites are the answer that costs nothing at all -- an OBJ at a given priority draws above
			    every background at that priority, whatever the game is doing with its layers -- and they
			    are the next piece of work. This stays as the fallback for when no object slot is free.
			*/
			return i;
		}
	}
	/*
	    All four enabled and none at priority 0, so the overlay outranks every one of them: any layer will
	    be seen and the last is as good as any. Free to handle rather than deny -- `return -1` here would
	    have been a refusal in a case where nothing was actually in the way.
	*/
	return 3;
}

/*
    The one byte that says what the overlay chose and what it chose it from.

    Packed after the layer and block are settled and before anything is written, so the reading is what
    the survey had to work from paired with what it decided. A function rather than inline in draw()
    because there are two paths now and both have to report -- and the object path has no layer or block
    to report, which is exactly why raSnapshot.overlaySpriteOam exists alongside it.

    See raSnapshot.overlayState for the bit layout.
*/
static void overlayStateFor(int lay, int blk) {
	raOverlayState = (u8)(((SUB_DISPCNT & (1u << 30)) ? 1 : 0)
	                      | ((lay & 3) << 1)
	                      | ((blk & 3) << 3)
	                      /* bit 5: the screen was being faded when this was raised */
	                      | (brightActive() ? 0x20 : 0)
	                      /* bit 6: this one was held back until a fade ended */
	                      | (pendingFrames ? 0x40 : 0));
}

/*
    The pixels and the map, and nothing that has to happen only once.

    Split out of draw() because the tick calls this every frame now, and draw() *saves the palette
    entries it is about to overwrite*. Called twice, the second call would have saved our own colours
    as the game's originals and hide() would have restored those -- leaving two entries of the game's
    palette permanently ours. A real bug, introduced and caught in the same edit, and the split is
    what makes the per-frame path unable to do it.
*/
static void drawTiles(int b, const void* text) {
	vu32* tiles = tilesOf(b);
	vu16* map = mapOf(b);
	const u32* src = (const u32*)text;
	int i;

	/*
	    Copied, not generated. This used to hold eleven glyphs in message order and expand them a bit at
	    a time; the font and the expansion live in cardenginei_arm9_ra now, where a font for printable
	    ASCII costs 760 bytes of a 256K window instead of competing with the code that draws it. What is
	    left here is the part that had to stay: everything that negotiates with the game for the VRAM.

	    Words, never bytes -- DS VRAM ignores 8-bit writes -- and tile 0 is skipped so it stays blank for
	    the map clear below.
	*/
	for (i = 0; i < RA_TEXT_WORDS; i++) {
		tiles[8 + i] = src[i];
	}

	for (i = 0; i < 32 * 32; i++) {
		map[i] = 0;  /* tile 0 is blank, so this clears the layer */
	}
	for (i = 0; i < RA_TEXT_TILES; i++) {
		const int row = i / RA_TEXT_COLS;
		const int col = i % RA_TEXT_COLS;

		map[(OVERLAY_ROW + row) * 32 + col] = (u16)((i + 1) | (OVERLAY_PAL_BANK << 12));
	}
}

/* ...and the once-only half: the state reading and the two palette entries this borrows. */
static void draw(int b, const void* text) {
	overlayStateFor(layer, block);
	savedPaletteEntry  = SUB_BG_PALETTE[OVERLAY_PAL_ENTRY];
	savedPaletteShadow = SUB_BG_PALETTE[OVERLAY_PAL_SHADOW];
	raOverlaySavedInk    = savedPaletteEntry;
	raOverlaySavedShadow = savedPaletteShadow;
	SUB_BG_PALETTE[OVERLAY_PAL_ENTRY]  = OVERLAY_INK_COLOUR;
	SUB_BG_PALETTE[OVERLAY_PAL_SHADOW] = OVERLAY_SHADOW_COLOUR;
	drawTiles(b, text);
}


/*
    ============================================================================================
    The sprite path, which is the one that costs the game nothing.
    ============================================================================================

    An object at a given priority is drawn *above every background* at that priority, whatever the game
    is doing with its layers. That is the whole reason for this: the background path works and was
    measured to work, but it can only be seen by taking a layer the game may be using, and on Contra 4
    that means a piece of the gameplay screen goes missing for three seconds.

    An object costs nothing that is in use, because what it needs -- a disabled OAM entry and a range of
    object VRAM nobody references -- can be *found* rather than borrowed. When it cannot be found, the
    background path is still there.

    Eight objects of 32x16 pixels, side by side, which is 256x16 -- exactly the 32x2 tiles of the strip.
    Eight rather than four 64x32 ones because 64x16 is not a shape the DS has, and four of the larger
    size would reserve twice the object VRAM to leave half of it blank.

    What is deliberately *not* supported, and falls back instead of guessing:

      2D mapping     (DISPCNT bit 4 clear) addresses object tiles as a 32-wide matrix rather than
                     consecutively, so the arithmetic below is simply wrong for it. Contra 4 uses 1D.
      OBJ disabled   (bit 12 clear) would mean enabling it, and a game that keeps it off may well have
                     an OAM full of entries it never intended to be seen. Turning it on would show them.
*/

/* Sub engine OAM, and sub object tile VRAM. */
#define SUB_OAM        ((vu16*)0x07000400)
#define SUB_OBJ_VRAM   0x06600000
/* Sub engine object palette: sixteen banks of sixteen, alongside the background one. */
#define SUB_OBJ_PALETTE ((vu16*)0x05000600)
#define OBJ_PAL_ENTRY   (OVERLAY_PAL_BANK * 16 + RA_TEXT_INK)
#define OBJ_PAL_SHADOW  (OVERLAY_PAL_BANK * 16 + RA_TEXT_SHADOW)

#define OAM_ENTRIES 128
#define RA_SPRITES  8            /* 8 x 32px = 256px, the screen's width */
#define RA_SPRITE_TILES 8        /* 4x2 tiles each */
#define RA_SPRITE_BYTES (RA_SPRITE_TILES * 32)   /* 4bpp */

/*
    Object VRAM is claimed in 2K units, which is what one strip needs: 8 sprites x 256 bytes.

    Only the first 16K is ever considered, and that is a mapping question rather than a tidiness one.
    How much VRAM is actually behind 0x06600000 depends on the game's VRAMCNT assignment, and a write
    past the end does not fail -- it mirrors, onto tiles the game *is* using. 16K is the smallest
    allocation a game that uses sub objects realistically makes, so staying inside it means the survey's
    answer and the hardware's agree.
*/
#define OBJ_SLOT_BYTES 2048
#define OBJ_SLOTS      8

/*
    Which 2K units of object VRAM the game's own sprites reference.

    Every entry is measured from its own shape, size and colour depth, because a sprite's tile
    footprint is not knowable any other way -- a 64x64 8bpp object is thirty-two times the VRAM of an
    8x8 4bpp one.

    **Every entry, including the disabled ones, and that is a correction rather than caution.** This
    used to skip a disabled entry on the reasoning that an object which is not being drawn is not
    using its tiles. It is not true: an entry off *this* frame still points at tile data the game
    means to keep, and toggling entries while leaving their banks loaded is the ordinary way to run a
    lot of sprites. A shooter does it constantly.

    Ketsui is what proved it. Its notification showed as garbage for the full three seconds, its own
    graphics broke, and it froze at a stage transition -- three symptoms of one cause, which is this
    survey calling a slot free and the blit writing over a parked sprite bank. Three rounds of making
    the overlay put its pixels back could not win that: the game was writing there too, and the only
    winning move is not to take the slot.

    The exception is our own eight entries, excluded by index. They used to be excluded *by* being
    disabled, which is what tied this function to the rule above -- so removing that rule meant
    naming them instead, or the overlay would evict itself on its first frame.
*/
static void surveyObjVram(bool* used, u32 boundary) {
	int i;

	for (i = 0; i < OBJ_SLOTS; i++) {
		used[i] = false;
	}

	for (i = 0; i < OAM_ENTRIES; i++) {
		const u16 attr0 = SUB_OAM[i * 4];
		const u16 attr1 = SUB_OAM[i * 4 + 1];
		const u16 attr2 = SUB_OAM[i * 4 + 2];
		u32 start, end, k;
		int wide, high;

		if (usingSprites) {
			int k2;
			int mine = 0;

			for (k2 = 0; k2 < RA_SPRITES; k2++) {
				if (spriteOam[k2] == (u8)i) {
					mine = 1;
					break;
				}
			}
			if (mine) {
				continue;
			}
		}

		/* Shape in attr0 bits 14-15, size in attr1 bits 14-15. */
		switch (((attr0 >> 14) & 3) * 4 + ((attr1 >> 14) & 3)) {
			case  0: wide = 1; high = 1; break;   /* square   8x8   */
			case  1: wide = 2; high = 2; break;   /*         16x16  */
			case  2: wide = 4; high = 4; break;   /*         32x32  */
			case  3: wide = 8; high = 8; break;   /*         64x64  */
			case  4: wide = 2; high = 1; break;   /* wide    16x8   */
			case  5: wide = 4; high = 1; break;   /*         32x8   */
			case  6: wide = 4; high = 2; break;   /*         32x16  */
			case  7: wide = 8; high = 4; break;   /*         64x32  */
			case  8: wide = 1; high = 2; break;   /* tall     8x16  */
			case  9: wide = 1; high = 4; break;   /*          8x32  */
			case 10: wide = 2; high = 4; break;   /*         16x32  */
			case 11: wide = 4; high = 8; break;   /*         32x64  */
			/*
			    Shape 3 is prohibited, so this is a register the game should never have written. Taken as
			    the largest object there is rather than the smallest: an invalid shape that under-states
			    its footprint would let this survey call somebody's tiles free.
			*/
			default: wide = 8; high = 8; break;
		}

		/*
		    attr0 bit 13 is the colour depth: 8bpp tiles are 64 bytes, 4bpp are 32.

		    **Both readings of the tile number are marked used, and that is deliberate insurance.** In 1D
		    mapping the number steps by the boundary from DISPCNT bits 20-21, which is what `boundary` is;
		    the classic reading, and the only one the GBA had, is a flat 32 bytes. If the interpretation
		    here is wrong in the direction of *too large*, the game's tiles appear further out in VRAM than
		    they are and this survey calls a slot free that is not -- and then the blit writes over
		    somebody's sprite.

		    Marking the union costs one extra range per entry and makes that impossible. It makes a free
		    slot slightly rarer, which is a fallback to the background path rather than a fault.

		    The overlay's *own* placement is not exposed to the same risk, and by construction rather than
		    by luck: spriteBlit() writes to the byte address of the slot the survey approved, computed
		    without reference to the boundary at all. A wrong boundary would make our objects *read* their
		    tiles from the wrong place -- a notification of visible nonsense -- and could never make them
		    write to it.
		*/
		{
			const u32 bytes = (u32)wide * (u32)high * ((attr0 & 0x2000) ? 64u : 32u);
			const u32 tile  = (u32)(attr2 & 0x03FF);
			int pass;

			for (pass = 0; pass < 2; pass++) {
				start = tile * (pass ? 32u : boundary);
				end   = start + bytes;
				for (k = start / OBJ_SLOT_BYTES;
				     k <= (end - 1) / OBJ_SLOT_BYTES && k < OBJ_SLOTS; k++) {
					used[k] = true;
				}
			}
		}
	}
}

/*
    Eight disabled OAM entries, scanning from the **back**.

    This scanned from the front, on the reasoning that objects are ordered among themselves by OAM index
    -- lower is in front -- so the earliest free entries are the ones least likely to be covered by one of
    the game's own sprites. Hardware outweighed that reasoning immediately, and the way it did is worth
    keeping: on Contra 4 the notification appeared and the game started losing *sprites* -- bullets, and
    sometimes the player -- erratically, for exactly as long as it was up.

    The mistake was believing "disabled right now" meant "not wanted". A game builds its object list from
    index 0 upward each frame, writes as many entries as it has sprites, and leaves the rest disabled --
    then DMAs the whole thing in its own VBlank handler. Ours is chained *after* that, so whatever the game
    had just put in the eight entries we hold is overwritten before the screen is drawn, every frame. The
    entries were not free. They were the next ones the game was going to need.

    Nothing avoids that except choosing entries the game does not reach, and the back is where those are:
    stealing from 127 downward only costs the game a sprite when it is already using more than 120, where
    stealing from 0 upward cost it one almost immediately.

    What it gives up is being in front of the game's own sprites -- at index 120 we are behind nearly all
    of them, so a bullet crossing the text wins that pixel. That is a fair trade twice over: text with a
    bullet through it is still legible, and a deleted bullet is a bug. And it gives up nothing that
    matters, because an object still beats **every background** at the same priority, which is the entire
    reason for this path.

    Returns the count found, filling `out`. They do not have to be consecutive.
*/
static int chooseSprites(u8* out) {
	int i;
	int n = 0;

	for (i = OAM_ENTRIES - 1; i >= 0 && n < RA_SPRITES; i--) {
		if ((SUB_OAM[i * 4] & 0x0300) == 0x0200) {
			out[n++] = (u8)i;
		}
	}
	return n;
}

/*
    ...and the first word of each object's own tiles, for the same reason: the blit is a gather, so
    which source word lands at the start of object k is not something the probe can work out without
    redoing the blit's index arithmetic. Recorded as it writes them.
*/
static u32  spriteFirstWord[RA_SPRITES];

/*
    Copy the strip into object VRAM, rearranged.

    The strip is laid out for a background: row 0 is 32 tiles, then row 1 is 32 tiles. An object with 1D
    mapping wants its own tiles consecutive, so sprite k needs the four tiles of row 0 at columns 4k..4k+3
    followed by the four of row 1 at the same columns. A gather rather than a copy, and the reason the
    strip is not simply stored in this order is that the background path needs the other one -- and
    keeping one layout with one rearrangement beats keeping two layouts that can disagree.
*/
static void spriteBlit(u32 base, const void* text) {
	const u32* src = (const u32*)text;
	vu32* dst = (vu32*)(SUB_OBJ_VRAM + base);
	int k, row, col, w;

	for (k = 0; k < RA_SPRITES; k++) {
		for (row = 0; row < RA_TEXT_ROWS; row++) {
			for (col = 0; col < 4; col++) {
				const int srcTile = row * RA_TEXT_COLS + k * 4 + col;

				for (w = 0; w < 8; w++) {
					spriteLastWord = src[srcTile * 8 + w];
					if (row == 0 && col == 0 && w == 0) {
						spriteFirstWord[k] = spriteLastWord;
					}
					*dst++ = spriteLastWord;
				}
			}
		}
	}
}

/* The three attributes of one of our objects, written fresh each frame while visible. */
static void spriteWrite(int k, u32 tileNumber) {
	const int i = spriteOam[k];

	/*
	    attr0: Y, rotation flag clear, not disabled, normal mode, 16-colour, shape 1 (wide).
	    attr1: X, no flip, size 2 -- which with shape 1 is 32x16.
	    attr2: tile number, priority 0, our palette bank.

	    attr3 is **not** touched, and that is not an omission. The fourth halfword of every OAM entry is
	    part of the interleaved affine matrix table, so a disabled entry's may hold a live parameter for a
	    sprite the game is rotating. Writing it would corrupt that sprite's matrix.
	*/
	SUB_OAM[i * 4]     = (u16)((OVERLAY_ROW * 8) | (1 << 14));
	SUB_OAM[i * 4 + 1] = (u16)((k * 32) | (2 << 14));
	SUB_OAM[i * 4 + 2] = (u16)(tileNumber | (OVERLAY_PAL_BANK << 12));
}

/*
    Try to show the notification on objects. Returns false to let the background path have it.
*/
static bool spriteShow(const void* text) {
	bool used[OBJ_SLOTS];
	const u32 d = SUB_DISPCNT;
	u32 boundary;
	int slot;
	int k;

	/* 1D mapping and objects already on, or this is not our path. See the note above. */
	if (!(d & (1u << 4)) || !(d & (1u << 12))) {
		return false;
	}
	/* DISPCNT bits 20-21: 32, 64, 128 or 256 bytes per tile-number step. */
	boundary = 32u << ((d >> 20) & 3);

	if (chooseSprites(spriteOam) < RA_SPRITES) {
		return false;
	}

	surveyObjVram(used, boundary);
	for (slot = 0; slot < OBJ_SLOTS; slot++) {
		if (!used[slot]) {
			break;
		}
	}
	if (slot == OBJ_SLOTS) {
		return false;
	}

	/*
	    The slot has to land on a tile number the boundary can express. 2K is a multiple of every
	    boundary the DS has, so this division is exact and the check is a statement of that rather than a
	    guard against it.
	*/
	spriteTile = (u32)slot * OBJ_SLOT_BYTES / boundary;
	spriteBase = (u32)slot * OBJ_SLOT_BYTES;
	spriteSlot = (u8)slot;
	raOverlaySpriteOam  = spriteOam[0];
	raOverlaySpriteSlot = (u8)slot;

	savedObjPaletteEntry  = SUB_OBJ_PALETTE[OBJ_PAL_ENTRY];
	savedObjPaletteShadow = SUB_OBJ_PALETTE[OBJ_PAL_SHADOW];
	SUB_OBJ_PALETTE[OBJ_PAL_ENTRY]  = OVERLAY_INK_COLOUR;
	SUB_OBJ_PALETTE[OBJ_PAL_SHADOW] = OVERLAY_SHADOW_COLOUR;

	shownText = text;
	spriteBlit(spriteBase, text);
	for (k = 0; k < RA_SPRITES; k++) {
		spriteWrite(k, spriteTile + (u32)k * RA_SPRITE_BYTES / boundary);
	}

	usingSprites = 1;
	return true;
}

/* Put every entry back the way it was found: disabled, with its own attributes. */
static void spriteHide(void) {
	int k;

	for (k = 0; k < RA_SPRITES; k++) {
		const int i = spriteOam[k];

		SUB_OAM[i * 4]     = 0x0200;   /* rotation flag clear, disabled */
		SUB_OAM[i * 4 + 1] = 0;
		SUB_OAM[i * 4 + 2] = 0;
	}
	SUB_OBJ_PALETTE[OBJ_PAL_ENTRY]  = savedObjPaletteEntry;
	SUB_OBJ_PALETTE[OBJ_PAL_SHADOW] = savedObjPaletteShadow;
	usingSprites = 0;
}

static void show(const void* text) {
	bool used[CHAR_BLOCKS];
	int b;
	int l;

	/*
	    Captured before a single register of the game's is disturbed, and before the layer is even chosen.
	    Both orderings matter: the layer-enable bit the overlay is about to set would otherwise show up
	    here as the game's own, and a *denial* has to record the state that caused it -- which it could
	    not when this lived in draw(), because a denial never reaches draw().
	*/
	raOverlayDispcnt = SUB_DISPCNT;
	vramSnapshot();
	raOverlayWindow  = (u32)SUB_WININ | ((u32)SUB_WINOUT << 16);

	/*
	    Nothing to say, so nothing is drawn. The strip comes from cardenginei_arm9_ra, which is also
	    where the unlock came from, so a trigger with no text means that binary rendered nothing -- a
	    bug over there, not a shortage over here. Counted as a denial anyway rather than dropped
	    silently: `shows` staying at 0 with `denied` climbing is a reading, and `shows` staying at 0
	    with everything else at 0 is the ambiguity that already cost one hardware run.
	*/
	if (!text) {
		raOverlayDenied++;
		return;
	}

	/*
	    Objects first, always. They cost the game nothing -- a disabled OAM entry and a range of object
	    VRAM nobody references are *found*, not borrowed -- where the background path can only be seen by
	    taking a layer the game may be using. It falls back rather than insisting; see spriteShow().
	*/
	if (spriteShow(text)) {
		framesLeft = OVERLAY_SHOW_FRAMES;
		raOverlayShows++;
		overlayStateFor(0, 0);
		return;
	}

	l = chooseLayer();
	if (l < 0) {
		raOverlayDenied++;
		raOverlayDeniedNoLayer++;
		return;  /* every layer in use: stay quiet rather than displace one */
	}

	surveyBlocks(used);
	for (b = 0; b < CHOOSABLE_BLOCKS; b++) {
		if (!used[b]) {
			break;
		}
	}
	if (b == CHOOSABLE_BLOCKS) {
		raOverlayDenied++;
		return;  /* no spare VRAM: stay quiet rather than corrupt the game */
	}

	raOverlaySpriteOam  = 0xFF;
	raOverlaySpriteSlot = 0xFF;
	layer = l;
	block = b;
	savedBgCnt = SUB_BGCNT(l);
	raOverlayBgCnt = savedBgCnt;
	savedHofs  = SUB_BGHOFS(l);
	savedVofs  = SUB_BGVOFS(l);
	savedDispcntBg = (SUB_DISPCNT & (1u << (8 + l))) != 0;

	shownText = text;
	draw(b, text);

	SUB_BGCNT(l)  = bgCntFor(b);
	SUB_BGHOFS(l) = 0;
	SUB_BGVOFS(l) = 0;
	SUB_DISPCNT |= (1u << (8 + l));

	framesLeft = OVERLAY_SHOW_FRAMES;
	raOverlayShows++;
}

static void hide(void) {
	framesLeft = 0;
	shownText  = 0;

	if (usingSprites) {
		spriteHide();
		return;
	}

	SUB_BGCNT(layer)  = savedBgCnt;
	SUB_BGHOFS(layer) = savedHofs;
	SUB_BGVOFS(layer) = savedVofs;
	if (!savedDispcntBg) {
		SUB_DISPCNT &= ~(1u << (8 + layer));
	}
	SUB_BG_PALETTE[OVERLAY_PAL_ENTRY]  = savedPaletteEntry;
	SUB_BG_PALETTE[OVERLAY_PAL_SHADOW] = savedPaletteShadow;
}

void ra_overlay_tick(u32 unlocks, const void* text) {
	/*
	    `overlay=0` in ra.cfg, carried in the session block the launcher staged.

	    Checked here, at the top, rather than at each of the places that draw: this is the one part of
	    this fork that negotiates with a running game for its hardware, and a switch that turns it off
	    should turn off *all* of it -- the surveys and the register reads included, not only the
	    pixels. Everything else about an unlock keeps working; see RA_CFG_OVERLAY.

	    Guarded on the magic, so a boot that staged nothing gets the popup rather than losing it: an
	    absent block must not read as a refusal.
	*/
	{
		const raSessionBlock* const session =
			(const raSessionBlock*)CARDENGINEI_ARM9_RA_SESSION_LOCATION;

		if (session->magic == RA_SESSION_MAGIC && session->overlay == 0) {
			return;
		}
	}

	if (stateMagic != STATE_MAGIC) {
		stateMagic = STATE_MAGIC;
		framesLeft = 0;
#if OVERLAY_DEMO_INTERVAL
		demoCounter = 0;
#endif
		pending = 0;
		pendingFrames = 0;
		/*
		    Synced to whatever the count already is, not to zero. Claiming with 0 would fire the
		    notification once for a session that had already unlocked something before this ran.
		*/
		lastUnlocks = unlocks;
		raOverlayShows = 0;
		raOverlayDenied = 0;
		raOverlayEvicted = 0;
		raOverlayRedrawn = 0;
		raOverlayDeniedNoLayer = 0;
	}

	/*
	    Published every tick, before any of the early returns below -- otherwise the one state worth
	    reporting, a notification stuck waiting, would be the one state that returns before reporting it.
	*/
	/*
	    Bit 7: a notification is owed and has not been raised yet. Refreshed here rather than in draw(),
	    which is the whole point -- draw() only runs when one *is* raised, so the state that needed
	    reporting was the one state nothing reported.

	    That absence cost a hardware run. Contra 4 came back with rcTriggered 1 and unlockSent 1 --
	    rcheevos fired and the id reached the ARM7 -- and shows, denied, evicted and deniedNoLayer all
	    zero. show() always increments one of those, so it had never been called: the notification was
	    still owed, held by a fade gate that could not pass, and nothing said so. It read as "nothing
	    happened", which is the one thing it was not.

	    A bit rather than a field, and that is a budget decision rather than a preference: the ARM9
	    cardengine's window overflowed by 44 bytes when this was a word of its own carrying the wait
	    count and the raw brightness register with it. Bit 7 of a byte that already exists costs nothing,
	    and it answers the only question that could not be answered any other way -- the register itself
	    is live at 0x0400106C for anyone who wants the value.
	*/
	raOverlayState = (u8)((raOverlayState & 0x7F) | (pending ? 0x80 : 0));
	/*
	    Every frame, and cheap enough to be: nine byte reads from I/O space against the ~47 scanlines
	    the reader's memref pass already spends. It has to be every frame -- the whole point is to
	    catch a bank the game turns on for one scene and off again.
	*/
	vramAccumulate();

	if (framesLeft) {
		if (usingSprites) {
			bool objUsed[OBJ_SLOTS];
			const u32 boundary = 32u << ((SUB_DISPCNT >> 20) & 3);
			int k;

			/*
			    Give the object VRAM back the moment the game wants it, for the same reason the
			    background path gives its block back: the survey is a sample of registers the game
			    rewrites, and a range that was free when the notification started can be the game's a
			    frame later. Our own eight entries are disabled while surveying, so they cannot make
			    the slot look used to us.
			*/
			surveyObjVram(objUsed, boundary);
			if (objUsed[spriteSlot]) {
				raOverlayEvicted++;
				hide();
				return;
			}

			if (--framesLeft == 0) {
				hide();
				return;
			}

			/*
			    Rewritten every frame rather than left in place, because a game that keeps a shadow copy
			    of OAM and DMAs the whole thing each frame -- which is the ordinary way to do it -- would
			    otherwise wipe these on its next transfer. Same reasoning as re-asserting BGCNT below.
			*/
			for (k = 0; k < RA_SPRITES; k++) {
				spriteWrite(k, spriteTile + (u32)k * RA_SPRITE_BYTES / boundary);
			}
			/*
			    **And the same argument, applied to the two things it was not applied to.**

			    The reasoning above is about OAM and it is equally true of the *pixels* and of the two
			    palette entries: a game that DMAs its object tiles or its object palette every frame is
			    doing something exactly as ordinary as DMAing OAM, and both were written once at show()
			    and never again. What that produces is an OAM entry of ours pointing at tiles that are
			    now the game's, in colours that are now the game's -- which is a notification that
			    appears, in the right place, made of the wrong pixels. Inconsistent between games, and
			    consistent within one, which is what was reported.

			    The eviction survey cannot catch it either: it looks for an OAM entry referencing our
			    slot, and a blanket transfer of tile data with no entry pointing here passes it while
			    having overwritten every byte.

			    Costs 512 word writes and two halfwords, and only on the frames a notification is up.
			*/
			SUB_OBJ_PALETTE[OBJ_PAL_ENTRY]  = OVERLAY_INK_COLOUR;
			SUB_OBJ_PALETTE[OBJ_PAL_SHADOW] = OVERLAY_SHADOW_COLOUR;
			/*
			    **Checked before rewritten**, and that is the difference between 512 writes a frame
			    and two reads.

			    The first version blitted unconditionally. It is only about two kilobytes and it
			    measured cheap in scanlines, but this runs inside a game's VBlank on top of a memref
			    pass already using 47 of the 70 available -- and this fork has already killed one game
			    by adding per-frame work to that budget. Ketsui hung at a boss kill, which is both the
			    moment an achievement fires and the moment a bullet-hell shooter has least to spare.

			    Two probes rather than one: the first and last words of our range, so a transfer that
			    landed on either end is caught. A transfer that overwrote the middle and left both ends
			    intact would be missed, and that is a stripe of wrong pixels for a frame rather than a
			    frame the game did not get -- the right way round for a notification to fail.
			*/
			if (shownText) {
				const vu32* ours = (const vu32*)(SUB_OBJ_VRAM + spriteBase);
				const u32   words = RA_SPRITES * RA_SPRITE_BYTES / 4;
				const u32   step  = RA_SPRITE_BYTES / 4;
				int         stale = 0;
				u32         w;

				/*
				    **One probe per object, not two for the whole strip.**

				    The first version probed the first and last words and said so: a transfer that
				    overwrote the middle and left both ends intact would be missed. Hardware found
				    exactly that. Contra 4 was correct either way because nothing there touches this
				    range; Ketsui showed garbage for the full three seconds, because whatever it
				    transfers lands between the two words that were being checked and the strip was
				    therefore never put back.

				    Nine reads against 512 writes, and a game overwriting a contiguous run cannot
				    now clear every object's first word without being caught.
				*/
				for (w = 0; w < words; w += step) {
					if (ours[w] != spriteFirstWord[w / step]) {
						stale = 1;
						break;
					}
				}
				if (!stale && ours[words - 1] != spriteLastWord) {
					stale = 1;
				}
				if (stale) {
					spriteBlit(spriteBase, shownText);
					raOverlayRedrawn++;
				}
			}
			return;
		}

		{
			bool used[CHAR_BLOCKS];

			/*
			    Give the block back the moment the game wants it. Holding on is exactly
			    what wrecked its graphics before: it moved a character base onto this
			    block and the overlay kept rewriting the tiles underneath.
			*/
			surveyBlocks(used);
			if (used[block]) {
				raOverlayEvicted++;
				hide();
				return;
			}

			if (--framesLeft == 0) {
				hide();
				return;
			}

			/* Hold the layer: the game rewrites these registers itself. */
			SUB_BGCNT(layer)  = bgCntFor(block);
			SUB_BGHOFS(layer) = 0;
			SUB_BGVOFS(layer) = 0;
			SUB_DISPCNT |= (1u << (8 + layer));
			/*
			    ...and the tiles and the palette, for the reason spelled out on the sprite path above:
			    holding the registers that *point* at our pixels does nothing for a game that rewrote
			    the pixels. A background path is if anything more exposed, since the block it borrowed
			    is character VRAM the game may be streaming tiles into.
			*/
			SUB_BG_PALETTE[OVERLAY_PAL_ENTRY]  = OVERLAY_INK_COLOUR;
			SUB_BG_PALETTE[OVERLAY_PAL_SHADOW] = OVERLAY_SHADOW_COLOUR;
			/*
			    Probed the same way, and this path had more to lose: drawTiles() also clears a
			    thousand map entries, so doing it unconditionally every frame was the more expensive
			    of the two by a wide margin.
			*/
			if (shownText) {
				const vu32* ours = tilesOf(block);
				const u32*  src  = (const u32*)shownText;
				int         stale = 0;
				u32         w;

				/*
				    Probed across the strip rather than at its two ends, for the reason spelled out on
				    the sprite path: a transfer landing in the middle was missed, and the notification
				    stayed wrong for its whole three seconds instead of for one frame. Every 32nd word
				    is one probe per tile.
				*/
				for (w = 0; w < RA_TEXT_WORDS; w += 8) {
					if (ours[8 + w] != src[w]) {
						stale = 1;
						break;
					}
				}
				if (stale) {
					drawTiles(block, shownText);
					raOverlayRedrawn++;
				}
			}
		}
		return;
	}

	/*
	    The real trigger, and the reason OVERLAY_DEMO_INTERVAL is now 0: an achievement unlocked.

	    Strictly greater, and resynced when it drops. rcheevos' count is reset when the runtime is
	    re-initialised, and a count that went backwards must not be read as a new unlock.
	*/
	/*
	    Raise what is owed, once the screen is worth drawing on. Checked before the trigger below so a
	    second unlock arriving while the first is still waiting does not queue behind itself -- one
	    notification is one notification, and `lastUnlocks` has already moved past both.
	*/
	if (pending) {
		/*
		    A hidden screen does not count against the bound, and that distinction is the whole fix.

		    The bound exists for a *fade*, which is a state a game can sustain -- a dark room, a pause
		    menu, a brightness setting -- so a notification owed during one has to be released eventually
		    or it is lost. "Might not be visible" deserves a deadline.

		    Forced blank and display-off are not that. They are "definitely not visible", and no game runs
		    with its screen switched off for long, because it needs the screen. Counting those frames
		    toward a 90-frame deadline is how the notification gets released *into* the blank and thrown
		    away -- which is exactly what hardware showed: the stage-1-clear transition on Contra 4 lasts
		    well over a second and a half, and the pulse only survived it because another pulse arrived
		    after the screen came back. A single real notification would have expired inside it.

		    So this waits as long as it takes and shows on the first frame the screen is real again.
		    overlayState bit 7 stays set throughout, so a notification owed indefinitely -- a game that
		    never uses the sub engine at all -- reads as owed rather than as nothing happening.
		*/
		if (screenHidden()) {
			return;
		}
		if (brightActive() && pendingFrames < OVERLAY_FADE_WAIT_TICKS) {
			pendingFrames++;
			return;
		}
		pending = 0;
		/*
		    show() reads pendingFrames for bit 6 of overlayState, so it is cleared *after* the call --
		    zeroing it first would report every deferred notification as one that never waited, which
		    is precisely the fact this bit exists to confirm.
		*/
		show(text);
		pendingFrames = 0;
		return;
	}

	if (unlocks > lastUnlocks) {
		lastUnlocks   = unlocks;
		/*
		    Owed now, shown when the screen is not mid-fade. Deferring rather than drawing immediately
		    is the whole fix: the draw always worked, it was the moment that was wrong.
		*/
		pending       = 1;
		pendingFrames = 0;
		return;
	}
	if (unlocks < lastUnlocks) {
		lastUnlocks = unlocks;
	}

	#if OVERLAY_DEMO_INTERVAL
	/*
	    Raises it through `pending` rather than calling show() directly, which is not tidiness: show() is
	    static, and a second call site stops gcc inlining it. The demo build overflowed the window by 20
	    bytes with a direct call and fits with this one. It also means the probe exercises the deferral,
	    which is the path a real unlock takes.
	*/
	if (++demoCounter >= OVERLAY_DEMO_INTERVAL) {
		demoCounter = 0;
		pending = 1;
		pendingFrames = 0;
	}
	#endif
}

#endif /* RA_READER_ENABLED */
