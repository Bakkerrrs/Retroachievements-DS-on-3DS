/*
    Does a real RetroAchievements set fit the cardengine's arena?

    Step 3d ends with 56 published definitions from GameID 14856 staged in DSi WRAM
    (docs/logs/ra_definitions-14856.txt, written by the console itself). Step 4 runs them, and
    the first thing it will do is hand every one of them to rc_runtime_activate_achievement()
    inside a 158,644-byte arena. Whether that succeeds is a number, and a number is worth
    measuring before a flash cycle rather than after one.

    Two things are checked, and they fail for different reasons:

      **Every definition parses.** This is the real check on step 3d's scanner. A scanner that
      dropped a character, decoded an escape wrongly, or spliced two definitions together would
      still produce plausible text -- and rcheevos is the only thing that can say the text is
      *valid*. 56 of 56 parsing is what makes the streaming extraction trustworthy rather than
      merely well-tested against fixtures I wrote.

      **The set fits the arena.** With margin, and the margin is printed rather than asserted at
      some arbitrary threshold, because it is a property of the set rather than of the code.

    Why a third binary rather than a section of one of the other two
    ---------------------------------------------------------------
    The counting wrappers below *replace* malloc, realloc, calloc and free for the whole link.
    tools/ra_reader_test.c deliberately does the opposite -- RA_ALLOC_NO_LIBC_NAMES exists there
    precisely so the cardengine's allocator does not end up underneath printf -- so this cannot
    live in that file without undoing the arrangement it depends on. Splitting is the same
    decision, for the same reason, as tools/ra_launcher_test.c being its own binary.

    Why the wrappers rather than the cardengine's own allocator
    ----------------------------------------------------------
    ra_alloc.c would be the more faithful measurement and it cannot be used here: ra_startup()
    stores its arena bounds through `(u32)bssEnd`, which truncates a 64-bit host pointer. That is
    correct for the DS, where a u32 *is* an address, and it is why ra_reader_test.c has to link at
    a fixed sub-4 GB address. Rather than drag that constraint into this file, the wrappers count
    what rcheevos *asks for* and the arena's 8-byte block header is added back explicitly.

    This file is part of nds-bootstrap and is licensed under the GPL-3.0,
    the same terms as the rest of the project.
*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>

#include "rc_runtime.h"
#include "rc_error.h"
/* For the memref list walk below -- rc_memrefs_t is not in the public headers. */
#include "rcheevos/rc_internal.h"
#include "locations.h"

/*
    The arena: from the cardengine's __bss_end up to the blocks reserved at the top of its window.
    The runner reads the real __bss_end out of the built .elf when there is one, so this fallback is
    only used before a first build -- and it is the value that binary actually had, recorded rather
    than rounded.

    **Keep it in step with the binary.** A stale fallback does not fail, it over-reports the arena by
    however far it has drifted, and only on runs where no .elf exists yet. That is a reading which
    looks like a measurement and is not: comparing one taken from the fallback against one taken from
    a real build reads as a sudden 17 KB regression that never happened. It cost a wrong diagnosis
    written into a commit message before the two were compared properly.
*/
#ifndef RA_WRAM_BSS_END
#define RA_WRAM_BSS_END 0x037559CCuL
#endif

/*
    **This must be whatever ra_startup() is passed as its window top, and nothing else.** It has been
    wrong in both directions already. Pointed below the real top it under-reported by 512 bytes, which
    was merely conservative; pointed above it, after the pending tally moved into the definitions'
    reservation, it over-reported by 32 KB -- which is the dangerous direction, since the check it
    exists to perform is "does the set still fit".

    The definitions block is that top. The pending tally lives inside the definitions' own 32 KB now
    rather than underneath it, precisely so this number does not have to move again: shortening the
    arena is what broke rcheevos on hardware.
*/
#define RA_WRAM_ARENA ((long)(CARDENGINEI_ARM9_RA_DEFS_LOCATION - RA_WRAM_BSS_END))

/* The cardengine's allocator puts an 8-byte header on every block. See raBlock in ra_alloc.c. */
#define RA_WRAM_BLOCK_HEADER 8

/*
    ------------------------------------------------------------------------------------
    Counting wrappers.

    Only active between `counting = 1` and `counting = 0`, so the printf below and the file read
    above do not land in the measurement. glibc's malloc_usable_size() rounds up by a few bytes
    per block, which makes the figure a slight over-estimate -- the right direction for a
    question about whether something fits.
    ------------------------------------------------------------------------------------
*/
extern void* __libc_malloc(size_t n);
extern void* __libc_realloc(void* p, size_t n);
extern void  __libc_free(void* p);

static int  counting;
static long live, peak, blocks, peakBlocks;

