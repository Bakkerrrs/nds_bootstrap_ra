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
	u8  flags;                  /* +0x17  RA_WATCH_FLAG_* */
} raWatch;                      /*        0x18 bytes */

/*
    Treat each pointer read mid-chain as a 24-bit console pointer rather than a DS address:
    mask to 24 bits and add main RAM's base.

    This is not a convenience, it is what RetroAchievements means. Its DS memory map is
    console-relative, so a pointer stored by the game as 0x02xxxxxx is documented and used
    as its low 24 bits -- which is why the published code notes for a DS game say
    "[24-Bit Pointer]" rather than "pointer".

    Reading such a word as a 32-bit DS address is what the walker did first, and hardware
    said no: three direct watches resolved and the one chain came back BAD_TARGET, while
    rcheevos -- which does mask -- reported no refusal for the same location. The two
    disagreeing is what identified the model as wrong rather than the address.
*/
#define RA_WATCH_FLAG_PTR24 0x01

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
    Step 3b: handing an earned achievement id from the game to the SD card.

    The cardengine has no network and the ARM9 has no SD card -- on a DSi the card is the ARM7's, and
    every file nds-bootstrap writes from inside a game goes through fileWrite() there. So an unlock
    detected by rcheevos on the ARM9 has to cross to the ARM7, which appends it to the queue file the
    launcher pre-created, and the *next* boot's launcher sends it. See RA_QUEUE_PATH in ra_wifi.h.

    The channel is the one that already exists rather than a new one. `sharedAddr` is how the ARM9
    cardengine has always driven the ARM7 -- card reads, the reset and exit commands, the in-game
    menu's battery and clock -- and the ARM7 already polls it from its VBlank handler. Two things come
    with that for free, and both would have been work: the region is coherent between the two CPUs
    with nothing but `volatile` (every card read in every game depends on that being true), and there
    is an established idiom of a magic in one slot and data in the next.

    Slots 0-8 are taken and slot 13 is UNPATCHED_FUNCTION_LOCATION, so these are two of the only four
    that exist. See CARDENGINE_SHARED_SLOTS -- the bound is asserted where these are used.

    The protocol, deliberately as small as it can be:

      ARM9  writes the id into ID, then the magic into REQ.  (that order: the ARM7 can wake between
                                                              the two writes, and a magic with a stale
                                                              id would award the wrong achievement)
      ARM7  sees REQ == magic, appends the id, writes 0 to REQ.
      ARM9  will not write another request while REQ is non-zero.

    One in flight at a time, which is not a limitation worth removing: the ARM7 clears it within a
    frame and a set does not fire two achievements in the same frame often enough to matter. The ARM9
    side keeps the rest in its own queue and offers one per frame.
