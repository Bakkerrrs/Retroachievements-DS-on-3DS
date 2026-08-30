/*
    RetroAchievements support for nds-bootstrap -- shared definitions.

    Layering (kept deliberately strict, see docs/retroachievements.md):

      ra_reader   -- the per-frame bridge in the cardengine, plus the snapshot that is this
                     project's only debug channel. The watchlist it used to hold, and later
                     the overlay, both moved to cardenginei_arm9_ra where there is room.
      ra_overlay  -- draws a notification over the game. Knows nothing about RA: it is handed
                     a count and some pixels. Lives in cardenginei_arm9_ra.
      ra_text     -- the font and the renderer, also in cardenginei_arm9_ra. 760 bytes of
                     printable ASCII that never had anywhere else to go.
      ra_rcheevos -- the rcheevos runtime, the staged set, and the unlock ring that crosses
                     to the ARM7. cardenginei_arm9_ra.
      ra_net      -- HTTP transport to the RA servers, in the launcher.
      ra_patch    -- the streaming scanner that turns r=patch into the staged block.
      ra_wifi     -- the fifteen-rung ladder the launcher climbs, and the log it writes.

    `rc_client` is deliberately absent and stays absent -- see open question #4. The session
    layer is the launcher's ladder instead.

    This file is part of nds-bootstrap and is licensed under the GPL-3.0,
    the same terms as the rest of the project.
*/

#ifndef RA_H
#define RA_H

#include <nds/ndstypes.h>

/*
    Master switch for the whole RA reader. Set to 0 to build a cardengine that
    behaves exactly like upstream nds-bootstrap: no extra per-frame work and no
    interrupt hook of its own.

    Note what it no longer switches off, because that changed: the reader used to force
    IRQ_VCOUNT on for games that never asked for one, sharing the colour LUT's hook. It
    chains onto the game's own VBlank handler now -- an interrupt the game already enables
    for itself -- so a reader build no longer turns anything on that was off.

    Eight cardengine variants compile this file, and the reader is not wanted in all
    of them. Two groups opt out by default:

      DLDI -- nds-bootstrap running from a flashcard in DS mode. Out of scope for the
      fork, which targets the DSi-capable path, and out of room besides: after phase 1
      `arm9_twlsdk_dldi` and `arm9_twlsdk3_dldi` have 176 bytes left in their windows
      and `arm9_dldi` has 208, against the reader's ~550.

      GSDD -- cardRead() is where the per-frame hook installs itself, and these variants
      do not take that path. The reader could never tick there; carrying it was only ever
      dead weight.

    Override on the command line (-DRA_READER_ENABLED=1) to build it anyway.
*/
#ifndef RA_READER_ENABLED
#if defined(DLDI) || defined(GSDD)
#define RA_READER_ENABLED 0
#else
#define RA_READER_ENABLED 1
#endif
#endif

/*
    How many watches the reader evaluates per frame, and how many pointer
    indirections a single watch may walk.

    These are no longer budget decisions. The watchlist lives in cardenginei_arm9_ra --
    256K of DSi WRAM -- rather than in the ARM9 cardengine's 12K window, so a watch costs
    24 bytes of somewhere that has room instead of 24 bytes of somewhere that has tens.

    It was a budget decision until then, and the history is worth keeping: RA_WATCH_MAX
    went 4 -> 2 to pay for the bridge into the separate binary, because the bridge is what
    ended the competition. Now it is 16 because nothing argues for less.

    Two indirections cover the shape RetroAchievements actually uses on the DS -- a
    pointer to a player or save structure, then a field inside it -- and a third can be
    added when a game needs one.
*/
#define RA_WATCH_MAX 16
#define RA_CHAIN_MAX 2

/*
    How a watch resolved on the most recent tick. Reported per watch rather than
    as one global flag, because a chain that stops resolving is normal -- games
    null their pointers between scenes -- and the useful question is which watch
    stopped and where.
*/
#define RA_WATCH_UNUSED       0  /* free slot */
#define RA_WATCH_PENDING      1  /* added, not yet evaluated */
#define RA_WATCH_OK           2  /* resolved and read this tick */
#define RA_WATCH_BAD_BASE     3  /* the chain's first address is not readable */
#define RA_WATCH_BAD_POINTER  4  /* a pointer read mid-chain is not usable */
#define RA_WATCH_BAD_TARGET   5  /* the resolved address is not readable */
#define RA_WATCH_MISALIGNED   6  /* resolved address not aligned for its size */

/*
    Diagnostic watch: the sub engine's DISPCNT. Worth having as watch 0 because it
    really does change -- the overlay sets and clears a background-enable bit in it
    when it borrows a layer -- so a direct watch has something live to show before
    there is a game address to point at.
*/
#define RA_DEFAULT_WATCH_ADDRESS 0x04001000

/*
    One watched value, with its result stored alongside its definition so the whole
    thing is one contiguous run of memory in the in-game menu's RAM viewer.

    A watch is either direct (depth 0: read `base`) or a pointer chain (depth n:
    read the word at `base`, add offsets[0], read the word there, add offsets[1],
    and so on, then read the value). The chain is walked again from scratch on every
    tick, which is the point: a cached resolved address is wrong the moment the game
    moves the structure it pointed into.
*/
typedef struct raWatch {
	u32 base;                   /* +0x00  absolute address, or start of the chain */
	u32 offsets[RA_CHAIN_MAX];  /* +0x04  added after each indirection */
	u32 address;                /* +0x0C  resolved address this tick, 0 if unresolved */
	u32 value;                  /* +0x10  value read, zero-extended */
	u8  depth;                  /* +0x14  indirections to walk; 0 = direct */
	u8  size;                   /* +0x15  bytes to read: 1, 2 or 4 */
	u8  status;                 /* +0x16  RA_WATCH_* */
	u8  flags;                  /* +0x17  RA_WATCH_FLAG_* */
} raWatch;                      /*        0x18 bytes */

/*
    Treat each pointer read mid-chain as a 24-bit console pointer rather than a DS address:
    mask to 24 bits and add main RAM's base.

    This is not a convenience, it is what RetroAchievements means. Its DS memory map is
    console-relative, so a pointer stored by the game as 0x02xxxxxx is documented and used
    as its low 24 bits -- which is why the published code notes for a DS game say
    "[24-Bit Pointer]" rather than "pointer".

    Reading such a word as a 32-bit DS address is what the walker did first, and hardware
    said no: three direct watches resolved and the one chain came back BAD_TARGET, while
    rcheevos -- which does mask -- reported no refusal for the same location. The two
    disagreeing is what identified the model as wrong rather than the address.
*/
#define RA_WATCH_FLAG_PTR24 0x01

/*
    What the snapshot carries per watch, as opposed to what the watchlist keeps.

    The descriptors live in the WRAM binary now, and there can be RA_WATCH_MAX of them;
    the snapshot is a debug channel read through a hex viewer, so it mirrors only the
    first RA_RESULT_MAX and only the fields that change. `base` and `offsets` are static
    configuration -- if they were wrong the address would not resolve, which `status`
    already says.
*/
typedef struct raResult {
	u32 address;                /* +0x00  resolved this tick, 0 if unresolved */
	u32 value;                  /* +0x04 */
	u8  depth;                  /* +0x08  which of the defaults this is, in practice */
	u8  size;                   /* +0x09 */
	u8  status;                 /* +0x0A  RA_WATCH_* */
	u8  reserved;               /* +0x0B */
} raResult;                     /*        0x0C bytes */

