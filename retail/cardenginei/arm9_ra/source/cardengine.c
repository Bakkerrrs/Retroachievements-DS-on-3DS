/*
    RetroAchievements support for nds-bootstrap -- the ARM9 code that lives outside the
    cardengine, and the watchlist it now owns.

    The cardengine is linked into a fixed 12K window and at its tightest had 28 bytes
    spare, against a single watch costing 24. This binary has 256K of DSi WRAM, so the
    watchlist and the pointer-chain walker moved here and RA_WATCH_MAX went from 2 to 16.
    The cardengine keeps the per-frame entry point and the snapshot; it hands over a
    pointer once a frame and knows nothing else about how any of this works.

    Two things shape the walker, and they are the same two that shaped it in the
    cardengine.

    The first is that a pointer read out of a running game is not trustworthy. It is
    whatever happens to be in that word this frame, including garbage while the game
    tears down one scene and builds the next. Dereferencing it blind takes a Data Abort
    in the middle of the game's interrupt handler, which is a crash. So every address is
    range-checked immediately before it is used, and a chain that does not resolve is
    reported as not resolved rather than chased.

    The second is that the resolved address may not be cached. That is the entire reason
    pointer chains exist: the structure moves, and an address that was correct last frame
    points into the middle of something else this frame. Walking the chain again every
    tick is not wasted work, it is the work.

    This file is part of nds-bootstrap and is licensed under the GPL-3.0,
    the same terms as the rest of the project.
*/

#include "ra.h"
#include "ra_overlay.h"
#include "locations.h"

/*
    retail/cardenginei/arm9_ra/source/startup.c -- the crt0 this window does not have. It
    takes the arena bounds rather than reading the linker symbols itself, so the host test
    can hand it a scratch buffer and exercise the real code.
*/
extern u8  ra_startup(char* bssStart, char* bssEnd, char* windowTop);
extern u32 ra_heap_size(void);
extern u32 ra_heap_used(void);
extern u32 ra_heap_break(void);
extern u32 ra_heap_top(void);
extern u32 ra_malloc_probe(void);
extern u32 ra_sbrk_probe(void);

/* retail/cardenginei/arm9_ra/source/ra_rcheevos.c -- the RetroAchievements runtime. */
extern void ra_rc_tick(raSnapshot* snapshot);
/* ...and where it left the rendered notification, or 0 before the first unlock. */
extern const void* ra_rc_text(void);

/* Placed by cardengine.ld: the .bss to clear, and the top of this binary's window. */
extern char __bss_start[];
extern char __bss_end[];
extern char __vram_top[];

/* Main RAM as the game sees it, mirrors included. */
#define RA_MAIN_RAM_START 0x02000000
#define RA_MAIN_RAM_END   0x03000000

/* Offset of raSnapshot.ticks -- what the self-test chains resolve to. */
#define RA_TICKS_OFFSET 4
typedef char raTicksOffsetCheck[
	(RA_TICKS_OFFSET == __builtin_offsetof(raSnapshot, ticks)) ? 1 : -1];

/*
    The watchlist, and this binary's own frame counter, both in .bss.

    Nothing had ever kept state in this window between frames before, so `frames` is
    also a test: the cardengine copies it into snapshot.wramTicks every tick rather than
    counting there itself, so if .bss here does not persist it sticks at 1 while
    snapshot.ticks climbs. Everything after this -- rcheevos above all -- depends on the
    answer being yes.

    Like the cardengine's .bss, this is never zeroed: the bootloader copies the loaded
    image and there is no crt0. Hence the magic.
*/
static u32     stateMagic;
static u32     frames;
/* ra_icache_claim()'s report, kept because it is written once and reported every tick. */
static u8      mpuBits;
static u8      watchCount;
static raWatch watches[RA_WATCH_MAX];

/*
    Where a pointer read out of the game is allowed to point. A game pointer is always a
    main RAM address, so one that is not has been read out of a structure that no longer
    exists -- which is the normal case between scenes, not an exceptional one.

    Written as `addr <= END - len` rather than `addr + len <= END` so a base near the top
    of the address space cannot wrap past the check.

    Not static, because ra_rcheevos.c needs it too -- it has to know whether an address is
    main RAM at all before writing a sentinel through it to test for mirroring.
*/
bool ra_in_main_ram(u32 addr, u32 len) {
	return addr >= RA_MAIN_RAM_START && addr <= RA_MAIN_RAM_END - len;
}

