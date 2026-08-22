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
    How wide a string prints, which is not its length: these lines carry colour, and `\x1b[31m` is
    five bytes that occupy no cell.

    It matters because the quiet screen pads every row to the console's width instead of using the
    erase-to-end-of-line escape -- so padding by strlen() would pad a coloured line five cells short
    per escape and leave the tail of whatever was there before. The same miscount truncated a
    thirty-eight-byte yellow line at thirty-two and cut its closing escape in half, which leaves the
    console yellow for everything printed afterwards.

    Counts an ESC '[' ... final-letter sequence as zero and anything else as one cell. An unterminated
    escape consumes the rest of the string, which is the safe reading: there is nothing after it that
    could be printed.
*/
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
/*
    Where a string of `cells` cells starts if it is to sit in the middle of a `width`-wide console.

    Pure and pinned for the same reason the bar is: a margin that goes negative or that pushes a line
    past the last column is not a cosmetic bug on this console. Writing into the final cell advances
    the cursor past the end, the console wraps to the next row, and every absolute row this code has
    addressed is then one out -- which is what "it comes apart from 40% on" looks like.

    Never returns a position that would let the string touch the last column: the caller draws inside
    `width - 1` and the final cell is left alone on purpose.
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

char raWifiSpinFrame(u8 tick) {
	static const char face[4] = { '.', 'o', 'O', 'o' };

	return face[tick & 3];
}

u32 raWifiVisible(const char* text) {
	u32 cells = 0;

	while (*text) {
		if (text[0] == 0x1B && text[1] == '[') {
			text += 2;
			while (*text && (*text < '@' || *text > '~')) {
				text++;
			}
			if (*text) {
				text++;
			}
			continue;
		}
		cells++;
		text++;
	}
	return cells;
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
