/*
    Step 3b: the RetroAchievements hash for the ROM the launcher is about to boot.

    Without this the launcher cannot ask the server *which* achievement set to fetch, so it is
    the last part of step 3 that can be built without handling a password -- and the only one
    whose correctness is a single number, checkable against RetroAchievements' own site rather
    than by a play session.

    ## What it hashes is rcheevos' decision, not ours

    `rc_hash_nintendo_ds()` in the vendored submodule defines it: 352 bytes of header, the ARM9
    code, the ARM7 code, and 2,560 bytes of the icon block, zero-padded if the ROM is short --
    plus a 512-byte offset when a SuperCard header is present. Getting that wrong is the one
    bug in step 3 that nothing local could catch: a well-formed MD5 over almost-the-right
    bytes, which the server simply does not recognise, indistinguishable from a game with no
    set.

    So it is not reimplemented from a description. `tools/ra_reader_test.c` compiles the real
    `rc_hash_nintendo_ds()` and asserts this function agrees with it, byte for byte, on real
    `.nds` files -- every time the host test runs. rcheevos stays the definition; this is an
    implementation of it that a divergence would fail loudly.

    ## Why it is not simply a call to their function

    Because their function cannot run here, and that was a measurement rather than a guess.
    `rc_hash_nintendo_ds()` allocates `max(0xA00, arm9_size, arm7_size)` in **one block** so it
    can hash each region from memory. nds-bootstrap's own `.nds` needs 353,164 bytes of that;
    a real DS game is commonly larger still. The launcher's ARM9 has about **352 K of heap
    before lwip is initialised and 191 K after** -- so it does not fit even in the best case,
    and reordering the probe would not have saved it.

    This streams instead: the same four ranges through one 1 K static buffer, so the cost is
    fixed and tiny no matter how large the ROM is. That is also the right shape for the
    shipped feature, where the launcher will do this on every boot with a game's worth of
    other work already in the heap.

    Only rcheevos' `md5.c` is linked -- the hashing, not the file plumbing.

    This file is part of nds-bootstrap and is licensed under the GPL-3.0,
    the same terms as the rest of the project.
*/

#include "ra_wifi.h"

#if RA_LAUNCHER_WIFI

#include <stdio.h>
#include <string.h>

#include "rhash/md5.h"

/* Fixed, and small on purpose: see the note above about the 353 K allocation. */
#define RA_HASH_CHUNK 1024

#define RA_HASH_HEADER_BYTES 0x160   /* the part of the header that is hashed */
#define RA_HASH_ICON_BYTES   0xA00   /* icon and labels, zero-padded if the ROM is short */

static char raHashError[96];

