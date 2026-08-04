/*
    RetroAchievements support for nds-bootstrap -- on-screen text overlay (ARM9).

    Composites text over the running game on the sub engine's BG0, which the display
    measurements found spare.

    The lesson from hardware: a game's VRAM layout is not fixed. This game moved its
    BG2 character base onto the block the overlay had chosen at boot, so holding that
    block meant overwriting the game's tiles and wrecking its graphics. Nothing here
    may assume a spot stays free.

    So the overlay negotiates rather than insists. It picks a block only when about to
    show, from the live registers; it re-checks every frame while visible and gives the
    block back the moment the game wants it; and it restores what it borrowed. A
    notification that corrupts the game is worse than no notification.

    This file is part of nds-bootstrap and is licensed under the GPL-3.0,
    the same terms as the rest of the project.
*/

#include "ra_overlay.h"

#if RA_READER_ENABLED

#define SUB_DISPCNT  (*(vu32*)0x04001000)
#define SUB_BG0CNT   (*(vu16*)0x04001008)
#define SUB_BGCNT(i) (*(vu16*)(0x04001008 + (i) * 2))
#define SUB_BG0HOFS  (*(vu16*)0x04001010)
#define SUB_BG0VOFS  (*(vu16*)0x04001012)

/* Standard sub-engine BG palette: 16 banks of 16 in 4bpp mode. */
#define SUB_BG_PALETTE ((vu16*)0x05000400)

/* Sub BG VRAM: bank C, 128K at 0x06200000, so eight 16K character base blocks. */
#define SUB_BG_VRAM 0x06200000
#define CHAR_BLOCKS 8

#define OVERLAY_PAL_BANK 15
#define OVERLAY_ROW 10
#define OVERLAY_COL 10

/* Frames the notification stays up. */
#define OVERLAY_SHOW_FRAMES 180

/*
    Temporary, until there is achievement logic to trigger it: raise the overlay on a
    timer, so it can be watched appearing and disappearing.
*/
#define OVERLAY_DEMO_INTERVAL 600

/*
    "RA UNLOCKED", one 1bpp 8x8 glyph per character in message order. In order rather
    than as a font, so there is no lookup table and no unused glyph -- the difference
    between fitting in the cardengine and not. Bit 7 is the leftmost pixel.
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
    Guarded by a magic value rather than a bool: the cardengine's .bss is never
    zeroed, so a plain flag starts as whatever happened to be in RAM.
*/
#define STATE_MAGIC 0x5241564C  /* "RAVL" */

static u32  stateMagic;
static u32  framesLeft;   /* non-zero while visible */
static u32  demoCounter;
static int  block;        /* character base block currently borrowed */
static u16  savedBg0Cnt;
static u16  savedPalette[16];
static bool savedDispcntBg0;

/* Read into the snapshot, so the negotiation is observable rather than guessed at. */
u32 raOverlayShows;
u32 raOverlayDenied;   /* wanted to show, found nothing free */
u32 raOverlayEvicted;  /* the game reclaimed the block mid-notification */

/*
    Which 16K blocks of sub BG VRAM the game is using, for tiles or for maps. Read
    from the live registers every time, because it changes as the game switches
    scenes -- which is the whole reason a block chosen at boot is not safe to keep.
*/
static void surveyBlocks(bool* used) {
	int i, k;

	for (i = 0; i < CHAR_BLOCKS; i++) {
		used[i] = false;
	}

	for (i = 1; i < 4; i++) {  /* BG0 is the layer being borrowed */
		const u16 cnt = SUB_BGCNT(i);
		int charBase, screenBase, mapBlocks;

		if (!(SUB_DISPCNT & (1u << (8 + i)))) {
			continue;  /* layer off, so its VRAM is not in use */
		}

		charBase = (cnt >> 2) & 0xF;
		if (charBase < CHAR_BLOCKS) {
			used[charBase] = true;
		}

		/* Maps are in 2K units, and screen size decides how many. */
		screenBase = (cnt >> 8) & 0x1F;
		switch ((cnt >> 14) & 3) {
			case 0:  mapBlocks = 1; break;
			case 3:  mapBlocks = 4; break;
			default: mapBlocks = 2; break;
		}
		for (k = 0; k < mapBlocks; k++) {
			const int unit = screenBase + k;
			if (unit / 8 < CHAR_BLOCKS) {
				used[unit / 8] = true;
			}
		}
	}
}

