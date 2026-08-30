/*
    Print the byte offsets of loadCrt0's fields, so they can be compared against the assembler's.

    This exists because of a bug it would have caught and did: `loadCrt0` in
    retail/common/include/load_crt0.h is mirrored **label for label** by load_crt0.s in *both*
    bootloaders, which read it positionally -- the launcher writes the C struct and the bootloader
    reads the assembly labels. Nothing in either language checks the other. A field that lands at a
    different offset on the two sides does not fail to build and does not fail loudly: it hands the
    ARM7 a value read out of the wrong four bytes, which for a cluster means writing into whatever
    file that number happens to name.

    Adding step 3b's field, `.align 4` was written where `.align 2` was meant. In GNU as for ARM the
    argument is a power of two, so that asked for sixteen-byte alignment and put the field at 336
    where C puts it at 324. The build was clean. This is what said otherwise.

    Deliberately not a test with its own verdict: the comparison needs `nm` output from a built
    object, so tools/ra_reader_test.sh does the diffing -- the same shape as the arena floor it
    already reads out of the cardengine's .elf.

    This file is part of nds-bootstrap and is licensed under the GPL-3.0,
    the same terms as the rest of the project.
*/

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "load_crt0.h"

int main(void) {
	/*
	    One line per field, `name offset`, ready to be joined against nm. Not every field -- the ones
	    that matter are the last few, where an appended field lands, plus two earlier ones as controls
	    so a wholesale shift is distinguishable from a padding mistake at the end.
	*/
	printf("srParamsFileCluster %zu\n", offsetof(loadCrt0, srParamsFileCluster));
	printf("sharedFontCluster %zu\n",   offsetof(loadCrt0, sharedFontCluster));
	printf("bannerSavPath %zu\n",       offsetof(loadCrt0, bannerSavPath));
	printf("version %zu\n",             offsetof(loadCrt0, version));
	printf("raUnlocksCluster %zu\n",    offsetof(loadCrt0, raUnlocksCluster));
	return 0;
}
