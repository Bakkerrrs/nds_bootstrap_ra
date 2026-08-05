/*
    RetroAchievements support for nds-bootstrap -- game RAM reader (ARM9).

    Re-resolves and re-reads a watchlist once per frame from inside the game's own
    VCOUNT interrupt handler. No RetroAchievements logic and no networking yet.

    Two things shape everything here.

    The first is that a pointer read out of a running game is not trustworthy. It
    is whatever happens to be in that word this frame, including garbage while the
    game tears down one scene and builds the next. Dereferencing it blind takes a
    Data Abort in the middle of the game's interrupt handler, which is a crash. So
    every address is range-checked immediately before it is used, and a chain that
    does not resolve is reported as not resolved rather than chased.

    The second is that the resolved address may not be cached. That is the entire
    reason pointer chains exist: the structure moves, and an address that was
    correct last frame points into the middle of something else this frame. Walking
    the chain again every tick is not wasted work, it is the work.

    This file is part of nds-bootstrap and is licensed under the GPL-3.0,
    the same terms as the rest of the project.
*/

#include "ra_reader.h"

#if RA_READER_ENABLED

/*
    Size, not speed, is the scarce resource in this file. The cardengine's window has
    a few hundred bytes left and the whole watchlist -- code, descriptors and results
    -- has to fit in them, while the cost being traded away is a few scanlines out of
    262, which snapshot.linesMax now measures rather than leaving to be assumed.

    So this one file is built for size while the rest of the cardengine stays at the
    -O2 the project uses. At -O2 the inliner duplicates ra_watch_eval() into the tick
    loop and ra_reader_watch_add() into each of claim()'s three default installs,
    which costs ~190 bytes for work that happens at most once per frame; the two
    noinline markers below are there for that reason and not for correctness.
*/
#pragma GCC optimize("Os")

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

/* The ARM9's current scanline. The only clock here the game does not own. */
#define RA_VCOUNT (*(vu16*)0x04000006)

/* 192 visible lines plus 71 of vblank; VCOUNT runs 0..262 and restarts. */
#define RA_SCANLINES_PER_FRAME 263

/* Main RAM as the game sees it, mirrors included. */
#define RA_MAIN_RAM_START 0x02000000
#define RA_MAIN_RAM_END   0x03000000

/*
    Self-test cells for the chain walker. raSelfCell holds the snapshot's address and
    raSelfCellPtr holds raSelfCell's, so a one-step and a two-step chain both land on
    snapshot.ticks, which climbs every frame.

    They exist because there is otherwise no way to show on hardware that the walker
    resolves and reads live memory: doing that with a game address means already
    knowing a game address, which is what phase 2 is for.

    Filled in by claim() rather than statically initialised. A static initialiser
    would work on the target -- taking an address into a u32 is a link-time
    relocation there -- but it is not a constant expression on a host with wider
    pointers, and tools/ra_reader_test.c compiles this file as it stands.
*/
static u32 raSelfCell;
static u32 raSelfCellPtr;

/*
    Offset of snapshot.ticks -- what the self-test chains resolve to. It has to be a
    number because it is a chain offset, not a field reference, so it is pinned to the
    struct here: reorder raSnapshot without updating this and the build fails rather
    than the self-test quietly reading the wrong field and still looking plausible.
*/
#define RA_TICKS_OFFSET 4
typedef char raTicksOffsetCheck[
	(RA_TICKS_OFFSET == __builtin_offsetof(raSnapshot, ticks)) ? 1 : -1];

/*
    Where a pointer read out of the game is allowed to point. A game pointer is
    always a main RAM address, so one that is not has been read out of a structure
    that no longer exists -- which is the normal case between scenes, not an
    exceptional one.

    Written as `addr <= END - len` rather than `addr + len <= END` so a base near the
    top of the address space cannot wrap past the check.
*/
static bool ra_in_main_ram(u32 addr, u32 len) {
	return addr >= RA_MAIN_RAM_START && addr <= RA_MAIN_RAM_END - len;
}

/*
    Where the final read is allowed to land. Main RAM is where every real watch
    lands; I/O is reachable only because the diagnostic watch reads a display
    register, and nothing else is listed -- an address outside these is reported as
    unreadable rather than dereferenced to find out what happens.
*/
static bool ra_readable(u32 addr, u32 len) {
	return ra_in_main_ram(addr, len)
	    || (addr >= 0x04000000 && addr <= 0x04001100 - len);
}

static u32 ra_read(u32 addr, u8 size) {
	if (size == 1) {
		return *(const vu8*)addr;
	}
	if (size == 2) {
		return *(const vu16*)addr;
	}
	return *(const vu32*)addr;
}

/*
    Something to look at on hardware before there is a game to watch: a live
    register read directly, and the same climbing counter reached through one and
    through two indirections. Between them they exercise every path in
    ra_watch_eval() except the failure ones.

    Called from claim(), which sets the magic before getting here -- so the
    ra_reader_watch_add() calls below find an already-claimed buffer and do not
    recurse back into it.
*/
static void ra_install_defaults(void) {
	u32 offsets[RA_CHAIN_MAX];

	raSelfCell    = (u32)&raSnapshotBuffer;
	raSelfCellPtr = (u32)&raSelfCell;

	ra_reader_watch_add(RA_DEFAULT_WATCH_ADDRESS, 2, 0, 0);

	offsets[0] = RA_TICKS_OFFSET;
	offsets[1] = 0;
	ra_reader_watch_add((u32)&raSelfCell, 4, 1, offsets);

	offsets[0] = 0;
	offsets[1] = RA_TICKS_OFFSET;
	ra_reader_watch_add((u32)&raSelfCellPtr, 4, 2, offsets);
}

