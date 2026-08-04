/*
    RetroAchievements support for nds-bootstrap -- shared definitions.

    Layering (kept deliberately strict, see docs/retroachievements.md):

      ra_reader  -- reads the running game's RAM. Knows nothing about RA.
      ra_client  -- wraps rcheevos' rc_client. Decides what to watch. (not yet)
      ra_net     -- HTTP transport to the RA servers.                  (not yet)

    Only ra_reader exists at this point.

    This file is part of nds-bootstrap and is licensed under the GPL-3.0,
    the same terms as the rest of the project.
*/

#ifndef RA_H
#define RA_H

#include <nds/ndstypes.h>

/*
    Master switch for the whole RA reader. Set to 0 to build a cardengine that
    behaves exactly like upstream nds-bootstrap: no extra per-frame work and no
    VCOUNT interrupt forced on for games that did not ask for one.
*/
#define RA_READER_ENABLED 1

/* If main RAM ends at 16MB, an address this much lower is the same memory. */
#define RA_MIRROR_SPAN 0x01000000

/* Bytes of game RAM captured per frame. */
#define RA_SNAPSHOT_WINDOW 0x10

/*
    Diagnostic. The registers read back exactly as written -- DISPCNT_SUB has bit 8
    set and BG0CNT is 0x0A08 -- so the game is not reverting them and the layer is
    configured. Nothing appears anyway, and a bad palette would show black letters
    rather than nothing at all, so the suspect is the map or the tiles.

    The map entries read back exactly as written too, so VRAM writes to bank C land
    and BG0 is reading a correct map. That leaves the palette -- and the earlier
    reason for dismissing it was wrong: a black entry would draw black glyphs, which
    against a dark bottom screen looks like nothing at all rather than like an
    obvious bug.

    The palette read back white too. Every verifiable link was correct because every
    one of them used halfword writes -- and the one remaining link, the tile data,
    was written a byte at a time. DS VRAM ignores 8-bit writes, so the tiles were
    never written and every pixel stayed at index 0, which is transparent.

    With the overlay drawing, point the window back at the sub engine's registers:
    if the text vanishes for a reason other than the tiles, this is where a changed
    char base or a disabled layer would show.
*/
#define RA_DEFAULT_WATCH_ADDRESS 0x04001000

/*
    Top of main RAM. A 3DS in DSi mode exposes 32MB at 0x0C000000; a DSi has 16MB.
    Only the first 16MB is mirrored into the 0x02xxxxxx window the game sees, so
    anything above 0x0D000000 is memory the game cannot reach even by accident --
    which is exactly what the RA state wants.
*/
#define RA_RAM_TOP_3DS 0x0E000000
#define RA_RAM_TOP_DSI 0x0D000000

/*
    Reads as the ASCII bytes "RA0S" in a byte-wise hex dump, which is how the
    in-game menu's RAM viewer displays memory. Lets you confirm at a glance that
    you are looking at the snapshot and not at unrelated memory.
*/
#define RA_SNAPSHOT_MAGIC0 'R'
#define RA_SNAPSHOT_MAGIC1 'A'
#define RA_SNAPSHOT_MAGIC2 '0'
#define RA_SNAPSHOT_MAGIC3 'S'

/*
    The snapshot lives in the cardengine's own .bss. That matters for two
    reasons: the game can never touch it, and it is never initialised -- the
    bootloader copies only the loaded image, which ends exactly where .bss
    begins, and an injected binary has no crt0 to zero the rest. So nothing here
    may assume a known initial state; the magic is what makes the buffer
    trustworthy.

    Beyond the reader's own output, the header reports the console's real memory
    map. The cardengine is linked into a fixed 12K window with only ~840 bytes
    spare, so neither a full watchlist nor rc_client can live there, and the plan
    is to reserve a region off the end of the ROM cache -- the same trick the
    cheat engine already uses. Picking that region from the constants alone is
    guesswork: the cache start and size depend on console model, DSi mode, SDK
    version and whether cheats are on. Reporting what the cardengine actually
    sees is what makes the choice safe, because getting it wrong means
    scribbling on the game's cached ROM.

    16-byte aligned so the in-game menu's RAM viewer, which only jumps in 0x10
    steps, can land exactly on it.
*/
typedef struct raSnapshot {
	u8  magic[4];        /* +0x00  'R','A','0','S' */
	u32 ticks;           /* +0x04  frames captured -- the reader is alive */
	u32 srcAddress;      /* +0x08  address data[] was copied from */
	u32 length;          /* +0x0C  valid bytes in data[] */
	/*
	    Times the overlay found its tiles clobbered and rewrote them. The text showed
	    up and then vanished, and the tiles were the one thing written once rather than
	    every frame -- so this says whether the game is reusing that VRAM, and how
	    often, rather than leaving it to be guessed.
	*/
	/*
	    How the overlay's negotiation for VRAM is going. The game moves its character
	    bases between scenes, so the overlay borrows a block per notification and hands
	    it straight back when the game wants it -- these say how often that happens
	    rather than leaving it to be inferred from glitches.
	*/
	u32 shows;           /* +0x10  notifications actually displayed */
	u32 denied;          /* +0x14  wanted to show, nothing was free */
	u32 evicted;         /* +0x18  game reclaimed the block mid-notification */
	u8  data[RA_SNAPSHOT_WINDOW];  /* +0x1C */
} raSnapshot;

#endif /* RA_H */
