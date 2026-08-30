#!/bin/sh
# Print the address of the RA snapshot buffer in each cardengine variant that
# contains the reader, and how much room is left in that variant's window.
#
# The buffer lives in the cardengine's .bss, so its address moves whenever the
# code around it changes. Re-run this after every build and use the address for
# the variant your game actually loads -- for a plain retail DS game on a DSi or
# 3DS that is cardenginei_arm9.
#
# Then open the in-game menu's RAM viewer, navigate to that address, and look for
# the ASCII bytes "RA2S" (52 41 32 53) followed by a frame counter that climbs
# once per frame.
#
# The digit is the layout version, so a stale address from an older build announces
# itself as "RA1S" or "RA0S" rather than being read as though the offsets still meant
# what this document says.
#
# The "free" column is __vram_top - __bss_end: the gap between the end of .bss and the
# end of the cardengine's window, which is all the room it has left. It is what limits
# RA_WATCH_MAX in retail/common/include/ra.h. The linker scripts assert it cannot go
# negative, so this reports a margin rather than a warning -- were it negative, the
# build would already have failed.
#
# Note it is measured against __vram_top and *not* against __sp_usr. The __sp_* symbols
# come from the stock NDS linker script this was derived from, and nothing in the
# cardengine installs them -- injected code runs on the game's stack. Measuring against
# __sp_usr understates the free space by 0x260 bytes, which is how this document's
# predecessor came to report 840 bytes free when there were 1,448.

set -e
cd "$(dirname "$0")/.."

found=0
for map in $(find retail hb -name '*.map' 2>/dev/null | sort); do
	# Read the symbol itself: ra_reader.o also holds other statics in .bss, so the
	# start of its .bss contribution is not necessarily the snapshot.
	addr=$(awk '$2 == "raSnapshotBuffer" { print $1 }' "$map" | head -n1)
	[ -n "$addr" ] || continue
	size=$(awk '/^ \.bss +0x[0-9a-f]+ +0x[0-9a-f]+ ra_reader\.o$/ { print $3 }' "$map")
	bssEnd=$(awk '$2 == "__bss_end" { print $1 }' "$map" | head -n1)
	vramTop=$(awk '$2 == "__vram_top" { print $1 }' "$map" | head -n1)
	free='?'
	if [ -n "$bssEnd" ] && [ -n "$vramTop" ]; then
		# Shell arithmetic, not awk: the addresses come out of the map as 0x...., and
		# POSIX awk reads a leading 0x as plain 0, which silently reports free=0.
		free=$(( vramTop - bssEnd ))
	fi
	printf '%-34s snapshot=%s size=%s free=%s\n' \
		"$(basename "$map" .map)" "$addr" "$size" "$free"
	found=$((found + 1))
done

if [ "$found" -eq 0 ]; then
	echo "No .map files with ra_reader.o found -- build first, and check that" >&2
	echo "RA_READER_ENABLED is 1 in retail/common/include/ra.h." >&2
	exit 1
fi
