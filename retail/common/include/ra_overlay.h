/*
    RetroAchievements support for nds-bootstrap -- on-screen text overlay.

    Draws text over the running game, which is what an unlock notification needs.
    This is a feasibility proof rather than the finished notification: it shows one
    fixed message, permanently, with only the glyphs that message uses. The real
    thing needs a font and layout code that will not fit in the cardengine's
    remaining ~840 bytes, and belongs in a separate ARM9 binary following the
    colour LUT's pattern. What it proves is the part that was genuinely in doubt --
    that a background layer can be taken from a running game and composited over
    it without disturbing the game.

    This file is part of nds-bootstrap and is licensed under the GPL-3.0,
    the same terms as the rest of the project.
*/

#ifndef RA_OVERLAY_H
#define RA_OVERLAY_H

#include "ra.h"

#if RA_READER_ENABLED

/*
    Set up the overlay if needed, then reassert its registers. Call once per frame
    from the VCOUNT handler -- games rewrite DISPCNT and the BG control registers,
    so holding the layer means re-claiming it every frame.
*/
/*
    Per frame, from ra_tick(). `unlocks` is the running count of achievements this session has
    triggered -- raSnapshot.rcTriggered -- and the notification is raised when it *goes up*.

    Passed in rather than read from the snapshot so this file stays a leaf: it borrows VRAM and hands
    it back, and it has no business reaching into the reader's state to decide when to do that.
*/
void ra_overlay_tick(u32 unlocks);

#endif /* RA_READER_ENABLED */

#endif /* RA_OVERLAY_H */
