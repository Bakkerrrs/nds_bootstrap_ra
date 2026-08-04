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
/*
    Written as words, never bytes: DS VRAM ignores 8-bit writes. That is why the map
    and the palette landed -- both are halfword writes -- while the tiles, built a
    byte at a time, silently never got written at all, leaving every pixel at index 0
    and the whole layer transparent.
*/
#define OVERLAY_TILES ((vu32*)0x06208000)  /* character base block 2 */

/*
    Priority 0 so it draws above the game's BG1-3, character base block 2, 16
    colours, screen base block 10, 256x256.

    16 colours matters: the game's 256-colour layers read the *extended* palette
    (DISPCNT bit 30 is set) whose bank is disabled, so a 256-colour overlay would
    come out black. A 4bpp layer uses the standard palette and sidesteps that.
*/
#define OVERLAY_BG0CNT ((0 << 0) | (2 << 2) | (10 << 8))

/*
    Palette bank 15. Every colour in the bank is set, not just the one the glyphs
    use: writing a single entry made "nothing visible" and "the entry is black"
    impossible to tell apart, since the game's own bottom screen is dark in places
    and black on black looks like nothing at all.
*/
#define OVERLAY_PAL_BANK  15
#define OVERLAY_PAL_INDEX (OVERLAY_PAL_BANK * 16 + 1)

static void writePalette(void) {
	int i;
	for (i = 1; i < 16; i++) {
		SUB_BG_PALETTE[OVERLAY_PAL_BANK * 16 + i] = 0x7FFF;  /* white */
	}
}

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

/*
    Not a bool. The cardengine's .bss is never zeroed -- the bootloader copies only
    the loaded image and an injected binary has no crt0 -- so a plain flag starts as
    whatever was in RAM, and any non-zero garbage means prepare() never runs. The
    registers would still be set every frame, leaving BG0 enabled and showing tile 0
    everywhere, which in 16-colour mode is transparent: invisible. A magic value is
    the same guard the snapshot uses, for the same reason.
*/
#define PREPARED_MAGIC 0x5241564C  /* "RAVL" */

static u32 preparedMagic;

static void prepare(void) {
	int g, y, x;

	writePalette();

	/* Tile 0 is left blank, so it clears the rest of the layer. */
	for (g = 0; g < GLYPH_COUNT; g++) {
		vu32* tile = OVERLAY_TILES + (g + 1) * 8;  /* 4bpp: 8 words per tile */
		for (y = 0; y < 8; y++) {
			const u8 bits = glyphs[g][y];
			u32 row = 0;
			for (x = 0; x < 8; x++) {
				/* Pixel x lives in bits 4x+3..4x, so the leftmost is the lowest. */
				if (bits & (0x80 >> x)) {
					row |= 1u << (x * 4);
				}
			}
			tile[y] = row;
		}
	}

	/*
	    A solid tile above the text, to tell two failures apart: a bar with no
	    letters means the glyph expansion is wrong, nothing at all means the layer
	    is not reaching the screen.
	*/
	{
		vu32* bar = OVERLAY_TILES + (GLYPH_COUNT + 1) * 8;
		for (y = 0; y < 8; y++) {
			bar[y] = 0x11111111;  /* every pixel colour 1 */
		}
	}

	for (y = 0; y < 32 * 32; y++) {
		OVERLAY_MAP[y] = 0;
	}
	for (g = 0; g < GLYPH_COUNT; g++) {
		OVERLAY_MAP[(OVERLAY_ROW - 2) * 32 + OVERLAY_COL + g] =
			(GLYPH_COUNT + 1) | (OVERLAY_PAL_BANK << 12);
	}
	for (g = 0; g < GLYPH_COUNT; g++) {
		OVERLAY_MAP[OVERLAY_ROW * 32 + OVERLAY_COL + g] =
			(g + 1) | (OVERLAY_PAL_BANK << 12);
	}

	preparedMagic = PREPARED_MAGIC;
}

void ra_overlay_tick(void) {
	if (preparedMagic != PREPARED_MAGIC) {
		prepare();
	}

	/* Re-claimed every frame: the game rewrites these registers itself. */
	writePalette();
	{
		/* Cheap to redraw, and survives the game touching this VRAM. */
		int g;
		for (g = 0; g < GLYPH_COUNT; g++) {
			OVERLAY_MAP[OVERLAY_ROW * 32 + OVERLAY_COL + g] =
				(g + 1) | (OVERLAY_PAL_BANK << 12);
		}
	}
	SUB_BG0CNT  = OVERLAY_BG0CNT;
	SUB_BG0HOFS = 0;
	SUB_BG0VOFS = 0;
	SUB_DISPCNT |= (1 << 8);  /* enable sub BG0 */
}

#endif /* RA_READER_ENABLED */
