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

/* Bytes of game RAM captured per frame. */
#define RA_SNAPSHOT_WINDOW 0x100

/* Start of the DS main RAM as the game sees it. */
#define RA_DEFAULT_WATCH_ADDRESS 0x02000000

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

	/* Where free memory might be. */
	u32 consoleModel;    /* +0x08  0 = DSi, >0 = 3DS (32MB instead of 16MB) */
	u32 valueBits;       /* +0x0C  ce9 feature flags */
	u32 cacheAddress;    /* +0x10  start of the ROM cache */
	u32 cacheEnd;        /* +0x14  computed end of the ROM cache */
	u32 cacheSlots;      /* +0x18 */
	u32 cacheBlockSize;  /* +0x1C */
	u32 romLocation;     /* +0x20  where the ROM lives when held in RAM */

	/* Hook chain, kept for regression checking. */
	u32 irqTable;        /* +0x24 */
	u32 cardReads;       /* +0x28 */
	u32 irqEnables;      /* +0x2C */
	u32 hookCalls;       /* +0x30 */
	u32 origVcount;      /* +0x34 */

	u32 srcAddress;      /* +0x38  address data[] was copied from */
	u32 length;          /* +0x3C  valid bytes in data[] */

	u8  data[RA_SNAPSHOT_WINDOW];  /* +0x40 */
} raSnapshot;

#endif /* RA_H */
