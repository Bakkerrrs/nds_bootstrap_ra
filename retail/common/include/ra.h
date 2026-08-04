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
    Top of main RAM. A 3DS in DSi mode exposes 32MB at 0x0C000000; a DSi has 16MB.
    Only the first 16MB is mirrored into the 0x02xxxxxx window the game sees, so
    anything above 0x0D000000 is memory the game cannot reach even by accident --
    which is exactly what the RA state wants.
*/
#define RA_RAM_TOP_3DS 0x0E000000
#define RA_RAM_TOP_DSI 0x0D000000

/* Never probe below this: it is the boundary of the 3DS-only upper 16MB. */
#define RA_PROBE_FLOOR 0x0D000000

/* Stay clear of the top of RAM, where the TWLSDK cheat engine lives. */
#define RA_PROBE_CEILING 0x0DFF0000

/*
    A CPU store just past the ROM cache (0x0DFCC000 on a 3DS) took a Data Abort on
    the very first try, which leaves two very different explanations: either there
    is no RAM up there, or there is and the MPU will not let the CPU reach it. The
    two lead to opposite plans -- give up on the region, or widen an MPU region --
    so guessing is not good enough.

    DMA settles it. It does not go through the MPU and it cannot fault: a transfer
    to memory that is not there simply goes nowhere. So round-trip a pattern
    through the address with NDMA and see whether it comes back. A matching
    round-trip means the RAM exists and only the MPU is in the way.

    Runs once rather than per frame, to stay out of the way of the card reads that
    share the NDMA hardware.
*/
#define RA_PROBE_ENABLED 1

/* Channels 0 and 1 are used for card reads; 3 is free. */
#define RA_PROBE_NDMA_CHANNEL 3

/*
    Bytes round-tripped by the probe. Four words is plenty to tell a real
    round-trip from a dead one, and the buffers live in the cardengine's .bss --
    where the tightest variant (arm9_twlsdk_dldi) has almost nothing to spare.
*/
#define RA_PROBE_BYTES 16

/* Reads as "RAHP" in a hex dump. */
#define RA_PROBE_MAGIC 0x50484152

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

	/* Reserved-region probe. Ran once; these are flags, not counters. */
	u32 probeBase;       /* +0x20  address probed, 0 if the probe did not run */
	u32 probeDmaOk;      /* +0x24  1 = the pattern round-tripped through probeBase */
	u32 probeControlOk;  /* +0x28  1 = the same round-trip works on known-good RAM */
	u32 probeReadBack;   /* +0x2C  first word that came back from probeBase */

	u32 cardReads;       /* +0x30 */
	u32 irqEnables;      /* +0x34 */
	u32 srcAddress;      /* +0x38  address data[] was copied from */
	u32 length;          /* +0x3C  valid bytes in data[] */

	u8  data[RA_SNAPSHOT_WINDOW];  /* +0x40 */
} raSnapshot;

#endif /* RA_H */
