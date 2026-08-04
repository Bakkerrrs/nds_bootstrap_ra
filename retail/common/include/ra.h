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

/* Reads as "RAHP" in a hex dump. */
#define RA_PROBE_MAGIC 0x50484152

/* Channels 0 and 1 carry card reads; 3 is free. */
#define RA_PROBE_NDMA_CHANNEL 3

/* Words moved by the mirror test. */
#define RA_PROBE_WORDS 4

/* If main RAM ends at 16MB, an address this much lower is the same memory. */
#define RA_MIRROR_SPAN 0x01000000

/* Bytes of game RAM captured per frame. */
#define RA_SNAPSHOT_WINDOW 0x100

/* Start of the DS main RAM as the game sees it. */
#define RA_DEFAULT_WATCH_ADDRESS 0x02000000

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

	/* Measured memory map. */
	u32 consoleModel;    /* +0x08  0 = DSi (16MB), >0 = 3DS (32MB) */
	u32 valueBits;       /* +0x0C  ce9 feature flags */
	u32 cacheAddress;    /* +0x10  start of the ROM cache */
	u32 cacheEnd;        /* +0x14  end of the ROM cache */
	u32 romLocation;     /* +0x18 */
	u32 freeBytes;       /* +0x1C  from cacheEnd to the top of RAM */

	/*
	    The eight ARM946 protection regions, read straight from CP15. Base is bits
	    31..12, size bits 5..1, enable bit 0 -- a word with bit 0 clear is a region
	    the game is not using, and therefore one the RA area can claim without
	    disturbing it.
	*/
	u32 mpuRegion[8];    /* +0x20 */

	/*
	    What may be done with those regions. The addresses above the ROM cache turn
	    out to be real, distinct memory -- a DMA write there left the candidate
	    mirror 16MB lower untouched -- and region 3 covers both them and the cache.
	    So the store that faulted was not about coverage. The cardengine only ever
	    *reads* the cache with the CPU, since fills go through NDMA, which fits a
	    region that permits loads and not stores. These registers settle it: four
	    bits per region for data, four for instructions.
	*/
	u32 mpuDataPerm;     /* +0x40 */
	u32 mpuInstrPerm;    /* +0x44 */
	u32 mpuCacheable;    /* +0x48 */
	u32 mpuBufferable;   /* +0x4C */

	/*
	    Display state, for working out whether a text overlay is possible. Drawing
	    over a running game means taking a background layer, a VRAM bank and some
	    palette out from under it, and how much is spare is entirely
	    game-dependent -- so measure it on real games rather than guess. DISPCNT
	    bits 8..11 are the BG enables; each VRAM bank control byte has bit 0 set
	    when the bank is mapped to an engine.
	*/
	u32 dispCntMain;     /* +0x50 */
	u32 dispCntSub;      /* +0x54 */
	u32 vramCr0;         /* +0x58  bank control bytes A..D */
	u32 vramCr1;         /* +0x5C  banks E..I, top byte unused */

	u32 cardReads;       /* +0x60 */
	u32 irqEnables;      /* +0x64 */
	u32 srcAddress;      /* +0x68  address data[] was copied from */
	u32 length;          /* +0x6C  valid bytes in data[] */

	u8  data[RA_SNAPSHOT_WINDOW];  /* +0x70 */
} raSnapshot;

#endif /* RA_H */
