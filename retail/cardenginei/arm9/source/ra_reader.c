/*
    RetroAchievements support for nds-bootstrap -- game RAM reader (ARM9).

    Phase 0: copy a fixed window of the game's RAM into a snapshot buffer once
    per frame so it can be observed with the in-game menu's RAM viewer. No
    RetroAchievements logic and no networking yet.

    This file is part of nds-bootstrap and is licensed under the GPL-3.0,
    the same terms as the rest of the project.
*/

#include "ra_reader.h"

#if RA_READER_ENABLED

/*
    Lives in the cardengine's .bss, which is inside the region reserved for the
    cardengine, so the game can never scribble on it. .bss is not zeroed for an
    injected binary, so treat every field as garbage until the magic says
    otherwise.
*/
static raSnapshot snapshot;

static u32 watchAddress = RA_DEFAULT_WATCH_ADDRESS;
static u32 watchLength  = RA_SNAPSHOT_WINDOW;

static bool headerValid(void) {
	return snapshot.magic[0] == RA_SNAPSHOT_MAGIC0
	    && snapshot.magic[1] == RA_SNAPSHOT_MAGIC1
	    && snapshot.magic[2] == RA_SNAPSHOT_MAGIC2
	    && snapshot.magic[3] == RA_SNAPSHOT_MAGIC3;
}

void ra_reader_set_window(u32 address, u32 length) {
	if (length > RA_SNAPSHOT_WINDOW) {
		length = RA_SNAPSHOT_WINDOW;
	}
	watchAddress = address & ~3;  /* keep the word copy below aligned */
	watchLength  = length & ~3;
}

const raSnapshot* ra_reader_snapshot(void) {
	return &snapshot;
}

void ra_reader_tick(void) {
	u32 length = watchLength;
	u32 i;

	if (!headerValid()) {
		/* First tick since the cardengine was loaded: claim the buffer. */
		snapshot.magic[0] = RA_SNAPSHOT_MAGIC0;
		snapshot.magic[1] = RA_SNAPSHOT_MAGIC1;
		snapshot.magic[2] = RA_SNAPSHOT_MAGIC2;
		snapshot.magic[3] = RA_SNAPSHOT_MAGIC3;
		snapshot.frame = 0;
	} else {
		snapshot.frame++;
	}

	snapshot.srcAddress = watchAddress;
	snapshot.length     = length;

	/*
	    Word-at-a-time so the copy stays cheap in the interrupt handler. The
	    source is volatile because it is the running game's memory and changes
	    underneath us; the window is deliberately not latched atomically, which
	    is fine for a per-frame sample.
	*/
	{
		const vu32* src = (const vu32*)watchAddress;
		u32* dst = (u32*)snapshot.data;
		for (i = 0; i < (length >> 2); i++) {
			dst[i] = src[i];
		}
	}
}

#endif /* RA_READER_ENABLED */