/*
    Initialise the buffer the first time anything touches it. Every entry point
    calls this, so the snapshot becomes readable as soon as any part of the
    cardengine runs -- not only once the per-frame hook is working.
*/
static void claim(void) {
	int i;

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
	snapshot.watchCount = 0;
	snapshot.resolved   = 0;
	snapshot.linesLast  = 0;
	snapshot.linesMax   = 0;

	for (i = 0; i < RA_WATCH_MAX; i++) {
		snapshot.watches[i].status = RA_WATCH_UNUSED;
	}

	ra_install_defaults();
}

/*
    Validated before claim() rather than after, so nothing has to be kept live
    across the call. In a module this size that is worth a few instructions, and it
    costs nothing: the checks do not touch the buffer.

    The slots are walked with a pointer rather than an index because a raWatch is 24
    bytes -- indexing would put a multiply in the loop.
*/
__attribute__((noinline))
int ra_reader_watch_add(u32 base, u8 size, u8 depth, const u32* offsets) {
	raWatch* w;
	int i;
	u8 d;

	if ((size != 1 && size != 2 && size != 4)
	 || depth > RA_CHAIN_MAX
	 || (depth && !offsets)) {
		return -1;
	}

	claim();

	for (i = 0, w = snapshot.watches; i < RA_WATCH_MAX; i++, w++) {
		if (w->status != RA_WATCH_UNUSED) {
			continue;
		}

		w->base  = base;
		w->size  = size;
		w->depth = depth;
		for (d = 0; d < RA_CHAIN_MAX; d++) {
			w->offsets[d] = 0;
		}
		for (d = 0; d < depth; d++) {
			w->offsets[d] = offsets[d];
		}
		w->address = 0;
		w->value   = 0;
		w->status  = RA_WATCH_PENDING;
		snapshot.watchCount++;
		return i;
	}

	return -1;
}

/*
    Walk one watch's chain from its base and read the value at the end. Every
    address is checked in the moment it is about to be used; nothing is carried
    over from the previous tick, because nothing from the previous tick is still
    known to be true.
*/
__attribute__((noinline))
static void ra_watch_eval(raWatch* w) {
	/*
	    Clamped rather than trusted. ra_reader_watch_add() validates depth and claim()
	    marks every slot unused before any of them is evaluated, so this cannot fire
	    today -- but the magic that makes the buffer trustworthy is only four bytes, and
	    if garbage .bss ever matched it, an unclamped depth would read past offsets[]
	    and walk arbitrarily many indirections inside an interrupt handler.
	*/
	const u8 depth = (w->depth > RA_CHAIN_MAX) ? RA_CHAIN_MAX : w->depth;
	u32 addr = w->base;
	u8 d;

	w->address = 0;

	for (d = 0; d < depth; d++) {
		if ((addr & 3) || !ra_in_main_ram(addr, 4)) {
			w->status = d ? RA_WATCH_BAD_POINTER : RA_WATCH_BAD_BASE;
			return;
		}
		addr = *(const vu32*)addr + w->offsets[d];
	}

	/*
	    An unaligned load does not fault on the ARM9, it silently returns rotated
	    data -- which would be worse than failing, because the value would look
	    plausible.
	*/
	if (addr & (u32)(w->size - 1)) {
		w->status = RA_WATCH_MISALIGNED;
		return;
	}
	if (!ra_readable(addr, w->size)) {
		w->status = depth ? RA_WATCH_BAD_TARGET : RA_WATCH_BAD_BASE;
		return;
	}

	w->address = addr;
	w->value   = ra_read(addr, w->size);
	w->status  = RA_WATCH_OK;
}

void ra_reader_tick(void) {
	const u16 startLine = RA_VCOUNT;
	u8 resolved = 0;
	raWatch* w;
	u32 lines;
	int i;

	claim();
	snapshot.ticks++;
	{
		extern u32 raOverlayShows, raOverlayDenied, raOverlayEvicted;
		snapshot.shows   = raOverlayShows;
		snapshot.denied  = raOverlayDenied;
		snapshot.evicted = raOverlayEvicted;
	}

	for (i = 0, w = snapshot.watches; i < RA_WATCH_MAX; i++, w++) {
		if (w->status == RA_WATCH_UNUSED) {
			continue;
		}
		ra_watch_eval(w);
		if (w->status == RA_WATCH_OK) {
			resolved++;
		}
	}
	snapshot.resolved = resolved;

	/*
	    Open question #2 was what the per-frame cost is; this measures it instead of
	    arguing about it. The handler fires at line 0, but VCOUNT is read rather than
	    assumed, so a tick that spans the end of the frame has to be handled: the
	    counter runs 0..262 and then restarts, so a smaller end line means it wrapped
	    and the elapsed count is the rest of the frame plus the end line. Reported in a
	    u8, so a pathological tick is pinned at 255 rather than truncated to something
	    that reads as cheap.
	*/
	{
		const u16 endLine = RA_VCOUNT;
		lines = (endLine >= startLine)
			? (u32)(endLine - startLine)
			: (u32)(RA_SCANLINES_PER_FRAME - startLine + endLine);
		if (lines > 255) {
			lines = 255;
		}
	}
	snapshot.linesLast = (u8)lines;
	if (lines > snapshot.linesMax) {
		snapshot.linesMax = (u8)lines;
	}
}

#endif /* RA_READER_ENABLED */
