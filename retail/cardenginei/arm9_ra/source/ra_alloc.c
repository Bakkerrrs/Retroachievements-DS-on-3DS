/*
    The allocator rcheevos runs on, written here rather than taken from newlib.

    newlib's malloc does not work in this window, and that is a measured fact rather than a
    suspicion. Two hardware readings established it: _sbrk() hands out the arena correctly
    -- the snapshot showed it returning exactly the predicted base address, with heapSize
    matching the prediction to the byte -- and malloc(1024) refused anyway, without ever
    calling _sbrk() successfully. Whatever newlib is unhappy about is inside newlib.

    Rather than keep guessing at it, this replaces it. That is the better engineering call
    for four reasons that hold independently of the bug:

      1. It is testable. newlib's allocator cannot be exercised by tools/ra_reader_test.sh,
         because on the host glibc's malloc is what runs -- which is exactly why the failure
         cost two flash cycles to characterise. This file is tested on the host like
         everything else in this binary.

      2. It is deterministic. This runs inside the game's VCOUNT interrupt handler, where a
         variable-time path is a dropped frame. dlmalloc trims, consolidates and remaps on
         its own schedule; the code below does the same work every time.

      3. It removes ~20K of dead weight. newlib's allocator drags in the reentrancy
         structure and, through it, stdio -- which is where the printf engine in this
         binary's image comes from. See docs/retroachievements.md.

      4. It needs no crt0. newlib's allocator keeps initialised state in .data and expects
         a startup it does not get here. This needs one call with two pointers.

    What rcheevos actually asks for is modest: allocations at activation time, roughly 1K
    per achievement, and none at all per frame. So a first-fit list with forward
    coalescing is not a compromise -- the O(n) walk happens while loading achievements and
    never inside the per-frame path.

    This file is part of nds-bootstrap and is licensed under the GPL-3.0,
    the same terms as the rest of the project.
*/

#include <stdlib.h>
#include <string.h>

#include "ra.h"

/*
    Every block is preceded by this, and every payload is 8-byte aligned because the header
    is 8 bytes and the arena base is aligned to 8 by ra_startup(). rcheevos stores 64-bit
    values in its typed-value union, so 8 is the alignment that has to hold, not 4.

    `size` is the payload in bytes, always a multiple of 8. `state` is a whole word rather
    than a bit stolen from the size, because a stolen bit makes every read of the size a
    mask and this arena has 190K to spare -- and because a distinctive value in there is
    what lets a corrupted block be recognised as corrupt rather than followed.
*/
#define RA_BLOCK_FREE 0x45455246  /* 'FREE' byte-wise */
#define RA_BLOCK_USED 0x44455355  /* 'USED' byte-wise */

typedef struct raBlock {
	u32 size;
	u32 state;
} raBlock;

static char* arenaStart;
static char* arenaEnd;
static u32   allocFailures;

#define RA_ALLOC_ALIGN 8
#define RA_ALIGN_UP(n) (((n) + (RA_ALLOC_ALIGN - 1)) & ~(u32)(RA_ALLOC_ALIGN - 1))

/*
    Lay the whole arena out as one free block. Called once, from ra_startup(), before
    anything is allowed to allocate -- a null arenaStart is what makes an allocation before
    that a refusal rather than a wild write.
*/
void ra_alloc_init(char* start, char* end) {
	raBlock* first;

	arenaStart    = 0;
	arenaEnd      = 0;
	allocFailures = 0;

	/* Aligned inward from both ends, so every header and payload in the arena is aligned. */
	start = (char*)RA_ALIGN_UP((u32)start);
	end   = (char*)((u32)end & ~(u32)(RA_ALLOC_ALIGN - 1));

	if (end <= start || (u32)(end - start) <= sizeof(raBlock)) {
		return;
	}

	first        = (raBlock*)start;
	first->size  = (u32)(end - start) - sizeof(raBlock);
	first->state = RA_BLOCK_FREE;

	arenaStart = start;
	arenaEnd   = end;
}

/* The block after this one, or 0 if this is the last. */
static raBlock* ra_block_next(raBlock* b) {
	char* next = (char*)b + sizeof(raBlock) + b->size;
	return (next < arenaEnd) ? (raBlock*)next : 0;
}

/*
    Merge every run of adjacent free blocks into one. Done on free rather than on allocate
    so that a long-lived allocation pattern cannot leave the arena as a list of unusable
    fragments -- which for rcheevos matters when a set of achievements is replaced.
*/
static void ra_coalesce(void) {
	raBlock* b = (raBlock*)arenaStart;

	while (b) {
		if (b->state == RA_BLOCK_FREE) {
			raBlock* next = ra_block_next(b);
			while (next && next->state == RA_BLOCK_FREE) {
				b->size += sizeof(raBlock) + next->size;
				next = ra_block_next(b);
			}
		}
		b = ra_block_next(b);
	}
}

