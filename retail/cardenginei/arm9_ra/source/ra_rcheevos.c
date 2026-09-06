/*
    rcheevos, running inside a DS game.

    This is the file where the project stops being a memory reader and starts being a
    RetroAchievements client: rcheevos is the same library the official emulator
    integrations use, and its runtime is what turns a definition string from the server
    into "this achievement just unlocked". Everything before it -- the WRAM window, the
    hand-written crt0, the heap -- existed to make this file possible.

    Three things about the DS shape the code here.

    The first is that achievement definitions come from the network. An address in a
    definition is a number someone else wrote, and on this platform dereferencing an
    address the console does not have is a Data Abort inside the game's interrupt handler.
    So peek() routes every read through the same ra_readable() the watchlist uses, and
    ra_rc_validate_address() is handed to rcheevos up front so an achievement that asks
    for memory this console cannot supply is disabled rather than evaluated against zeros.

    The second is that RetroAchievements addresses are console addresses, not DS
    addresses: the server's map puts DS system RAM at 0 and the emulator's frontend is
    expected to translate. We translate with our own three-line map rather than calling
    rcheevos' rc_console_memory_regions(), because that function's switch references the
    table for every console it supports -- forty-odd tables of regions and names -- and
    calling it would drag all of them into a 64K image to answer a question about one
    console.

    The third is that the ARM9 does not fault on an unaligned 32-bit load, it silently
    returns rotated data. Achievement authors write unaligned reads routinely and the
    server will happily serve them, so peek() assembles those from bytes. Refusing them
    would break real definitions and trusting the hardware would return plausible
    nonsense, which is worse.

    This file is part of nds-bootstrap and is licensed under the GPL-3.0,
    the same terms as the rest of the project.
*/

#include <stdlib.h>

#include "ra.h"
#include "locations.h"
#include "ra_wifi.h"   /* raPendingBlock -- the menu's Sync Pending tally */
#include "ra_text.h"

#include "rc_runtime.h"
#include "rc_runtime_types.h"

/*
    A private header of the library, included for one reason: sizeof(rc_memrefs_t), which
    is the allocation rc_runtime_init() makes without checking the result. Probing for the
    real size rather than a guessed one means the check cannot drift out of date when
    upstream changes the structure -- and this is an in-tree build of rcheevos, so its
    private headers are as available as its public ones.
*/
#include "rcheevos/rc_internal.h"

/*
    retail/cardenginei/arm9_ra/source/cardengine.c -- the watchlist's own validation and
    read, reused here on purpose. rcheevos inheriting exactly the checks the walker uses
    is the point: there is one answer in this binary to "may this address be read", and
    a definition from the server does not get a weaker one than a hand-written watch.
*/
extern bool ra_readable(u32 addr, u32 len);
extern u32  ra_read(u32 addr, u8 size);
extern int  ra_watch_add(u32 base, u8 size, u8 depth, const u32* offsets);
extern int  ra_watch_add_flags(u32 base, u8 size, u8 depth, const u32* offsets, u8 flags);
extern void ra_watch_clear(void);

/* VCOUNT, read directly -- the game owns every hardware timer. See raSnapshot.linesLast. */
#define RA_VCOUNT            (*(const vu16*)0x04000006)
#define RA_SCANLINES_PER_FRAME 263
/*
    The first scanline of the vertical blanking period, and the ceiling on how long the reader may
    sit in the game's VBlank interrupt.

    **This is the constraint the tearing came from.** The steady-state cost was measured at 28-31
    scanlines for 45 definitions, against 71 scanlines of blanking, and the note that recorded it
    said what made that affordable: the work fits inside the blanking period and touches no visible
    line. A larger set does not fit. Chrono Trigger spills past line 262 into drawn pixels, which is
    tearing on a good frame and a dead ARM9 on a frame where the game had no slack -- entering Leene
    Square, every time.

    RA_RC_FRAME_SKIP_MAX bounds the throttle rather than the cost, so a pathological reading cannot
    stall detection for seconds. Three means the set is evaluated at worst every fourth frame, about
    15 Hz. The old VCOUNT hook ran on 8% of frames -- nearer 5 Hz -- and achievements still fired, so
    this floor is well inside what this project has already shipped.
*/
/*
    RA_RC_FRAME_STARVE_MAX bounds the *refusals* the same way RA_RC_FRAME_SKIP_MAX bounds the
    payback: after this many frames declined for want of room, the evaluation runs anyway. Sixteen
    is about 3.7 Hz at worst, and it is chosen against this project's own evidence rather than a
    feeling -- the old VCOUNT hook ran on roughly 8% of frames, one in twelve, and achievements
    still fired correctly.
*/
/*
    ...and RA_RC_PARTS_MAX bounds the *division of the set*, which is the fix hardware asked for after
    the two above turned out to be scheduling a job that does not fit any schedule.

    Measured on Chrono Trigger: `rcLinesMax` 101, `rcRoomMax` 70. The second number is the most
    blanking the reader ever found unspent, and it is one line short of the 71 that exist -- so the
    game leaves essentially all of it and the work still needs 40% more than the hardware has. No
    gate and no throttle can place 101 lines inside 71. The set has to be evaluated in pieces.

    Eight is the floor on sample rate rather than a guess: a trigger visited every eighth frame is
    7.5 Hz, and this project measured the old VCOUNT hook running on 8% of frames -- one in twelve --
    without missing an unlock.
*/
#define RA_VBLANK_FIRST_LINE   192
#define RA_RC_FRAME_SKIP_MAX   3
#define RA_RC_FRAME_STARVE_MAX 16
#define RA_RC_PARTS_MAX        8

/*
    The RetroAchievements memory map for the Nintendo DS, which is the part of
    rcheevos' consoleinfo.c we actually need:

      console 0x0000000-0x03FFFFF   ->  0x02000000   system RAM (4M)
      console 0x0400000-0x0FFFFFF   ->  unused, padding to keep the DSi map aligned
      console 0x1000000-0x1003FFF   ->  data TCM

    Data TCM is deliberately not translated. Its base is not fixed -- it is whatever the
    game programmed into CP15 c9,c1 -- so there is no constant to map it to, and a guess
    would read real memory belonging to something else and produce values that look
    plausible. An achievement that reads DTCM is reported as unsupported instead, which
    shows up as rcPeeksRejected and as the achievement being disabled rather than as a
    wrong unlock.
*/
#define RA_DS_SYSTEM_RAM_BASE 0x02000000
#define RA_DS_SYSTEM_RAM_SIZE 0x00400000

/*
    Console address to DS address, or 0 for "this console does not have that". Zero is
    usable as the failure value because console address 0 maps to 0x02000000, so a real
    translation is never 0.
*/
static u32 ra_rc_translate(u32 consoleAddress, u32 len) {
	if (len <= RA_DS_SYSTEM_RAM_SIZE && consoleAddress <= RA_DS_SYSTEM_RAM_SIZE - len) {
		return RA_DS_SYSTEM_RAM_BASE + consoleAddress;
	}
	return 0;
}

/*
    ------------------------------------------------------------------------------------
    A stack of our own for rcheevos, which is the fix for two Data Aborts.

    Everything in this binary runs inside the game's VCOUNT interrupt handler -- see
    myIrqHandlerVcount() in cardenginei_arm9 -- so it runs on the game's IRQ stack, whose size is
    the game's business and not something we get to know. Measured on a host with
    -finstrument-functions:

        rc_runtime_do_frame()                 767 bytes
        rc_runtime_activate_achievement()   2,383 bytes

    The first has run every frame for many sessions without trouble, so the IRQ stack
    accommodates it. The second wants 3.1 times as much, and it is what the first real
    achievement set introduced: fifty-six parses instead of three. Both hardware crashes had a
    wild PC inside the *game's* memory with a null data address, which is what trampling the
    memory below an IRQ stack looks like from the outside.

    Why the three-definition builds worked is worth being honest about: they overflowed too. They
    did it three times, during boot, over memory the game had not started using. That is luck,
    and this document has a section about the last time luck was mistaken for a result.

    8 KB against a measured 2,383, and it costs nothing that matters -- the arena has 130 KB of
    margin with the set this large. The high-water mark is reported, so the next reading replaces
    the host's number with the console's.
    ------------------------------------------------------------------------------------
*/
#define RA_RC_STACK_BYTES   8192
#define RA_RC_STACK_WORDS   (RA_RC_STACK_BYTES / 4)
#define RA_RC_STACK_PATTERN 0x5A5A5A5AuL

/* 8-byte aligned because AAPCS wants sp 8-byte aligned at a public interface. */
static u32 raRcStack[RA_RC_STACK_WORDS] __attribute__((aligned(8)));
static u8  raRcStackReady;

typedef u8 (*raRcStep)(raSnapshot*);

/*
    Call fn(snapshot) with sp pointing at our stack, then put sp back.

    r4 and r5 hold the old sp and the target across the switch and are in the clobber list, which
    is what keeps the compiler from placing an input in either -- and that matters: an earlier
    shape of this took the function pointer in r0 and then loaded the argument into r0 before
    branching. `blx` because this is ARMv5TE and the callee may be either instruction set.
*/
#ifdef __arm__
static u8 ra_rc_on_stack(raRcStep fn, raSnapshot* snapshot) {
	u32 result;

	__asm__ volatile (
		"mov  r4, sp        \n"
		"mov  r5, %[fn]     \n"
		"mov  r0, %[arg]    \n"
		"mov  sp, %[top]    \n"
		"blx  r5            \n"
		"mov  sp, r4        \n"
		"mov  %[res], r0    \n"
		: [res] "=r" (result)
		: [fn] "r" (fn), [arg] "r" (snapshot),
		  [top] "r" ((char*)raRcStack + RA_RC_STACK_BYTES)
		: "r0", "r1", "r2", "r3", "r4", "r5", "r12", "lr", "cc", "memory"
	);
	return (u8)result;
}
#else
/*
    The host build calls straight through. tools/ra_reader_test.c therefore does *not* exercise
    the switch, which is worth stating rather than leaving implied -- what it does exercise is
    that everything reached through it still works when the stack is someone else's.
*/
static u8 ra_rc_on_stack(raRcStep fn, raSnapshot* snapshot) {
	return fn(snapshot);
}
#endif