#define RA_RESULT_MAX 4

/*
    Written into the snapshot by cardenginei_arm9_ra on its first call, so a counter
    coming from that binary can be told apart from whatever was in the buffer. Reads as
    "RAH1" in a byte-wise dump -- H for helper, since it is not the snapshot's own magic.
*/
#define RA_WRAM_MAGIC 0x31484152

/*
    How far the WRAM binary got in bringing itself up, reported so a failure names its own
    stage instead of showing up as silence. It only ever moves forward, and each value is
    reached exactly once per boot.

    The stages exist because the ones before rcheevos cannot be taken for granted. Nothing
    had ever run a library in this window: .bss arrives as whatever the previous occupant
    left, so it has to be zeroed by hand before newlib -- which assumes zeroed .bss like
    every other C library -- is allowed near it, and there is no crt0 here to do that.
*/
#define RA_STAGE_NONE     0  /* .bss still garbage, or the binary never ran */
#define RA_STAGE_BSS      1  /* .bss zeroed by hand; a C library may now be trusted */
#define RA_STAGE_HEAP     2  /* the arena is measured and _sbrk() will hand it out */
#define RA_STAGE_ALLOC    3  /* an allocation was made, written to and read back */
#define RA_STAGE_WATCHES  4  /* the watchlist is installed and evaluating */

/*
    What the cardengine made of the separate binary on the most recent tick. Reported so
    each link in the chain -- built, packed, loaded, copied, recognised, called -- fails
    visibly and separately rather than as one silent absence.
*/
#define RA_WRAM_ABSENT  0  /* the bootloader did not report it loaded */
#define RA_WRAM_NO_CODE 1  /* the window does not begin with a branch, so nothing valid is there */
#define RA_WRAM_CALLED  2  /* called on the most recent tick */

/*
    Reads as the ASCII bytes "RA2S" in a byte-wise hex dump, which is how the in-game
    menu's RAM viewer displays memory. Lets you confirm at a glance that you are looking
    at the snapshot and not at unrelated memory -- and the digit is the layout version,
    so a stale address from an older build announces itself as "RA1S" or "RA0S" instead
    of being read as garbage. It goes up whenever the layout below changes, which it did
    when the watchlist moved into DSi WRAM.
*/
#define RA_SNAPSHOT_MAGIC0 'R'
#define RA_SNAPSHOT_MAGIC1 'A'
#define RA_SNAPSHOT_MAGIC2 '2'
#define RA_SNAPSHOT_MAGIC3 'S'

/*
    Step 3b: handing an earned achievement id from the game to the SD card.

    The cardengine has no network and the ARM9 has no SD card -- on a DSi the card is the ARM7's, and
    every file nds-bootstrap writes from inside a game goes through fileWrite() there. So an unlock
    detected by rcheevos on the ARM9 has to cross to the ARM7, which appends it to the queue file the
    launcher pre-created, and the *next* boot's launcher sends it. See RA_QUEUE_PATH in ra_wifi.h.

    The channel is the one that already exists rather than a new one. `sharedAddr` is how the ARM9
    cardengine has always driven the ARM7 -- card reads, the reset and exit commands, the in-game
    menu's battery and clock -- and the ARM7 already polls it from its VBlank handler. Two things come
    with that for free, and both would have been work: the region is coherent between the two CPUs
    with nothing but `volatile` (every card read in every game depends on that being true), and there
    is an established idiom of a magic in one slot and data in the next.

    Slots 0-8 are taken and slot 13 is UNPATCHED_FUNCTION_LOCATION, so these are two of the only four
    that exist. See CARDENGINE_SHARED_SLOTS -- the bound is asserted where these are used.

    The protocol, deliberately as small as it can be:

      ARM9  writes the id into ID, then the magic into REQ.  (that order: the ARM7 can wake between
                                                              the two writes, and a magic with a stale
                                                              id would award the wrong achievement)
      ARM7  sees REQ == magic, appends the id, writes 0 to REQ.
      ARM9  will not write another request while REQ is non-zero.

    One in flight at a time, which is not a limitation worth removing: the ARM7 clears it within a
    frame and a set does not fire two achievements in the same frame often enough to matter. The ARM9
    side keeps the rest in its own queue and offers one per frame.
*/
#define RA_SHARED_UNLOCK_REQ   9
#define RA_SHARED_UNLOCK_ID    10
#define RA_SHARED_UNLOCK_MAGIC 0x4C554152u   /* 'RAUL' -- softcore */
/*
    And the same request from a hardcore session. **The mode rides in the magic rather than in a slot
    of its own**, and that is a deliberate trade rather than a squeeze.

    Slots 11 and 12 are free and either would hold a flag. There are only four slots in total, two of
    them already spent here, and spending a third on one bit would leave this feature holding half of
    everything the ARM9 will ever be able to say to the ARM7. The magic is already a word the ARM7
    compares against a constant, so a second constant costs one more compare and no address space.

    It also makes the wrong thing impossible rather than merely unlikely. With a separate flag slot,
    a request written without setting it reads as softcore *by omission* -- a missed store in a path
    added later, a stale value from the previous unlock. Here the mode and the request are the same
    word: there is no way to raise the request without saying which kind it is.

    Softcore stays 'RAUL' so a new cardengine and an old ARM7 half still agree about the common case.
*/
#define RA_SHARED_UNLOCK_HARDCORE 0x48554152u   /* 'RAUH' -- hardcore */

/*
    And **how much of the request the ARM7 should actually carry out**, in a slot of its own.

    This is an instrument, not a feature, and it exists because the last round of reasoning was wrong
    and cost a hardware boot to find out. `queue=0` proved the freeze lives somewhere in the handoff;
    a gate on `readOngoing` closed the one window that argument pointed at, and Ketsui froze again on
    the same boss. Worse, the queue record it wrote came back **complete and correct** -- id, stamp,
    game code, title, hardcore flag -- which says the append ran to the end. So the freeze is not a
    hang inside the write, and every remaining suspect is a *side effect* of work the ARM7 does on the
    frame an achievement fires:

      the RTC read     rtcTransaction() bit-bangs RTC_CR8 with swiDelay(48) per edge, twice: about
                       128 + 32 delays, order 4-5 ms sitting inside the VBlank handler with the I bit
                       set, which is 4-5 ms the FIFO handler is not servicing the ARM9's card reads
      the SD work      6 sector reads and a read-modify-write, on the controller and NDMA channel the
                       game streams through
      the handoff      two words into the shared block and a request the ARM7 answers

    Three candidates, no way to tell them apart by reading, and each guess costs a flash and a boss
    fight. So the level says which of them to do, it rides in a slot the ARM9 writes before the magic
    exactly as it writes the id, and the ladder is walked from the quiet end:

      0  the ARM9 does not offer at all          gated in ra_rc_offer_unlock(), never reaches here
      3  handoff only                            the request is acknowledged and cleared, nothing else
      2  no RTC stamp                            **retired** -- see below; behaves as 1
      1  everything                              the shipping behaviour

    Whichever step the freeze comes back at is the step that causes it, and each one is an `ra.cfg`
    edit rather than a rebuild. Level 2 is also the only one that is a usable fallback rather than
    purely diagnostic: a bare id still submits, it just loses `o=`, so the server dates the unlock by
    the boot that reported it.

    **A slot rather than more magics**, which is the opposite of the call made for hardcore one field
    up -- and for a reason that inverts cleanly. The mode had to be inseparable from the request
    because a missing mode reads as softcore *by omission* and silently mis-files a real unlock. A
    missing level reads as garbage, and garbage is not a mode this can be wrong about quietly: the
    ARM7 tests for 3 and for 2 and does everything otherwise, so anything unrecognised lands on the
    shipping behaviour. The failure direction is toward doing the work, not toward skipping it.
*/
#define RA_SHARED_UNLOCK_LEVEL 11

