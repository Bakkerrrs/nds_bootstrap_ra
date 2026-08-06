/*
    RetroAchievements support for nds-bootstrap -- the ARM9 code that lives outside
    the cardengine.

    The ARM9 cardengine is linked into a fixed 12K window with tens of bytes to spare,
    and everything still to be built is larger than that by orders of magnitude:
    `rcheevos` measures 48K linked, a font the overlay could use is a few K, and the
    overlay's own rewrite needs room to be correct rather than clever. So this binary
    exists, in DSi WRAM, following the colour LUT's pattern -- which is the one thing in
    this project already proven to execute from that region on hardware.

    Right now it does one thing: prove the chain. Built, packed into the .nds, loaded,
    copied into WRAM, recognised as code, called, and reporting back through the
    snapshot the cardengine already exposes. Nothing here is useful yet; what matters is
    that every link can be seen separately in a RAM viewer, so when one of them fails it
    is obvious which. That is the order phases 0 and 0.5 were built in, for the same
    reason: a flash cycle is expensive and a silent failure costs several.

    This file is part of nds-bootstrap and is licensed under the GPL-3.0,
    the same terms as the rest of the project.
*/

#include "ra.h"

/*
    Called once per frame from ra_tick() in the cardengine, with a pointer to the
    snapshot buffer. The pointer is passed rather than assumed because the snapshot
    lives in the cardengine's .bss and moves whenever that code changes -- this binary
    cannot know its address at build time, and should not try.

    Runs in the game's VCOUNT interrupt handler, so the same rules apply here as in the
    reader: short, and no blocking.
*/
void ra_wram_tick(raSnapshot* snapshot) {
	/*
	    Claimed the same way the snapshot itself is, and for the same reason: nothing in
	    this window is initialised. The binary is copied in by the bootloader, so .text
	    and .data arrive intact, but .bss does not, and neither does whatever the
	    previous occupant of this memory left behind. The magic is what makes the counter
	    below trustworthy rather than a number that happened to be there.
	*/
	if (snapshot->wramMagic != RA_WRAM_MAGIC) {
		snapshot->wramMagic = RA_WRAM_MAGIC;
		snapshot->wramTicks = 0;
	}

	snapshot->wramTicks++;
}
