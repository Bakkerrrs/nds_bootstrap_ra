/*
    RetroAchievements support for nds-bootstrap -- on-screen notification.

    An unlock that nobody can see is not worth much, so this is the feedback
    channel -- and it doubles as the debug channel, which beats reading hex out of
    the in-game menu's RAM viewer.

    Drawing text over a running game means taking display resources the game owns:
    a spare VRAM bank, a spare background layer, palette entries. Whether any are
    spare depends entirely on the game, so that comes after measuring. What works
    everywhere, and needs nothing from the game, is the master brightness
    register -- so the first notification is a flash.

    This file is part of nds-bootstrap and is licensed under the GPL-3.0,
    the same terms as the rest of the project.
*/

#ifndef RA_TOAST_H
#define RA_TOAST_H

#include "ra.h"

#if RA_READER_ENABLED

/*
    Flash both screens for `frames` frames. Ignored if a flash is already running,
    so a burst of unlocks cannot stack into a strobe.
*/
void ra_toast_flash(u32 frames);

/* Advance the flash. Call once per frame from the VCOUNT handler. */
void ra_toast_tick(void);

#endif /* RA_READER_ENABLED */

#endif /* RA_TOAST_H */