#define RA_QUEUE_LEVEL_OFF     0   /* never seen by the ARM7 */
#define RA_QUEUE_LEVEL_FULL    1
/*
    Retired, and kept as a number rather than reused.

    It answered the question it was built for -- `queue=2` cleared the stage that `queue=1` froze, and
    the only difference between them was reading the RTC -- and the fix that followed took the RTC out
    of the game's way entirely: raStampCapture() now reads it once, at init. There is nothing left for
    this rung to skip, so the ARM7 no longer tests for it and a `queue=2` behaves as `queue=1`.

    Not renumbered, because the number is in the record: docs/retroachievements.md reports results by
    rung, and moving 3 down to 2 would silently rewrite what those readings meant.
*/
#define RA_QUEUE_LEVEL_NOSTAMP 2
#define RA_QUEUE_LEVEL_HANDOFF 3
#define RA_QUEUE_LEVEL_MAX     3

/*
    And **when** either side may act on that request, which is the whole of the Ketsui freeze.

    The append runs on the ARM7, from its VBlank handler, with IME off. The comment there argued that
    IME off was enough because "a competing read cannot *start* underneath this write". That is true
    and insufficient: a card read does not have to start underneath the write to be ruined by it, it
    can already be *half done*.

    The ROM read path on that CPU is resumable. start_cardRead_arm9() calls fileReadNonBLocking(),
    which issues an sdmmc command, returns false, and leaves `readOngoing` set with the outstanding
    command id in the reader's context; the transfer finishes in the SD controller, and a later poke of
    the FIFO handler calls resumeFileRead(), which asks CARD_CheckCommand() whether that command is
    done. Between those two moments the game's read exists only as hardware state -- one command
    outstanding on the one SD controller, plus 512 bytes of my_fat.c's single `globalBuffer`.

    A VBlank landing in that window ran the append straight through it: new commands on the same
    controller, the same buffer rewritten. Which is exactly the pair of symptoms reported, in both
    directions at once -- the game's read never completed and the ARM9 spun on it forever (the freeze
    at Ketsui's boss kill, twice, with the on-screen popup already switched off), and the record that
    came out held the game's bytes (ra_unlocks.txt containing Ketsui's credits text). Setting queue=0,
    which skips the append and nothing else, removed the freeze. That is the diagnosis: not the
    overlay, not the unlock, the write.

    So the work waits, and it waits on **two sides for two different reasons**:

      ARM7   `readOngoing`, at the top of raUnlockService(). Correctness, and it can only live there:
             the flag is that CPU's own state and nothing else can see it. The request word is left
             standing, so the next frame retries.

      ARM9   raCardReadPending(), below, in ra_rc_offer_unlock(). Courtesy, not correctness -- a
             request the ARM7 has not served yet cannot corrupt anything, it just means the ARM9 is
             sitting in its card-read stub waiting, and the FIFO handler that would serve it cannot run
             while the append holds IME off. So the unlock is not offered at all until the read has
             been answered; it stays in the ring and goes out on a later frame.

    The split is not symmetry for its own sake, it is where the bytes are. Testing one more word on the
    ARM7 cost more than that binary has: cardenginei_arm7 for TWL-SDK games links into 33K with 60
    bytes spare, and adding the check there put .bss four bytes past the end of the region. The ARM9
    side has room, already holds `shared` in a register, and is the side that decides when to offer.
    Measured, not guessed -- see the size note in raUnlockService().

    The predicate lives in a header both sides can see because it is the one part of this that can be
    checked without a card: a pure test over one word, whose truth table tools/ra_reader_test.c pins.
*/
#define RA_CARD_REQ_FIRST 0x025FFB08u   /* ARM9 card read, lowest of the three magics */
#define RA_CARD_REQ_LAST  0x025FFB0Au   /* ...and the highest; 0x0A is the DMA variant */

static inline int raCardReadPending(u32 cardReq) {
	return (cardReq >= RA_CARD_REQ_FIRST && cardReq <= RA_CARD_REQ_LAST) ? 1 : 0;
}

