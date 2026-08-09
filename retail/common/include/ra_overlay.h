/*
    RetroAchievements support for nds-bootstrap -- on-screen text overlay.

    Draws text over the running game, which is what an unlock notification needs.

    It used to be a feasibility proof: one fixed message, with only the glyphs that message uses,
    because a font and layout code would not fit next to it. That prediction held -- the font is in
    cardenginei_arm9_ra now, exactly as the note here said it would have to be -- but the split went
    the other way round from what was expected. This file did not move. It keeps the whole
    borrow-and-return negotiation with the game, which is the part that was genuinely in doubt and
    expensive to get right, and gives up only the part that knew what letters look like: it is handed
    a finished strip of tiles and blits it.

    This file is part of nds-bootstrap and is licensed under the GPL-3.0,
    the same terms as the rest of the project.
*/

#ifndef RA_OVERLAY_H
#define RA_OVERLAY_H

#include "ra.h"

#if RA_READER_ENABLED

/*
    Per frame, from ra_tick(). Games rewrite DISPCNT and the BG control registers, so holding a
    borrowed layer means re-claiming it on every frame the notification is up.

    `unlocks` is the running count of achievements this session has triggered --
    raSnapshot.rcTriggered -- and the notification is raised when it *goes up*.

    `text` is RA_TEXT_BYTES of 4bpp tiles to blit, or NULL when there is nothing to say. Its range has
    already been checked by the caller; this file does not know or care where WRAM is.

    Both passed in rather than read from the snapshot, so this file stays a leaf: it borrows VRAM and
    hands it back, and it has no business reaching into the reader's state to decide when to do that
    or what to put there.
*/
void ra_overlay_tick(u32 unlocks, const void* text);

#endif /* RA_READER_ENABLED */

#endif /* RA_OVERLAY_H */
