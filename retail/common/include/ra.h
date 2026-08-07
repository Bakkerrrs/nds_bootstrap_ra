/*
    RetroAchievements support for nds-bootstrap -- shared definitions.

    Layering (kept deliberately strict, see docs/retroachievements.md):

      ra_reader  -- the per-frame bridge in the cardengine, plus the snapshot that is
                    this project's only debug channel. The watchlist it used to hold now
                    lives in cardenginei_arm9_ra, where there is room for it.
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

    These are no longer budget decisions. The watchlist lives in cardenginei_arm9_ra --
    256K of DSi WRAM -- rather than in the ARM9 cardengine's 12K window, so a watch costs
    24 bytes of somewhere that has room instead of 24 bytes of somewhere that has tens.

    It was a budget decision until then, and the history is worth keeping: RA_WATCH_MAX
    went 4 -> 2 to pay for the bridge into the separate binary, because the bridge is what
    ended the competition. Now it is 16 because nothing argues for less.

    Two indirections cover the shape RetroAchievements actually uses on the DS -- a
    pointer to a player or save structure, then a field inside it -- and a third can be
    added when a game needs one.
*/
#define RA_WATCH_MAX 16
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
    What the snapshot carries per watch, as opposed to what the watchlist keeps.

    The descriptors live in the WRAM binary now, and there can be RA_WATCH_MAX of them;
    the snapshot is a debug channel read through a hex viewer, so it mirrors only the
    first RA_RESULT_MAX and only the fields that change. `base` and `offsets` are static
    configuration -- if they were wrong the address would not resolve, which `status`
    already says.
*/
typedef struct raResult {
	u32 address;                /* +0x00  resolved this tick, 0 if unresolved */
	u32 value;                  /* +0x04 */
	u8  depth;                  /* +0x08  which of the defaults this is, in practice */
	u8  size;                   /* +0x09 */
	u8  status;                 /* +0x0A  RA_WATCH_* */
	u8  reserved;               /* +0x0B */
} raResult;                     /*        0x0C bytes */

#define RA_RESULT_MAX 4

/*
    Written into the snapshot by cardenginei_arm9_ra on its first call, so a counter
    coming from that binary can be told apart from whatever was in the buffer. Reads as
    "RAH1" in a byte-wise dump -- H for helper, since it is not the snapshot's own magic.
*/
#define RA_WRAM_MAGIC 0x31484152

/*
    How far the WRAM binary got in bringing itself up, reported so a failure names its own
    stage instead of showing up as silence. It only ever moves forward, and each value is
    reached exactly once per boot.

    The stages exist because the ones before rcheevos cannot be taken for granted. Nothing
    had ever run a library in this window: .bss arrives as whatever the previous occupant
    left, so it has to be zeroed by hand before newlib -- which assumes zeroed .bss like
    every other C library -- is allowed near it, and there is no crt0 here to do that.
*/
#define RA_STAGE_NONE     0  /* .bss still garbage, or the binary never ran */
#define RA_STAGE_BSS      1  /* .bss zeroed by hand; a C library may now be trusted */
#define RA_STAGE_HEAP     2  /* the arena is measured and _sbrk() will hand it out */
#define RA_STAGE_ALLOC    3  /* an allocation was made, written to and read back */
#define RA_STAGE_WATCHES  4  /* the watchlist is installed and evaluating */

/*
    What the cardengine made of the separate binary on the most recent tick. Reported so
    each link in the chain -- built, packed, loaded, copied, recognised, called -- fails
    visibly and separately rather than as one silent absence.
*/
#define RA_WRAM_ABSENT  0  /* the bootloader did not report it loaded */
#define RA_WRAM_NO_CODE 1  /* the window does not begin with a branch, so nothing valid is there */
#define RA_WRAM_CALLED  2  /* called on the most recent tick */

/*
    Reads as the ASCII bytes "RA2S" in a byte-wise hex dump, which is how the in-game
    menu's RAM viewer displays memory. Lets you confirm at a glance that you are looking
    at the snapshot and not at unrelated memory -- and the digit is the layout version,
    so a stale address from an older build announces itself as "RA1S" or "RA0S" instead
    of being read as garbage. It goes up whenever the layout below changes, which it did
    when the watchlist moved into DSi WRAM.
*/
#define RA_SNAPSHOT_MAGIC0 'R'
#define RA_SNAPSHOT_MAGIC1 'A'
#define RA_SNAPSHOT_MAGIC2 '2'
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
	u8  magic[4];        /* +0x00  'R','A','2','S' */
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
	/*
	    Of `denied`, the share where no background layer was free rather than no VRAM
	    block. Split because the two have different answers and the obvious hypothesis
	    was otherwise untestable -- see the phase 1 field notes in
	    docs/retroachievements.md.
	*/
	u32 deniedNoLayer;   /* +0x14 */
	u8  watchCount;      /* +0x18  slots in use */
	u8  resolved;        /* +0x19  of those, how many resolved this tick */
	/*
	    The per-frame cost, in scanlines. The game owns the hardware timers, so
	    VCOUNT is the only clock the reader can read without taking something the
	    game is using; a scanline is coarse (~1600 ARM9 cycles) but the question
	    being answered is "does the watchlist eat into the frame", and for that it is
	    exactly the right unit.
	*/
	u8  linesLast;       /* +0x1A  scanlines the last tick consumed */
	u8  linesMax;        /* +0x1B  worst seen since the buffer was claimed */
	/*
	    Everything from here is written from the far side of the cardengine boundary, by
	    cardenginei_arm9_ra.
	*/
	u32 wramMagic;       /* +0x1C  RA_WRAM_MAGIC, written by the binary itself */
	/*
	    The WRAM binary's own frame counter, kept in *its* .bss and copied here each
	    tick -- not incremented here. That is deliberate: nothing had ever stored state
	    in that window between frames, and if it does not persist this counter sticks at
	    1 while `ticks` climbs. It is the cheapest possible test of the thing everything
	    after this depends on.
	*/
	u32 wramTicks;       /* +0x20 */
	u8  wramState;       /* +0x24  RA_WRAM_*, written by the cardengine */
	u8  reserved[3];     /* +0x25 */
	/*
	    The two cells the self-test chains walk. They live here, in main RAM, rather than
	    in the WRAM binary alongside the watchlist -- because a pointer the chain walker
	    follows has to be a main RAM address, and relaxing that check to accommodate our
	    own cells would weaken it for the game addresses it exists to guard.

	    selfCell holds this buffer's address and selfCellPtr holds selfCell's, so a
	    two-step chain lands on `ticks`. Written by the WRAM binary, which is handed the
	    snapshot pointer and so is the only side that knows the address.
	*/
	u32 selfCell;        /* +0x28 */
	u32 selfCellPtr;     /* +0x2C */
	raResult results[RA_RESULT_MAX];  /* +0x30 */
	/*
	    The WRAM binary bringing itself up. Appended after the results rather than
	    inserted, so every offset above keeps its address and the hardware checklist in
	    docs/retroachievements.md stays valid -- which is also why the magic did not need
	    another bump for this.

	    heapSize is the arena between the end of the binary's .bss and the top of its
	    window; heapUsed is what has been handed out. Both stay useful once rcheevos is in
	    there and you want to know how much of 256K it ate.
	*/
	u32 heapSize;        /* +0x60 */
	u32 heapUsed;        /* +0x64 */
	u8  wramStage;       /* +0x68  RA_STAGE_*, how far it got */
	u8  reserved2[3];    /* +0x69 */
} raSnapshot;            /*              0x6C bytes */

#endif /* RA_H */
