/*
    RetroAchievements support for nds-bootstrap -- on-screen text overlay (ARM9).

    Composites text over the running game on a spare sub-engine background layer.

    The lesson from hardware: a game's VRAM layout is not fixed. This game moved its
    BG2 character base onto the block the overlay had chosen at boot, so holding that
    block meant overwriting the game's tiles and wrecking its graphics. Nothing here
    may assume a spot stays free.

    The same applies to the layer. This game enables its own sub BG0 at times, with a
    character base of its own, so treating BG0 as the overlay's by right displaced a
    layer the game was using. The layer is chosen at show time too, from whichever is
    currently switched off.

    So the overlay negotiates rather than insists. It picks a layer and a block only
    when about to show, from the live registers; it re-checks every frame while visible
    and gives them back the moment the game wants them; and it restores what it
    borrowed. A notification that corrupts the game is worse than no notification.

    This file is part of nds-bootstrap and is licensed under the GPL-3.0,
    the same terms as the rest of the project.
*/

#include "ra_overlay.h"

#if RA_READER_ENABLED

#define SUB_DISPCNT  (*(vu32*)0x04001000)
/*
    The sub engine's master brightness. Read at show() time for one reason: a real unlock fires when a
    stage ends, which is exactly when a game fades the screen -- and a fade applies to our glyphs too.
    That would explain a notification that reports drawn, is never denied or evicted, and is not seen,
    while the demo timer's identical call *is* seen because it fires on an ordinary frame.
*/
#define SUB_MASTER_BRIGHT (*(vu16*)0x0400106C)
#define SUB_BG0CNT   (*(vu16*)0x04001008)
#define SUB_BGCNT(i) (*(vu16*)(0x04001008 + (i) * 2))
/* Scroll is per layer, four bytes apart -- not always BG0's. */
#define SUB_BGHOFS(i) (*(vu16*)(0x04001010 + (i) * 4))
#define SUB_BGVOFS(i) (*(vu16*)(0x04001012 + (i) * 4))

/* Standard sub-engine BG palette: 16 banks of 16 in 4bpp mode. */
#define SUB_BG_PALETTE ((vu16*)0x05000400)

/* Sub BG VRAM: bank C, 128K at 0x06200000, so eight 16K character base blocks. */
#define SUB_BG_VRAM 0x06200000
#define CHAR_BLOCKS 8

#define OVERLAY_PAL_BANK 15

/*
    The glyphs are drawn entirely in colour index 1 -- see draw(), which ORs a nibble of
    1 for every set pixel -- so exactly one palette entry has to be borrowed. An earlier
    version saved and whitened all sixteen in the bank, which cost nothing to write and
    everything to a game using those colours: on Final Fantasy III's title screen
    fourteen entries the overlay never needed went white for the three seconds a
    notification was up, and came back when it hid. A transient graphical fault tied to
    the toaster appearing, for no benefit.

    One entry is still one entry the game may be using; that is unavoidable, since the
    text has to be *some* colour. But the blast radius is now the minimum the design
    requires rather than fifteen times it.
*/
#define OVERLAY_PAL_INDEX 1
#define OVERLAY_PAL_ENTRY (OVERLAY_PAL_BANK * 16 + OVERLAY_PAL_INDEX)
#define OVERLAY_ROW 10
#define OVERLAY_COL 10

/* Frames the notification stays up. */
#define OVERLAY_SHOW_FRAMES 180

/*
    How long a notification will wait for the screen to stop fading before giving up and showing anyway.

    Bounded rather than patient: a game that leaves the screen dimmed -- a pause menu, a dark room, a
    brightness setting -- must not swallow the notification forever. Ten seconds is far longer than any
    transition and far shorter than a session, so the worst case is a notification that arrives late
    rather than one that never arrives.
*/
#define OVERLAY_FADE_WAIT_FRAMES 600

