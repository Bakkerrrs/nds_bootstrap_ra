/*
    RetroAchievements support for nds-bootstrap -- shared definitions.

    Layering (kept deliberately strict, see docs/retroachievements.md):

      ra_reader  -- reads the running game's RAM. Knows nothing about RA.
      ra_client  -- wraps rcheevos' rc_client. Decides what to watch. (not yet)
      ra_net     -- HTTP transport to the RA servers.                  (not yet)

    Only ra_reader exists at this point (phase 0).

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

/* Bytes of game RAM captured per frame during phase 0. */
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

    Everything up to data[] is diagnostic. The counters exist because the reader
    depends on a chain of things going right -- the cardengine running at all,
    the game calling the patched irqEnable, the IRQ table being found, and the
    VCOUNT interrupt actually firing -- and when the snapshot stays empty they
    are what says which link broke. 16-byte aligned so the in-game menu's RAM
    viewer, which only jumps in 0x10 steps, can land exactly on it.
*/
typedef struct raSnapshot {
	u8  magic[4];      /* +0x00  'R','A','0','S' */
	u32 ticks;         /* +0x04  ra_reader_tick() calls, i.e. frames captured */
	u32 cardReads;     /* +0x08  cardRead() calls -- proves the cardengine runs */
	u32 irqEnables;    /* +0x0C  myIrqEnable() calls */
	u32 hookCalls;     /* +0x10  hookIPC_SYNC() calls that reached the install */
	u32 irqTable;      /* +0x14  ce9->irqTable; zero means the table was not found */
	u32 vcountRef;     /* +0x18  ce9->patches->vcountHandlerRef */
	u32 origVcount;    /* +0x1C  the game's original VCOUNT handler, if any */
	u32 srcAddress;    /* +0x20  address data[] was copied from */
	u32 length;        /* +0x24  valid bytes in data[] */
	u32 reserved[2];   /* +0x28  pad data[] onto a 16-byte boundary */
	u8  data[RA_SNAPSHOT_WINDOW];  /* +0x30 */
} raSnapshot;

#endif /* RA_H */
