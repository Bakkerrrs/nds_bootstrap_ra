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
    Capture the current watch window. Call once per frame; it runs in the VCOUNT
    interrupt handler, so it must stay short and must not block.
*/
void ra_reader_tick(void);

/*
    Point the reader at a different range. length is clamped to
    RA_SNAPSHOT_WINDOW. Safe to call while ticking: the change is picked up on
    the next tick.
*/
void ra_reader_set_window(u32 address, u32 length);

/* The snapshot buffer. Never NULL. */
const raSnapshot* ra_reader_snapshot(void);

#endif /* RA_READER_ENABLED */

#endif /* RA_READER_H */