/*
    The snapshot lives in the cardengine's own .bss. That matters for two
    reasons: the game can never touch it, and it is never initialised -- the
    bootloader copies only the loaded image, which ends exactly where .bss
    begins, and an injected binary has no crt0 to zero the rest. So nothing here
    may assume a known initial state; the magic is what makes the buffer
    trustworthy.

    It is also the debug channel for all of this work: there is no console and no
    logging inside an injected cardengine, so the snapshot is read with the in-game
    menu's RAM viewer. That is why the results sit next to the watch definitions and
    why the counters are here at all.

    16-byte aligned so the RAM viewer, which only jumps in 0x10 steps, can land
    exactly on it.
*/
typedef struct raSnapshot {
	u8  magic[4];        /* +0x00  'R','A','2','S' */
	u32 ticks;           /* +0x04  frames captured -- the reader is alive */
	/*
	    How the overlay's negotiation for VRAM is going. The game moves its
	    character bases between scenes, so the overlay borrows a block per
	    notification and hands it straight back when the game wants it -- these say
	    how often that happens rather than leaving it to be inferred from glitches.
	*/
	u32 shows;           /* +0x08  notifications actually displayed */
	u32 denied;          /* +0x0C  wanted to show, nothing was free */
	u32 evicted;         /* +0x10  game reclaimed the block mid-notification */
	/*
	    Of `denied`, the share where no background layer was free rather than no VRAM
	    block. Split because the two have different answers and the obvious hypothesis
	    was otherwise untestable -- see the phase 1 field notes in
	    docs/retroachievements.md.

	    **Retired at 0, like rearmDispstat.** chooseLayer() cannot fail any more: it takes the layer it
	    needs to be seen on, enabled or not, so there is always one. A field that reads 0 for the rest of
	    time is the clearest statement available that a whole category of denial is gone -- and if it ever
	    moves again, something reintroduced a refusal.
	*/
	u32 deniedNoLayer;   /* +0x14 */
	u8  watchCount;      /* +0x18  slots in use */
	u8  resolved;        /* +0x19  of those, how many resolved this tick */
	/*
	    The per-frame cost, in scanlines. The game owns the hardware timers, so
	    VCOUNT is the only clock the reader can read without taking something the
	    game is using; a scanline is coarse (~1600 ARM9 cycles) but the question
	    being answered is "does the watchlist eat into the frame", and for that it is
	    exactly the right unit.
	*/
	u8  linesLast;       /* +0x1A  scanlines the last tick consumed */
	u8  linesMax;        /* +0x1B  worst seen since the buffer was claimed */
	/*
	    Everything from here is written from the far side of the cardengine boundary, by
	    cardenginei_arm9_ra.
	*/
	u32 wramMagic;       /* +0x1C  RA_WRAM_MAGIC, written by the binary itself */
	/*
	    The WRAM binary's own frame counter, kept in *its* .bss and copied here each
	    tick -- not incremented here. That is deliberate: nothing had ever stored state
	    in that window between frames, and if it does not persist this counter sticks at
	    1 while `ticks` climbs. It is the cheapest possible test of the thing everything
	    after this depends on.
	*/
	u32 wramTicks;       /* +0x20 */
	u8  wramState;       /* +0x24  RA_WRAM_*, written by the cardengine */
	/*
	    How many scanlines rc_update_memref_values() cost on its own, worst seen -- and it is the
	    number that says how much of the per-frame cost dividing the set can never touch.

	    The memref pass runs on **every** call whatever the slice, so it is the fixed term in
	    `cost = rcMemrefLines + (evaluation / rcParts)`. With it measured, the arithmetic stops
	    being an inference from two whole-frame readings on two different games: the ceiling on what
	    slicing can achieve is exactly rcMemrefLines, and rcMemrefLines at or above rcRoomMax means
	    no division of the triggers can ever fit.

	    It lives up here in the reserved bytes rather than beside rcLinesMax because the bytes next
	    to rcLinesMax are spent. No existing offset moves.
	*/
	u8  rcMemrefLines;   /* +0x25  scanlines the memref pass cost alone, worst seen */
	/*
	    What ra_icache_claim() found and did, packed into one byte, and the reason it is a report
	    rather than a boolean is that there are four different ways this can come back and three of
	    them mean something different has to be tried.

	        bits 0-2  the winning MPU region for 0x03740000
	        bit 3     0x08  the region also covers main RAM, so it was left alone
	        bit 4     0x10  that region was already instruction-cacheable
	        bit 5     0x20  ...it was not, and this turned it on
	        bit 6     0x40  the instruction cache is enabled globally in CP15 c1
	        bit 7     0x80  the region is data-cacheable too, which is informational only

	    Plus RA_MPU_SPANS_MAIN_RAM: the winning region also covers 0x02000000, so it governs how the
	    *game's* code is fetched and this declines to touch it. See the note on ra_icache_claim().

	    Bit 6 clear makes every other bit here moot -- a per-region cacheability bit does nothing
	    while the cache itself is off -- and it is the first thing to read.
	*/
	u8  raMpuBits;       /* +0x26 */
	/*
	    The *cheapest* memref pass seen, against rcMemrefLines' worst -- and the pair is one
	    discriminator rather than two numbers.

	    A read costs ~845 ARM9 cycles, confirmed twice: 237 reads in 47 scanlines on Chrono Trigger,
	    and Contra 4's 69 reads at the same rate leaving 117 cycles per condition for its 1,946, which
	    is a sane figure. Nothing in the peek path spends 845 cycles -- a translate, two range checks
	    and a load -- and the instruction cache was already on. What is left is the read itself: main
	    RAM, from inside the game's VBlank, where the game's own DMA holds the bus and the CPU gets a
	    slot when it is given one. The trigger loop is fast because it reads memref values out of
	    cached DSi WRAM instead, which is not on that bus.

	    So: a min far below the max means the cost is contention and varies with what the game is
	    transferring, which would make running outside the blanking period the cheaper place to be. A
	    min equal to the max means it is fixed work and the bus is exonerated. One byte answers it,
	    and it answers on any play session rather than needing a run of its own.
	*/
	u8  rcMemrefMin;     /* +0x27  cheapest memref pass seen, 0 until the first one */
	/*
	    The two cells the self-test chains walk. They live here, in main RAM, rather than
	    in the WRAM binary alongside the watchlist -- because a pointer the chain walker
	    follows has to be a main RAM address, and relaxing that check to accommodate our
	    own cells would weaken it for the game addresses it exists to guard.

	    selfCell holds this buffer's address and selfCellPtr holds selfCell's, so a
	    two-step chain lands on `ticks`. Written by the WRAM binary, which is handed the
	    snapshot pointer and so is the only side that knows the address.
	*/
	u32 selfCell;        /* +0x28 */
	u32 selfCellPtr;     /* +0x2C */
	raResult results[RA_RESULT_MAX];  /* +0x30 */
	/*
	    The WRAM binary bringing itself up. Appended after the results rather than
	    inserted, so every offset above keeps its address and the hardware checklist in
	    docs/retroachievements.md stays valid -- which is also why the magic did not need
	    another bump for this.

	    heapSize is the arena between the end of the binary's .bss and the top of its
	    window; heapUsed is what has been handed out. Both stay useful once rcheevos is in
	    there and you want to know how much of 256K it ate.
	*/
	u32 heapSize;        /* +0x60 */
	u32 heapUsed;        /* +0x64 */
	u8  wramStage;       /* +0x68  RA_STAGE_*, how far it got */
	/*
	    How many pieces the trigger list is being evaluated in, and it is the field that says whether
	    dividing the set was enough.

	    1 means the set fits inside whatever blanking the game leaves and nothing was divided -- which
	    is every game measured before Chrono Trigger. Between 2 and RA_RC_PARTS_MAX means the reader
	    found a division that fits, and the only cost is that a trigger is visited every N frames.

	    Pinned at RA_RC_PARTS_MAX **while rcLinesMax still exceeds rcRoomMax** is the reading that
	    matters: rc_update_memref_values() runs on every call whatever the slice, so a share of the
	    cost cannot be divided at all, and that combination says the share alone does not fit. No
	    further division would help and the next thing to attack is the peek path.

	    Costs the byte reserved at this offset since the struct was written, so no offset moves.
	*/
	u8  rcParts;         /* +0x69  slices the trigger list is evaluated in, 1 = undivided */
	/*
	    The deepest excursion rcheevos made on the private stack, in bytes -- and it is the
	    number that explains why a real achievement set crashed a retail game twice.

	    Everything in this binary runs inside the game's VCOUNT interrupt handler, on the game's
	    IRQ stack. Measured on a host, rc_runtime_do_frame() needs **767 bytes** and has run every
	    frame for many sessions without trouble; rc_runtime_activate_achievement() needs
	    **2,383** -- 3.1 times as much. So the parse was overflowing an IRQ stack that the
	    evaluation fits in, trampling whatever the game keeps below it, and both crashes had the
	    ARM9 executing the game's own data with a wild PC.

	    rcheevos runs on a stack of ours now, and this reports the high-water mark of it so the
	    2,383 stops being a host extrapolation. Measured by painting the region and finding the
	    deepest word that changed -- which works here, unlike on the host, because we own the
	    whole region rather than borrowing the end of someone else's.
	*/
	u16 rcStackUsed;     /* +0x6A */
	/*
	    rcheevos. Appended for the same reason the heap fields were -- every offset above
	    keeps its address, so the hardware checklist in docs/retroachievements.md stays
	    valid and the magic does not need another bump.

	    This block answers three separate questions that would otherwise all look like
	    "rcheevos does not work": did it come up (rcStage, rcActivate), is it evaluating
	    (rcMeasured against rcTarget, climbing), and what does it cost (rcLines).
	*/
	u8  rcStage;         /* +0x6C  RA_RC_*, how far rcheevos got */
	s8  rcActivate;      /* +0x6D  rc_runtime_activate_achievement(): 0 ok, RC_* if not */
	u8  rcTriggerState;  /* +0x6E  RC_TRIGGER_STATE_* of the test achievement */
	/*
	    The one-time parse, measured on its own. Activating an achievement mallocs, md5s
	    the definition and parses it, and that happens inside the game's VCOUNT handler --
	    so it is much more expensive than a frame of evaluation. Reported separately
	    because otherwise it lands in linesMax and makes the steady-state cost look
	    twenty times worse than it is.

	    **The slowest single activation**, not the total, and the change is what made this
	    number usable. It used to be one VCOUNT delta across the whole of ra_rc_init(), taken
	    modulo 263 -- which is correct for three definitions and silently wrong for fifty-six:
	    a parse spanning four frames reports whatever the remainder happens to be, and a slow
	    init reads as a fast one. There is no way to count the frames from inside a handler
	    that is not being re-entered, so the measurement moved down a level: each activation
	    is timed on its own, this is the worst of them, and rcInitTotal is the sum.

	    255 means it saturated, and then rcInitTotal is a lower bound.
	*/
	u8  rcInitLines;     /* +0x6F */
	u32 rcTriggered;     /* +0x70  ACHIEVEMENT_TRIGGERED events delivered */
	/*
	    Measured progress of the test achievement, which is what makes this observable
	    rather than boolean: it climbs one per frame toward rcTarget, so a hex viewer shows
	    rcheevos evaluating rather than merely having been initialised.
	*/
	u32 rcMeasured;      /* +0x74 */
	u32 rcTarget;        /* +0x78 */
	/*
	    peek() traffic. rcPeeks is per frame -- it should equal the number of distinct
	    addresses the active definitions read, and a zero here with an active trigger means
	    do_frame is not reaching memory. rcPeeksRejected is cumulative and should stay 0:
	    it counts addresses a definition asked for that this console cannot supply, which
	    is the case that used to mean a Data Abort.
	*/
	u32 rcPeeks;         /* +0x7C */
	u32 rcPeeksRejected; /* +0x80 */
	u8  rcLines;         /* +0x84  scanlines rc_runtime_do_frame() cost, last tick */
	u8  rcLinesMax;      /* +0x85  worst seen, excluding the init frame */
	u8  rcEvents;        /* +0x86  events of any kind delivered, clamped at 255 */
	/*
	    The most blanking the reader has ever found still unspent when its turn came, and it is
	    read *against* rcLinesMax rather than on its own.

	    The reader is chained after the game's own VBlank handler, so what is left is the game's
	    decision, not ours. That makes this the number that says whether the per-frame cost can be
	    scheduled around at all: a rcRoomMax comfortably above rcLinesMax means there are frames
	    with room to spare and declining the others costs only sample rate, while a rcRoomMax at or
	    below it means no frame in the session could hold the work and the cost itself is the only
	    thing left to attack.

	    Zero means every evaluation began outside the blanking period -- the game's own handler had
	    already run past line 262 before ours started -- which is the worst reading available here
	    and not a missing measurement.

	    Costs the byte that has been reserved at this offset since the struct was written, so no
	    existing offset moves and the hardware checklist stays valid.
	*/
	u8  rcRoomMax;       /* +0x87  blanking left when the reader ran, best seen, of 71 */
	/*
	    The allocator, reported separately from the arena. Added after a reading that could
	    not be diagnosed: rcheevos failed to allocate 32 bytes while heapSize said 189K was
	    free, and nothing in the snapshot could say whether the arena bookkeeping was wrong
	    or newlib was refusing for its own reasons.

	    heapBreak and heapTop are _sbrk()'s own two numbers, so the arena is *shown* rather
	    than inferred from a subtraction. sbrkProbe is the discriminator: a non-zero value
	    here next to a zero mallocProbe means _sbrk() will hand out memory that malloc()
	    will not, which puts the fault in newlib rather than in the window.

	    Only written on the failure path -- sbrkProbe moves the break, which is free on a
	    heap that is already dead and not something to do to a working one.
	*/
	u32 heapBreak;       /* +0x88 */
	u32 heapTop;         /* +0x8C */
	u32 mallocProbe;     /* +0x90  what malloc(32) returned; 0 = refused */
	u32 sbrkProbe;       /* +0x94  what _sbrk(64) returned; 0 = refused */
	/*
	    Which definition is being evaluated. Reported because the two possibilities look
	    identical once running, and telling them apart is the whole point of being able to
	    supply one from a file: a definition that does not unlock is a very different problem
	    depending on whether the file was picked up at all.
	*/
	u8  rcFromFile;      /* +0x98  1 if the staged file was used, 0 for the built-in */
	/*
	    How many definitions in the file parsed, and which line was the first not to.
	    Reported together because "two of three worked" is a completely different situation
	    from "none did", and a photograph of a hex viewer cannot show a parser message.
	*/
	u8  rcActivated;     /* +0x99  definitions successfully activated */
	u16 rcDefLength;     /* +0x9A  length of the staged text */
	u8  rcBadLine;       /* +0x9C  1-based line of the first parse failure, 0 if none */
	/*
	    Which definition unlocked first, 1-based, and it exists so that a prediction can be
	    made in advance rather than a set being declared "working" because a counter moved.

	    With fifty-six definitions loaded, `rcTriggered` climbing says something fired and
	    nothing about what. The set for GameID 14856 opens with `1=1.300.` -- always true, three
	    hundred hits -- so line 1 should unlock about five seconds in, and this field is how that
	    gets checked. Anything else here first means a definition is reading memory it should
	    not, which is a real bug and not a success.
	*/
	u8  rcFirstTriggered; /* +0x9D */
	/*
	    Scanlines summed over every activation, clamped at 0xFFFF. 263 per frame, so ~2,900 is
	    eleven frames -- and eleven frames spent inside the game's VCOUNT handler is the hazard
	    this step exists to measure, not a detail. See rcInitLines for why the total could not
	    simply be one delta.
	*/
	u16 rcInitTotal;      /* +0x9E */
	/*
	    Step 5's three fields, appended for the reason every block above them was: **every existing
	    offset keeps its address**, so the hardware checklist in docs/retroachievements.md stays
	    valid and the magic does not need another bump. The struct grows from 0xA0 to 0xA8.

	    The staged definitions carry RetroAchievements' own achievement ids now -- each line is
	    `<id>:<memaddr>`. Without them nothing can be reported to the server: `r=unlocks` answers in
	    ids and `r=awardachievement` is asked in them, and "which achievement fired" had no answer
	    the server would recognise.

	    rcFirstId is the one that proves it end to end. rcFirstTriggered says *which line* unlocked
	    first and has since the offline half; this says which achievement, in the server's own
	    numbering, which is the number a person can look up on the set's page.
	*/
	u32 rcFirstId;        /* +0xA0  RA id of the first achievement to unlock, 0 if none */
	u16 rcDefsWithId;     /* +0xA4  staged lines that carried an id */
	u16 rcDefsNoId;       /* +0xA6  ...and lines that did not, which cannot be reported */
	/*
	    Step 3b. The address of the cardengine's shared block, published by ra_tick() because the
	    binary in DSi WRAM cannot work it out: it is 0x027FFA0C or 0x02FFFA0C depending on the game's
	    SDK version, and only the ARM9 cardengine knows which. Passing it beats guessing -- a wrong
	    guess writes four bytes into a running game's memory.
	*/
	u32 shared;           /* +0xA8  CARDENGINE_SHARED_ADDRESS_* for this game, 0 if unknown */
	/*
	    ...and what crossing it has done, because the RAM viewer is the only debug channel in here.
	    `unlockSent` is what reached the ARM7; `unlockLost` is what the ring dropped, which must never
	    be a number nobody can see.
	*/
	u16 unlockSent;       /* +0xAC  ids handed to the ARM7 */
	u8  unlockQueued;     /* +0xAE  still waiting in the ring */
	u8  unlockLost;       /* +0xAF  fired while the ring was full */
	/*
	    How often the per-frame hook had to be put back, and *which* part of it was gone.

	    Contra 4 froze `ticks` at 19 while Mario 64 ran to 2,863 on the same binary, so something in
	    that game takes the hook away three tenths of a second in. Three things have to hold for it to
	    fire -- the game's IRQ table entry, IRQ_VCOUNT in REG_IE, and the Y-trigger in REG_DISPSTAT --
	    and a blind re-arm would fix it without ever saying which. These count them separately, so the
	    next run names the mechanism instead of leaving it inferred.

	    Zero on a game that never disturbs it, which is every game measured before this one.
	*/
	u8  rearmTable;       /* +0xB0  irqTable[0] no longer pointed at our handler */
	u8  rearmIe;          /* +0xB1  IRQ_VBLANK had been cleared from REG_IE */
	/*
	    The Y-trigger interrupt had been switched off -- and this field is **retired at 0**, which is the
	    whole point of it.

	    It is what named the mechanism. Saturated at 255 on Contra 4 while `ticks` reached only 1,132
	    across a session of many thousands of frames: the game was clearing DISP_YTRIGGER_IRQ constantly,
	    each re-arm from cardRead() bought about one tick, and the reader therefore ran on roughly 8% of
	    frames. The overlay has to re-assert its borrowed layer every frame because the game rewrites
	    those registers every frame, so it was visible about one frame in twelve -- the fast intermittent
	    flash seen on screen.

	    The reader hooks VBlank now, which has no Y-trigger to lose. A field that reads 0 for the rest of
	    time is the clearest statement available that the fragile condition is gone, and it costs one byte
	    that no longer has anything better to do. `ticks` climbing with the frame count is the other half
	    of the same reading.
	*/
	u8  rearmDispstat;    /* +0xB2 */
	/*
	    Whether the sub engine had **BG extended palettes** enabled the last time the notification was
	    raised, read straight out of SUB_DISPCNT bit 30.

	    Because `shows` reaching 1 on Contra 4 with denied, evicted and deniedNoLayer all 0 says the
	    overlay borrowed its block and its layer and drew -- and it still was not visible. The colour is
	    written to standard palette RAM at 0x05000400, and with extended palettes on the hardware
	    ignores that entirely for backgrounds: the glyphs would be drawn in whatever the game's extended
	    palette holds at that index, which is very likely nothing at all.

	    One bit, so the next run either confirms that or kills it. Fits in the byte that was already
	    reserved, so the snapshot does not grow and no offset moves.
	*/
	/*
	    The sub engine's state as of the last show(), packed into one byte because there is no room for
	    more: the ARM9 cardengine's window went to **-56 bytes free** when this was four separate fields,
	    which is a hard stop rather than a tight fit.

	        bit 0     SUB_DISPCNT bit 30 -- BG extended palettes were on
	        bits 1-2  the background layer borrowed
	        bits 3-4  the 16K character block borrowed
	        bit 5     the sub engine was blending the screen toward white or black -- a real fade
	        bit 6     this notification had been held back waiting for a fade to end
	        bit 7     a notification is owed *right now* and has not been raised

	    Bit 5 is there because a real unlock fires when a stage ends, which is precisely when a game fades
	    the screen, and a fade dims our glyphs along with everything else. It would explain a notification
	    that reports drawn, is never denied or evicted, and is still not seen -- while the demo timer's
	    identical call is seen, because it fires on an ordinary frame.

	    **And bit 5 was wrong when that explanation was called confirmed.** It tested only the blend factor
	    in SUB_MASTER_BRIGHT bits 0-4 and ignored the mode in bits 14-15 -- and with the mode clear there is
	    no fade at all, however large the factor. So a set bit 5 proved much less than was claimed from it.
	    It consults the mode now; the register is live at 0x0400106C for anyone who wants the value itself.

	    Bit 7 is the one that was missing, and its absence cost a hardware run. Every other bit here is
	    written by draw(), which only runs when a notification *is* raised -- so the state that most needed
	    reporting was the one state nothing could report. Contra 4 came back with rcTriggered 1 and
	    unlockSent 1, and shows, denied, evicted and deniedNoLayer all zero: show() always increments one of
	    those, so it had never been called at all. The notification was still owed, held by the fade gate,
	    and the snapshot said "nothing happened" -- which is the one thing it was not. Bit 7 is refreshed
	    every tick, so a notification stuck in the queue now says so.

	    Registers and choice belong together and that is why the choice is what is stored: the registers
	    themselves are hardware, readable live from the RAM viewer at 0x04001000 and 0x04001008, and the
	    one thing a photograph cannot recover is which block surveyBlocks() concluded was free. Contra 4
	    reported `shows 1` with nothing denied or evicted and flickered the game's own graphics
	    underneath, so the survey called a block free that the game was using -- and the block number is
	    the missing half of that.
	*/
	u8  overlayState;     /* +0xB3  see the bit layout above */
	/*
	    Where the notification's pixels are: a strip of 4bpp background tiles, RA_TEXT_BYTES of them,
	    rendered by cardenginei_arm9_ra and blitted by the overlay in the ARM9 cardengine.

	    The split is a consequence of the budget rather than a preference. A font for printable ASCII
	    is 760 bytes and the ARM9 cardengine's window has 24 free -- it has already turned down a
	    four-byte debug field and a fifty-six-byte diagnostic mode. So the side with 256K owns the
	    font, the character lookup and the centring, and the side that negotiates with the game for
	    VRAM owns the drawing. The overlay keeps every line of that negotiation, which was expensive
	    to get right, and loses only the part that knew what letters look like.

	    Zero until an unlock has rendered one, which is what lets the drawing side tell "nothing to
	    say" from "something to say". ra_tick() checks it is inside the WRAM binary's window before
	    passing it on: a wrong pointer here would be two kilobytes of arbitrary memory copied into a
	    running game's VRAM.
	*/
	u32 overlayText;      /* +0xB4 */
	/*
	    The sub engine as the *game* had it configured, captured at show() time before the overlay
	    touches anything. Two words, and they exist because every explanation this project had for
	    "borrowed, drawn, held, invisible" is now dead.

	    The reading that killed them: Contra 4, `shows 1`, `denied` `evicted` `deniedNoLayer` all 0,
	    `overlayState` 0x4A -- layer 1, block 1, extended palettes off, **not** inside a fade, deferred
	    and released -- `overlayText` 0x03754288, which is exactly where nm puts raTextStrip in the WRAM
	    binary. `ticks` 12,736 against a three-minute session, so the hook held every frame; `rcActivated`
	    45 with `rcBadLine` 0, so all 45 definitions parsed with their titles attached. The overlay wrote
	    two kilobytes of real glyphs into a block the survey said was free, enabled its layer at priority
	    0, and held it for 180 frames. Nothing appeared.

	    So the question moved to whether a background *can* appear at all, and neither of the two
	    mechanisms that would answer it was being measured:

	      dispcnt  bits 16-17 are the display mode -- 0 is display off, and no background shows in it
	               whatever its registers say. Bits 8-12 say which layers the game itself has enabled,
	               which decides whether one of them sits in front of ours: at equal priority the DS
	               resolves ties by *layer number*, so the game's BG0 covers our BG1. Bits 0-2 are the
	               BG mode and bit 30 is extended palettes, both free in the same word.

	      window   WININ in the low half, WINOUT in the high. If the game has windows enabled -- DISPCNT
	               bits 13-15 -- then a layer is only drawn where these permit it, and a game masking its
	               HUD with a window that excludes BG1 would hide ours everywhere. Nothing in this
	               project has ever looked at them.

	    Whole registers rather than picked bits, because the picked bit is what went wrong last time:
	    `overlayState` bit 5 was reported as "the screen was fading" when all it could prove was that a
	    blend factor was non-zero, and a conclusion was built on it.
	*/
	u32 overlayDispcnt;   /* +0xB8  sub DISPCNT, 0x04001000 */
	u32 overlayWindow;    /* +0xBC  WININ 0x04001048 low, WINOUT 0x0400104A high */
	/*
	    Which path drew the last notification, and what it took.

	    The object path is the one that costs the game nothing: an object at a given priority is drawn
	    above every background at that priority, so it does not have to take a layer at all -- and what it
	    does need, a disabled OAM entry and a range of object VRAM nobody references, it *finds* rather
	    than borrows. The background path stays as the fallback for when neither can be found.

	    0xFF in overlaySpriteOam means the background path ran, and that is why this is a separate field
	    rather than another bit of overlayState: an object has no layer and no character block, so
	    reporting through those bits would make "objects" indistinguishable from "layer 0, block 0".

	    Otherwise it is the first of the eight OAM indices used, and overlaySpriteSlot is the 2K unit of
	    object VRAM claimed. Both are worth having because both are negotiated with the game rather than
	    fixed, so a failure to be seen has to be able to say which slot and which entries were in play.
	*/
	u8  overlaySpriteOam;  /* +0xC0  first OAM entry used, or 0xFF for the background path */
	u8  overlaySpriteSlot; /* +0xC1  2K unit of object VRAM claimed, or 0xFF */
	/*
	    Triggers refused the ring because their id was synthetic -- see RA_SYNTHETIC_ID_BASE.

	    A number rather than nothing, because this is the difference between "the self-test fired and
	    was correctly not queued" and "nothing fired at all", and on a game the server does not know
	    those are the two states worth telling apart. It took a card's queue file to tell them apart
	    last time.
	*/
	u8  unlockSynthetic;   /* +0xC2  triggers dropped for having a synthetic id */
	u8  pad0;              /* +0xC3  to word-align what follows */
	/*
	    What ra_definition() actually found where the bootloader was supposed to leave the staged
	    definitions, published raw instead of collapsed into rcFromFile's single bit.

	    This exists because six builds were spent inferring the answer from code. The launcher's log
	    says it staged 45 definitions and reached stage 15 of 15; the game says rcFromFile is 0 and
	    runs its built-in self-test. Both readings are certain and they cannot both be describing a
	    working copy, so the block is lost between them -- and nothing in the snapshot could say
	    *how*, which is why the search went to inspection instead of measurement.

	    Read them together:

	      defsMagic 0x31414452 ('RDA1') with a bad defsLength  -- copied, header wrong
	      defsMagic 0                                          -- the bootloader took its else branch:
	                                                              it looked, found no magic staged,
	                                                              and cleared the destination
	      defsMagic anything else                              -- nothing was copied at all and this
	                                                              is whatever the window held

	    The third case is the one no amount of reading the copy would have found, because the copy is
	    correct: it would mean the branch containing it never ran.
	*/
	u32 defsMagic;         /* +0xC4  first word at CARDENGINEI_ARM9_RA_DEFS_LOCATION */
	u32 defsLength;        /* +0xC8  second word, the length the launcher wrote */
	/*
	    How many frames the overlay found its own pixels overwritten and put them back.

	    Appended, so every existing offset keeps its address and the hardware checklist stays valid.

	    It exists because "re-assert the pixels every frame" was a *deduction* -- the file's own
	    argument about OAM, applied to the two things it had not been applied to -- and a deduction
	    that costs work inside a game's VBlank has to be able to say whether it was needed. Zero on a
	    game that leaves object VRAM alone; climbing on the game whose notification looked wrong.
	*/
	u16 overlayRedrawn;
	/*
	    **What the game had in the two palette entries this borrows, and how the borrowed layer was
	    configured.** Three fields, eight bytes, and every one of them exists because a photograph of
	    Ketsui's results screen could not settle an argument that a register would settle in one glance.

	    The overlay writes white and black into bank 15's ink and shadow entries and puts them back
	    when the notification goes. Whether that is free or destructive depends entirely on what was
	    there, and on whether the layer is 4bpp at all:

	      overlaySavedInk / overlaySavedShadow
	          the game's own colours at those two indices, in BGR555, captured before they were
	          overwritten. Two blacks mean the entries were spare and the theft costs nothing. Two
	          real colours mean the game is using bank 15 and the screen changes colour for three
	          seconds, which is exactly the symptom reported.

	      overlayBgCnt
	          SUB_BGCNT of the layer that was borrowed, whole, because every field in it matters and
	          none of them can be recovered from a photograph: character base (bits 2-5), screen base
	          (bits 8-12), priority (bits 0-1), size (bits 14-15) -- and **bit 7, 256-colour**.

	    Bit 7 is the one worth naming. At 8bpp there are no sixteen-colour banks: the map's palette
	    field is ignored and a pixel value indexes all 256 entries directly. So `OVERLAY_PAL_BANK * 16
	    + ink` stops being "a spare corner of bank 15" and becomes plain entry 241 of the game's single
	    palette -- as likely to be in use as any other. The overlay's whole palette argument assumes
	    4bpp and has never checked.
	*/
	u16 overlaySavedInk;     /* +0xCE */
	u16 overlaySavedShadow;  /* +0xD0 */
	u16 overlayBgCnt;        /* +0xD2 */
} raSnapshot;            /*              0xD4 bytes */