static u32 raHashU32(const unsigned char* p) {
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

/*
    Hash `bytes` from `offset`, through the fixed buffer.

    Returns how many bytes were actually hashed, which is what lets the icon block be
    zero-padded the way rcheevos pads it: homebrew ROMs routinely stop before the icon is
    complete, and a short read there is normal rather than an error.
*/
static u32 raHashRange(FILE* file, md5_state_t* md5, u32 offset, u32 bytes) {
	static unsigned char chunk[RA_HASH_CHUNK];
	u32                  done = 0;

	if (fseek(file, (long)offset, SEEK_SET) != 0) {
		return 0;
	}
	while (done < bytes) {
		u32    want = bytes - done;
		size_t got;

		if (want > sizeof(chunk)) {
			want = sizeof(chunk);
		}
		got = fread(chunk, 1, want, file);
		if (got == 0) {
			break;
		}
		md5_append(md5, (const md5_byte_t*)chunk, (int)got);
		done += (u32)got;
	}
	return done;
}

static void raHashPadZeros(md5_state_t* md5, u32 bytes) {
	static const unsigned char zeros[64] = { 0 };

	while (bytes > 0) {
		u32 n = bytes > sizeof(zeros) ? (u32)sizeof(zeros) : bytes;

		md5_append(md5, (const md5_byte_t*)zeros, (int)n);
		bytes -= n;
	}
}

bool raHashRom(const char* path, char hash[33], raHashInfo* info) {
	static unsigned char header[512];
	md5_state_t          md5;
	md5_byte_t           digest[16];
	FILE*                file;
	u32                  offset = 0;
	u32                  arm9Addr, arm7Addr, iconAddr, iconRead;
	int                  i;

	memset(info, 0, sizeof(*info));
	raHashError[0] = 0;
	hash[0]        = 0;

	file = fopen(path, "rb");
	if (!file) {
		snprintf(raHashError, sizeof(raHashError), "cannot open the ROM");
		return false;
	}

	if (fread(header, 1, sizeof(header), file) != sizeof(header)) {
		snprintf(raHashError, sizeof(raHashError), "ROM shorter than a header");
		fclose(file);
		return false;
	}

	/*
	    A SuperCard-patched dump carries its own 512-byte header in front of the real one.
	    rcheevos detects it by the branch at +0 and the signature at +0xB0 and skips it; the
	    same two tests, because a dump like that must hash as the ROM it wraps.
	*/
	if (header[0] == 0x2E && header[1] == 0x00 && header[2] == 0x00 && header[3] == 0xEA
	 && header[0xB0] == 0x44 && header[0xB1] == 0x46 && header[0xB2] == 0x96
	 && header[0xB3] == 0x00) {
		offset = 512;
		if (fseek(file, (long)offset, SEEK_SET) != 0
		 || fread(header, 1, sizeof(header), file) != sizeof(header)) {
			snprintf(raHashError, sizeof(raHashError), "SuperCard header but no ROM behind it");
			fclose(file);
			return false;
		}
	}

	arm9Addr       = raHashU32(&header[0x20]);
	info->arm9Size = raHashU32(&header[0x2C]);
	arm7Addr       = raHashU32(&header[0x30]);
	info->arm7Size = raHashU32(&header[0x3C]);
	iconAddr       = raHashU32(&header[0x68]);

	/*
	    rcheevos' own sanity check, kept: code blocks are under a megabyte each in practice, so
	    a pair totalling more than 16 MB means this is not a DS ROM. Worth having because the
	    launcher will one day run this on whatever file it was pointed at.
	*/
	if (info->arm9Size > 16u * 1024 * 1024
	 || info->arm7Size > 16u * 1024 * 1024 - info->arm9Size) {
		snprintf(raHashError, sizeof(raHashError), "arm9 %lu + arm7 %lu: not a DS ROM",
		         (unsigned long)info->arm9Size, (unsigned long)info->arm7Size);
		fclose(file);
		return false;
	}

	/*
	    What their implementation would have had to allocate, reported for the record rather
	    than used. This is the number that says why this function streams.
	*/
	info->bufferBytes = RA_HASH_ICON_BYTES;
	if (info->arm9Size > info->bufferBytes) {
		info->bufferBytes = info->arm9Size;
	}
	if (info->arm7Size > info->bufferBytes) {
		info->bufferBytes = info->arm7Size;
	}

	md5_init(&md5);
	md5_append(&md5, (const md5_byte_t*)header, RA_HASH_HEADER_BYTES);

	if (raHashRange(file, &md5, offset + arm9Addr, info->arm9Size) != info->arm9Size) {
		snprintf(raHashError, sizeof(raHashError), "ROM ends inside the arm9 code");
		fclose(file);
		return false;
	}
	if (raHashRange(file, &md5, offset + arm7Addr, info->arm7Size) != info->arm7Size) {
		snprintf(raHashError, sizeof(raHashError), "ROM ends inside the arm7 code");
		fclose(file);
		return false;
	}

	/*
	    A short icon block is not an error. Some homebrew provides no complete one and nothing
	    after it, and rcheevos zero-fills the remainder -- so a ROM like that still has one
	    stable hash rather than none.
	*/
	iconRead = raHashRange(file, &md5, offset + iconAddr, RA_HASH_ICON_BYTES);
	if (iconRead < RA_HASH_ICON_BYTES) {
		raHashPadZeros(&md5, RA_HASH_ICON_BYTES - iconRead);
	}

	fclose(file);
	md5_finish(&md5, digest);

	for (i = 0; i < 16; i++) {
		static const char hex[] = "0123456789abcdef";

		hash[i * 2]     = hex[digest[i] >> 4];
		hash[i * 2 + 1] = hex[digest[i] & 0xF];
	}
	hash[32] = 0;
	return true;
}

const char* raHashLastError(void) {
	return raHashError[0] ? raHashError : "none";
}

#endif /* RA_LAUNCHER_WIFI */