/*
    Run one step on our stack and record how deep it went.

    The region is painted once and never repainted, so the mark is the high-water mark across the
    whole session rather than the last call's. Scanned from the low end: the first word that is
    still the pattern bounds everything that has ever been used above it.
*/
static u8 ra_rc_step(raRcStep fn, raSnapshot* snapshot) {
	u8  stage;
	u32 i;

	if (!raRcStackReady) {
		for (i = 0; i < RA_RC_STACK_WORDS; i++) {
			raRcStack[i] = RA_RC_STACK_PATTERN;
		}
		raRcStackReady = 1;
	}

	stage = ra_rc_on_stack(fn, snapshot);

	for (i = 0; i < RA_RC_STACK_WORDS; i++) {
		if (raRcStack[i] != RA_RC_STACK_PATTERN) {
			break;
		}
	}
	snapshot->rcStackUsed = (u16)((RA_RC_STACK_WORDS - i) * 4);
	/*
	    Saturating rather than wrapping would be wrong here: 8192 fits a u16 exactly, and a mark
	    *at* 8192 means the paint was consumed to the last word, which is the one reading that
	    would mean the stack is too small. It is reported as 8192 and read as "suspect".
	*/
	return stage;
}

static rc_runtime_t runtime;
static u32 peeksThisFrame;
static u32 peeksRejected;
static u32 triggeredCount;
static u32 eventCount;
/*
    Which definition unlocked first, and how expensive the one-time parse was. Statics rather
    than locals because the event handler has no user-data pointer and because the activation
    functions report through the snapshot they are handed rather than owning one.
*/
static u8  firstTriggered;
static u32 firstId;
static u8  initMaxLines;
static u32 initTotalLines;
static u8  rcStage;
static u8  linesMax;
/*
    Frames left to sit out before the next evaluation, and the deepest throttle reached.

    Zero-initialised for free: ra_startup() zeroes this binary's .bss on its first call, which is
    the same guarantee stateMagic depends on in cardengine.c.
*/
static u8  frameSkip;
static u8  frameSkipMax;
/*
    What the last evaluation actually cost, the best blanking ever left to us, and how many frames
    in a row have been declined because the two did not fit together.

    linesLastRun is deliberately not snapshot->rcLines: that field is zero on a frame the reader sat
    out, which is the right thing to report and the wrong thing to predict from.
*/
static u8  linesLastRun;
static u8  roomMax;
static u8  starve;
/*
    How many pieces the trigger list is evaluated in, and which piece is next. rcParts is 1 until a
    measurement says otherwise -- an untuned reader behaves exactly as it did before this existed,
    which is what makes a game that never needed the division pay nothing for it.
*/
static u8  rcParts = 1;
static u8  rcSlice;
/*
    What the memref pass costs on its own, worst seen. The fixed term of the per-frame cost, and
    therefore the ceiling on what dividing the trigger list can ever recover.
*/
static u8  memrefLinesMax;
/*
    ...and the cheapest. Zero means "none seen yet" rather than being initialised to 0xFF, because
    this binary has no crt0: ra_startup() zeroes .bss and nothing copies a .data initialiser here.
    A genuine cost of zero reading as unset is harmless -- 237 reads do not come free.
*/
static u8  memrefLinesMin;

/*
    How many frames to sit out after an evaluation that cost `lines` scanlines with `room` scanlines
    of blanking available when it started.

    Pure, and its own function, for the reason ra_queue.c is pure: the decision is the part with the
    logic and the part a host can check, while the thing it depends on -- VCOUNT advancing -- is the
    one thing a host does not have. tools/ra_reader_test.c drives this directly.

    **A cost of zero never throttles, whatever the room says.** Work that consumed no measurable
    scanline cannot have overrun anything, and treating a zero as an overrun would be throttling on a
    measurement rather than on a cost. That is also what lets the suite exercise the frame path at
    all: on a host RA_VCOUNT is a mapped register that never advances, so every frame measures free.

    `room` is zero on hardware only if the evaluation began outside the blanking period, meaning the
    game's own handler had already run past it. Real work there is over budget by definition and
    there is no denominator to scale by, so it throttles to the floor.
*/
static u8 ra_rc_frame_skip(u16 lines, u16 room) {
	if (lines == 0) {
		return 0;
	}
	if (room == 0) {
		return RA_RC_FRAME_SKIP_MAX;
	}
	if (lines > room) {
		const u16 want = (u16)(lines / room);

		return (want > RA_RC_FRAME_SKIP_MAX) ? RA_RC_FRAME_SKIP_MAX : (u8)want;
	}
	return 0;
}

/*
    Whether to start an evaluation at all this frame, given what the last one cost, how much blanking
    is left right now, and how long it has been since one was allowed to run.

    This is the half ra_rc_frame_skip() cannot do, and hardware said so. Skipping frames lowers the
    *average* cost and leaves the *peak* exactly where it was: the frame that does run still spills
    past line 262 by however much it always did. That is enough to cure a game the accumulated cost
    was killing -- Leene Square stopped freezing -- and it cannot cure an artefact caused by one
    overrunning frame. A tear or a wobble every fourth frame is what a bounded reactive throttle
    looks like on screen, which is what Chrono Trigger's world map kept showing.

    So the decision moves in front of the work: run when the room measured *this* frame can hold what
    the last one cost, and otherwise do not start. The predictor is the last cost rather than the
    worst-ever cost on purpose -- linesMax is raised for good by a single expensive frame, such as the
    one where a trigger fires and events are delivered, and predicting from a high-water mark would
    starve every ordinary frame after it.

    Two escapes, both required:

      - **A cost of zero always runs.** Nothing has been measured yet, or the work is free; either
        way there is nothing to schedule around. This is also what keeps the host suite honest, since
        on a host RA_VCOUNT never advances and every frame measures both free and roomless.
      - **Starvation always runs.** A game whose own VBlank handler leaves nothing behind would
        otherwise stop the reader for the session, and a reader that never evaluates is a worse bug
        than a visible one.
*/
static u8 ra_rc_frame_fits(u8 cost, u8 room, u8 declined) {
	if (cost == 0) {
		return 1;
	}
	if (declined >= RA_RC_FRAME_STARVE_MAX) {
		return 1;
	}
	return (room >= cost) ? 1 : 0;
}

/*
    How many pieces the trigger list should be evaluated in, given how many it is being evaluated in
    now and what that last piece cost against the room it had.

    **It only ever rises.** Not for want of ambition: a step down would have to predict the cost of a
    larger piece, and the cost is not proportional to the piece -- rc_update_memref_values() runs on
    every call whatever the slice, so a fixed share of every measurement belongs to work that dividing
    cannot reduce. Guessing that share wrong in the optimistic direction produces exactly the
    oscillation this is here to end. Rising only is monotone, converges in at most seven frames, and
    what a spurious step costs is sample rate rather than correctness.

    It also *reports* the thing no separate measurement had to be taken for. If rcParts settles below
    the ceiling, the division worked and the fixed share is small. If it pins at RA_RC_PARTS_MAX and
    the reader is still over the room, then the memref pass alone does not fit and no division of the
    triggers ever will -- which is a different problem, and this is how it announces itself.

    A cost of zero or a room of zero says nothing either way and changes nothing, for the reason it
    does in the two functions above: on a host neither number ever moves.
*/
static u8 ra_rc_frame_parts(u8 parts, u8 cost, u8 room) {
	if (parts == 0) {
		parts = 1;
	}
	if (cost == 0 || room == 0) {
		return parts;
	}
	if (cost > room && parts < RA_RC_PARTS_MAX) {
		return (u8)(parts + 1);
	}
	return parts;
}

/*
    128, raised from 8 when `r=patch` arrived.

    The 8 was right for what it was for: the definitions file was a hand-typed line or three,
    and a limit that small made the split obviously bounded. A real achievement set is a
    hundred definitions or more, so the number had to follow the source of the definitions
    changing from a person to a server.

    What it costs is 4 bytes of pointer each, and they are `static` below rather than on the
    stack for that reason -- `ra_rc_init()` is reached from the cardengine's own context, whose
    stack is not this binary's to spend 512 bytes of. It runs once, so static is not a
    compromise.

    The other half of the limit is the block itself: 32,760 bytes of text at
    CARDENGINEI_ARM9_RA_DEFS_MAX. 128 definitions therefore average 255 bytes each before the
    block runs out first, which is the constraint worth knowing about -- RA memaddr strings run
    from tens to a few hundred characters. tools/ra_reader_test.c pins the two numbers against
    each other so raising one without the other fails on the host.
*/
#define RA_DEFS_MAX_LINES 128