/*
    A note on what that pair settled, because it is the answer to four hardware runs and it lives here
    rather than in the overlay: **the notification is only ever visible on BG0.**

    The DS orders backgrounds by BGCNT bits 0-1, lower value in front, and **breaks ties by layer number**.
    The overlay takes priority 0, the strongest there is, so it beats any enabled layer at priority 1 or
    worse and loses to any *lower-numbered* enabled layer at priority 0 -- with nothing below 0 to reach
    for.

    Measured: during stage 1 gameplay on Contra 4 the pulse probe read `overlayState` 0x08 -- layer **0** --
    and was plainly visible, with `overlayDispcnt` 0x00211E10 showing the game's BG0 *off* and BG1, BG2, BG3
    and OBJ on. Both real unlocks read 0x4A -- layer **1** -- and were not seen at all.

    The same reading closed the other two doors it was taken to test: bits 13-15 clear, so no window is
    active and `overlayWindow` is 0 for a reason rather than by accident; bits 16-17 = 1, so the display
    mode is graphics; bit 7 clear, so no forced blank.

    The priorities themselves are not published, and do not need to be: chooseLayer() only refuses at layer
    N when layer N is enabled *and* at priority 0, so a denial together with that layer's enable bit in
    overlayDispcnt says the priority was 0 by construction.
*/