/*
    Where the final read is allowed to land. Main RAM is where every real watch lands;
    I/O is reachable only because the diagnostic watch reads a display register, and
    nothing else is listed -- an address outside these is reported as unreadable rather
    than dereferenced to find out what happens.

    Note this does *not* include the window this code is running from. A watch has no
    business reading DSi WRAM, and leaving it out means a chain that somehow produces an
    address in here is reported rather than followed.

    Not static, because ra_rcheevos.c routes rcheevos' peek() through it. That is the
    whole point: an address out of a definition the server sent gets checked by the same
    function as an address out of a hand-written watch, so there is one answer in this
    binary to "may this be read" rather than two that can drift apart.
*/
bool ra_readable(u32 addr, u32 len) {
	return ra_in_main_ram(addr, len)
	    || (addr >= 0x04000000 && addr <= 0x04001100 - len);
}

/*
    Read `size` bytes, and the three-byte case is why this is not a two-line function.

    It used to fall through to a 32-bit load for anything that was not 1 or 2, which is correct
    for the only sizes a watch line can carry -- ra_watch_add_flags() rejects everything but
    1, 2 and 4. rcheevos is not so limited: `0xW` is a **24-bit** read, and the first achievement
    set this project did not write contains one. A 32-bit load answers that with a fourth byte
    that is not part of the value, and through AddAddress that byte becomes part of a *pointer* --
    so the read that follows lands at an address the definition never named. Nothing would have
    reported it; the achievement would simply never fire.

    Anything that is not a native width is therefore assembled from bytes, which also makes it
    alignment-proof: an unaligned LDR on the ARM9 returns the word *rotated* rather than
    faulting, so the hardware would have answered, just wrongly.
*/
u32 ra_read(u32 addr, u8 size) {
	if (size == 1) {
		return *(const vu8*)addr;
	}
	if (size == 2 && (addr & 1) == 0) {
		return *(const vu16*)addr;
	}
	if (size == 4 && (addr & 3) == 0) {
		return *(const vu32*)addr;
	}
	{
		u32 value = 0;
		u32 i;

		for (i = 0; i < size; i++) {
			value |= (u32)(*(const vu8*)(addr + i)) << (i * 8);
		}
		return value;
	}
}