/*
    The split definitions, and how far activation has got through them.

    File statics rather than locals because activation is spread over frames now: ra_rc_prepare()
    fills these in once and ra_rc_activate_next() is called on later ticks. The pointers are into
    the staging block, which is not going anywhere -- ra_split_definitions() writes its
    terminators in place and never copies.
*/
static char* defLines[RA_DEFS_MAX_LINES];
/*
    Each line's RetroAchievements id, or RA_SYNTHETIC_ID_BASE + index when the line carried none.
    Parallel to defLines rather than packed with it because the pointers are into the staging block
    and the ids are not in it any more -- ra_take_id() consumes them on the way past.
*/
static u32   defIds[RA_DEFS_MAX_LINES];
/*
    Each line's achievement title, or NULL when it carried none. Pointers into the staging block
    exactly as defLines are -- the launcher writes `<id>:<memaddr>\t<title>` and ra_split_definitions()
    turns the tab into the memaddr's terminator, so the title is already a C string sitting just past
    it. Nothing is copied.
*/
static const char* defTitles[RA_DEFS_MAX_LINES];
/*
    The rendered notification, and why it is a pointer rather than a flag: the strip lives in this
    binary's .bss and the overlay lives in the ARM9 cardengine, which has no room for a font. The
    address crosses in the snapshot. NULL until an unlock has actually rendered something, so the
    other side can tell "nothing to say" from "something to say".
*/
static const void* textStrip;

/*
    The first line of the notification, above the achievement's own name.

    Kept, rather than letting the title stand alone, because the two lines answer different questions:
    the heading says a RetroAchievements unlock just happened and the title says which. A bare game
    phrase appearing over a game would read as part of the game.
*/
#define RA_TEXT_HEADING "ACHIEVEMENT"

/*
    Where the rendered notification is, for the overlay -- which lives beside this file now rather than
    across the cardengine boundary, so this is a pointer handed to a neighbour instead of an address
    published through the snapshot and range-checked on arrival.
*/
const void* ra_rc_text(void) {
	return textStrip;
}
static u8    defCount;
static u8    defIndex;
static u8    activatedCount;
static int   defFirstError;

/*
    The test achievement's id.

    **It is RA_SYNTHETIC_ID_BASE, and the reason is a bug that reached a real account.**

    This used to be 1, under a comment that said "any non-zero number does; it is only how the
    runtime identifies the trigger back to us, and nothing here talks to the server yet". Every
    clause of that was true when it was written. The last one stopped being true, and nothing
    brought the constant along.

    So on every game RetroAchievements does not know -- which is when the self-test runs -- this
    fired, went down the ring to the ARM7, was written to sd:/ra_unlocks.txt, and was submitted.
    Achievement 1 is a real, published achievement on a Mega Drive game, so the server accepted it
    and filed it on the player's account. It had been doing that since the self-test existed.

    The guard in ra_rc_queue_unlock() below was written for exactly this failure and did not catch
    it, because it tests `id >= RA_SYNTHETIC_ID_BASE` and 1 is not. That guard was added after a
    card was found carrying `4026531840` -- 0xF0000000 -- which is what a *staged definition with no
    id* gets. The built-in self-test is the other kind of idless definition and had a constant of its
    own, four hundred lines away, that the fix never touched. One bug, two spellings, and the fix
    spelled it one way.

    Making the two the same value is what closes it rather than another check: there is no longer a
    number here that the guard has to be told about separately. Nothing collides -- an idless staged
    definition takes `RA_SYNTHETIC_ID_BASE + i`, and the self-test only activates when no definition
    was activated at all, so index 0 is never both.
*/
#define RA_TEST_ACHIEVEMENT_ID RA_SYNTHETIC_ID_BASE

/*
    rcheevos asks for memory through this, once per distinct address per frame.

    **num_bytes is not only 1, 2 or 4.** This comment used to say it was, on the reasoning that
    rc_peek_value() decomposes larger widths -- and the code below trusted that with an alignment
    mask of `numBytes - 1`. A definition using `0xW` asks for **three**, the mask becomes 2, and a
    32-bit load happens at an address that is 1 mod 4. The first achievement set this project did
    not write contains a `0xW`, which is how the assumption was found. Widths are handed to
    ra_read() unfiltered now and it assembles anything that is not a native aligned width.

    There is no error channel: peek returns a value, so a read this console cannot serve
    has to return something. Zero is the right something. It makes the condition compare
    against zero and be false, which is "the achievement does not unlock" -- the safe
    direction. The refusal is counted rather than swallowed, so it is visible in the
    snapshot instead of being indistinguishable from a genuine zero in memory.
*/
static uint32_t ra_rc_peek(uint32_t consoleAddress, uint32_t numBytes, void* ud) {
	const u32 address = ra_rc_translate(consoleAddress, numBytes);

	(void)ud;
	peeksThisFrame++;

	if (address == 0 || !ra_readable(address, numBytes)) {
		peeksRejected++;
		return 0;
	}

	/*
	    Every width goes through the watchlist's own read now, including the odd ones, because
	    that is where the byte assembly belongs -- there is one answer in this binary to "read
	    these bytes" rather than two that can drift apart.

	    The test this replaced was `(address & (numBytes - 1)) == 0`, and it is worth recording
	    why it was wrong rather than just deleting it. It assumes numBytes is a power of two.
	    rcheevos asks for **three** when a definition uses `0xW`, and then the mask is 2 -- so an
	    address that is 1 mod 4 passes a test it should fail, and a 32-bit load happens at an odd
	    address. The first set this project did not write contains a `0xW`.
	*/
	return ra_read(address, (u8)numBytes);
}

/*
    Handed to rc_runtime_validate_addresses() once, after activation. Non-zero means the
    address is one this console can supply; rcheevos disables any achievement that
    references one that is not.

    This is the difference between checking a definition and checking every read it makes.
    Both happen -- peek() still validates, because a definition can compute an address at
    runtime through an indirection rcheevos calls AddAddress -- but doing it here as well
    means a definition that could never work says so on the frame it is loaded.
*/
static int ra_rc_validate_address(uint32_t consoleAddress) {
	return ra_rc_translate(consoleAddress, 1) != 0;
}

/*
    Step 3b, the producer side: earned ids waiting to cross to the ARM7.

    Held here rather than handed over immediately because the handover is one-at-a-time -- the ARM7
    clears the request within a frame, and this runs inside the game's IRQ handler where waiting for it
    is not an option. So an unlock goes in this ring and ra_rc_offer_unlock() feeds the channel from the
    frame loop.

    Sized at 8 because a set does not fire eight achievements before the next frame; if it somehow did,
    `unlockLost` counts what fell off rather than letting the ring wrap silently over an unsent id. An
    unlock this fork failed to report is exactly the thing that must not be invisible.
*/
#define RA_UNLOCK_RING 8
static u32 unlockRing[RA_UNLOCK_RING];
static u8  unlockHead;      /* next slot to write */
static u8  unlockTail;      /* next slot to offer */
static u8  unlockQueued;    /* how many are in the ring */
static u8  unlockLost;      /* ring was full when one fired */
static u16 unlockSent;      /* handed to the ARM7 and acknowledged */
static u8  unlockNoChannel; /* the published shared address was not one of the two legal ones */
static u8  unlockSynthetic; /* triggers refused for carrying a synthetic id */

static void ra_rc_queue_unlock(u32 id) {
	if (id == 0) {
		return;
	}
	/*
	    A synthetic id is not an achievement and must never leave this binary.

	    RA_SYNTHETIC_ID_BASE is what a definition gets when it arrived without an id of its own, which
	    in practice means the built-in self-test -- and the self-test is what runs in **every game
	    RetroAchievements does not know**. So on an unsupported game this fired, went down the ring to
	    the ARM7, and was appended to sd:/ra_unlocks.txt as though a player had earned something.

	    What that cost, read off one card and one log:

	      the queue was never empty      the launcher cleared it at boot and the self-test refilled it
	                                     seconds later, every single boot
	      the menu showed a pending row  a fake unlock is indistinguishable from a real one once it is
	                                     in the file, so Sync Pending reported work that did not exist
	      the server was sent nonsense   `4026531840 refused: Unknown achievement` -- a 404 per boot,
	                                     forever, from a client whose User-Agent is still trying to get
	                                     sanctioned

	    The guard goes here rather than in the ARM7 or the launcher because this is the one place that
	    knows the id is synthetic. Downstream it is just a large number, and a large number is exactly
	    what a real id looks like.

	    Counted, not silently dropped, and the notification is untouched: the overlay is driven by
	    triggeredCount, which the event handler bumps before calling this. The self-test still proves
	    on screen that the reader, the runtime and the overlay all work. It just no longer claims a
	    player earned something.
	*/
	if (id >= RA_SYNTHETIC_ID_BASE) {
		if (unlockSynthetic < 255) {
			unlockSynthetic++;
		}
		return;
	}
	if (unlockQueued >= RA_UNLOCK_RING) {
		if (unlockLost < 255) {
			unlockLost++;
		}
		return;
	}
	unlockRing[unlockHead] = id;
	unlockHead = (u8)((unlockHead + 1) % RA_UNLOCK_RING);
	unlockQueued++;

	/*
	    And the achievements page learns about it now rather than next boot. The launcher's copy of
	    the queue is what it read at boot, so an achievement earned since is in neither list -- and
	    "I just got that, is it safe" is the question a player opens this page to ask.
	*/
	{
		raViewerBlock* const v = (raViewerBlock*)CARDENGINEI_ARM9_RA_VIEWER_LOCATION;

		if (v->magic == RA_VIEWER_MAGIC) {
			u16 k;

			for (k = 0; k < v->count && k < RA_VIEWER_MAX_ENTRIES; k++) {
				if (v->entry[k].id == id) {
					/*
					    **EARNED as well as QUEUED**, and the missing half of that test was a real
					    bug: this only checked QUEUED, so an achievement the server had already
					    reported as earned, firing again this session, came out carrying *both*
					    flags and bumping `queued` for something already inside `earned`.

					    That can happen -- a cached set carries its own filtering, as old as the
					    cache, so `sync=0` or a unlock deleted server-side leaves an earned
					    achievement active -- and the menu's header now adds the two counts to
					    print one number. Both flags on one entry would make it count twice and
					    could put it past the size of the set.

					    An entry that is already EARNED needs nothing from this: it is shown as
					    earned, it is in the total, and the unlock still goes down the ring to the
					    queue file exactly as before. Only the display is left alone.
					*/
					if (!(v->entry[k].flags & (RA_VIEWER_EARNED | RA_VIEWER_QUEUED))) {
						v->entry[k].flags |= RA_VIEWER_QUEUED;
						v->queued++;
					}
					break;
				}
			}
		}
	}
}