/*
    The shape of that strip, and it is here rather than with the code that fills it because **both
    binaries compile from it**. cardenginei_arm9_ra renders RA_TEXT_BYTES and the ARM9 cardengine
    copies RA_TEXT_BYTES; two constants that had to agree would eventually not.

    32 by 2 tiles: the full width of the screen, and two rows -- a heading that says a
    RetroAchievements unlock happened, and the achievement's own name underneath.

    Fixed rather than sized to the message. A length carried across the boundary is a length the
    receiving side has to trust, and a wrong one is two kilobytes written into a running game's VRAM.
*/
#define RA_TEXT_COLS  32
#define RA_TEXT_ROWS  2
#define RA_TEXT_TILES (RA_TEXT_COLS * RA_TEXT_ROWS)
/* 4bpp: eight words of eight nibbles per tile. */
#define RA_TEXT_WORDS (RA_TEXT_TILES * 8)
#define RA_TEXT_BYTES (RA_TEXT_WORDS * 4)

/*
    Columns left clear at the right-hand end, and the message is right-aligned against them.

    One column, and it earns its keep twice. The strip is exactly the width of the screen, so text
    flush against column 31 is text touching the bezel -- and the drop shadow below hangs a pixel to
    the right of every glyph, which at column 31 would fall off the strip and be clipped away on the
    one character where its absence is most visible.
*/
#define RA_TEXT_MARGIN 1