*/
#define RA_SHARED_UNLOCK_REQ   9
#define RA_SHARED_UNLOCK_ID    10
#define RA_SHARED_UNLOCK_MAGIC 0x4C554152u   /* 'RAUL' */

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
	u8  reserved2;       /* +0x69 */
	/*
	    The deepest excursion rcheevos made on the private stack, in bytes -- and it is the
	    number that explains why a real achievement set crashed a retail game twice.

	    Everything in this binary runs inside the game's VCOUNT interrupt handler, on the game's
	    IRQ stack. Measured on a host, rc_runtime_do_frame() needs **767 bytes** and has run every
	    frame for many sessions without trouble; rc_runtime_activate_achievement() needs
	    **2,383** -- 3.1 times as much. So the parse was overflowing an IRQ stack that the
	    evaluation fits in, trampling whatever the game keeps below it, and both crashes had the
	    ARM9 executing the game's own data with a wild PC.

	    rcheevos runs on a stack of ours now, and this reports the high-water mark of it so the
	    2,383 stops being a host extrapolation. Measured by painting the region and finding the
	    deepest word that changed -- which works here, unlike on the host, because we own the
	    whole region rather than borrowing the end of someone else's.
	*/
	u16 rcStackUsed;     /* +0x6A */
	/*
	    rcheevos. Appended for the same reason the heap fields were -- every offset above
	    keeps its address, so the hardware checklist in docs/retroachievements.md stays
	    valid and the magic does not need another bump.

	    This block answers three separate questions that would otherwise all look like
	    "rcheevos does not work": did it come up (rcStage, rcActivate), is it evaluating
	    (rcMeasured against rcTarget, climbing), and what does it cost (rcLines).
	*/
	u8  rcStage;         /* +0x6C  RA_RC_*, how far rcheevos got */
	s8  rcActivate;      /* +0x6D  rc_runtime_activate_achievement(): 0 ok, RC_* if not */
	u8  rcTriggerState;  /* +0x6E  RC_TRIGGER_STATE_* of the test achievement */
	/*
	    The one-time parse, measured on its own. Activating an achievement mallocs, md5s
	    the definition and parses it, and that happens inside the game's VCOUNT handler --
	    so it is much more expensive than a frame of evaluation. Reported separately
	    because otherwise it lands in linesMax and makes the steady-state cost look
	    twenty times worse than it is.

	    **The slowest single activation**, not the total, and the change is what made this
	    number usable. It used to be one VCOUNT delta across the whole of ra_rc_init(), taken
	    modulo 263 -- which is correct for three definitions and silently wrong for fifty-six:
	    a parse spanning four frames reports whatever the remainder happens to be, and a slow
	    init reads as a fast one. There is no way to count the frames from inside a handler
	    that is not being re-entered, so the measurement moved down a level: each activation
	    is timed on its own, this is the worst of them, and rcInitTotal is the sum.

	    255 means it saturated, and then rcInitTotal is a lower bound.
	*/
	u8  rcInitLines;     /* +0x6F */
	u32 rcTriggered;     /* +0x70  ACHIEVEMENT_TRIGGERED events delivered */
	/*
	    Measured progress of the test achievement, which is what makes this observable
	    rather than boolean: it climbs one per frame toward rcTarget, so a hex viewer shows
	    rcheevos evaluating rather than merely having been initialised.
	*/
	u32 rcMeasured;      /* +0x74 */
	u32 rcTarget;        /* +0x78 */
	/*
	    peek() traffic. rcPeeks is per frame -- it should equal the number of distinct
	    addresses the active definitions read, and a zero here with an active trigger means
	    do_frame is not reaching memory. rcPeeksRejected is cumulative and should stay 0:
	    it counts addresses a definition asked for that this console cannot supply, which
	    is the case that used to mean a Data Abort.
	*/
	u32 rcPeeks;         /* +0x7C */
	u32 rcPeeksRejected; /* +0x80 */
	u8  rcLines;         /* +0x84  scanlines rc_runtime_do_frame() cost, last tick */
	u8  rcLinesMax;      /* +0x85  worst seen, excluding the init frame */
	u8  rcEvents;        /* +0x86  events of any kind delivered, clamped at 255 */
	u8  reserved3;       /* +0x87 */
	/*
	    The allocator, reported separately from the arena. Added after a reading that could
	    not be diagnosed: rcheevos failed to allocate 32 bytes while heapSize said 189K was
	    free, and nothing in the snapshot could say whether the arena bookkeeping was wrong
	    or newlib was refusing for its own reasons.

	    heapBreak and heapTop are _sbrk()'s own two numbers, so the arena is *shown* rather
	    than inferred from a subtraction. sbrkProbe is the discriminator: a non-zero value
	    here next to a zero mallocProbe means _sbrk() will hand out memory that malloc()
	    will not, which puts the fault in newlib rather than in the window.

	    Only written on the failure path -- sbrkProbe moves the break, which is free on a
	    heap that is already dead and not something to do to a working one.
	*/
	u32 heapBreak;       /* +0x88 */
	u32 heapTop;         /* +0x8C */
	u32 mallocProbe;     /* +0x90  what malloc(32) returned; 0 = refused */
	u32 sbrkProbe;       /* +0x94  what _sbrk(64) returned; 0 = refused */
	/*
	    Which definition is being evaluated. Reported because the two possibilities look
	    identical once running, and telling them apart is the whole point of being able to
	    supply one from a file: a definition that does not unlock is a very different problem
	    depending on whether the file was picked up at all.
	*/
	u8  rcFromFile;      /* +0x98  1 if the staged file was used, 0 for the built-in */
	/*
	    How many definitions in the file parsed, and which line was the first not to.
	    Reported together because "two of three worked" is a completely different situation
	    from "none did", and a photograph of a hex viewer cannot show a parser message.
	*/
	u8  rcActivated;     /* +0x99  definitions successfully activated */
	u16 rcDefLength;     /* +0x9A  length of the staged text */
	u8  rcBadLine;       /* +0x9C  1-based line of the first parse failure, 0 if none */
	/*
	    Which definition unlocked first, 1-based, and it exists so that a prediction can be
	    made in advance rather than a set being declared "working" because a counter moved.

	    With fifty-six definitions loaded, `rcTriggered` climbing says something fired and
	    nothing about what. The set for GameID 14856 opens with `1=1.300.` -- always true, three
	    hundred hits -- so line 1 should unlock about five seconds in, and this field is how that
	    gets checked. Anything else here first means a definition is reading memory it should
	    not, which is a real bug and not a success.
	*/
	u8  rcFirstTriggered; /* +0x9D */
	/*
	    Scanlines summed over every activation, clamped at 0xFFFF. 263 per frame, so ~2,900 is
	    eleven frames -- and eleven frames spent inside the game's VCOUNT handler is the hazard
	    this step exists to measure, not a detail. See rcInitLines for why the total could not
	    simply be one delta.
	*/
	u16 rcInitTotal;      /* +0x9E */
	/*
	    Step 5's three fields, appended for the reason every block above them was: **every existing
	    offset keeps its address**, so the hardware checklist in docs/retroachievements.md stays
	    valid and the magic does not need another bump. The struct grows from 0xA0 to 0xA8.

	    The staged definitions carry RetroAchievements' own achievement ids now -- each line is
	    `<id>:<memaddr>`. Without them nothing can be reported to the server: `r=unlocks` answers in
	    ids and `r=awardachievement` is asked in them, and "which achievement fired" had no answer
	    the server would recognise.

	    rcFirstId is the one that proves it end to end. rcFirstTriggered says *which line* unlocked
	    first and has since the offline half; this says which achievement, in the server's own
	    numbering, which is the number a person can look up on the set's page.
	*/
	u32 rcFirstId;        /* +0xA0  RA id of the first achievement to unlock, 0 if none */
	u16 rcDefsWithId;     /* +0xA4  staged lines that carried an id */
	u16 rcDefsNoId;       /* +0xA6  ...and lines that did not, which cannot be reported */
	/*
	    Step 3b. The address of the cardengine's shared block, published by ra_tick() because the
	    binary in DSi WRAM cannot work it out: it is 0x027FFA0C or 0x02FFFA0C depending on the game's
	    SDK version, and only the ARM9 cardengine knows which. Passing it beats guessing -- a wrong
	    guess writes four bytes into a running game's memory.
	*/
	u32 shared;           /* +0xA8  CARDENGINE_SHARED_ADDRESS_* for this game, 0 if unknown */
	/*
	    ...and what crossing it has done, because the RAM viewer is the only debug channel in here.
	    `unlockSent` is what reached the ARM7; `unlockLost` is what the ring dropped, which must never
	    be a number nobody can see.
	*/
	u16 unlockSent;       /* +0xAC  ids handed to the ARM7 */
	u8  unlockQueued;     /* +0xAE  still waiting in the ring */
	u8  unlockLost;       /* +0xAF  fired while the ring was full */
	/*
	    How often the per-frame hook had to be put back, and *which* part of it was gone.

	    Contra 4 froze `ticks` at 19 while Mario 64 ran to 2,863 on the same binary, so something in
	    that game takes the hook away three tenths of a second in. Three things have to hold for it to
	    fire -- the game's IRQ table entry, IRQ_VCOUNT in REG_IE, and the Y-trigger in REG_DISPSTAT --
	    and a blind re-arm would fix it without ever saying which. These count them separately, so the
	    next run names the mechanism instead of leaving it inferred.

	    Zero on a game that never disturbs it, which is every game measured before this one.
	*/
	u8  rearmTable;       /* +0xB0  irqTable[2] no longer pointed at our handler */
	u8  rearmIe;          /* +0xB1  IRQ_VCOUNT had been cleared from REG_IE */
	u8  rearmDispstat;    /* +0xB2  the Y-trigger interrupt had been switched off */
	/*
	    Whether the sub engine had **BG extended palettes** enabled the last time the notification was
	    raised, read straight out of SUB_DISPCNT bit 30.

	    Because `shows` reaching 1 on Contra 4 with denied, evicted and deniedNoLayer all 0 says the
	    overlay borrowed its block and its layer and drew -- and it still was not visible. The colour is
	    written to standard palette RAM at 0x05000400, and with extended palettes on the hardware
	    ignores that entirely for backgrounds: the glyphs would be drawn in whatever the game's extended
	    palette holds at that index, which is very likely nothing at all.

	    One bit, so the next run either confirms that or kills it. Fits in the byte that was already
	    reserved, so the snapshot does not grow and no offset moves.
	*/
	u8  overlayExtPal;    /* +0xB3  1 when SUB_DISPCNT bit 30 was set at show() time */
} raSnapshot;            /*              0xB4 bytes */