/*
    Offer one id to the ARM7, if it is not already busy with the last one.

    The write order is the whole protocol: id first, magic second. The ARM7 polls from its VBlank
    handler and can wake between the two stores, and a magic paired with a stale id would append the
    wrong achievement -- which the server would accept, because it is a perfectly valid id.

    Nothing waits here. If the ARM7 has not finished, the ring keeps the id and the next frame tries
    again; there are sixty chances a second and the write is one sector.
*/
static void ra_rc_offer_unlock(raSnapshot* snapshot) {
	vu32* shared;

	if (unlockQueued == 0) {
		return;
	}
	/*
	    `queue=0` in ra.cfg. Stopped here, one step before the handoff, so nothing downstream runs at
	    all: no FIFO request, no file read on the ARM7, no sector write to the card while the game is
	    streaming from it. The ring keeps filling and unlockQueued keeps reporting, so the snapshot
	    still says an achievement fired -- which is the point of a diagnostic.
	*/
	{
		const raSessionBlock* const session =
			(const raSessionBlock*)CARDENGINEI_ARM9_RA_SESSION_LOCATION;

		if (session->magic == RA_SESSION_MAGIC && session->queue == RA_QUEUE_LEVEL_OFF) {
			return;
		}
	}
	/*
	    Validated, not trusted -- and the host suite is what insisted on it. This structure's own header
	    says nothing in it may be assumed initialised: it lives in .bss that no crt0 zeroes, so every
	    field is garbage until claim() says otherwise. A null check would pass on garbage and this
	    function would then *write four bytes to an arbitrary address inside a running game*.

	    On hardware ra_tick() publishes it before calling in, every frame, so in practice it is always
	    set. "Correct because the caller happens to do it first" is the kind of coupling this project
	    checks rather than assumes -- and there are exactly two legal values, so checking is a compare
	    rather than a heuristic. Anything else is refused and counted.
	*/
	if (snapshot->shared != CARDENGINE_SHARED_ADDRESS_SDK1
	 && snapshot->shared != CARDENGINE_SHARED_ADDRESS_SDK5) {
		if (unlockNoChannel < 255) {
			unlockNoChannel++;
		}
		return;
	}
	shared = (vu32*)snapshot->shared;
	if (shared[RA_SHARED_UNLOCK_REQ] != 0) {
		return;   /* the previous one has not been picked up yet */
	}
	/*
	    And not while a card read of our own is still unserved. raCardReadPending() carries the whole
	    reasoning; the short version is that the ARM7 answers this request from its VBlank handler with
	    IME off, so raising it now would make the game wait on a read that the frame after next would
	    have served for free. The unlock stays in the ring, and unlockQueued keeps reporting it.

	    Slot 3 of the same block -- the word this binary's own card-read hook writes. Deferred here
	    rather than on the ARM7 because that binary had four bytes too few for the test; see
	    raUnlockService().
	*/
	if (raCardReadPending(shared[3])) {
		return;
	}
	shared[RA_SHARED_UNLOCK_ID]  = unlockRing[unlockTail];
	/*
	    And how much of the request the ARM7 should carry out -- see RA_SHARED_UNLOCK_LEVEL, which
	    carries the ladder and why it exists. Written before the magic for the same reason the id is:
	    the ARM7 can wake between the two stores, and a request with a stale level beside it would do
	    the wrong amount of work.

	    An absent session block reads as FULL, which is the shipping behaviour and the same default the
	    mode below takes.
	*/
	{
		const raSessionBlock* const session =
			(const raSessionBlock*)CARDENGINEI_ARM9_RA_SESSION_LOCATION;

		shared[RA_SHARED_UNLOCK_LEVEL] = (session->magic == RA_SESSION_MAGIC)
		                                 ? session->queue : RA_QUEUE_LEVEL_FULL;
	}
	/*
	    Which mode this was earned in, decided here because this is the only side that can.

	    The ARM7 writes the queue record but has no way to know: the session block lives in this
	    binary's own window, which the ARM7 does not map. The launcher knows but is long gone. So the
	    answer crosses with the request, and it crosses in the request rather than beside it -- see
	    RA_SHARED_UNLOCK_HARDCORE.

	    Read on every request rather than cached in a static, and that is not laziness. Statics in
	    this binary live in .bss that no crt0 zeroes, so a cached copy would need its own validity
	    magic to be trusted -- more state, and more of it uninitialised, to save a load and a compare
	    on the frame an achievement unlocks. The block is a word in memory this binary already owns.

	    An absent block is softcore, which is the same default the in-game menu takes and safe for the
	    same reason: no launcher staged a session, so no launcher is going to submit one as hardcore.
	*/
	{
		const raSessionBlock* const session =
			(const raSessionBlock*)CARDENGINEI_ARM9_RA_SESSION_LOCATION;
		const int hardcore = (session->magic == RA_SESSION_MAGIC && session->hardcore != 0);

		shared[RA_SHARED_UNLOCK_REQ] = hardcore ? RA_SHARED_UNLOCK_HARDCORE
		                                        : RA_SHARED_UNLOCK_MAGIC;
	}

	unlockTail = (u8)((unlockTail + 1) % RA_UNLOCK_RING);
	unlockQueued--;
	if (unlockSent < 0xFFFF) {
		unlockSent++;
	}
}

static void ra_rc_event_handler(const rc_runtime_event_t* runtimeEvent) {
	eventCount++;
	if (runtimeEvent->type == RC_RUNTIME_EVENT_ACHIEVEMENT_TRIGGERED) {
		triggeredCount++;
		/*
		    Queued before anything else in this branch, because this is the only moment the id exists
		    and everything below is reporting. An unlock that is not queued here is lost for good.
		*/
		ra_rc_queue_unlock(runtimeEvent->id);
		/*
		    The line this id came from, looked up rather than derived. It used to be `id - base + 1`,
		    which only worked because every definition was numbered from RA_TEST_ACHIEVEMENT_ID in
		    order; with the server's own ids there is no arithmetic that recovers a line, and a search
		    over at most 128 entries costs nothing on the frame an achievement unlocks.

		    Done on every trigger now, not only the first, because the title hangs off it -- and one
		    search serves both readers.
		*/
		{
			u8 line = 0xFF;
			u8 i;

			for (i = 0; i < defCount; i++) {
				if (defIds[i] == runtimeEvent->id) {
					line = i;
					break;
				}
			}
			/*
			    The first one only, and recorded as a line number rather than an id so it can be
			    looked up in the file by eye. With one definition loaded a counter was enough; with
			    fifty-six, "something fired" is not a reading -- the set's first definition is
			    `1=1.300.` and should unlock about five seconds in, so this is what turns that into
			    a prediction that can be wrong.
			*/
			if (firstId == 0) {
				firstId = runtimeEvent->id;
				if (line != 0xFF) {
					firstTriggered = (u8)(line + 1);
				}
			}
			/*
			    Rendered here, on the frame the achievement fires, and that is the only moment it can
			    be: this is where the id exists. The overlay may not raise the notification for another
			    ninety frames -- it waits for the screen to stop fading -- so the strip has to be
			    prepared now and left where it can be found.

			    A trigger whose line cannot be found renders the heading alone rather than nothing. It
			    means the id came from somewhere other than the staged set, which should not happen;
			    saying "RA UNLOCKED" with no name is a better answer to that than silence.
			*/
			textStrip = ra_text_render(RA_TEXT_HEADING,
			                           (line != 0xFF) ? defTitles[line] : 0);
		}
	}
}

/*
    The definition to evaluate, in the server's own syntax:

        M:0xH000000>=0.600.

    Read as: measured, the byte at console address 0 is at least zero, six hundred times.
    The comparison is always true, so it counts one hit per frame and unlocks after 600
    frames -- about ten seconds.

    It is anchored at console address 0, and that is the interesting part.

    The obvious anchor would have been the snapshot's own tick counter, and that is what
    this was until hardware said otherwise. It does not work, for a reason worth writing
    down: RetroAchievements maps 4M of DS system RAM, and on this hardware main RAM is 16M.
    The cardengine lives at 0x027FC000 -- eight megabytes in -- so the snapshot has no
    console address at all. It is not a mirror of 0x023FC000 either; that was tested
    directly, by writing a sentinel through one address and reading at the other, and they
    are separate memory.

    Console address 0 is the first word of the game's own RAM. Always mapped, always
    readable, never written by us.

    What this covers: a memref read, a comparison, a hit target, the measured flag, the
    trigger, and rc_runtime_do_frame() reaching memory every frame. What it does not cover
    is the delta memref, which needs a value that changes and therefore a game address
    nobody can name in advance. The first real achievement will exercise it.
*/
#define RA_TEST_DEFINITION "M:0xH000000>=0.600."