/*
    Was: raise the overlay on a timer, because when this was written there was no achievement logic to
    trigger it. There is now -- rcheevos delivers RC_RUNTIME_EVENT_ACHIEVEMENT_TRIGGERED and the count
    reaches here as raSnapshot.rcTriggered -- so the timer is off and the notification means something.

    Kept rather than deleted, at 0 so it compiles out, and overridable from the command line -- which
    is what it is for. Contra 4 froze `ticks` at 19, so the reader dies about three tenths of a second
    into the game and nothing can ever unlock; a build with -DOVERLAY_DEMO_INTERVAL=60 answers "is
    ra_tick still being called" by pulsing once a second on screen, with no RAM viewer and nothing to
    interpret. It also exercises the borrow-and-return negotiation without spending an achievement.
*/
#ifndef OVERLAY_DEMO_INTERVAL
#define OVERLAY_DEMO_INTERVAL 0
#endif

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
static u32  lastUnlocks;
/*
    A notification owed but not yet raised, because the screen was being faded when it was earned.

    Measured, not guessed: a real unlock in Contra 4 read `shows 1` with denied, evicted and
    deniedNoLayer all zero -- borrowed, held, drew, nobody took it back -- and was not seen, while the
    demo timer's identical call in the same game was. overlayState bit 5 came back set, so the sub
    engine's master brightness was non-zero: the glyphs were drawn into a screen on its way to black and
    spent their 180 frames inside the transition.

    Which is exactly when an achievement completes. A stage ends, the game fades, and that is the moment
    rcheevos fires.
*/
static u8   pending;        /* a notification is owed */
static u16  pendingFrames;  /* how long it has waited for the fade to end */  /* rcTriggered as of the last frame; the notification fires on a rise */
static int  block;        /* character base block currently borrowed */
static int  layer;        /* background layer currently borrowed */
static u16  savedBgCnt;
static u16  savedHofs;
static u16  savedVofs;
static u16  savedPaletteEntry;
static bool savedDispcntBg;

/* Read into the snapshot, so the negotiation is observable rather than guessed at. */
u32 raOverlayShows;
u32 raOverlayDenied;   /* wanted to show, found nothing free */
u32 raOverlayEvicted;  /* the game reclaimed the block mid-notification */
/*
    Of those denials, the ones where no background layer was switched off -- as opposed
    to no VRAM block being spare. The two have different answers, and lumping them
    together left the obvious hypothesis untestable: on Final Fantasy III the
    notification went missing across map transitions, where the screen fades and the
    game plausibly has every sub layer enabled to do it. If that is right, this counter
    accounts for nearly all of them.
*/
u32 raOverlayDeniedNoLayer;
/* SUB_DISPCNT bit 30 as of the last show(); see raSnapshot.overlayExtPal. */
/* The sub engine's state and our choice out of it, as of the last show(). See raSnapshot. */
u8  raOverlayState;

