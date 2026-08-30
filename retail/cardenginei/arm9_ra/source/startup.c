/*
    Bringing cardenginei_arm9_ra up far enough to run a C library.

    This binary is copied into DSi WRAM by the bootloader and jumped into. There is no
    crt0, because there is nothing here to run one: the loaded image arrives, and that is
    the whole of the startup this window gets. `.text`, `.rodata` and `.data` are inside
    the image and therefore correct. `.bss` is not in the image, so it arrives as whatever
    the previous occupant of this memory left behind -- and the bootloader's copy actively
    writes staging garbage over it, since it copies a fixed length rather than the exact
    image size.

    Everything written for this window so far coped with that by guarding on a magic and
    initialising by hand. `rcheevos` will not: it calls `malloc`, and newlib's allocator
    keeps state in `.bss` and assumes, like every other C library, that it starts zeroed.
    Handing it garbage would not fail cleanly, it would corrupt a heap.

    So this file is the crt0 the window does not have. It zeroes `.bss` exactly once per
    boot, then hands newlib a heap over what is left of the 256K window. The staging is
    reported through the snapshot as it goes, so a failure names its own stage rather than
    showing up as a game that stopped working.

    This file is part of nds-bootstrap and is licensed under the GPL-3.0,
    the same terms as the rest of the project.
*/

#include <stdlib.h>

#include "ra.h"

/*
    retail/cardenginei/arm9_ra/source/ra_alloc.c -- the allocator this window runs on, ours
    rather than newlib's. See the comment at the top of that file for why.
*/
extern void  ra_alloc_init(char* start, char* end);
extern void* ra_alloc_malloc(size_t want);
extern void  ra_alloc_free(void* p);
extern u32  ra_alloc_size(void);
extern u32  ra_alloc_used(void);
extern u32  ra_alloc_largest(void);
extern u32  ra_alloc_failures(void);

/*
    "Have we run yet" deliberately lives in .data rather than .bss, and the non-zero
    initialiser is what puts it there.

    .data is inside the loaded image, so the bootloader's copy sets it correctly on every
    boot -- which makes it a *reliable* flag rather than one that has to be distinguished
    from garbage. A .bss flag would also work, but only if written after the zeroing below
    clears it, and that is a subtle ordering dependency in code that runs once per boot
    inside an interrupt handler. Nobody would notice it breaking.

    It also means no magic has to be guessed at here: unlike every other claim in this
    project, this one is not a bet that garbage will not coincide.
*/
#define RA_STARTUP_FRESH 0x48534552  /* 'RESH' byte-wise -- the image's initial value */
#define RA_STARTUP_DONE  0x31424152  /* 'RAB1' */

static u32 startupState = RA_STARTUP_FRESH;

/*
    The stage the one and only run actually reached, remembered so repeat calls report it.

    This used to return RA_STAGE_ALLOC unconditionally once startupState was set, which was
    a bug and an expensive one: the flag is set *before* the allocator probe, so a probe
    that failed on the first frame was reported as a working heap on every frame after it.
    The snapshot showed wramStage 04 -- "the watchlist is installed and evaluating", which
    was true -- while the heap underneath was dead, and rcheevos then failed to allocate
    32 bytes out of 189K for no visible reason.

    In .data alongside startupState, for the same reason: it is written once per boot and
    read on every call, so it must not depend on the .bss zeroing having happened.
*/
static u8 startupStage = RA_STAGE_NONE;

/*
    In .bss, and therefore garbage until the zeroing below has run. Set afterwards, never
    before -- the zeroing would clear them.
*/
static char* heapBreak;
static char* heapTop;
static char* heapBase;

/* What the two probes below returned, kept so the snapshot can show them. */
static u32 mallocProbeResult;
static u32 sbrkProbeResult;

/*
    Refuses everything, on purpose.

    This used to hand out the arena, back when newlib's malloc was the allocator. It is not
    anymore -- ra_alloc.c owns that memory in one piece -- and two allocators sharing one
    range is how you get a corruption that only shows up under load. So the arena has
    exactly one owner and this says no.

    It cannot simply be deleted. newlib's snprintf is still linked (reached statically from
    rcheevos' rich presence, see docs/retroachievements.md), and through it newlib's own
    _malloc_r, which references _sbrk_r and therefore this. Nothing reachable calls that
    path, but the link needs the symbol -- and if some future change ever does reach it, a
    refusal is the outcome to want.
*/
void* _sbrk(int incr) {
	(void)incr;
	return (void*)-1;
}