/*
    The definition actually evaluated: whatever the launcher staged from
    sd:/_nds/nds-bootstrap/ra_achievements.txt, or the built-in self-test if there is none.

    A file beats a constant here for one reason that matters more than flexibility: testing
    a definition against a running game costs a build, a flash, a play session and a
    photograph, and definitions are exactly the kind of thing that is wrong the first two
    times. Through a file, trying another one is an edit.

    Everything about the string is still distrusted. It is length-checked by the launcher
    before it is staged, terminated here regardless of what the file contained, and handed
    to rcheevos' parser -- which reports a bad definition as an error code rather than
    misbehaving. A definition from a text file gets no more faith than one from the server,
    because eventually it *is* one from the server.
*/

/*
    Split the staged text into lines, in place.

    One definition per line, because a hardware session is the scarce resource in this
    project and testing one definition per session wastes it. Several can be tried at once
    and the snapshot says how many parsed and how many fired.

    Blank lines and lines beginning with '#' are skipped, so the file can carry a note about
    what each definition is meant to do -- which matters when the answer arrives hours later
    as a photograph.
*/
/*
    The viewer's index, filled while the block is still pristine.

    Called once per record by ra_split_definitions(), from inside the loop and **before** that loop
    replaces this record's tabs with NULs. That ordering is the whole reason this function exists
    rather than living in the menu: see CARDENGINEI_ARM9_RA_VIEWER_LOCATION.

    `base` is the definitions text, so the offsets recorded here are what the menu can use against
    its own view of the same bytes -- the block is at one address here and another in the launcher
    that staged it, and only an offset means the same thing in both.

    Fields are found by counting tabs from the start of the record, which works for both shapes
    without knowing which it is: neither a memaddr nor an id can contain one. So tab 1 opens the
    title, tab 2 the points and tab 3 the description, whether the record began with `<id>:<memaddr>`
    or with `#!<id>`.

    **And each tab is replaced by a NUL as it is recorded**, which is what makes every offset here
    the start of a C string. The first version left that to the caller and got it half right: the
    armed path cut the first two tabs and nothing cut the third, so `pointsOff` pointed at
    `5\tClear Stage 2` -- and an earned record, which the split skips entirely, had all three tabs
    intact and gave a title of `Welcome to the Jungle\t3\tClear Stage 1`. The host suite caught both
    by reading the fields back through the index rather than by inspecting the buffer.

    Cutting here also removes a duplication: the memaddr and the title are terminated by the same
    two cuts, so ra_split_definitions() now takes both from the entry instead of walking the record
    a second time with its own copy of the rule.

    Returns the entry, or NULL when the index is full, so the caller can use the offsets it just
    recorded.
*/
static raViewerEntry* ra_viewer_add(char* base, char* record, u8 flags) {
	raViewerBlock* v = (raViewerBlock*)CARDENGINEI_ARM9_RA_VIEWER_LOCATION;
	raViewerEntry* e;
	char*          p = record;
	char*          d;
	u32            id = 0;
	int            tab = 0;

	if (v->count >= RA_VIEWER_MAX_ENTRIES) {
		return 0;
	}
	e = &v->entry[v->count];
	e->id        = 0;
	e->titleOff  = 0;
	e->pointsOff = 0;
	e->descOff   = 0;
	e->flags     = flags;
	e->pad       = 0;
	/*
	    Cleared explicitly like every field beside it, and it matters more than the others: the block
	    is written where a previous boot's block was, so an entry left alone keeps the last session's
	    value -- which for a date means an achievement showing when a *different* one was earned.
	*/
	e->when      = 0;

	if (flags & RA_VIEWER_EARNED) {
		p += 2;   /* past the `#!` */
	}

	/*
	    The id, and it is read rather than taken: ra_take_id() advances a pointer everything
	    downstream depends on, and this runs before any of that. A record with no id keeps 0, which
	    is what a hand-written file produces and what the menu shows as an achievement it cannot
	    name to the server.
	*/
	d = p;
	while (*d >= '0' && *d <= '9') {
		id = id * 10u + (u32)(*d - '0');
		d++;
	}
	if (d != p && (*d == ':' || (flags & RA_VIEWER_EARNED))) {
		e->id = id;
	}

	for (d = record; *d; d++) {
		if (*d != '\t') {
			continue;
		}
		if (++tab > 4) {
			break;
		}
		*d = 0;   /* the field before it ends here, and the one after it starts as a string */
		if (tab == 4) {
			/*
			    When it was earned, packed by the launcher -- read as a number rather than kept as an
			    offset, because the menu wants five fields and shifts, not a string to parse.

			    Read here and nowhere else: this is the only pass that sees the record before the
			    block is mutated, which is the whole reason the index exists.
			*/
			const char* w = d + 1;
			u32         value = 0;

			while (*w >= '0' && *w <= '9') {
				value = value * 10 + (u32)(*w - '0');
				w++;
			}
			e->when = value;
			continue;
		}
		{
			const u32 off = (u32)(d + 1 - base);

			/*
			    Past what a u16 can hold means the field is unreachable from the index, and the
			    honest answer is to report it absent rather than to record a wrapped offset pointing
			    at another achievement's text. The block is 32K and the field is 16 bits, so this
			    cannot happen today; it is here because the block size is a tunable.
			*/
			if (off < 0x10000u) {
				if (tab == 1)      e->titleOff  = (u16)off;
				else if (tab == 2) e->pointsOff = (u16)off;
				else               e->descOff   = (u16)off;
			}
		}
	}

	/*
	    Earned but not yet sent, which the launcher knows and this binary does not: it read the queue
	    file at boot and left the ids beside the pending tally. Matched here rather than at display
	    time so the menu stays a renderer -- and guarded on the tally's own magic, because a boot that
	    staged nothing leaves that window holding whatever it held.
	*/
	if (e->id && !(flags & RA_VIEWER_EARNED)) {
		const raPendingBlock* const pend =
			(const raPendingBlock*)CARDENGINEI_ARM9_RA_PENDING_LOCATION;

		if (pend->magic == RA_PENDING_MAGIC) {
			u16 k;

			for (k = 0; k < pend->queuedCount && k < RA_PENDING_QUEUED_MAX; k++) {
				if (pend->queued[k] == e->id) {
					e->flags |= RA_VIEWER_QUEUED;
					/*
					    ...and its time, from the queue record's own stamp. An achievement that is
					    queued was earned on this console, so this is the local clock's answer --
					    which is the same thing the server's dates were converted into.
					*/
					if (!e->when) {
						e->when = pend->queuedWhen[k];
					}
					v->queued++;
					break;
				}
			}
		}
	}

	v->count++;
	if (flags & RA_VIEWER_EARNED) {
		v->earned++;
	}
	return e;
}

static u8 ra_split_definitions(char* text, u32 length, char** lines, const char** titles) {
	raViewerBlock* const viewer = (raViewerBlock*)CARDENGINEI_ARM9_RA_VIEWER_LOCATION;
	u8  count = 0;
	u32 i     = 0;

	/*
	    Claimed before the first record, and the magic written first rather than last -- the opposite
	    of every other block in this project, on purpose. Elsewhere the magic says "this is complete";
	    here it says "this binary owns the window now", and the count beside it is what says how much
	    of it is real. A menu opened while this is still filling would read a short list, which is a
	    state that cannot happen -- the split runs at init, long before a player can press X.
	*/
	viewer->magic  = RA_VIEWER_MAGIC;
	viewer->count  = 0;
	viewer->earned = 0;
	viewer->queued = 0;
	viewer->pad    = 0;

	while (i < length && count < RA_DEFS_MAX_LINES) {
		char* start;

		while (i < length && (text[i] == '\n' || text[i] == '\r'
		                   || text[i] == ' '  || text[i] == '\t')) {
			i++;
		}
		if (i >= length) {
			break;
		}
		start = &text[i];
		while (i < length && text[i] != '\n' && text[i] != '\r') {
			i++;
		}
		text[i++] = 0;

		/* Trailing whitespace, because an editor leaves it and rcheevos rejects it. */
		{
			char* end = start;
			while (*end) {
				end++;
			}
			while (end > start && (end[-1] == ' ' || end[-1] == '\t')) {
				*--end = 0;
			}
		}
		/*
		    Indexed here, which is also where the record is cut into fields: `start` arrives whole
		    and NUL-terminated with its tabs intact, and ra_viewer_add() records each field's offset
		    as it replaces that field's tab. Both shapes go in, because the menu shows an earned
		    achievement beside a pending one and only the flag tells them apart.

		    A `#` line that is not `#!` is a comment somebody typed, and gets neither an index entry
		    nor a definition.
		*/
		if (start[0] == '#') {
			if (start[1] == '!') {
				ra_viewer_add(text, start, RA_VIEWER_EARNED);
			}
			continue;   /* earned or a typed comment; either way not a definition */
		}
		if (*start) {
			const raViewerEntry* const e = ra_viewer_add(text, start, 0);

			/*
			    The title comes from the entry rather than from a second walk of the record. One
			    place decides where a field begins, so the notification and the menu cannot disagree
			    about it -- and the record is already cut into strings by the call above, which is
			    what rcheevos needs of `start` anyway.

			    A full index gives no entry, and the definition still arms: an achievement that
			    cannot be listed is a smaller loss than one that cannot fire.
			*/
			titles[count] = (e && e->titleOff) ? (const char*)(text + e->titleOff) : 0;
			lines[count++] = start;
		}
	}
	return count;
}

