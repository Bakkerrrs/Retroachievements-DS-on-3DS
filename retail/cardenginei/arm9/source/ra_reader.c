/*
    RetroAchievements support for nds-bootstrap -- the cardengine side.

    What is left here after the watchlist moved out: the per-frame entry point, the
    snapshot buffer, and the bridge into cardenginei_arm9_ra. About a third of what this
    file used to be.

    The watchlist and the pointer-chain walker live in the WRAM binary now, and this file
    deliberately knows nothing about how they work -- it hands over a pointer to the
    snapshot and lets the other side fill it in. The reason is the 12K window this is
    linked into: at its tightest it had 28 bytes spare, and a single watch cost 24 of
    them. The other side has 256K.

    The overlay went the same way, and later, for the same reason taken to its end: a font for
    printable ASCII is 760 bytes and sprite code is more, against a window that was refusing
    four-byte fields. It draws from over there now and this file does not know it exists.

    What could not move is the snapshot. It is the only debug channel this project has --
    there is no console inside an injected cardengine, so everything is read as hex
    through the in-game menu's RAM viewer -- and it is read at a fixed address in the
    cardengine's .bss that the RAM viewer is known to reach. Keeping it here also means
    the counters below still work when the WRAM binary is absent, which is exactly when
    you most want to see them.

    This file is part of nds-bootstrap and is licensed under the GPL-3.0,
    the same terms as the rest of the project.
*/

#include "ra_reader.h"
#include "locations.h"

#if RA_READER_ENABLED

/*
    Lives in the cardengine's .bss, which is inside the region reserved for the
    cardengine, so the game can never scribble on it. The bootloader copies only the
    loaded image, which ends where .bss begins, and there is no crt0 to zero what
    follows -- so every field is garbage until claim() says otherwise.

    Aligned to 16 because the in-game menu's RAM viewer can only jump to addresses that
    are a multiple of 0x10.
*/
/* Global so the link map names it; tools/ra_snapshot_addr.sh reads it from there. */
raSnapshot raSnapshotBuffer __attribute__((aligned(16)));
#define snapshot raSnapshotBuffer

/*
    cardengine.c's own pointer to the shared block, whose address depends on the game's SDK version.
    Borrowed rather than recomputed, so there is one answer in this binary to "where is it".
*/
extern vu32* volatile sharedAddr;

/* The ARM9's current scanline. The only clock here the game does not own. */
#define RA_VCOUNT (*(vu16*)0x04000006)

/* 192 visible lines plus 71 of vblank; VCOUNT runs 0..262 and restarts. */
#define RA_SCANLINES_PER_FRAME 263

/*
    Initialise the buffer the first time anything touches it, so it becomes readable as
    soon as any part of the cardengine runs rather than only once the per-frame hook is
    working.

    The watch results are deliberately not cleared here. The WRAM binary owns them,
    claims them with its own magic, and is the only thing that writes them -- clearing
    them from this side would be this file having an opinion about a structure it no
    longer maintains.
*/
static void claim(void) {
	if (snapshot.magic[0] == RA_SNAPSHOT_MAGIC0
	 && snapshot.magic[1] == RA_SNAPSHOT_MAGIC1
	 && snapshot.magic[2] == RA_SNAPSHOT_MAGIC2
	 && snapshot.magic[3] == RA_SNAPSHOT_MAGIC3) {
		return;
	}

	snapshot.magic[0] = RA_SNAPSHOT_MAGIC0;
	snapshot.magic[1] = RA_SNAPSHOT_MAGIC1;
	snapshot.magic[2] = RA_SNAPSHOT_MAGIC2;
	snapshot.magic[3] = RA_SNAPSHOT_MAGIC3;

	snapshot.ticks     = 0;
	snapshot.linesLast = 0;
	snapshot.linesMax  = 0;
	snapshot.wramMagic = 0;
	snapshot.wramTicks = 0;
	snapshot.wramState = RA_WRAM_ABSENT;
}