/*
    Make this binary's own window instruction-cacheable, once, and report what was found.

    **Why there is anything to do here at all.** 47 of the 70 available scanlines go on
    rc_update_memref_values(), which is 237 reads -- about 840 ARM9 cycles for a translate, a range
    check and a load. The trigger loop shows the same ratio, ~2,000 cycles per definition. Nothing in
    either does work of that order, and two unrelated halves being slow by the same factor is not a
    property of what the code does but of how it executes. This binary runs from DSi WRAM at
    0x03740000, and a DS game's own MPU setup has no reason to have marked that region cacheable --
    on a retail DS there is nothing there.

    **Instruction cache only, and that asymmetry is the whole safety argument.** Cacheability is per
    MPU region, and the region that happens to cover 0x03740000 may well cover much more: I/O
    registers at 0x04000000, and the ROM cache below us at 0x03700000 which the card DMAs into. A
    data cache over either of those is fatal -- VCOUNT would stop advancing and cached ROM would go
    stale. An *instruction* cache over them is inert, because nothing is ever executed from an I/O
    register or from a ROM cache line. Only code is fetched, and the only code in this region is
    ours, written once by the bootloader before we ever run and never modified afterwards.

    **And it declines to touch a region that covers main RAM.** If the winning region for our address
    turns out to be one that also spans 0x02000000 -- which nds-bootstrap's own MPU patch can produce,
    it widens a region to PAGE_128M at base 0 -- then flipping this bit would change how the *game's*
    code is fetched, and a loader that DMAs overlays into main RAM is exactly the program that must
    not have an instruction cache switched on underneath it. In practice such a region already has
    the bit set, because no DS game gives up its instruction cache; either way the decision belongs
    to a measurement rather than to this function, so it reports and leaves it alone.

    The returned byte is that report, and it is the only output. See raSnapshot.raMpuBits.
*/
/*
    Which of the eight regions wins for `addr`, and what its base is, from the eight raw region
    registers -- and it is a pure function taking them as an array for the reason ra_rc_frame_skip()
    is one: the arithmetic is the part with the logic, the coprocessor read is the part a host does
    not have, and tools/ra_reader_test.c drives this directly.

    Region format on the ARM946: bits 31-12 the base, bits 5-1 the size as 2^(N+1), bit 0 the enable.
    Regions are priority-ordered with the highest number winning, so iterating upward and overwriting
    on each match leaves the winner. A size field below 11 is under the 4 KB minimum and is not a
    region; 31 is the whole address space, whose length cannot be shifted for without overflowing.

    Returns -1 if no enabled region covers the address, and writes the winner's base to *base.
*/
int ra_mpu_region_pick(const u32* regs, u32 addr, u32* base) {
	int best = -1;
	u8  n;

	*base = 0;
	for (n = 0; n < 8; n++) {
		const u32 r = regs[n];
		u32 sz, regionBase;

		if (!(r & 1)) {
			continue;
		}
		sz         = (r >> 1) & 0x1F;
		regionBase = r & 0xFFFFF000;
		if (sz < 11) {
			continue;
		}
		if (sz >= 31) {
			best  = n;
			*base = 0;
			continue;
		}
		if (addr >= regionBase && (addr - regionBase) < (1u << (sz + 1))) {
			best  = n;
			*base = regionBase;
		}
	}
	return best;
}

/*
    ...and the report itself, from the region choice and the three CP15 words. Pure for the same
    reason, so every branch of the decision is exercised on a host rather than only on a 3DS.

    `spansMainRam` is the caller's, not decided here, because "does this region reach main RAM" is
    one comparison and passing the answer keeps this function about the bits.
*/
u8 ra_mpu_report(int region, u32 icfg, u32 dcfg, u32 control, u8 spansMainRam) {
	u8 bits;

	if (region < 0) {
		return RA_MPU_NO_REGION;
	}

	bits = (u8)region;
	if (icfg & (1u << region)) {
		bits |= RA_MPU_ICACHE_WAS_ON;
	}
	if (dcfg & (1u << region)) {
		bits |= RA_MPU_DCACHE_ON;
	}
	if (control & (1u << 12)) {
		bits |= RA_MPU_ICACHE_GLOBAL;
	}
	if (spansMainRam) {
		bits |= RA_MPU_SPANS_MAIN_RAM;
	}
	return bits;
}

#ifdef __arm__
static u32 ra_mpu_region_reg(u8 n) {
	u32 v = 0;

	switch (n) {
		case 0: asm volatile("mrc p15,0,%0,c6,c0,0" : "=r"(v)); break;
		case 1: asm volatile("mrc p15,0,%0,c6,c1,0" : "=r"(v)); break;
		case 2: asm volatile("mrc p15,0,%0,c6,c2,0" : "=r"(v)); break;
		case 3: asm volatile("mrc p15,0,%0,c6,c3,0" : "=r"(v)); break;
		case 4: asm volatile("mrc p15,0,%0,c6,c4,0" : "=r"(v)); break;
		case 5: asm volatile("mrc p15,0,%0,c6,c5,0" : "=r"(v)); break;
		case 6: asm volatile("mrc p15,0,%0,c6,c6,0" : "=r"(v)); break;
		case 7: asm volatile("mrc p15,0,%0,c6,c7,0" : "=r"(v)); break;
	}
	return v;
}

