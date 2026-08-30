#!/bin/sh
# Build and run the host-side test for the RA reader's watchlist and chain walker.
#
# Needs only a host compiler -- no devkitARM, no hardware. See the comment at the
# top of ra_reader_test.c for what it covers and why it is worth having.
#
# The two link options are what make it a real test:
#
#   --defsym=__bss_start/__bss_end/__vram_top
#       the cardengine takes the *addresses* of these three to decide what to zero and where
#       its arena goes. They used to be 1-byte dummies inside ra_reader_test.c, which made the
#       arena depend on the linker's ordering of three symbols -- and that ordering changed
#       twice, silently, each time something was added to the link or to the file, giving a
#       negative span and an allocator that scribbled over the whole process. Here they are
#       absolute addresses inside the real WRAM window the test mmaps, so the arena is chosen
#       rather than inherited and nothing added later can move it.
#
#   -no-pie -Wl,-Ttext-segment=0x02100000
#       puts the test's globals, including the reader's own snapshot buffer, inside
#       the 0x02000000-0x03000000 range the reader validates addresses against. The
#       chain self-tests resolve for the same reason they do on hardware rather than
#       because a check was relaxed.
#
# The I/O page at 0x04000000 is mapped by the test itself.
#
# The two -Wno-*-cast flags are for the u32/pointer round trips the reader does on
# purpose: on the DS a u32 *is* an address. They are lossless here only because of the
# link address above, which is why that is not optional.

set -e
cd "$(dirname "$0")/.."

CC=${CC:-cc}
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

mkdir -p "$out/include/nds"
cat > "$out/include/nds/ndstypes.h" <<'EOF'
/* Just enough of libnds' ndstypes.h for the reader to compile on the host. */
#ifndef NDSTYPES_H
#define NDSTYPES_H
#include <stdint.h>
#include <stdbool.h>
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef volatile uint8_t  vu8;
typedef volatile uint16_t vu16;
typedef volatile uint32_t vu32;
#endif
EOF

# rcheevos is compiled in rather than stubbed out, for the same reason cardengine.c is:
# the expensive failures here are a definition that does not parse and an address that
# translates wrongly, and both are pure logic that a host can check. Catching either one
# here costs seconds; catching it on hardware costs a flash cycle and a photograph of a
# hex viewer.
#
# The file list mirrors the whitelist in retail/cardenginei/arm9_ra/Makefile -- the
# runtime, not the whole library. -lm is for fmod(), which rcheevos' modulus operator
# references whether or not any definition uses it.
RC=retail/cardenginei/arm9_ra/rcheevos
if [ ! -f "$RC/include/rc_runtime.h" ]; then
	echo "$RC is empty -- run: git submodule update --init" >&2
	exit 2
fi