u8 ra_startup(char* bssStart, char* bssEnd, char* windowTop) {
	if (startupState == RA_STARTUP_DONE) {
		return startupStage;
	}
	startupState = RA_STARTUP_DONE;

	{
		u32* p   = (u32*)bssStart;
		u32* end = (u32*)bssEnd;
		while (p < end) {
			*p++ = 0;
		}
	}

	/*
	    After the zeroing, never before: these three live in what was just cleared.

	    The base is rounded up to 8, because newlib's allocator wants 8-byte alignment and
	    __bss_end is only guaranteed to be 4. Left unaligned it still works -- dlmalloc
	    notices and corrects by asking _sbrk() for the difference -- but it costs an extra
	    MORECORE call on the first allocation and leaves the arena permanently offset by
	    four bytes, which is confusing to read in a snapshot for no benefit.
	*/
	heapBase  = (char*)(((u32)bssEnd + 7) & ~(u32)7);
	heapBreak = heapBase;
	heapTop   = windowTop;

	/* Hand the whole arena to our allocator. Nothing may allocate before this line. */
	ra_alloc_init(heapBase, heapTop);
	startupStage = RA_STAGE_HEAP;

	/*
	    Prove the allocator rather than assume it. This is the first time anything in this
	    project has called into a C library from this window, and the failure mode if
	    newlib's state were still garbage is a silently corrupted heap -- which would
	    surface much later as `rcheevos` misbehaving for no visible reason.

	    Allocated, written across its whole length, read back at both ends, and freed. Not
	    a proof of much, but it is the difference between "malloc returned non-null" and
	    "the memory it returned is real".
	*/
	{
		volatile unsigned char* block = (volatile unsigned char*)ra_alloc_malloc(1024);
		u32 i;
		bool ok = (block != 0);

		mallocProbeResult = (u32)block;

		if (ok) {
			for (i = 0; i < 1024; i++) {
				block[i] = (unsigned char)(i & 0xFF);
			}
			ok = (block[0] == 0 && block[1023] == (unsigned char)(1023 & 0xFF));
			ra_alloc_free((void*)block);
		}
		if (!ok) {
			/*
			    Record the arena's own view before giving up, so the failure names itself.
			    A largest-free-block of zero means ra_alloc_init() never took the arena;
			    anything else means it did and the request was still refused, which would
			    be a bug in the allocator rather than in the window.

			    Done here rather than in the caller because the caller stops as soon as it
			    sees a failed stage, and this has to be recorded before that.
			*/
			sbrkProbeResult = ra_alloc_largest();
			return startupStage;   /* RA_STAGE_HEAP: arena taken, allocation refused */
		}
	}

	startupStage = RA_STAGE_ALLOC;
	return startupStage;
}

/*
    _sbrk()'s own two numbers, and the two probe results, reported so the arena can be told
    apart from the allocator sitting on top of it.

    This exists because of a reading that could not be diagnosed: rcheevos failed to
    allocate 32 bytes while heapSize said 189K was free, and nothing in the snapshot could
    say whether the arena was wrong or newlib was refusing for its own reasons.
*/
u32 ra_heap_break(void) {
	return (u32)heapBreak;
}

u32 ra_heap_top(void) {
	return (u32)heapTop;
}

u32 ra_malloc_probe(void) {
	return mallocProbeResult;
}

u32 ra_sbrk_probe(void) {
	return sbrkProbeResult;
}

/* Reported through the snapshot so the arena is visible rather than inferred. */
/*
    Reported through the snapshot so the arena is visible rather than inferred. Both come
    from the allocator now rather than from _sbrk()'s break: the break no longer moves,
    since ra_alloc_init() takes the arena in one piece.
*/
u32 ra_heap_size(void) {
	return ra_alloc_size();
}

u32 ra_heap_used(void) {
	return ra_alloc_used();
}