u8 ra_icache_claim(u32 addr) {
	u32 regs[8];
	u32 icfg = 0, dcfg = 0, control = 0;
	u32 base = 0;
	int best;
	u8  n;
	u8  bits;

	for (n = 0; n < 8; n++) {
		regs[n] = ra_mpu_region_reg(n);
	}
	best = ra_mpu_region_pick(regs, addr, &base);

	asm volatile("mrc p15,0,%0,c2,c0,1" : "=r"(icfg));
	asm volatile("mrc p15,0,%0,c2,c0,0" : "=r"(dcfg));
	asm volatile("mrc p15,0,%0,c1,c0,0" : "=r"(control));

	/* A base at or below main RAM means the region reaches from there to us, so it holds the
	   game's own code. Reported and left alone -- see the note above. */
	bits = ra_mpu_report(best, icfg, dcfg, control, (u8)(best >= 0 && base <= RA_MAIN_RAM_START));

	if (best >= 0
	 && !(bits & (RA_MPU_ICACHE_WAS_ON | RA_MPU_SPANS_MAIN_RAM))) {
		icfg |= 1u << best;
		asm volatile("mcr p15,0,%0,c2,c0,1" :: "r"(icfg));
		asm volatile("mcr p15,0,%0,c7,c5,0" :: "r"(0));   /* invalidate the instruction cache */
		bits |= RA_MPU_ICACHE_SET;
	}

	return bits;
}
#else
/* No coprocessor on the host. The two functions above are what the suite exercises. */
u8 ra_icache_claim(u32 addr) {
	(void)addr;
	return RA_MPU_NO_REGION;
}
#endif

/*
    Add a watch, returning its index or -1 if the list is full or the request is
    malformed. Nothing is trusted at add time -- addresses are validated on every tick,
    not here -- because a chain that resolves now may not resolve next frame, and the
    walker has to survive that either way.
*/
int ra_watch_add_flags(u32 base, u8 size, u8 depth, const u32* offsets, u8 flags) {
	raWatch* w;
	u8 d;

	if ((size != 1 && size != 2 && size != 4)
	 || depth > RA_CHAIN_MAX
	 || (depth && !offsets)
	 || watchCount >= RA_WATCH_MAX) {
		return -1;
	}

	w = &watches[watchCount];
	w->base  = base;
	w->size  = size;
	w->depth = depth;
	for (d = 0; d < RA_CHAIN_MAX; d++) {
		w->offsets[d] = 0;
	}
	for (d = 0; d < depth; d++) {
		w->offsets[d] = offsets[d];
	}
	w->address = 0;
	w->value   = 0;
	w->flags   = flags;
	w->status  = RA_WATCH_PENDING;

	return watchCount++;
}

int ra_watch_add(u32 base, u8 size, u8 depth, const u32* offsets) {
	return ra_watch_add_flags(base, size, depth, offsets, 0);
}

void ra_watch_clear(void) {
	watchCount = 0;
}

/*
    Walk one watch's chain from its base and read the value at the end. Every address is
    checked in the moment it is about to be used; nothing is carried over from the
    previous tick, because nothing from the previous tick is still known to be true.
*/
static void ra_watch_eval(raWatch* w) {
	/*
	    Clamped rather than trusted. ra_watch_add() validates depth, so this cannot fire
	    today -- but the magic that makes this state trustworthy is only four bytes, and
	    if garbage .bss ever matched it, an unclamped depth would read past offsets[] and
	    walk arbitrarily many indirections inside an interrupt handler.
	*/
	const u8 depth = (w->depth > RA_CHAIN_MAX) ? RA_CHAIN_MAX : w->depth;
	u32 addr = w->base;
	u8 d;

	w->address = 0;

	for (d = 0; d < depth; d++) {
		if ((addr & 3) || !ra_in_main_ram(addr, 4)) {
			w->status = d ? RA_WATCH_BAD_POINTER : RA_WATCH_BAD_BASE;
			return;
		}
		/*
		    A pointer the game stores is a DS address; a pointer RetroAchievements documents
		    is its low 24 bits. Which one this is has to be told to the walker, because both
		    are plausible words and picking wrong yields either an unresolvable chain or --
		    worse -- a resolvable one pointing somewhere meaningless.
		*/
		if (w->flags & RA_WATCH_FLAG_PTR24) {
			addr = ((*(const vu32*)addr) & 0x00FFFFFF) + RA_MAIN_RAM_START + w->offsets[d];
		} else {
			addr = *(const vu32*)addr + w->offsets[d];
		}
	}

	/*
	    An unaligned load does not fault on the ARM9, it silently returns rotated data --
	    which would be worse than failing, because the value would look plausible.
	*/
	if (addr & (u32)(w->size - 1)) {
		w->status = RA_WATCH_MISALIGNED;
		return;
	}
	if (!ra_readable(addr, w->size)) {
		w->status = depth ? RA_WATCH_BAD_TARGET : RA_WATCH_BAD_BASE;
		return;
	}

	w->address = addr;
	w->value   = ra_read(addr, w->size);
	w->status  = RA_WATCH_OK;
}

