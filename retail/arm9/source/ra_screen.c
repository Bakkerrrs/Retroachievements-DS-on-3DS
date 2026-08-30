/*
    The quiet screen's progress bar, as arithmetic.

    Split out of ra_wifi.c for the reason ra_wifi_verdict.c was: everything else in that file needs
    libnds -- a console to print to, a FIFO, a socket -- and this needs nothing but sniprintf(). So it
    is the half a host can drive, and tools/ra_launcher_test.c drives it.

    That is not ceremony for four lines of division. A progress bar is a thing whose bugs are
    invisible: a full bar beside "95%" gets reported as a fault in the loader, a bar that reaches 90%
    and stops looks like a hang, and neither would ever fail a build. The cells and the percentage
    come from one rounded division here, and both ends of the range are pinned on the host.

    This file is part of nds-bootstrap and is licensed under the GPL-3.0,
    the same terms as the rest of the project.
*/

#include "ra_wifi.h"

#if RA_LAUNCHER_WIFI

#include <stdio.h>


/*
    Where a string of `cells` cells starts if it is to sit in the middle of a `width`-wide console.

    Pure and pinned for the same reason the bar is: a margin that goes negative, or one that lets a
    line reach the final column, is not a cosmetic bug on this console. It is why the first two
    versions of this screen came apart -- see the drawing note in ra_wifi.c.

    Never returns a position from which the string could touch the last column: the caller draws
    inside `width - 1` and the final cell is left alone on purpose.
*/
u32 raWifiCentre(u32 width, u32 cells) {
	if (width == 0) {
		return 0;
	}
	if (cells + 1 >= width) {
		return 0;
	}
	return (width - 1 - cells) / 2;
}

/*
    The character that says "still working", as one cell that changes in place.

    A rotating `- \ | /` was the first version and it reads as noise at this resolution: four glyphs
    of different widths flickering next to a word, which looks like corruption rather than progress.
    This pulses one dot instead -- `.` `o` `O` `o` -- so the thing that changes is the *size* of a
    mark that never moves, which is the shape of a progress animation rather than of a spinning
    stick.

    Symmetric on purpose: the sequence returns through `o` rather than snapping from `O` back to `.`,
    so it breathes instead of ticking.

    Pure and pinned on the host because a wrong mask here shows a space, and a spinner that stops is
    precisely the thing this exists to rule out -- a stalled run would look identical to a waiting one
    again, which is the bug it was added for.
*/
char raWifiSpinFrame(u8 tick) {
	static const char face[4] = { '.', 'o', 'O', 'o' };

	return face[tick & 3];
}

void raWifiBar(char* out, u32 size, u8 step, u8 steps) {
	u32 i;
	u32 filled;
	u32 percent;

	if (size < RA_BAR_MIN) {
		if (size) {
			out[0] = 0;
		}
		return;
	}
	if (steps == 0) {
		steps = 1;
	}
	if (step > steps) {
		step = steps;
	}

	/*
	    Rounded rather than truncated, so the last step reads 100% and not 95%. The bar and the
	    number come from the same division for the same reason -- a full bar beside "95%" is the
	    kind of thing that gets reported as a bug.
	*/
	percent = ((u32)step * 100 + steps / 2) / steps;
	filled  = ((u32)step * RA_BAR_CELLS + steps / 2) / steps;
	if (filled > RA_BAR_CELLS) {
		filled = RA_BAR_CELLS;
	}

	out[0] = '[';
	for (i = 0; i < RA_BAR_CELLS; i++) {
		out[1 + i] = (i < filled) ? '#' : '-';
	}
	out[1 + RA_BAR_CELLS] = ']';
	sniprintf(out + 2 + RA_BAR_CELLS, size - 2 - RA_BAR_CELLS, " %3lu%%", (unsigned long)percent);
}

#endif   /* RA_LAUNCHER_WIFI */
