/*
    RetroAchievements support for nds-bootstrap -- on-screen notification (ARM9).

    Flashes the screens by driving the master brightness registers. This is the
    one visual channel that needs nothing from the game: no VRAM bank, no
    background layer, no palette entry, no OAM slot. Text comes later, once the
    display measurements say what a game actually leaves spare.

    The previous value is saved and put back afterwards, because games use master
    brightness themselves for fades and clobbering it would leave a game stuck
    bright or dark.

    This file is part of nds-bootstrap and is licensed under the GPL-3.0,
    the same terms as the rest of the project.
*/

#include "ra_toast.h"

#if RA_READER_ENABLED

#define REG_MASTER_BRIGHT_MAIN (*(vu16*)0x0400006C)
#define REG_MASTER_BRIGHT_SUB  (*(vu16*)0x0400106C)

#define BRIGHT_MODE_UP (1 << 14)

/*
    Temporary: fire a flash on a timer so the channel can be confirmed on hardware
    before there is any achievement logic to fire it. Set to 0 to silence it.
*/
#define RA_TOAST_DEMO_INTERVAL 600  /* frames, so about ten seconds */

static u16  savedBright[2];
static u32  framesLeft;
static bool flashing;
static u32  demoCounter;

void ra_toast_flash(u32 frames) {
	if (flashing || frames == 0) {
		return;
	}
	savedBright[0] = REG_MASTER_BRIGHT_MAIN;
	savedBright[1] = REG_MASTER_BRIGHT_SUB;
	framesLeft = frames;
	flashing = true;
}

void ra_toast_tick(void) {
	#if RA_TOAST_DEMO_INTERVAL
	if (++demoCounter >= RA_TOAST_DEMO_INTERVAL) {
		demoCounter = 0;
		ra_toast_flash(30);
	}
	#endif

	if (!flashing) {
		return;
	}

	framesLeft--;
	if (framesLeft == 0) {
		/* Hand the register back exactly as it was found. */
		REG_MASTER_BRIGHT_MAIN = savedBright[0];
		REG_MASTER_BRIGHT_SUB  = savedBright[1];
		flashing = false;
		return;
	}

	/* Pulse rather than hold, so it reads as a notification and not a glitch. */
	{
		const u16 level = (framesLeft & 4) ? 12 : 0;
		REG_MASTER_BRIGHT_MAIN = BRIGHT_MODE_UP | level;
		REG_MASTER_BRIGHT_SUB  = BRIGHT_MODE_UP | level;
	}
}

#endif /* RA_READER_ENABLED */