/*
    The real entry points carry ra_ names, and the libc names below are one-line wrappers
    over them.

    That split exists for the host test. On the target, `malloc` has to be *the* malloc,
    because that is the name rcheevos calls. On the host it must not be: the test links
    against glibc, and replacing its allocator underneath printf and the test harness itself
    would turn any bug here into an unreadable crash somewhere else. So the test defines
    RA_ALLOC_NO_LIBC_NAMES and exercises these directly.
*/
void* ra_alloc_malloc(size_t want) {
	u32      need;
	raBlock* b;

	if (arenaStart == 0) {
		allocFailures++;
		return 0;
	}
	/*
	    Zero is a legal request. Returning a real one-unit block rather than 0 keeps
	    callers that check for null from treating it as failure.
	*/
	need = RA_ALIGN_UP((u32)(want ? want : 1));
	/* A request big enough to wrap the alignment is a refusal, not an allocation. */
	if (need < want) {
		allocFailures++;
		return 0;
	}

	for (b = (raBlock*)arenaStart; b; b = ra_block_next(b)) {
		if (b->state != RA_BLOCK_FREE || b->size < need) {
			continue;
		}
		/*
		    Split only when the remainder can hold a header and a minimum payload.
		    Otherwise hand over the whole block: a few wasted bytes cost less than a
		    zero-payload block that can never be used and has to be walked past forever.
		*/
		if (b->size >= need + sizeof(raBlock) + RA_ALLOC_ALIGN) {
			raBlock* rest = (raBlock*)((char*)b + sizeof(raBlock) + need);
			rest->size  = b->size - need - sizeof(raBlock);
			rest->state = RA_BLOCK_FREE;
			b->size     = need;
		}
		b->state = RA_BLOCK_USED;
		return (char*)b + sizeof(raBlock);
	}

	allocFailures++;
	return 0;
}

void ra_alloc_free(void* p) {
	raBlock* b;

	if (p == 0 || arenaStart == 0) {
		return;
	}
	/*
	    Outside the arena, or not where a header should be. Ignored rather than trusted:
	    this is the one entry point a caller can get wrong in a way that would corrupt
	    everything else, and there is no cost to checking.
	*/
	if ((char*)p <= arenaStart || (char*)p >= arenaEnd) {
		return;
	}
	b = (raBlock*)((char*)p - sizeof(raBlock));
	if (b->state != RA_BLOCK_USED) {
		return;   /* double free, or a pointer that never came from here */
	}
	b->state = RA_BLOCK_FREE;
	ra_coalesce();
}

void* ra_alloc_realloc(void* p, size_t want) {
	raBlock* b;
	void*    fresh;
	u32      copy;

	if (p == 0) {
		return ra_alloc_malloc(want);
	}
	if (want == 0) {
		ra_alloc_free(p);
		return 0;
	}
	if ((char*)p <= arenaStart || (char*)p >= arenaEnd) {
		return 0;
	}
	b = (raBlock*)((char*)p - sizeof(raBlock));
	if (b->state != RA_BLOCK_USED) {
		return 0;
	}
	/*
	    Already big enough. Kept in place rather than shrunk: splitting here would mean
	    coalescing on a path that is not a free, and rcheevos only ever grows.
	*/
	if (b->size >= RA_ALIGN_UP((u32)want)) {
		return p;
	}

	fresh = ra_alloc_malloc(want);
	if (fresh == 0) {
		return 0;   /* the original is deliberately left intact */
	}
	copy = b->size;
	if (copy > (u32)want) {
		copy = (u32)want;
	}
	memcpy(fresh, p, copy);
	ra_alloc_free(p);
	return fresh;
}

void* ra_alloc_calloc(size_t count, size_t size) {
	void* p;
	u32   total = (u32)count * (u32)size;

	/* Refuse an overflowing product rather than allocate less than was asked for. */
	if (count != 0 && total / (u32)count != (u32)size) {
		allocFailures++;
		return 0;
	}
	p = ra_alloc_malloc(total);
	if (p) {
		memset(p, 0, total);
	}
	return p;
}

/* Reported through the snapshot, so the arena is visible rather than inferred. */

u32 ra_alloc_size(void) {
	return arenaStart ? (u32)(arenaEnd - arenaStart) : 0;
}

u32 ra_alloc_used(void) {
	raBlock* b;
	u32      used = 0;

	if (arenaStart == 0) {
		return 0;
	}
	for (b = (raBlock*)arenaStart; b; b = ra_block_next(b)) {
		if (b->state == RA_BLOCK_USED) {
			used += (u32)sizeof(raBlock) + b->size;
		}
	}
	return used;
}

/*
    The largest single allocation that would still succeed. Worth reporting next to `used`
    because they answer different questions: a large `free` total with a small largest block
    is fragmentation, and that is the failure mode this allocator could plausibly have.
*/
u32 ra_alloc_largest(void) {
	raBlock* b;
	u32      largest = 0;

	if (arenaStart == 0) {
		return 0;
	}
	for (b = (raBlock*)arenaStart; b; b = ra_block_next(b)) {
		if (b->state == RA_BLOCK_FREE && b->size > largest) {
			largest = b->size;
		}
	}
	return largest;
}

u32 ra_alloc_failures(void) {
	return allocFailures;
}

/*
    The libc names, which is what rcheevos and newlib's headers expect. Compiled out for the
    host test -- see the note above ra_alloc_malloc().
*/
#ifndef RA_ALLOC_NO_LIBC_NAMES

void* malloc(size_t want) {
	return ra_alloc_malloc(want);
}

void free(void* p) {
	ra_alloc_free(p);
}

void* realloc(void* p, size_t want) {
	return ra_alloc_realloc(p, want);
}

void* calloc(size_t count, size_t size) {
	return ra_alloc_calloc(count, size);
}

#endif
