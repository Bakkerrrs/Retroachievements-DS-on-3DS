#!/bin/sh
#
# Build the ROM that gets handed to a person, and refuse to produce one that is not what it claims.
#
# This exists because of a specific failure, and the failure is worth writing down because nothing in
# the build would have caught it. The pre-delivery checklist in docs/retroachievements.md says a ROM
# must compile at RA_LAUNCHER_WIFI=0 and =2. Running both `make`s satisfies that -- and leaves
# retail/bin/nds-bootstrap.nds holding whichever one ran *last*. Copying it after a `=0` build hands
# over a launcher with no network: no RetroAchievements ladder, nothing staged for the cardengine, and
# cardenginei_arm9_ra falling back to its built-in self-test, which fires one unlock with the synthetic
# id 0xF0000000 seconds into the game.
#
# That was mistaken for an intermittent memory fault for several days. The arena, the pending block's
# address and the launcher's statics were all blamed in turn, on evidence that was really "did the =2
# build happen to run last this time". The logs agreed with each other and not with the console,
# because a boot with no network never rewrites ra_wifi_launcher.log -- so the log being read was the
# previous, working boot's.
#
# So: the order is fixed here rather than remembered, and the result is verified against the binary
# rather than against intent. `=0` is built first because it is only a compile check; `=2` is built
# last because it is the deliverable.
#
set -e

cd "$(dirname "$0")/.."

OUT=${1:-nds-bootstrap-release.nds}

echo "checking RA_LAUNCHER_WIFI=0 still compiles"
make RA_LAUNCHER_WIFI=0 >/dev/null 2>&1

echo "building the deliverable at RA_LAUNCHER_WIFI=2"
make RA_LAUNCHER_WIFI=2 >/dev/null 2>&1

# The proof, taken from the ELF the ROM was built from. A =2 launcher links dsiwifi into its ARM7 and
# a =0 launcher does not, so this separates them without trusting which make ran last.
WIFI=$(arm-none-eabi-nm retail/arm7/nds-bootstrap.elf 2>/dev/null | grep -ci 'dsiwifi\|wifi_' || true)
if [ "$WIFI" -lt 20 ]; then
	echo "REFUSING: the ARM7 has $WIFI wifi symbols, so this is not a network build" >&2
	exit 1
fi

# The in-game menu's one unchecked mirror, taken from the link rather than from intent.
#
# struct IgmText's size is written by hand a second time as a `.space` in
# arm9_igm/source/card_engine_header.s, and nothing links the two files. When they disagreed --
# 0xF40 there against 0xF68 in the header -- every symbol after the text block landed short of where
# the C side computes it, and opening the menu jumped into the middle of the text. Contra 4 died on
# the frame the menu opened, and the cause took a session to find because the build was clean.
#
# sharedAddr sits immediately after the block, 16-byte aligned, so its offset from igmText *is*
# IGM_TEXT_SIZE_ALIGNED. If the two numbers disagree this is where it shows, before a card does.
echo "the in-game menu's text mirror"
IGM_ELF=retail/cardenginei/arm9_igm/build/cardenginei_arm9_igm.elf
if [ -f "$IGM_ELF" ]; then
	IGM_BASE=$(arm-none-eabi-nm "$IGM_ELF" | awk '$3 == "igmText" { print $1 }')
	IGM_SHARED=$(arm-none-eabi-nm "$IGM_ELF" | awk '$3 == "sharedAddr" { print $1 }')
	IGM_SIZE=$(printf '%d' $((0x$IGM_SHARED - 0x$IGM_BASE)))
	IGM_WANT=$(grep -o 'sizeof(IgmText) == 0x[0-9A-Fa-f]*' retail/common/include/igm_text.h \
	           | grep -o '0x[0-9A-Fa-f]*')
	IGM_WANT=$(printf '%d' $(( ( $IGM_WANT + 0xF ) & ~0xF )))
	if [ "$IGM_SIZE" -ne "$IGM_WANT" ]; then
		echo "REFUSING: igmText occupies $IGM_SIZE bytes, the header says $IGM_WANT" >&2
		echo "  the .space in arm9_igm/source/card_engine_header.s does not match struct IgmText" >&2
		exit 1
	fi
	echo "  igmText $IGM_SIZE bytes, header and .space agree"
else
	echo "REFUSING: $IGM_ELF is missing, so the mirror cannot be checked" >&2
	exit 1
fi

echo "host suite"
# The exit status, not a grep for a word.
#
# This used to be `| grep -q "PASSED"`, and that is not a check: the suite runs several binaries and
# prints a verdict for each, so one FAILED among three PASSED still matched. It let a build ship with
# a stale `sizeof(raSnapshot)` pin -- a field had been appended and the assertion still named the old
# size -- and said nothing, which is the one thing this script exists not to do.
#
# ra_reader_test.sh already exits with the right status. Output is held back so a passing run stays
# quiet, and printed when it matters.
if ! SUITE=$(bash tools/ra_reader_test.sh 2>&1); then
	printf '%s\n' "$SUITE" | grep -E "FAIL|rror" >&2
	echo "REFUSING: the host suite did not pass" >&2
	exit 1
fi
# ...and a belt for the braces: a binary that reports FAILED without a non-zero exit is still a fail.
if printf '%s\n' "$SUITE" | grep -q "FAILED"; then
	printf '%s\n' "$SUITE" | grep -E "FAIL" >&2
	echo "REFUSING: the host suite reported a failure" >&2
	exit 1
fi

echo "snapshot address and free bytes"
bash tools/ra_snapshot_addr.sh

cp retail/bin/nds-bootstrap.nds "$OUT"
echo
echo "$OUT"
echo "  commit  $(git rev-parse --short HEAD)$(git diff --quiet || echo ' (WORKING TREE DIRTY)')"
echo "  wifi    $WIFI symbols -- RA_LAUNCHER_WIFI=2"
echo "  md5     $(md5sum "$OUT" | cut -d' ' -f1)"
