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
    The snapshot lives in the cardengine's own .bss, so it is memory the game
    never touches. The header is rewritten on every tick because .bss is not
    zeroed for an injected binary -- there is no crt0 to do it -- so nothing here
    may assume a known initial state.
*/
typedef struct raSnapshot {
	u8  magic[4];    /* 'R','A','0','S' */
	u32 frame;       /* incremented once per captured frame */
	u32 srcAddress;  /* address data[] was copied from */
	u32 length;      /* valid bytes in data[] */
	u8  data[RA_SNAPSHOT_WINDOW];
} raSnapshot;

#endif /* RA_H */
