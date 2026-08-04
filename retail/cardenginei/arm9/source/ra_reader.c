/*
    RetroAchievements support for nds-bootstrap -- game RAM reader (ARM9).

    Copies a window of the game's RAM into a snapshot buffer once per frame, and
    reports the memory map and MPU layout so a home can be found for the RA state
    that will never fit in the cardengine's 12K window. No RetroAchievements logic
    and no networking yet.

    This file is part of nds-bootstrap and is licensed under the GPL-3.0,
    the same terms as the rest of the project.
*/

#include "ra_reader.h"

#if RA_READER_ENABLED

/*
    Lives in the cardengine's .bss, which is inside the region reserved for the
    cardengine, so the game can never scribble on it. The bootloader copies only
    the loaded image, which ends where .bss begins, and there is no crt0 to zero
    what follows -- so every field is garbage until claim() says otherwise.

    Aligned to 16 because the in-game menu's RAM viewer can only jump to
    addresses that are a multiple of 0x10.
*/
/* Global so the link map names it; tools/ra_snapshot_addr.sh reads it from there. */
raSnapshot raSnapshotBuffer __attribute__((aligned(16)));
#define snapshot raSnapshotBuffer

static u32 watchAddress = RA_DEFAULT_WATCH_ADDRESS;
static u32 watchLength  = RA_SNAPSHOT_WINDOW;

/*
    Initialise the buffer the first time anything touches it. Every entry point
    calls this, so the snapshot becomes readable as soon as any part of the
    cardengine runs -- not only once the per-frame hook is working.
*/
static void claim(void) {
	if (snapshot.magic[0] == RA_SNAPSHOT_MAGIC0
	 && snapshot.magic[1] == RA_SNAPSHOT_MAGIC1
	 && snapshot.magic[2] == RA_SNAPSHOT_MAGIC2
	 && snapshot.magic[3] == RA_SNAPSHOT_MAGIC3) {
		return;
	}

	snapshot.magic[0] = RA_SNAPSHOT_MAGIC0;
	snapshot.magic[1] = RA_SNAPSHOT_MAGIC1;
	snapshot.magic[2] = RA_SNAPSHOT_MAGIC2;
	snapshot.magic[3] = RA_SNAPSHOT_MAGIC3;

	snapshot.ticks      = 0;
	snapshot.srcAddress = 0;
	snapshot.length     = 0;
	snapshot.repairs    = 0;

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

	claim();
	snapshot.ticks++;
	{
		extern u32 raOverlayRepairs;
		snapshot.repairs = raOverlayRepairs;
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
