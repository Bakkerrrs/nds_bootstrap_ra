/*
    RetroAchievements support for nds-bootstrap -- shared definitions.

    Layering (kept deliberately strict, see docs/retroachievements.md):

      ra_reader  -- reads the running game's RAM. Knows nothing about RA.
      ra_overlay -- draws a notification over the game. Knows nothing about RA.
      ra_client  -- wraps rcheevos' rc_client. Decides what to watch. (not yet)
      ra_net     -- HTTP transport to the RA servers.                  (not yet)

    Only ra_reader and ra_overlay exist at this point.

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

    Eight cardengine variants compile this file, and the reader is not wanted in all
    of them. Two groups opt out by default:

      DLDI -- nds-bootstrap running from a flashcard in DS mode. Out of scope for the
      fork, which targets the DSi-capable path, and out of room besides: after phase 1
      `arm9_twlsdk_dldi` and `arm9_twlsdk3_dldi` have 176 bytes left in their windows
      and `arm9_dldi` has 208, against the reader's ~550.

      GSDD -- hookIPC_SYNC() is compiled out under #ifndef GSDD, so these variants
      link the reader but never install the per-frame handler. It could never tick
      there; carrying it was only ever dead weight.

    Override on the command line (-DRA_READER_ENABLED=1) to build it anyway.
*/
#ifndef RA_READER_ENABLED
#if defined(DLDI) || defined(GSDD)
#define RA_READER_ENABLED 0
#else
#define RA_READER_ENABLED 1
#endif
#endif

/*
    How many watches the reader evaluates per frame, and how many pointer
    indirections a single watch may walk.

    Both are budget decisions, not design limits. The reader lives in the ARM9
    cardengine's fixed window and the whole watchlist -- code, descriptors and
    results -- has to fit in what is left of it. The tightest variant that links
    this code has roughly 600 bytes spare, so a watch costs about 4% of the
    remaining space; see docs/retroachievements.md for the per-variant figures and
    tools/ra_snapshot_addr.sh for the current ones. The linker scripts assert that
    .bss stays below the stacks, so raising these too far fails the build rather
    than corrupting the stack on hardware.

    Two indirections cover the shape RetroAchievements actually uses on the DS --
    a pointer to a player or save structure, then a field inside it -- and a third
    can be added when a game needs one.
*/
#define RA_WATCH_MAX 4
#define RA_CHAIN_MAX 2

/*
    How a watch resolved on the most recent tick. Reported per watch rather than
    as one global flag, because a chain that stops resolving is normal -- games
    null their pointers between scenes -- and the useful question is which watch
    stopped and where.
*/
#define RA_WATCH_UNUSED       0  /* free slot */
#define RA_WATCH_PENDING      1  /* added, not yet evaluated */
#define RA_WATCH_OK           2  /* resolved and read this tick */
#define RA_WATCH_BAD_BASE     3  /* the chain's first address is not readable */
#define RA_WATCH_BAD_POINTER  4  /* a pointer read mid-chain is not usable */
#define RA_WATCH_BAD_TARGET   5  /* the resolved address is not readable */
#define RA_WATCH_MISALIGNED   6  /* resolved address not aligned for its size */

/*
    Diagnostic watch: the sub engine's DISPCNT. Worth having as watch 0 because it
    really does change -- the overlay sets and clears a background-enable bit in it
    when it borrows a layer -- so a direct watch has something live to show before
    there is a game address to point at.
*/
#define RA_DEFAULT_WATCH_ADDRESS 0x04001000

/*
    One watched value, with its result stored alongside its definition so the whole
    thing is one contiguous run of memory in the in-game menu's RAM viewer.

    A watch is either direct (depth 0: read `base`) or a pointer chain (depth n:
    read the word at `base`, add offsets[0], read the word there, add offsets[1],
    and so on, then read the value). The chain is walked again from scratch on every
    tick, which is the point: a cached resolved address is wrong the moment the game
    moves the structure it pointed into.
*/
typedef struct raWatch {
	u32 base;                   /* +0x00  absolute address, or start of the chain */
	u32 offsets[RA_CHAIN_MAX];  /* +0x04  added after each indirection */
	u32 address;                /* +0x0C  resolved address this tick, 0 if unresolved */
	u32 value;                  /* +0x10  value read, zero-extended */
	u8  depth;                  /* +0x14  indirections to walk; 0 = direct */
	u8  size;                   /* +0x15  bytes to read: 1, 2 or 4 */
	u8  status;                 /* +0x16  RA_WATCH_* */
	u8  reserved;               /* +0x17 */
} raWatch;                      /*        0x18 bytes */

/*
    Reads as the ASCII bytes "RA1S" in a byte-wise hex dump, which is how the
    in-game menu's RAM viewer displays memory. Lets you confirm at a glance that
    you are looking at the snapshot and not at unrelated memory -- and the digit is
    the layout version, so a stale address from an older build announces itself as
    "RA0S" instead of being read as garbage.
*/
#define RA_SNAPSHOT_MAGIC0 'R'
#define RA_SNAPSHOT_MAGIC1 'A'
#define RA_SNAPSHOT_MAGIC2 '1'
#define RA_SNAPSHOT_MAGIC3 'S'

/*
    The snapshot lives in the cardengine's own .bss. That matters for two
    reasons: the game can never touch it, and it is never initialised -- the
    bootloader copies only the loaded image, which ends exactly where .bss
    begins, and an injected binary has no crt0 to zero the rest. So nothing here
    may assume a known initial state; the magic is what makes the buffer
    trustworthy.

    It is also the debug channel for all of this work: there is no console and no
    logging inside an injected cardengine, so the snapshot is read with the in-game
    menu's RAM viewer. That is why the results sit next to the watch definitions and
    why the counters are here at all.

    16-byte aligned so the RAM viewer, which only jumps in 0x10 steps, can land
    exactly on it.
*/
typedef struct raSnapshot {
	u8  magic[4];        /* +0x00  'R','A','1','S' */
	u32 ticks;           /* +0x04  frames captured -- the reader is alive */
	/*
	    How the overlay's negotiation for VRAM is going. The game moves its
	    character bases between scenes, so the overlay borrows a block per
	    notification and hands it straight back when the game wants it -- these say
	    how often that happens rather than leaving it to be inferred from glitches.
	*/
	u32 shows;           /* +0x08  notifications actually displayed */
	u32 denied;          /* +0x0C  wanted to show, nothing was free */
	u32 evicted;         /* +0x10  game reclaimed the block mid-notification */
	u8  watchCount;      /* +0x14  slots in use */
	u8  resolved;        /* +0x15  of those, how many resolved this tick */
	/*
	    The per-frame cost, in scanlines. The game owns the hardware timers, so
	    VCOUNT is the only clock the reader can read without taking something the
	    game is using; a scanline is coarse (~1600 ARM9 cycles) but the question
	    being answered is "does the watchlist eat into the frame", and for that it is
	    exactly the right unit.
	*/
	u8  linesLast;       /* +0x16  scanlines the last tick consumed */
	u8  linesMax;        /* +0x17  worst seen since the buffer was claimed */
	raWatch watches[RA_WATCH_MAX];  /* +0x18 */
} raSnapshot;

#endif /* RA_H */