/* Tiles at the start of the borrowed block, map 2K in: one block covers both. */
static vu32* tilesOf(int b)   { return (vu32*)(SUB_BG_VRAM + b * 0x4000); }
static vu16* mapOf(int b)     { return (vu16*)(SUB_BG_VRAM + b * 0x4000 + 0x800); }
static u16   bg0CntFor(int b) { return (u16)((b << 2) | (((b * 8) + 1) << 8)); }

static void draw(int b) {
	vu32* tiles = tilesOf(b);
	vu16* map = mapOf(b);
	int g, y, x;

	for (g = 0; g < 16; g++) {
		savedPalette[g] = SUB_BG_PALETTE[OVERLAY_PAL_BANK * 16 + g];
		if (g > 0) {
			SUB_BG_PALETTE[OVERLAY_PAL_BANK * 16 + g] = 0x7FFF;  /* white */
		}
	}

	/* Words, never bytes: DS VRAM ignores 8-bit writes. */
	for (g = 0; g < GLYPH_COUNT; g++) {
		vu32* tile = tiles + (g + 1) * 8;
		for (y = 0; y < 8; y++) {
			const u8 bits = glyphs[g][y];
			u32 row = 0;
			for (x = 0; x < 8; x++) {
				if (bits & (0x80 >> x)) {
					row |= 1u << (x * 4);  /* leftmost pixel is the lowest nibble */
				}
			}
			tile[y] = row;
		}
	}

	for (y = 0; y < 32 * 32; y++) {
		map[y] = 0;  /* tile 0 is blank, so this clears the layer */
	}
	for (g = 0; g < GLYPH_COUNT; g++) {
		map[OVERLAY_ROW * 32 + OVERLAY_COL + g] = (g + 1) | (OVERLAY_PAL_BANK << 12);
	}
}

static void show(void) {
	bool used[CHAR_BLOCKS];
	int b;

	surveyBlocks(used);
	for (b = 0; b < CHAR_BLOCKS; b++) {
		if (!used[b]) {
			break;
		}
	}
	if (b == CHAR_BLOCKS) {
		raOverlayDenied++;
		return;  /* nothing spare: stay quiet rather than corrupt the game */
	}

	block = b;
	savedBg0Cnt = SUB_BG0CNT;
	savedDispcntBg0 = (SUB_DISPCNT & (1u << 8)) != 0;

	draw(b);

	SUB_BG0CNT  = bg0CntFor(b);
	SUB_BG0HOFS = 0;
	SUB_BG0VOFS = 0;
	SUB_DISPCNT |= (1u << 8);

	framesLeft = OVERLAY_SHOW_FRAMES;
	raOverlayShows++;
}

static void hide(void) {
	int g;

	framesLeft = 0;

	SUB_BG0CNT = savedBg0Cnt;
	if (!savedDispcntBg0) {
		SUB_DISPCNT &= ~(1u << 8);
	}
	for (g = 0; g < 16; g++) {
		SUB_BG_PALETTE[OVERLAY_PAL_BANK * 16 + g] = savedPalette[g];
	}
}

void ra_overlay_tick(void) {
	if (stateMagic != STATE_MAGIC) {
		stateMagic = STATE_MAGIC;
		framesLeft = 0;
		demoCounter = 0;
		raOverlayShows = 0;
		raOverlayDenied = 0;
		raOverlayEvicted = 0;
	}

	if (framesLeft) {
		bool used[CHAR_BLOCKS];

		/*
		    Give the block back the moment the game wants it. Holding on is exactly
		    what wrecked its graphics before: it moved a character base onto this
		    block and the overlay kept rewriting the tiles underneath.
		*/
		surveyBlocks(used);
		if (used[block]) {
			raOverlayEvicted++;
			hide();
			return;
		}

		if (--framesLeft == 0) {
			hide();
			return;
		}

		/* Hold the layer: the game rewrites these registers itself. */
		SUB_BG0CNT  = bg0CntFor(block);
		SUB_BG0HOFS = 0;
		SUB_BG0VOFS = 0;
		SUB_DISPCNT |= (1u << 8);
		return;
	}

	#if OVERLAY_DEMO_INTERVAL
	if (++demoCounter >= OVERLAY_DEMO_INTERVAL) {
		demoCounter = 0;
		show();
	}
	#endif
}

#endif /* RA_READER_ENABLED */