/*
    Something to look at on hardware before there is a game to watch: a live register
    read directly, and a climbing counter reached through two indirections. Between them
    they exercise every success path in ra_watch_eval().

    The chain walks cells in the *snapshot*, not here. A pointer the walker follows has
    to be a main RAM address, and this binary is at 0x0374xxxx -- relaxing that check to
    accommodate our own cells would weaken it for the game addresses it exists to guard.
    So the cells live in the snapshot, and this is the only side that can fill them in,
    since it is the side handed the address.
*/
static void ra_install_defaults(raSnapshot* snapshot) {
	u32 offsets[RA_CHAIN_MAX];

	snapshot->selfCell    = (u32)snapshot;
	snapshot->selfCellPtr = (u32)&snapshot->selfCell;

	ra_watch_add(RA_DEFAULT_WATCH_ADDRESS, 2, 0, 0);

	offsets[0] = 0;
	offsets[1] = RA_TICKS_OFFSET;
	ra_watch_add((u32)&snapshot->selfCellPtr, 4, 2, offsets);
}

/*
    Called once per frame from ra_tick() in the cardengine, with a pointer to the
    snapshot. The pointer is passed rather than assumed because the snapshot lives in the
    cardengine's .bss and moves whenever that code changes -- this binary cannot know its
    address at build time, and should not try.

    Runs in the game's VCOUNT interrupt handler, so the same rules apply here as in the
    cardengine: short, and no blocking.
*/
void ra_wram_tick(raSnapshot* snapshot) {
	u8 resolved = 0;
	u8 stage;
	u8 i;

	/*
	    Bring the window up before anything else touches .bss -- including the magic check
	    below, whose variable lives there. ra_startup() zeroes .bss on its first call, so
	    stateMagic is guaranteed to read 0 the first time this line is reached and the
	    watchlist installs itself exactly once.
	*/
	/*
	    The arena stops below the definitions block at the top of the window.

	    **Do not shorten this further.** Taking 512 bytes off it to reserve room underneath broke this
	    binary on hardware -- rcheevos stopped triggering and the unlock ring produced 0xF0000000 --
	    with 11 KB of margin still reported by the fit test, so scarcity was not the reason and the
	    real one is not understood. Anything needing a reservation in this window goes inside the
	    definitions' 32 KB instead; see CARDENGINEI_ARM9_RA_PENDING_LOCATION.
	*/
	stage = ra_startup(__bss_start, __bss_end,
	                   (char*)(CARDENGINEI_ARM9_RA_DEFS_LOCATION));
	snapshot->wramStage = stage;
	snapshot->heapSize  = ra_heap_size();
	snapshot->heapUsed  = ra_heap_used();
	/*
	    _sbrk()'s own view, written on every tick including the failing ones -- which is the
	    point. The previous build reported a 189K arena and a failed 32-byte allocation with
	    no way to tell which of the two was lying.
	*/
	snapshot->heapBreak   = ra_heap_break();
	snapshot->heapTop     = ra_heap_top();
	snapshot->mallocProbe = ra_malloc_probe();
	snapshot->sbrkProbe   = ra_sbrk_probe();
	if (stage < RA_STAGE_ALLOC) {
		/*
		    The allocator did not come up. Stop here rather than run the watchlist anyway:
		    it would work, and it would make a broken heap look like a working binary.
		*/
		return;
	}

	if (stateMagic != RA_WRAM_MAGIC) {
		stateMagic = RA_WRAM_MAGIC;
		frames     = 0;
		watchCount = 0;
		ra_install_defaults(snapshot);
		/*
		    Once per session, and here rather than in ra_startup() because this is the first place
		    that runs after .bss is up and is already the one-time hook. The MPU is the game's and
		    does not change under us; if it did, re-asserting every frame would cost an I-cache
		    invalidate every frame, which is the opposite of the point.
		*/
		mpuBits = ra_icache_claim(CARDENGINEI_ARM9_RA_LOCATION);
	}

	snapshot->raMpuBits = mpuBits;

	frames++;

	for (i = 0; i < watchCount; i++) {
		ra_watch_eval(&watches[i]);
		if (watches[i].status == RA_WATCH_OK) {
			resolved++;
		}

		/* Mirror the first few into the snapshot, which is a hex viewer's worth. */
		if (i < RA_RESULT_MAX) {
			snapshot->results[i].address  = watches[i].address;
			snapshot->results[i].value    = watches[i].value;
			snapshot->results[i].depth    = watches[i].depth;
			snapshot->results[i].size     = watches[i].size;
			snapshot->results[i].status   = watches[i].status;
			snapshot->results[i].reserved = 0;
		}
	}

	snapshot->watchCount = watchCount;
	snapshot->resolved   = resolved;
	snapshot->wramMagic  = RA_WRAM_MAGIC;
	snapshot->wramTicks  = frames;
	snapshot->wramStage  = RA_STAGE_WATCHES;

	/*
	    rcheevos last, and after the watchlist rather than instead of it. The two are
	    independent readers of the same memory: the watchlist is what this project can
	    debug by eye, and keeping it running alongside means a disagreement between them
	    is visible rather than a question of which one to believe.
	*/
	ra_rc_tick(snapshot);

	/* After rcheevos, so its allocations are included rather than measured a frame late. */
	snapshot->heapUsed = ra_heap_used();

	/*
	    The overlay last, and *after* rcheevos rather than before it, which is a small improvement the
	    move paid for. It used to run in the cardengine one call earlier in the frame, so it saw the
	    previous frame's trigger count; from here it sees this frame's, and a notification is raised on
	    the frame the achievement fires rather than the one after.

	    ra_overlay_tick() is handed the count and the pixels and nothing else, so this file stays the only
	    thing here that knows what a snapshot is -- the same arrangement the cardengine had. What has gone
	    is the range check on the strip: it used to be a pointer crossing a binary boundary and now it is
	    this binary's own address, so there is nothing left to distrust.
	*/
	ra_overlay_tick(snapshot->rcTriggered, ra_rc_text());
	{
		extern u32 raOverlayShows, raOverlayDenied, raOverlayEvicted, raOverlayDeniedNoLayer;
		extern u32 raOverlayRedrawn;
		extern u32 raOverlayDispcnt, raOverlayWindow;
		extern u8  raOverlayState;
		extern u8  raOverlaySpriteOam, raOverlaySpriteSlot;

		snapshot->shows          = raOverlayShows;
		snapshot->denied         = raOverlayDenied;
		snapshot->evicted        = raOverlayEvicted;
		snapshot->deniedNoLayer  = raOverlayDeniedNoLayer;
		snapshot->overlayRedrawn = (u16)((raOverlayRedrawn > 0xFFFF) ? 0xFFFF : raOverlayRedrawn);
		{
			extern u16 raOverlaySavedInk, raOverlaySavedShadow, raOverlayBgCnt;

			snapshot->overlaySavedInk    = raOverlaySavedInk;
			snapshot->overlaySavedShadow = raOverlaySavedShadow;
			snapshot->overlayBgCnt       = raOverlayBgCnt;
		}
		{
			extern u8  raOverlayVramCnt[9];
			extern u16 raOverlayVramEverOn;
			int b;

			for (b = 0; b < 9; b++) {
				snapshot->vramCnt[b] = raOverlayVramCnt[b];
			}
			snapshot->vramEverOn = raOverlayVramEverOn;
		}
		snapshot->overlayState   = raOverlayState;
		snapshot->overlayDispcnt = raOverlayDispcnt;
		snapshot->overlayWindow  = raOverlayWindow;
		snapshot->overlayText    = (u32)ra_rc_text();
		snapshot->overlaySpriteOam  = raOverlaySpriteOam;
		snapshot->overlaySpriteSlot = raOverlaySpriteSlot;
	}
}