void* malloc(size_t n) {
	void* p = __libc_malloc(n);

	if (counting && p) {
		live += (long)malloc_usable_size(p);
		blocks++;
		if (live > peak) {
			peak = live;
		}
		if (blocks > peakBlocks) {
			peakBlocks = blocks;
		}
	}
	return p;
}

void* realloc(void* old, size_t n) {
	const long was = (counting && old) ? (long)malloc_usable_size(old) : 0;
	void*      p   = __libc_realloc(old, n);

	if (counting && p) {
		live += (long)malloc_usable_size(p) - was;
		if (!old) {
			blocks++;
		}
		if (live > peak) {
			peak = live;
		}
	}
	return p;
}

void free(void* p) {
	if (counting && p) {
		live -= (long)malloc_usable_size(p);
		blocks--;
	}
	__libc_free(p);
}

void* calloc(size_t count, size_t size) {
	void* p = malloc(count * size);

	if (p) {
		memset(p, 0, count * size);
	}
	return p;
}

/*
    A counting peek, so the same set that is measured for memory is measured for *traffic*.

    Hardware said the memref pass costs 47 of the 70 available scanlines and that ra_rc_peek() was
    called 237 times in a frame -- roughly 845 ARM9 cycles per read for a translate, a range check
    and a load, which is an order of magnitude more than that code can spend. Reading it did not
    explain it and neither did the instruction cache, which turned out to be on already. So the next
    thing to establish is the denominator: how many reads a real set actually makes, split between
    the memref pass and trigger evaluation, on a set whose hardware cost is also known.

    Returns a value that changes every call. A constant would leave every delta at zero and every
    trigger permanently in the same state, which is not the traffic a running game produces.
*/
static long peeks;
static unsigned peekTick;

static unsigned counting_peek(unsigned address, unsigned numBytes, void* ud) {
	(void)ud;
	peeks++;
	return (address + peekTick) & ((numBytes >= 4) ? 0xFFFFFFFFu : ((1u << (numBytes * 8)) - 1));
}

static int failures;