/*
    The two colour indices the glyphs use, and they are here for the same reason the geometry is:
    ra_text.c writes these nibbles and ra_overlay.c is what makes them a colour. They were a literal
    1 on one side and an OVERLAY_PAL_INDEX on the other, which agreed only because nobody had had a
    reason to change either. This project has paid for that shape of coupling more than once.

    Index 0 stays the tile's transparency, so the game shows through everywhere the text does not.
*/
#define RA_TEXT_INK    1
#define RA_TEXT_SHADOW 2

/*
    How far rcheevos got, reported for the same reason RA_STAGE_* is: each step can fail
    for its own reason, and without this they all present as an achievement that does not
    unlock.
*/
#define RA_RC_NONE        0  /* not reached -- the window did not come up */
#define RA_RC_NO_MEMORY   1  /* malloc() refused; see heapBreak/heapTop/sbrkProbe */
#define RA_RC_NO_MEMREFS  2  /* malloc() worked but rc_runtime_init()'s allocation did not */
#define RA_RC_NO_ADDRESS  3  /* the snapshot has no console address; self-test only */
#define RA_RC_PARSE_BAD   4  /* the definition was rejected; see rcActivate */
/*
    Still activating, one definition per frame.

    Exists because fifty-six definitions cannot be parsed inside a single interrupt. Each one
    costs the same ~2.4 KB of stack -- measured, and flat, so it is not depth that scales -- and
    its own slice of time, and doing all of them in one VCOUNT handler means holding the game's
    interrupt for however long that adds up to. That total is unmeasured and is the leading
    suspect for the Data Abort the first fifty-six-definition run produced.

    Splitting it also makes a failure *name itself*: rcActivated is published before each
    activation is attempted, so a crash localises to one line of the set rather than to the set.

    **Inserted at 5, which moved RA_RC_ACTIVE to 6 and RA_RC_FRAME to 7.** Every guard in the
    cardengine is written as `rcStage < RA_RC_ACTIVE` and so needs no change, but any reading
    photographed before this build reads one lower -- the "rcStage 06 = RA_RC_FRAME" in the
    hardware checklist is now 07. Renumbered rather than parked above RA_RC_ACTIVE because a
    loading state that sorts *after* active would invert every one of those guards, and a wrong
    guard is worse than a stale number in a document I control.
*/
/*
    The id given to a definition that arrived without one -- a hand-written file, or a set from a
    server that omitted the field.

    Far out of the range real RA ids occupy, and that is the whole point rather than tidiness:
    rcheevos identifies achievements *by id* and will treat two definitions sharing one as the same
    achievement, reusing the first's trigger for the second. Numbering the id-less ones from 1 (as
    every build before this did) was safe only while nothing carried a real id; the moment both
    appear in one file, a real id of 3 would collide with the third id-less line. Real ids are six
    or seven digits, so 0xF0000000 cannot be reached by one.
*/
#define RA_SYNTHETIC_ID_BASE 0xF0000000u

#define RA_RC_LOADING     5  /* activating, one per frame; rcActivated says how far */
#define RA_RC_ACTIVE      6  /* every definition activated and validated */
/*
    ra_icache_claim()'s report. See raSnapshot.raMpuBits.
*/
#define RA_MPU_REGION_MASK    0x07
#define RA_MPU_ICACHE_WAS_ON  0x10
#define RA_MPU_ICACHE_SET     0x20
#define RA_MPU_ICACHE_GLOBAL  0x40
#define RA_MPU_DCACHE_ON      0x80
#define RA_MPU_SPANS_MAIN_RAM 0x08
/*
    A sentinel rather than a bit, because "no region covers our window" shares nothing with the
    other readings and needs no room beside them. Unambiguous: a real report never sets both
    RA_MPU_ICACHE_WAS_ON and RA_MPU_ICACHE_SET, so 0xFF cannot arise from one.
*/
#define RA_MPU_NO_REGION      0xFF

#define RA_RC_FRAME       7  /* rc_runtime_do_frame() has run */

#endif /* RA_H */