/*
    Hexadecimal up to the next delimiter, advancing the cursor. Returns whether anything
    was read, because an empty field and a zero are different mistakes.
*/
static bool ra_parse_hex(const char** p, u32* out) {
	const char* s     = *p;
	u32         value = 0;
	bool        any   = false;

	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		s += 2;
	}
	while (1) {
		u32 digit;
		if (*s >= '0' && *s <= '9')      digit = (u32)(*s - '0');
		else if (*s >= 'a' && *s <= 'f') digit = (u32)(*s - 'a' + 10);
		else if (*s >= 'A' && *s <= 'F') digit = (u32)(*s - 'A' + 10);
		else break;
		value = (value << 4) | digit;
		any   = true;
		s++;
	}
	*p   = s;
	*out = value;
	return any;
}

/*
    A watch line: `W:<address>:<size>[:<offset>[:<offset>]]`, addresses being console
    addresses like everywhere else in this file.

    This exists because of what the first hardware run showed. Every mechanical part worked
    -- three definitions parsed, the pointer chain walked four memrefs a frame, nothing
    refused -- and no achievement fired. That is not a bug to debug, it is a fact about the
    game that nobody has measured: the conditions were written from published code notes and
    something about them does not match this ROM.

    Guessing at another definition costs a session. Reading the addresses costs nothing extra,
    because the watchlist already resolves chains and reports values into the snapshot. So
    the same file that carries definitions can carry watches, and the next session answers
    "what does this memory actually hold" instead of "did my next guess work".

      W:1593d0:4        the 32-bit value at console 0x1593d0
      W:159164:4:9c     the 32-bit value at (24-bit pointer at 0x159164) + 0x9c

    Any watch line replaces the built-in self-test watches, so the first four land in the
    snapshot's results[] where they can be read.
*/
static bool ra_add_watch_line(const char* line, u8 flags) {
	u32 base;
	u32 size;
	u32 offsets[RA_CHAIN_MAX];
	u8  depth = 0;

	if (!ra_parse_hex(&line, &base) || *line != ':') {
		return false;
	}
	line++;
	if (!ra_parse_hex(&line, &size)) {
		return false;
	}
	while (*line == ':' && depth < RA_CHAIN_MAX) {
		u32 offset;
		line++;
		if (!ra_parse_hex(&line, &offset)) {
			return false;
		}
		offsets[depth++] = offset;
	}

	/*
	    Console address to DS address, the same translation peek() does. A watch written
	    beside a definition should mean the same thing the definition means.
	*/
	base = ra_rc_translate(base, size ? size : 1);
	if (base == 0) {
		return false;
	}
	return ra_watch_add_flags(base, (u8)size, depth, offsets, flags) >= 0;
}

static const char* ra_definition(raSnapshot* snapshot) {
	const u32* block = (const u32*)CARDENGINEI_ARM9_RA_DEFS_LOCATION;

	/*
	    Published before anything is decided from them, so a photograph says what was actually there
	    rather than what the code concluded. See raSnapshot.defsMagic.
	*/
	snapshot->defsMagic  = block[0];
	snapshot->defsLength = block[1];

	if (block[0] != CARDENGINEI_ARM9_RA_DEFS_MAGIC) {
		snapshot->rcFromFile = 0;
		return RA_TEST_DEFINITION;
	}
	{
		u32   length = block[1];
		char* text   = (char*)(CARDENGINEI_ARM9_RA_DEFS_LOCATION
		                       + CARDENGINEI_ARM9_RA_DEFS_HEADER);

		if (length == 0
		 || length >= CARDENGINEI_ARM9_RA_DEFS_MAX - CARDENGINEI_ARM9_RA_DEFS_HEADER) {
			snapshot->rcFromFile = 0;
			return RA_TEST_DEFINITION;
		}
		/*
		    Terminated here rather than trusted. The launcher writes a terminator, but this
		    string is about to be walked by a parser and the cost of making sure is one
		    store.
		*/
		text[length] = 0;
		/*
		    Trailing newline and carriage return trimmed, because the file was typed by a
		    human in a text editor and rcheevos would reject the whitespace as syntax.
		*/
		while (length > 0 && (text[length - 1] == '\n' || text[length - 1] == '\r'
		                   || text[length - 1] == ' '  || text[length - 1] == '\t')) {
			text[--length] = 0;
		}
		snapshot->rcFromFile = 1;
		snapshot->rcDefLength = (u16)length;
		return text;
	}
}

/*
    Take a leading `<digits>:` off a line and return the id, advancing the pointer past it.

    Zero means the line had no id, which is not an error -- a hand-written ra_achievements.txt is
    not expected to carry them and the set this project shipped as an artifact does not.

    The test is **digits then colon**, and it is exact rather than heuristic. Every memaddr prefix
    flag that ends in a colon is a letter (`A:`, `M:`, `N:`, `O:`, `P:`, `Q:`, `R:`, `T:`, `I:`,
    `K:`, `Z:`, `G:`, `C:`, `B:`), so a digit run before the first colon cannot be memaddr syntax.
    A definition may certainly *begin* with a digit -- the real set's first line is `1=1.300.` --
    which is why the colon is required and why tools/ra_reader_test.c feeds exactly that line.
*/
static u32 ra_take_id(char** line) {
	const char* at = *line;
	u32         id = 0;
	u32         digits = 0;

	while (at[digits] >= '0' && at[digits] <= '9') {
		digits++;
	}
	if (digits == 0 || at[digits] != ':') {
		return 0;
	}

	/*
	    The prefix is stripped whether or not the number survives, and that distinction matters: a
	    line left with `<digits>:` still on the front is not memaddr syntax and rcheevos would refuse
	    the whole definition. So an id that will not fit costs the *reporting* of one achievement, not
	    the achievement.
	*/
	*line = (char*)(at + digits + 1);

	{
		u32 i;
		for (i = 0; i < digits; i++) {
			const u32 digit = (u32)(at[i] - '0');

			/*
			    Refused on overflow rather than clamped, which is a correction. The first version
			    stopped accumulating above 100,000,000 because "RA ids are six or seven digits", and
			    the real set then arrived with 101000001 on its first line. A ten-digit id fits a u32
			    and would have come out one digit short -- naming a different achievement, silently.
			*/
			if (id > (0xFFFFFFFFu - digit) / 10u) {
				return 0;
			}
			id = id * 10 + digit;
		}
	}
	return id;
}

/*
    Bring rcheevos up and report how far it got. Two functions rather than one, and the split is
    the whole point of this build.

    ra_rc_prepare() runs once: it probes the arena, initialises the runtime, reads the staged
    definitions and installs any watch lines. ra_rc_activate_next() then activates **one**
    definition per frame until the set is in.

    Why: fifty-six definitions cannot be parsed inside a single interrupt. Each costs the same
    ~2.4 KB of stack -- measured on a host, and flat, so depth is not what scales -- plus its own
    slice of time, and the first run that tried all fifty-six in one VCOUNT handler ended in a
    Data Abort with the ARM9 executing the definition text as code. That total time is still
    unmeasured, which is exactly why it is the leading suspect and why the fix is to stop doing
    it rather than to reason about it further.

    The second reason is diagnostic and matters just as much. rcActivated is published *before*
    each activation is attempted, so a crash names the line it died on. A set that dies at the
    same definition every time is one definition's problem; a set that gets through all of them
    and dies later is the frame budget's. Those are different bugs and the old code could not
    tell them apart.
*/
static u8 ra_rc_prepare(raSnapshot* snapshot) {
	char* text;
	u8    i;

	{
		void* probe = malloc(sizeof(rc_memrefs_t));

		if (probe == 0) {
			return RA_RC_NO_MEMORY;
		}
		free(probe);
	}

	rc_runtime_init(&runtime);
	if (runtime.memrefs == 0) {
		return RA_RC_NO_MEMREFS;
	}

	text        = (char*)ra_definition(snapshot);
	defLines[0] = text;
	defCount    = 1;
	if (snapshot->rcFromFile) {
		defCount = ra_split_definitions(text, snapshot->rcDefLength, defLines, defTitles);
	}
	/*
	    All of it reset, not just the index. On hardware this runs once, so it would never have
	    mattered there -- and tools/ra_reader_test.c prepares twice, which is how a static that
	    carried a previous run's count showed up. A function whose correctness depends on being
	    called only once is a function that will eventually be called twice.
	*/
	/*
	    Ids taken here, once, right after the split -- not at activation time. The pointers in
	    defLines are what everything downstream uses, so they have to already be past the id; doing
	    it later would mean every user of a line remembering to skip it.
	*/
	snapshot->rcDefsWithId = 0;
	snapshot->rcDefsNoId   = 0;
	for (i = 0; i < defCount; i++) {
		defIds[i] = ra_take_id(&defLines[i]);
		if (defIds[i]) {
			snapshot->rcDefsWithId++;
		} else {
			/*
			    Numbered far from anything real, because rcheevos identifies achievements by id and
			    reuses the trigger of one it has already seen. See RA_SYNTHETIC_ID_BASE.
			*/
			defIds[i] = RA_SYNTHETIC_ID_BASE + i;
			snapshot->rcDefsNoId++;
		}
	}

	defIndex       = 0;
	defFirstError  = RC_OK;
	activatedCount = 0;
	initMaxLines   = 0;
	initTotalLines = 0;

	snapshot->rcActivated = 0;
	snapshot->rcActivate  = 0;
	snapshot->rcInitLines = 0;
	snapshot->rcInitTotal = 0;

	/*
	    A strip rendered before any achievement has fired, so raSnapshot.overlayText is a valid address
	    from the first frame instead of zero until the first unlock.

	    It exists to make the overlay testable without spending an achievement. A build with
	    -DRA_OVERLAY_DEMO=60 pulses the notification once a second, which is the only way to ask "can
	    anything be seen at all" in ten seconds rather than in a three-minute session that ends with one
	    achievement permanently earned. With a zero strip that probe draws a blank box and answers
	    nothing.

	    Harmless in an ordinary build: nothing raises a notification except a trigger, and a trigger
	    re-renders with the real title first. The placeholder can only ever reach the screen in a build
	    that asked for it.
	*/
	textStrip = ra_text_render(RA_TEXT_HEADING, "probe 0123456789");

	/*
	    Watches first, and only clearing the defaults if the file actually supplies some -- a
	    file of definitions alone should still show the self-test watches, which are the thing
	    that says the reader is alive at all.

	    Still done in one go: a watch line is a handful of hex fields, not a parse.
	*/
	{
		bool anyWatch = false;

		for (i = 0; i < defCount; i++) {
			const char* rest  = 0;
			u8          flags = 0;

			/* `W:` is a plain chain; `W24:` masks each pointer to 24 bits. */
			if (defLines[i][0] == 'W' && defLines[i][1] == ':') {
				rest = defLines[i] + 2;
			} else if (defLines[i][0] == 'W' && defLines[i][1] == '2'
			        && defLines[i][2] == '4' && defLines[i][3] == ':') {
				rest  = defLines[i] + 4;
				flags = RA_WATCH_FLAG_PTR24;
			}
			if (!rest) {
				continue;
			}
			if (!anyWatch) {
				ra_watch_clear();
				anyWatch = true;
			}
			if (!ra_add_watch_line(rest, flags)) {
				snapshot->rcBadLine = i + 1;
			}
		}
	}

	return RA_RC_LOADING;
}