#define CHECK(cond) do { \
	if (cond) { \
		printf("  ok    %s\n", #cond); \
	} else { \
		printf("  FAIL  %s\n", #cond); \
		failures++; \
	} \
} while (0)

/* The set the console fetched, as it wrote it. */
#define RA_SET_PATH "docs/logs/ra_definitions-14856.txt"

/*
    56, and it is asserted rather than merely printed. The count is what says the *stream* was
    read whole: 59 MemAddr keys came back, 3 were unofficial, and a scanner that lost one to a
    packet boundary would produce 55 valid definitions and no complaint anywhere.
*/
#define RA_SET_DEFINITIONS 56

static void discarding_event_handler(const rc_runtime_event_t* e) {
	(void)e;
}

int main(void) {
	static char  text[CARDENGINEI_ARM9_RA_DEFS_MAX];
	static char* lines[RA_SET_DEFINITIONS + 32];
	rc_runtime_t runtime;
	FILE*        f = fopen(RA_SET_PATH, "rb");
	long         length;
	long         afterInit;
	char*        line;
	int          count = 0, activated = 0, refused = 0, i;

	printf("a real achievement set, in the arena the cardengine actually has\n");

	if (!f) {
		printf("  %s missing -- cannot run\n", RA_SET_PATH);
		return 2;
	}
	length = (long)fread(text, 1, sizeof(text) - 1, f);
	text[length] = 0;
	fclose(f);

	/*
	    The staged block is bounded by the same constant the launcher writes against, so the file
	    fitting it is part of the check rather than an assumption about how it was produced.
	*/
	printf("\nthe set as the console staged it\n");
	CHECK(length > 0);
	CHECK(length < (long)(CARDENGINEI_ARM9_RA_DEFS_MAX - CARDENGINEI_ARM9_RA_DEFS_HEADER));
	printf("        %ld bytes of %d in the block (%.1f%% full)\n",
	       length, CARDENGINEI_ARM9_RA_DEFS_MAX - CARDENGINEI_ARM9_RA_DEFS_HEADER - 1,
	       100.0 * length / (CARDENGINEI_ARM9_RA_DEFS_MAX - CARDENGINEI_ARM9_RA_DEFS_HEADER - 1));

	for (line = strtok(text, "\n");
	     line && count < (int)(sizeof(lines) / sizeof(lines[0]));
	     line = strtok(NULL, "\n")) {
		lines[count++] = line;
	}
	CHECK(count == RA_SET_DEFINITIONS);

	/*
	    128 is RA_DEFS_MAX_LINES, in the cardengine, which is what will do this splitting for
	    real. A set larger than that would be silently truncated at the reader rather than here.
	*/
	CHECK(count <= 128);

	counting = 1;
	rc_runtime_init(&runtime);
	afterInit = live;
	counting  = 0;
	CHECK(runtime.memrefs != NULL);

	/*
	    Activated through the runtime rather than sized with rc_trigger_size(), and the difference
	    is 33 KB on this set -- enough to turn "fits with 30 K to spare" into "does not fit".
	    rc_trigger_size() sizes a *standalone* trigger, so it counts memrefs in every definition;
	    rc_runtime_activate_achievement() passes the runtime's communal pool as existing_memrefs
	    and each trigger only pays for what is new. The path that runs on hardware is the runtime
	    one, so it is the one measured.
	*/
	printf("\nevery definition the server sent parses\n");
	for (i = 0; i < count; i++) {
		int result;

		counting = 1;
		result   = rc_runtime_activate_achievement(&runtime, i + 1, lines[i], NULL, 0);
		counting = 0;

		if (result == RC_OK) {
			activated++;
		} else {
			refused++;
			printf("  FAIL  line %d: %d (%s), %u bytes of memaddr\n",
			       i + 1, result, rc_error_str(result), (unsigned)strlen(lines[i]));
			failures++;
		}
	}
	CHECK(activated == RA_SET_DEFINITIONS);
	CHECK(refused == 0);

	/*
	    What one frame of this set costs in reads, which is the denominator the hardware timing had
	    to be divided by and did not have. Measured on the same runtime the arena figures come from.
	*/
	printf("\nand what one frame of it asks of memory\n");
	{
		long memrefPeeks, framePeeks;

		peeks = 0;
		peekTick = 1;
		rc_update_memref_values(runtime.memrefs, counting_peek, NULL);
		memrefPeeks = peeks;

		peeks = 0;
		peekTick = 2;
		rc_runtime_do_frame(&runtime, discarding_event_handler, counting_peek, NULL, NULL);
		framePeeks = peeks;

		/*
		    And how many entries the loop actually walks, which is the assumption the whole cycle
		    arithmetic rested on and which nothing had checked. rc_update_memref_values() steps over
		    every memref in the list and only calls peek for those whose value has a type -- so the
		    iteration count and the read count are not the same number, and if they differ by an
		    order of magnitude then 845 cycles per *read* is really a much smaller figure per *entry*.
		*/
		{
			const rc_memref_list_t* list = &runtime.memrefs->memrefs;
			long entries = 0, typed = 0, modified = 0;

			do {
				unsigned k;

				for (k = 0; k < list->count; k++) {
					entries++;
					if (list->items[k].value.type != RC_VALUE_TYPE_NONE) {
						typed++;
					}
				}
				list = list->next;
			} while (list);

			{
				const rc_modified_memref_list_t* mlist = &runtime.memrefs->modified_memrefs;

				do {
					modified += mlist->count;
					mlist = mlist->next;
				} while (mlist);
			}

			printf("        memref list    %ld entries, %ld typed, %ld modified\n",
			       entries, typed, modified);
		}
		printf("        memref pass    %ld reads\n", memrefPeeks);
		printf("        whole frame    %ld reads (%ld in trigger evaluation)\n",
		       framePeeks, framePeeks - memrefPeeks);
		/*
		    The memref pass is where nearly all of it should be: rcheevos updates every memref once
		    per frame and conditions then read the updated values rather than memory. A trigger-side
		    count of the same order would mean the definitions are full of AddAddress indirection,
		    which is read at evaluation time and would put the cost somewhere else entirely.
		*/
		CHECK(memrefPeeks > 0);
		CHECK(framePeeks >= memrefPeeks);
	}

	printf("\nand the whole set fits the arena\n");
	{
		const long used = peak + peakBlocks * RA_WRAM_BLOCK_HEADER;

		printf("        arena          %ld bytes (0x%08lX to 0x%08X)\n",
		       RA_WRAM_ARENA, (unsigned long)RA_WRAM_BSS_END,
		       CARDENGINEI_ARM9_RA_DEFS_LOCATION);
		printf("        rc_runtime_init %ld bytes\n", afterInit);
		printf("        peak            %ld bytes in %ld blocks, +%ld of headers\n",
		       peak, peakBlocks, peakBlocks * RA_WRAM_BLOCK_HEADER);
		printf("        used            %ld of %ld  (%.1f%%)\n",
		       used, RA_WRAM_ARENA, 100.0 * used / RA_WRAM_ARENA);
		printf("        margin          %ld bytes\n", RA_WRAM_ARENA - used);

		CHECK(used < RA_WRAM_ARENA);
		/*
		    A margin rather than a bare fit, because the arena's floor moves: every byte the
		    cardengine's .bss gains comes straight out of this. 8 K is roughly 5% and is the line
		    at which the next change to that binary needs to be looked at rather than assumed.
		*/
		CHECK(RA_WRAM_ARENA - used > 8192);
	}

	printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
	       failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
