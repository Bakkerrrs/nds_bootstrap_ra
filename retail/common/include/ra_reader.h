/*
    RetroAchievements support for nds-bootstrap -- game RAM reader.

    Runs inside the ARM9 cardengine, which is injected into the game's own
    address space, so reading the game's RAM is just a pointer dereference --
    the same trick the in-game menu's RAM viewer uses.

    This file is part of nds-bootstrap and is licensed under the GPL-3.0,
    the same terms as the rest of the project.
*/

#ifndef RA_READER_H
#define RA_READER_H

#include "ra.h"

#if RA_READER_ENABLED

/*
    Everything this fork does per frame, from the VCOUNT interrupt handler: the
    notification overlay, then the watchlist. Pass ce9->consoleModel; the work is
    skipped on anything but the 3DS family, which is the only target -- see the scope
    note in docs/retroachievements.md.

    Runs in an interrupt handler, so it must stay short and must not block.
*/
void ra_tick(u8 consoleModel);

/*
    Re-resolve and re-read every watch. Called by ra_tick(); separate so the reader
    stays testable on its own.
*/
void ra_reader_tick(void);

/*
    Add a watch, returning its index or -1 if the list is full or the request is
    malformed.

    size must be 1, 2 or 4. depth is the number of pointer indirections to walk
    before the final read, up to RA_CHAIN_MAX; offsets[] supplies the offset added
    after each one and may be NULL when depth is 0.

    Nothing is trusted at add time -- addresses are validated on every tick, not
    here -- because a chain that resolves now may not resolve next frame, and the
    reader has to survive that either way.
*/
int ra_reader_watch_add(u32 base, u8 size, u8 depth, const u32* offsets);

/*
    There is deliberately no ra_reader_watch_clear() yet. It is the obvious
    counterpart to _add and phase 2 will want it, but nothing calls it today and the
    cardengine window has tens of bytes spare, not hundreds -- see the space budget in
    docs/retroachievements.md. It costs about 44 of them, so it waits for its caller.
*/

/*
    The watch results and the reader's own diagnostics, in the cardengine's .bss.

    Exposed directly rather than behind an accessor because there is nothing an
    accessor could add: the buffer is at a fixed link-time address, it is what
    tools/ra_snapshot_addr.sh reports and what the in-game menu's RAM viewer is
    pointed at, and in a module with a few hundred bytes of headroom a function that
    only returns &buffer is not worth its own code.

    Do not read it without checking magic[] first -- .bss here is never zeroed.
*/
extern raSnapshot raSnapshotBuffer;

#endif /* RA_READER_ENABLED */

#endif /* RA_READER_H */