/*
    Which 16K blocks of sub BG VRAM the game is using, for tiles or for maps. Read
    from the live registers every time, because it changes as the game switches
    scenes -- which is the whole reason a block chosen at boot is not safe to keep.
*/
static void surveyBlocks(bool* used, int skipLayer) {
	int i, k;

	for (i = 0; i < CHAR_BLOCKS; i++) {
		used[i] = false;
	}

	for (i = 0; i < 4; i++) {
		const u16 cnt = SUB_BGCNT(i);
		int charBase, screenBase, mapBlocks;

		if (i == skipLayer) {
			continue;  /* the layer being borrowed */
		}
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
static u16   bgCntFor(int b)  { return (u16)((b << 2) | (((b * 8) + 1) << 8)); }

/*
    A layer the game currently has switched off. Taking one it is using would displace
    its graphics, which is exactly the mistake that corrupted them before.
*/
static int chooseLayer(void) {
	int i;
	for (i = 0; i < 4; i++) {
		if (!(SUB_DISPCNT & (1u << (8 + i)))) {
			return i;
		}
	}
	return -1;
}

static void draw(int b) {
	vu32* tiles = tilesOf(b);
	vu16* map = mapOf(b);
	int g, y, x;

	/*
	    Recorded, not worked around. With BG extended palettes on, this write goes to memory the sub
	    engine is not reading for backgrounds, so the glyphs come out in the game's own colour at this
	    index -- which is the leading explanation for a notification that reports drawn and is not seen.
	*/
	/*
	    Packed here, after the block and layer are chosen and before a single tile is written, so the
	    reading is what surveyBlocks() had to work from paired with what it decided. One byte, because
	    the cardengine's window has none to spare. See raSnapshot.overlayState.
	*/
	raOverlayState = (u8)(((SUB_DISPCNT & (1u << 30)) ? 1 : 0)
	                      | ((layer & 3) << 1)
	                      | ((block & 3) << 3)
	                      /* bit 5: the screen was being faded when this was raised */
	                      | ((SUB_MASTER_BRIGHT & 0x1F) ? 0x20 : 0)
	                      /* bit 6: this one was held back until a fade ended */
	                      | (pendingFrames ? 0x40 : 0));
	savedPaletteEntry = SUB_BG_PALETTE[OVERLAY_PAL_ENTRY];
	SUB_BG_PALETTE[OVERLAY_PAL_ENTRY] = 0x7FFF;  /* white */

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

	const int l = chooseLayer();
	if (l < 0) {
		raOverlayDenied++;
		raOverlayDeniedNoLayer++;
		return;  /* every layer in use: stay quiet rather than displace one */
	}

	surveyBlocks(used, l);
	for (b = 0; b < CHAR_BLOCKS; b++) {
		if (!used[b]) {
			break;
		}
	}
	if (b == CHAR_BLOCKS) {
		raOverlayDenied++;
		return;  /* no spare VRAM: stay quiet rather than corrupt the game */
	}

	layer = l;
	block = b;
	savedBgCnt = SUB_BGCNT(l);
	savedHofs  = SUB_BGHOFS(l);
	savedVofs  = SUB_BGVOFS(l);
	savedDispcntBg = (SUB_DISPCNT & (1u << (8 + l))) != 0;

	draw(b);

	SUB_BGCNT(l)  = bgCntFor(b);
	SUB_BGHOFS(l) = 0;
	SUB_BGVOFS(l) = 0;
	SUB_DISPCNT |= (1u << (8 + l));

	framesLeft = OVERLAY_SHOW_FRAMES;
	raOverlayShows++;
}

static void hide(void) {
	framesLeft = 0;

	SUB_BGCNT(layer)  = savedBgCnt;
	SUB_BGHOFS(layer) = savedHofs;
	SUB_BGVOFS(layer) = savedVofs;
	if (!savedDispcntBg) {
		SUB_DISPCNT &= ~(1u << (8 + layer));
	}
	SUB_BG_PALETTE[OVERLAY_PAL_ENTRY] = savedPaletteEntry;
}

void ra_overlay_tick(u32 unlocks) {
	if (stateMagic != STATE_MAGIC) {
		stateMagic = STATE_MAGIC;
		framesLeft = 0;
		demoCounter = 0;
		pending = 0;
		pendingFrames = 0;
		/*
		    Synced to whatever the count already is, not to zero. Claiming with 0 would fire the
		    notification once for a session that had already unlocked something before this ran.
		*/
		lastUnlocks = unlocks;
		raOverlayShows = 0;
		raOverlayDenied = 0;
		raOverlayEvicted = 0;
		raOverlayDeniedNoLayer = 0;
	}

	if (framesLeft) {
		bool used[CHAR_BLOCKS];

		/*
		    Give the block back the moment the game wants it. Holding on is exactly
		    what wrecked its graphics before: it moved a character base onto this
		    block and the overlay kept rewriting the tiles underneath.
		*/
		surveyBlocks(used, layer);
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
		SUB_BGCNT(layer)  = bgCntFor(block);
		SUB_BGHOFS(layer) = 0;
		SUB_BGVOFS(layer) = 0;
		SUB_DISPCNT |= (1u << (8 + layer));
		return;
	}

	/*
	    The real trigger, and the reason OVERLAY_DEMO_INTERVAL is now 0: an achievement unlocked.

	    Strictly greater, and resynced when it drops. rcheevos' count is reset when the runtime is
	    re-initialised, and a count that went backwards must not be read as a new unlock.
	*/
	/*
	    Raise what is owed, once the screen is worth drawing on. Checked before the trigger below so a
	    second unlock arriving while the first is still waiting does not queue behind itself -- one
	    notification is one notification, and `lastUnlocks` has already moved past both.
	*/
	if (pending) {
		if ((SUB_MASTER_BRIGHT & 0x1F) && pendingFrames < OVERLAY_FADE_WAIT_FRAMES) {
			pendingFrames++;
			return;
		}
		pending = 0;
		/*
		    show() reads pendingFrames for bit 6 of overlayState, so it is cleared *after* the call --
		    zeroing it first would report every deferred notification as one that never waited, which
		    is precisely the fact this bit exists to confirm.
		*/
		show();
		pendingFrames = 0;
		return;
	}

	if (unlocks > lastUnlocks) {
		lastUnlocks   = unlocks;
		/*
		    Owed now, shown when the screen is not mid-fade. Deferring rather than drawing immediately
		    is the whole fix: the draw always worked, it was the moment that was wrong.
		*/
		pending       = 1;
		pendingFrames = 0;
		return;
	}
	if (unlocks < lastUnlocks) {
		lastUnlocks = unlocks;
	}

	#if OVERLAY_DEMO_INTERVAL
	if (++demoCounter >= OVERLAY_DEMO_INTERVAL) {
		demoCounter = 0;
		show();
	}
	#endif
}

#endif /* RA_READER_ENABLED */
