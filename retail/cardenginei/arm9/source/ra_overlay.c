/*
    RetroAchievements support for nds-bootstrap -- on-screen text overlay (ARM9).

    Composites text over the running game using the sub engine's BG0. Everything
    here is chosen from what the display measurements said this game leaves spare;
    see docs/retroachievements.md. On a game that leaves nothing spare this cannot
    work, which is why the final notification has to detect at runtime rather than
    assume -- but the mechanism is what needed proving first.

    This file is part of nds-bootstrap and is licensed under the GPL-3.0,
    the same terms as the rest of the project.
*/

#include "ra_overlay.h"

#if RA_READER_ENABLED

#define SUB_DISPCNT (*(vu32*)0x04001000)
#define SUB_BG0CNT  (*(vu16*)0x04001008)
#define SUB_BG0HOFS (*(vu16*)0x04001010)
#define SUB_BG0VOFS (*(vu16*)0x04001012)

/* Standard sub-engine BG palette: 16 banks of 16 in 4bpp mode. */
#define SUB_BG_PALETTE ((vu16*)0x05000400)

/*
    Sub BG VRAM is bank C at 0x06200000. The game's maps occupy 0x06200000-
    0x06204FFF and its tiles start at char base block 3 (0x0620C000), leaving
    0x06205000-0x0620BFFF. Screen base block 10 and character base block 2 both sit
    inside that gap.
*/
#define OVERLAY_MAP   ((vu16*)0x06205000)  /* screen base block 10 */
#define OVERLAY_TILES ((vu8*)0x06208000)   /* character base block 2 */

/*
    Priority 0 so it draws above the game's BG1-3, character base block 2, 16
    colours, screen base block 10, 256x256.

    16 colours matters: the game's 256-colour layers read the *extended* palette
    (DISPCNT bit 30 is set) whose bank is disabled, so a 256-colour overlay would
    come out black. A 4bpp layer uses the standard palette and sidesteps that.
*/
#define OVERLAY_BG0CNT ((0 << 0) | (2 << 2) | (10 << 8))

/* Palette bank 15, colour 1. */
#define OVERLAY_PAL_BANK  15
#define OVERLAY_PAL_INDEX (OVERLAY_PAL_BANK * 16 + 1)

#define OVERLAY_ROW 10
#define OVERLAY_COL 10

/*
    "RA UNLOCKED", one 1bpp 8x8 glyph per character in message order. Storing them
    in order rather than as a font means no character lookup and no unused glyphs --
    the difference between fitting in the cardengine and not. Bit 7 is the leftmost
    pixel; a blank entry is the space.
*/
static const u8 glyphs[][8] = {
	{ 0xFC, 0x82, 0x82, 0xFC, 0x88, 0x84, 0x82, 0x00 },  /* R */
	{ 0x38, 0x44, 0x82, 0x82, 0xFE, 0x82, 0x82, 0x00 },  /* A */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  /*   */
	{ 0x82, 0x82, 0x82, 0x82, 0x82, 0x82, 0x7C, 0x00 },  /* U */
	{ 0x82, 0xC2, 0xA2, 0x92, 0x8A, 0x86, 0x82, 0x00 },  /* N */
	{ 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0xFE, 0x00 },  /* L */
	{ 0x7C, 0x82, 0x82, 0x82, 0x82, 0x82, 0x7C, 0x00 },  /* O */
	{ 0x7C, 0x82, 0x80, 0x80, 0x80, 0x82, 0x7C, 0x00 },  /* C */
	{ 0x82, 0x84, 0x88, 0xF0, 0x88, 0x84, 0x82, 0x00 },  /* K */
	{ 0xFE, 0x80, 0x80, 0xFC, 0x80, 0x80, 0xFE, 0x00 },  /* E */
	{ 0xF8, 0x84, 0x82, 0x82, 0x82, 0x84, 0xF8, 0x00 },  /* D */
};

#define GLYPH_COUNT ((int)(sizeof(glyphs) / sizeof(glyphs[0])))

static bool prepared;

static void prepare(void) {
	int g, y, x;

	SUB_BG_PALETTE[OVERLAY_PAL_INDEX] = 0x7FFF;  /* white */

	/* Tile 0 is left blank, so it clears the rest of the layer. */
	for (g = 0; g < GLYPH_COUNT; g++) {
		vu8* tile = OVERLAY_TILES + (g + 1) * 32;  /* 4bpp: 32 bytes per tile */
		for (y = 0; y < 8; y++) {
			const u8 bits = glyphs[g][y];
			for (x = 0; x < 8; x += 2) {
				/* Two pixels per byte, low nibble leftmost. */
				const u8 left  = (bits & (0x80 >> x)) ? 1 : 0;
				const u8 right = (bits & (0x80 >> (x + 1))) ? 1 : 0;
				tile[y * 4 + (x >> 1)] = left | (right << 4);
			}
		}
	}

	for (y = 0; y < 32 * 32; y++) {
		OVERLAY_MAP[y] = 0;
	}
	for (g = 0; g < GLYPH_COUNT; g++) {
		OVERLAY_MAP[OVERLAY_ROW * 32 + OVERLAY_COL + g] =
			(g + 1) | (OVERLAY_PAL_BANK << 12);
	}

	prepared = true;
}

void ra_overlay_tick(void) {
	if (!prepared) {
		prepare();
	}

	/* Re-claimed every frame: the game rewrites these registers itself. */
	SUB_BG0CNT  = OVERLAY_BG0CNT;
	SUB_BG0HOFS = 0;
	SUB_BG0VOFS = 0;
	SUB_DISPCNT |= (1 << 8);  /* enable sub BG0 */
}

#endif /* RA_READER_ENABLED */