rc_runtime_sources=$(ls "$RC"/src/rcheevos/*.c | grep -v 'rc_validate\.c$')

# ra_wifi_verdict.c is linked rather than #included: see the note in ra_reader_test.c about
# __bss_end and __vram_top. Nothing else joins this link -- step 3b's hash check is a separate
# binary below, for the reason documented at the top of tools/ra_launcher_test.c.
#
$CC -std=gnu99 -Wall -Wno-unused-function -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -O1 \
	-DRA_LAUNCHER_WIFI=1 \
	-I"$out/include" -Iretail/common/include -Iretail/cardenginei/arm9_ra/include \
	-I"$RC/include" -I"$RC/src" \
	-no-pie -Wl,-Ttext-segment=0x02100000 \
	-Wl,--defsym=__bss_start=0x03740000 \
	-Wl,--defsym=__bss_end=0x03744000 \
	-Wl,--defsym=__vram_top=0x03780000 \
	tools/ra_reader_test.c \
	$rc_runtime_sources \
	"$RC"/src/rc_util.c "$RC"/src/rc_compat.c "$RC"/src/rc_version.c \
	"$RC"/src/rhash/md5.c \
	retail/arm9/source/ra_wifi_verdict.c \
	retail/cardenginei/arm9_ra/source/ra_text.c \
	retail/cardenginei/arm9_ra/source/ra_overlay.c \
	-lm -o "$out/ra_reader_test"

set +e
"$out/ra_reader_test"
status=$?
set -e

#---------------------------------------------------------------------------------
# The launcher's own pure logic: the ROM hash (3b), the config file and the query encoder (3c).
#
# Its own binary, and the note at the top of tools/ra_launcher_test.c says why -- joining the
# link above moved the allocator's arena and segfaulted the suite. Nothing here needs the fixed
# link address or the mapped pages.
#
# ra_net.c is not compiled here: it is lwip sockets end to end, and the two functions worth
# testing in it -- raNetUrlEncode() and raNetJsonString() -- are string logic with no lwip in
# them, so the test file includes just those. Linking ra_net.c would mean linking lwip.
#
# The RC_HASH_NO_* defines drop rhash's disc, encryption and zip paths, which this needs none of
# and which would otherwise drag in a CD reader and AES.
#
# sniprintf is newlib's integer-only printf, which this codebase uses throughout to keep float
# formatting out of the link. glibc has no such symbol, so the host build maps it to snprintf --
# the same kind of shim as the fabricated ndstypes.h above, and for the same reason: test the
# real source rather than a copy of it.
#---------------------------------------------------------------------------------
$CC -std=gnu99 -Wall -O1 \
	-DRA_LAUNCHER_WIFI=1 -DRC_HASH_NO_DISC -DRC_HASH_NO_ENCRYPTED -DRC_HASH_NO_ZIP \
	-Dsniprintf=snprintf \
	-I"$out/include" -Iretail/common/include -Iretail/cardenginei/arm9_ra/include -I"$RC/include" -I"$RC/src" \
	tools/ra_launcher_test.c \
	retail/arm9/source/ra_hash.c retail/arm9/source/ra_cfg.c \
	retail/arm9/source/ra_screen.c \
	retail/arm9/source/ra_patch.c retail/arm9/source/ra_queue.c \
	"$RC"/src/rhash/md5.c "$RC"/src/rhash/hash.c "$RC"/src/rhash/hash_rom.c \
	"$RC"/src/rc_util.c "$RC"/src/rc_compat.c \
	-o "$out/ra_launcher_test"

set +e
"$out/ra_launcher_test" || status=1
set -e

#---------------------------------------------------------------------------------
# Does a real achievement set fit the cardengine's arena?
#
# A third binary, and for a concrete reason rather than tidiness: this one *replaces* malloc,
# realloc, calloc and free for its whole link, in order to count what rcheevos asks for. The
# first binary above does the opposite on purpose -- RA_ALLOC_NO_LIBC_NAMES keeps the
# cardengine's allocator from ending up underneath printf -- so the two arrangements cannot
# share a link.
#
# The arena's lower bound is the cardengine's __bss_end, which moves with every change to that
# binary, so it is read out of the built .elf when one exists instead of being carried as a
# constant. Without a build the test falls back to the value the measured binary had, and says
# which it used.
#---------------------------------------------------------------------------------
echo
wram_elf=retail/cardenginei/arm9_ra/build/cardenginei_arm9_ra.elf
bss_end_flag=
if [ -f "$wram_elf" ] && command -v arm-none-eabi-nm >/dev/null 2>&1; then
	bss_end=$(arm-none-eabi-nm "$wram_elf" | awk '$3 == "__bss_end" { print $1 }')
	if [ -n "$bss_end" ]; then
		bss_end_flag="-DRA_WRAM_BSS_END=0x${bss_end}uL"
		echo "arena floor from $wram_elf: __bss_end = 0x$bss_end"
	fi
fi
if [ -z "$bss_end_flag" ]; then
	echo "no cardengine .elf built -- using the recorded __bss_end"
fi

$CC -std=gnu99 -Wall -O1 \
	$bss_end_flag \
	-I"$out/include" -Iretail/common/include -Iretail/cardenginei/arm9_ra/include -I"$RC/include" -I"$RC/src" \
	tools/ra_fit_test.c \
	$rc_runtime_sources \
	"$RC"/src/rc_util.c "$RC"/src/rc_compat.c "$RC"/src/rc_version.c \
	"$RC"/src/rhash/md5.c \
	-lm -o "$out/ra_fit_test"

set +e
"$out/ra_fit_test" || status=1
set -e

# The other half of pinning step two's log classifier.
#
# ra_wifi_verdict.c decides how the Atheros chip arrived by matching dsiwifi's printf text,
# because the driver exposes none of those facts any other way. The C test above proves it
# reads the log a real console produced; this proves those strings are still the ones the
# submodule prints. Between them, bumping libs/dsiwifi fails here in seconds instead of
# reporting the wrong world after a play session.
#
# Skipped rather than failed when the submodule is absent: dsiwifi is only needed for the
# step-two diagnostic build, so a clone without it is a normal state to be in, unlike a
# clone without rcheevos.
DSIWIFI=libs/dsiwifi
if [ -f "$DSIWIFI/arm_iop/source/wifi_card.twl.c" ]; then
	echo
	echo "dsiwifi still prints what the classifier looks for"
	for say in \
		'Mfg %08lx' \
		'needs firmware upload' \
		'BMI version:' \
		'Launching!' \
		'ready, handshaking' \
		'fully initialized!' \
		'bad mbox alloc'
	do
		if grep -qF -- "$say" "$DSIWIFI/arm_iop/source/wifi_card.twl.c"; then
			echo "  ok    $say"
		else
			echo "  FAIL  $say -- libs/dsiwifi does not print this any more"
			status=1
		fi
	done
else
	echo
	echo "libs/dsiwifi absent -- skipping the classifier's string pins"
	echo "  (git submodule update --init, if you are building RA_LAUNCHER_WIFI=1)"
fi

#---------------------------------------------------------------------------------
# Does loadCrt0's C layout match the assembly that mirrors it?
#
# retail/common/include/load_crt0.h is read *positionally* by load_crt0.s in both
# bootloaders: the launcher writes the C struct, the bootloader reads the labels.
# Nothing in either language checks the other, and a mismatch does not fail to
# build -- it hands the ARM7 a value out of the wrong four bytes.
#
# It has already happened once. `.align 4` was written where `.align 2` was meant,
# and in GNU as for ARM the argument is a power of two, so step 3b's field landed
# at 336 where C puts it at 324. This is the check that said so.
#---------------------------------------------------------------------------------
echo
echo "loadCrt0's two layouts agree, field by field"
crt0_obj=retail/bootloaderi/build/load_crt0.o
if [ -f "$crt0_obj" ] && command -v arm-none-eabi-nm >/dev/null 2>&1; then
	$CC -std=gnu99 -Wall -O1 -I"$out/include" -Iretail/common/include -Iretail/cardenginei/arm9_ra/include \
		tools/ra_crt0_offsets.c -o "$out/ra_crt0_offsets"
	# A file rather than process substitution, and shell arithmetic rather than awk's
	# strtonum(): this script is #!/bin/sh, and strtonum is a gawk extension that is
	# absent on the awk most systems ship.
	"$out/ra_crt0_offsets" > "$out/crt0_c_offsets"
	arm-none-eabi-nm "$crt0_obj" > "$out/crt0_nm"
	while read -r field c_off; do
		# nm gives the offset within the section in hex; the labels share the struct's
		# base, so the numbers are directly comparable once converted.
		asm_hex=$(awk -v f="$field" '$3 == f { print $1 }' "$out/crt0_nm")
		if [ -z "$asm_hex" ]; then
			echo "  FAIL  $field -- no such label in $crt0_obj"
			status=1
			continue
		fi
		asm_off=$((0x$asm_hex))
		if [ "$asm_off" = "$c_off" ]; then
			echo "  ok    $field at $c_off"
		else
			echo "  FAIL  $field -- C says $c_off, the assembler says $asm_off"
			status=1
		fi
	done < "$out/crt0_c_offsets"
else
	echo "  no bootloaderi build -- skipping (make bootloaderi to enable)"
fi

exit $status