/*
    How far rcheevos got, reported for the same reason RA_STAGE_* is: each step can fail
    for its own reason, and without this they all present as an achievement that does not
    unlock.
*/
#define RA_RC_NONE        0  /* not reached -- the window did not come up */
#define RA_RC_NO_MEMORY   1  /* malloc() refused; see heapBreak/heapTop/sbrkProbe */
#define RA_RC_NO_MEMREFS  2  /* malloc() worked but rc_runtime_init()'s allocation did not */
#define RA_RC_NO_ADDRESS  3  /* the snapshot has no console address; self-test only */
#define RA_RC_PARSE_BAD   4  /* the definition was rejected; see rcActivate */
/*
    Still activating, one definition per frame.

    Exists because fifty-six definitions cannot be parsed inside a single interrupt. Each one
    costs the same ~2.4 KB of stack -- measured, and flat, so it is not depth that scales -- and
    its own slice of time, and doing all of them in one VCOUNT handler means holding the game's
    interrupt for however long that adds up to. That total is unmeasured and is the leading
    suspect for the Data Abort the first fifty-six-definition run produced.

    Splitting it also makes a failure *name itself*: rcActivated is published before each
    activation is attempted, so a crash localises to one line of the set rather than to the set.

    **Inserted at 5, which moved RA_RC_ACTIVE to 6 and RA_RC_FRAME to 7.** Every guard in the
    cardengine is written as `rcStage < RA_RC_ACTIVE` and so needs no change, but any reading
    photographed before this build reads one lower -- the "rcStage 06 = RA_RC_FRAME" in the
    hardware checklist is now 07. Renumbered rather than parked above RA_RC_ACTIVE because a
    loading state that sorts *after* active would invert every one of those guards, and a wrong
    guard is worse than a stale number in a document I control.
*/
/*
    The id given to a definition that arrived without one -- a hand-written file, or a set from a
    server that omitted the field.

    Far out of the range real RA ids occupy, and that is the whole point rather than tidiness:
    rcheevos identifies achievements *by id* and will treat two definitions sharing one as the same
    achievement, reusing the first's trigger for the second. Numbering the id-less ones from 1 (as
    every build before this did) was safe only while nothing carried a real id; the moment both
    appear in one file, a real id of 3 would collide with the third id-less line. Real ids are six
    or seven digits, so 0xF0000000 cannot be reached by one.
*/
#define RA_SYNTHETIC_ID_BASE 0xF0000000u

#define RA_RC_LOADING     5  /* activating, one per frame; rcActivated says how far */
#define RA_RC_ACTIVE      6  /* every definition activated and validated */
#define RA_RC_FRAME       7  /* rc_runtime_do_frame() has run */

#endif /* RA_H */