/*
    As many definitions as fit a scanline budget, then out. Returns RA_RC_LOADING while any remain.

    It used to do exactly one per frame, and hardware showed why that was wrong. A Contra 4 session
    read `rcStage 5` (loading), `rcActivated 14` of 45 and **`rcPeeks 0`** -- rc_runtime_do_frame had
    never run once, so nothing could ever unlock. `ticks` and `wramTicks` both stopped at 15, so the
    reader got fifteen frames and one-per-frame needed forty-five. The set was never armed.

    Why the reader stops at fifteen frames is a separate and still-open question. This does not depend
    on the answer: finishing in a handful of frames instead of forty-five means the set is armed inside
    whatever window exists, which is worth having either way -- an achievement that arms half a second
    into a session rather than three quarters is strictly better even when nothing is broken.

    The budget is measured rather than guessed. rcInitLines reported 27 scanlines for the most
    expensive single definition of this set against a frame of 263, so RA_RC_INIT_BUDGET_LINES sits
    under half a frame and at least one definition is always attempted -- the check is after the work,
    so a definition costing more than the whole budget still makes progress instead of deadlocking.

    Every line gets its own achievement id, numbered from RA_TEST_ACHIEVEMENT_ID, so the first
    keeps the id the measured-progress fields report on and the rest still count toward
    rcTriggered. rcActivate carries the *first* failure rather than the last -- a set with one bad
    line among fifty-six should say which, not be overwritten by whichever came last.
*/
#define RA_RC_INIT_BUDGET_LINES 120

static u8 ra_rc_activate_next(raSnapshot* snapshot) {
	int one;
	u16 startLine = 0;
	u16 spent;
	u16 spentThisFrame = 0;
	u8  line;

	for (;;) {
	/* Skip watch lines; ra_rc_prepare() already dealt with them. */
	while (defIndex < defCount
	    && defLines[defIndex][0] == 'W'
	    && (defLines[defIndex][1] == ':' || defLines[defIndex][1] == '2')) {
		defIndex++;
	}

	if (defIndex >= defCount) {
		break;
	}
	{
		line = defIndex;
		defIndex++;

		/*
		    Timed one definition at a time, which is also the only way the total can be right:
		    a single VCOUNT delta around the whole set is taken modulo 263, so a parse spanning
		    four frames reports the remainder and a slow init reads as a fast one. Per-definition
		    deltas sum correctly as long as no single activation exceeds a frame, and
		    initMaxLines is what says whether that held.
		*/
		startLine = RA_VCOUNT;
		one = rc_runtime_activate_achievement(
			&runtime, defIds[line], defLines[line], 0, 0);
		spent = (u16)((RA_VCOUNT - startLine + RA_SCANLINES_PER_FRAME)
		              % RA_SCANLINES_PER_FRAME);

		initTotalLines += spent;
		if (spent > initMaxLines) {
			initMaxLines = (u8)((spent > 255) ? 255 : spent);
		}
		snapshot->rcInitLines = initMaxLines;
		snapshot->rcInitTotal = (u16)((initTotalLines > 0xFFFF) ? 0xFFFF : initTotalLines);

		if (one == RC_OK) {
			activatedCount++;
		} else if (defFirstError == RC_OK) {
			defFirstError        = one;
			snapshot->rcBadLine  = (u8)(line + 1);
		}
		/*
		    Published after each definition rather than after all of them, which is what makes a
		    crash name its own line: rcActivated is the count that succeeded, so dying inside
		    definition k leaves k-1 here. Kept as a count rather than briefly holding the index
		    being attempted -- a field that means two things depending on when you read it is not
		    a reading, and rcActivate being 0 already says none of the k-1 failed.
		*/
		snapshot->rcActivated = activatedCount;
		snapshot->rcActivate  = (s8)defFirstError;

		/*
		    Checked after the work, so a single definition more expensive than the whole budget still
		    advances by one rather than being attempted forever.
		*/
		spentThisFrame += spent;
		if (spentThisFrame >= RA_RC_INIT_BUDGET_LINES) {
			return RA_RC_LOADING;
		}
	}
	}

	/*
	    A file of watches alone is legitimate -- measuring memory is a reason to boot -- so only
	    a file that offered definitions and had none parse is a failure. The self-test keeps the
	    runtime doing something either way.
	*/
	if (activatedCount == 0) {
		if (rc_runtime_activate_achievement(&runtime, RA_TEST_ACHIEVEMENT_ID,
		                                   RA_TEST_DEFINITION, 0, 0) != RC_OK) {
			return RA_RC_PARSE_BAD;
		}
		/*
		    Recorded in defIds too, so the measured-progress lookup and the line search below both
		    find it. The fallback used to be indistinguishable from a staged definition because both
		    were numbered 1; now it has to say so.
		*/
		defLines[0]           = (char*)RA_TEST_DEFINITION;
		defIds[0]             = RA_TEST_ACHIEVEMENT_ID;
		defCount              = 1;
		activatedCount        = 1;
		snapshot->rcActivated = 1;
	}

	/*
	    Ask rcheevos to check every address the set ended up referencing, now, against what this
	    console has. rcheevos disables any achievement that names one it cannot supply, so this
	    is what turns "an achievement silently never fires" into rcPeeksRejected.

	    Done once, here, and deliberately after the last definition rather than after each one:
	    it walks the whole memref pool, so per-definition it would be O(n squared) over a pool
	    that ends up hundreds long.
	*/
	rc_runtime_validate_addresses(&runtime, ra_rc_event_handler, ra_rc_validate_address);

	return RA_RC_ACTIVE;
}

/*
    One frame of evaluation, as a step so it can be run on the private stack like the rest.

    Returns a stage only to fit raRcStep; the caller keeps using RA_RC_FRAME. A wrapper rather
    than an asm call to rc_runtime_do_frame() directly, because the callbacks it needs are static
    to this file and the trampoline takes one argument.
*/
/*
    rc_runtime_do_frame(), with the trigger loop divided into `parts` slices and only slice `slice`
    evaluated. Memrefs are updated on every call regardless.

    **Reimplemented here rather than patched into rcheevos, because rcheevos is a submodule** pinned
    at RetroAchievements' own v12.4.0. A change to its working tree belongs to no commit this project
    can make: the parent records a gitlink, so the edit would build on this machine and vanish from a
    fresh clone. The first version of this was exactly that mistake.

    What is lost by not calling upstream's function is the part of it this fork has no use for. Its
    loop raises nine event types; ra_rc_event_handler() acts on one, RC_RUNTIME_EVENT_ACHIEVEMENT_
    TRIGGERED, and counts the rest. There are no leaderboards and no rich presence here, so those two
    loops iterate zero times. What is kept is everything that changes behaviour: memrefs updated
    first, triggers skipped when null or holding an invalid memref, RESET read back off the trigger
    rather than treated as a state, and events raised only on a real transition.

    `eventCount` therefore counts state transitions rather than upstream's nine event kinds, which is
    a narrowing of what raSnapshot.rcEvents means and is why it is written down here.

    Modulo rather than a contiguous range, so a change of `parts` mid-session cannot leave a band of
    triggers unvisited for a whole cycle.
*/
static void ra_rc_do_frame_slice(u8 slice, u8 parts) {
	rc_runtime_event_t ev;
	int32_t i;

	if (parts == 0) {
		parts = 1;
	}

	/*
	    Timed on its own, because it is the one part of this function no slice count can shrink: every
	    memref is updated on every call. Whole-frame readings on two games put this at roughly half
	    the cost and that was arithmetic on two data points -- this is the measurement.
	*/
	{
		const u16 mrStart = RA_VCOUNT;
		u16 mrLines;

		rc_update_memref_values(runtime.memrefs, ra_rc_peek, 0);

		mrLines = (RA_VCOUNT - mrStart + RA_SCANLINES_PER_FRAME) % RA_SCANLINES_PER_FRAME;
		if (mrLines > 255) {
			mrLines = 255;
		}
		if ((u8)mrLines > memrefLinesMax) {
			memrefLinesMax = (u8)mrLines;
		}
		if (memrefLinesMin == 0 || (u8)mrLines < memrefLinesMin) {
			memrefLinesMin = (u8)mrLines;
		}
	}

	ev.value = 0;

	for (i = (int32_t)runtime.trigger_count - 1; i >= 0; --i) {
		rc_trigger_t* trigger = runtime.triggers[i].trigger;
		int old_state, new_state;

		if (!trigger || runtime.triggers[i].invalid_memref) {
			continue;
		}
		if (parts > 1 && ((u32)i % parts) != slice) {
			continue;
		}

		old_state = trigger->state;
		new_state = rc_evaluate_trigger(trigger, ra_rc_peek, 0, 0);
		/*
		    RESET is a notification rather than a state -- upstream raises an event for it and then
		    reads the real state back off the trigger. Nothing here consumes the notification, so
		    only the read-back is kept.
		*/
		if (new_state == RC_TRIGGER_STATE_RESET) {
			new_state = trigger->state;
		}

		if (new_state == old_state) {
			continue;
		}

		if (new_state == RC_TRIGGER_STATE_TRIGGERED) {
			ev.type = RC_RUNTIME_EVENT_ACHIEVEMENT_TRIGGERED;
			ev.id   = runtime.triggers[i].id;
			ra_rc_event_handler(&ev);   /* counts itself */
		} else {
			eventCount++;
		}
	}
}