/*
    Everything this fork does per frame, from the game's VCOUNT interrupt handler.

    consoleModel > 0 is the 3DS family, which is what this fork supports; see the scope
    note in docs/retroachievements.md. The check is here as well as at hook-install time
    because the colour LUT installs the same handler and does run on a DSi, so being
    called is not proof the reader was wanted.
*/
void ra_tick(u8 consoleModel, bool wramLoaded) {
	const u16 startLine = RA_VCOUNT;
	u32 lines;

	if (consoleModel == 0) {
		return;
	}

	claim();
	snapshot.ticks++;
	/*
	    Step 3b: tell the WRAM binary where the shared block is. It cannot work that out -- the address
	    depends on the game's SDK version and only this side knows which -- and a guess would be four
	    bytes written into a running game. Republished every frame rather than once, because this file
	    makes no assumption about what order anything here ran in.
	*/
	snapshot.shared = (u32)sharedAddr;
	/*
	    From misc.c, where the re-arm lives. Published here rather than there for the same reason the
	    overlay's counters are: this file owns the snapshot and the others stay leaves.

	    Note what a non-zero rearmTable together with a rising `ticks` means -- the hook was removed and
	    put back, and the reader is alive *because* of the re-arm rather than in spite of it.
	*/
	{
		extern u8 raRearmTable, raRearmIe, raRearmDispstat;
		snapshot.rearmTable    = raRearmTable;
		snapshot.rearmIe       = raRearmIe;
		snapshot.rearmDispstat = raRearmDispstat;
	}

	/*
	    Hand the frame to cardenginei_arm9_ra, which evaluates the watchlist.

	    Two gates, because there are two ways for this to be wrong and they are worth
	    telling apart from a RAM viewer. wramLoaded is the bootloader's claim that it
	    copied the binary in. The branch check is whether that claim is true: the window
	    is DSi WRAM, which holds whatever the previous occupant left, so an unloaded or
	    half-copied window reads as plausible garbage rather than as zeroes. A branch is
	    the first instruction of the binary by construction -- the same check the colour
	    LUT makes on itself for the same reason.

	    Calling into a window that failed either gate would be a jump into arbitrary
	    data, in an interrupt handler, in the middle of a game.
	*/
	if (!wramLoaded) {
		snapshot.wramState = RA_WRAM_ABSENT;
	} else if (*(const vu16*)(CARDENGINEI_ARM9_RA_LOCATION + 2) != 0xEA00) {
		snapshot.wramState = RA_WRAM_NO_CODE;
	} else {
		((void (*)(raSnapshot*))CARDENGINEI_ARM9_RA_LOCATION)(&snapshot);
		snapshot.wramState = RA_WRAM_CALLED;
	}

	/*
	    Open question #2 was what the per-frame cost is; this measures it instead of
	    arguing about it. The handler fires at line 0, but VCOUNT is read rather than
	    assumed, so a tick that spans the end of the frame has to be handled: the counter
	    runs 0..262 and then restarts, so a smaller end line means it wrapped and the
	    elapsed count is the rest of the frame plus the end line. Reported in a u8, so a
	    pathological tick is pinned at 255 rather than truncated to something that reads
	    as cheap.

	    The scope is now the overlay and the WRAM call as well, which is the honest one:
	    it is what the game pays per frame for all of this.
	*/
	{
		const u16 endLine = RA_VCOUNT;
		lines = (endLine >= startLine)
			? (u32)(endLine - startLine)
			: (u32)(RA_SCANLINES_PER_FRAME - startLine + endLine);
		if (lines > 255) {
			lines = 255;
		}
		snapshot.linesLast = (u8)lines;
		if (lines > snapshot.linesMax) {
			snapshot.linesMax = (u8)lines;
		}
	}
}

#endif /* RA_READER_ENABLED */