static u8 ra_rc_frame_step(raSnapshot* snapshot) {
	/*
	    One slice of the trigger list, and every memref. rcParts is 1 until a measurement raises it,
	    so on a set that fits inside the game's blanking period this evaluates everything, every
	    frame, exactly as rc_runtime_do_frame() did.
	*/
	ra_rc_do_frame_slice(rcSlice, rcParts);

	/*
	    After the frame, not inside the event handler. The handler runs deep inside rcheevos with the
	    runtime mid-update, and the handover writes to memory the other CPU polls -- doing it here keeps
	    the two apart and costs one compare on a frame with nothing to send.
	*/
	ra_rc_offer_unlock(snapshot);
	snapshot->unlockSent      = unlockSent;
	snapshot->unlockSynthetic = unlockSynthetic;
	/*
	    And into the menu's pending block, so Sync Pending counts what this session earned rather than
	    only what was owed at boot -- which is the difference between the page answering "did the one I
	    just earned get queued" and never showing it until the next boot.

	    A store into the window this binary owns. Guarded on the launcher's magic so a boot that staged
	    nothing is left alone rather than given a header it never wrote.

	    This was removed during an isolation run and stayed removed by accident; the page has been
	    reporting boot-time state only ever since.
	*/
	if (((raPendingBlock*)CARDENGINEI_ARM9_RA_PENDING_LOCATION)->magic == RA_PENDING_MAGIC) {
		((raPendingBlock*)CARDENGINEI_ARM9_RA_PENDING_LOCATION)->session = unlockSent;
	}
	snapshot->unlockQueued = unlockQueued;
	snapshot->unlockLost   = unlockLost;

	return RA_RC_FRAME;
}

/*
    Called once per frame from ra_wram_tick(), after the watchlist. Runs inside the game's
    VCOUNT interrupt handler like everything else in this binary, so the cost is measured
    rather than assumed -- see rcLines.
*/
void ra_rc_tick(raSnapshot* snapshot) {
	const rc_trigger_t* trigger;
	unsigned measured = 0;
	unsigned target   = 0;
	u16 startLine = 0;
	u16 lines;

	/*
	    Coming up, spread over frames: prepare on one tick, then one definition per tick until
	    the set is in. Nothing evaluates until it is -- do_frame on a half-loaded runtime would
	    make rcLinesMax a measurement of a moving target.

	    An error stage matches neither branch and is left alone, so a failure stays reported
	    rather than being retried every frame forever.
	*/
	if (rcStage < RA_RC_ACTIVE) {
		if (rcStage == RA_RC_NONE) {
			rcStage = ra_rc_step(ra_rc_prepare, snapshot);
		} else if (rcStage == RA_RC_LOADING) {
			rcStage = ra_rc_step(ra_rc_activate_next, snapshot);
		}
		snapshot->rcStage = rcStage;
		if (rcStage < RA_RC_ACTIVE) {
			return;
		}
	}

	peeksThisFrame = 0;

	/*
	    On our stack too, and not only because 767 bytes is a lot to borrow: one stack for all of
	    rcheevos means rcStackUsed is the high-water mark for everything the library does rather
	    than for the parse alone, and it means there is one thing to reason about instead of two.
	    peek() is called from in here, so it runs on our stack as well -- which it should, since
	    it is rcheevos that decides how deep to call it from.
	*/
	/*
	    Evaluate, unless the last evaluation overran the blanking period and this frame is one of
	    the ones being sat out to pay for it.

	    The budget is not a constant: it is **how much blanking is actually left** when this runs.
	    We are chained after the game's own VBlank handler, so what remains depends on what the game
	    just did, and startLine is already sampled. Measuring the room rather than assuming it is
	    what makes this correct on a game nobody has tested.

	    Whole frames are skipped rather than the set being split across them. rc_runtime_do_frame()
	    updates every memref and evaluates every trigger in one call, so half a set on this frame and
	    half on the next would hand the delta operators two different notions of "previous". Skipping
	    is only a lower sample rate, and this project has already shipped one: on the old VCOUNT hook
	    the reader ran on about 8% of frames and achievements still fired.
	*/
	lines = 0;
	if (frameSkip) {
		frameSkip--;
	} else {
		u16 room;

		startLine = RA_VCOUNT;
		room = (startLine >= RA_VBLANK_FIRST_LINE)
		       ? (u16)(RA_SCANLINES_PER_FRAME - startLine)
		       : 0;
		if ((u8)room > roomMax) {
			roomMax = (u8)room;
		}

		if (!ra_rc_frame_fits(linesLastRun, (u8)room, starve)) {
			/* Not enough blanking left this frame to hold what the last one cost. */
			if (starve < 255) {
				starve++;
			}
		} else {
			starve = 0;

			ra_rc_step(ra_rc_frame_step, snapshot);
			lines = (RA_VCOUNT - startLine + RA_SCANLINES_PER_FRAME) % RA_SCANLINES_PER_FRAME;

			if (lines > 255) {
				lines = 255;
			}
			linesLastRun = (u8)lines;
			if ((u8)lines > linesMax) {
				linesMax = (u8)lines;
			}

			/*
			    Advance to the next slice, and divide the set further if this one did not fit.

			    linesLastRun is thrown away on a change of rcParts rather than scaled: what a
			    smaller slice will cost is exactly the thing that cannot be predicted from a
			    larger one, and a zero is already defined everywhere here as "not measured, so
			    run" -- which is what makes the next frame a measurement instead of a refusal.
			    Convergence is one frame per step and there are at most seven.
			*/
			rcSlice = (u8)((rcSlice + 1) % rcParts);
			{
				const u8 want = ra_rc_frame_parts(rcParts, (u8)lines, (u8)room);

				if (want != rcParts) {
					rcParts = want;
					rcSlice = 0;
					linesLastRun = 0;
				}
			}

			/*
			    Sit out enough frames that the average lands inside the room we had. Kept
			    alongside the gate above rather than replaced by it: the gate stops the
			    overruns nobody had to take, and this pays back the ones starvation forced.
			*/
			frameSkip = ra_rc_frame_skip(lines, room);
			if (frameSkip > frameSkipMax) {
				frameSkipMax = frameSkip;
			}
		}
	}

	rcStage = RA_RC_FRAME;

	/*
	    Reported for the *first staged definition*, whatever its id turned out to be, rather than for
	    the constant 1. Those were the same thing while every definition was numbered from
	    RA_TEST_ACHIEVEMENT_ID; with real ids they are not, and asking for 1 would have quietly
	    reported on an achievement that does not exist.
	*/
	{
		const u32 firstId = defCount ? defIds[0] : RA_TEST_ACHIEVEMENT_ID;

		rc_runtime_get_achievement_measured(&runtime, firstId, &measured, &target);
		trigger = rc_runtime_get_achievement(&runtime, firstId);
	}

	snapshot->rcStage         = rcStage;
	snapshot->rcTriggerState  = trigger ? trigger->state : RC_TRIGGER_STATE_INACTIVE;
	snapshot->rcTriggered     = triggeredCount;
	snapshot->rcFirstTriggered = firstTriggered;
	snapshot->rcFirstId        = firstId;
	/*
	    Latched at the last active reading rather than copied blindly. rcheevos reports
	    measured progress only while a trigger is active, so both of these go back to zero
	    the moment the achievement fires -- which would leave a snapshot taken after the
	    unlock showing a pair of zeros and no sign of how it got there -- which is what the
	    first successful hardware reading did show.

	    The latched value is the last one reported while the trigger was active, so it is
	    one short of the target: on the frame the count reaches it, the trigger fires and
	    rcheevos has already stopped reporting. 599 of 600 beside rcTriggered = 1 is the
	    honest reading, not an off-by-one.
	*/
	if (target != 0) {
		snapshot->rcMeasured = measured;
		snapshot->rcTarget   = target;
	}
	snapshot->rcPeeks         = peeksThisFrame;
	snapshot->rcPeeksRejected = peeksRejected;
	snapshot->rcLines         = (u8)lines;
	snapshot->rcLinesMax      = linesMax;
	snapshot->rcRoomMax       = roomMax;
	snapshot->rcParts         = rcParts;
	snapshot->rcMemrefLines   = memrefLinesMax;
	snapshot->rcMemrefMin     = memrefLinesMin;
	snapshot->rcEvents        = (u8)((eventCount > 255) ? 255 : eventCount);
}
