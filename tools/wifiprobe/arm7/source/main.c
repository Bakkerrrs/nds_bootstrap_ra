/*
    The ARM7 side of the WiFi probe -- a stock DSi-mode core with dsiwifi's FIFO installed.

    Almost all of this is the standard libnds ARM7 template, and it is here rather than
    reduced because the point of the probe is to test WiFi under *ordinary* conditions.
    Trimming the core to the minimum would introduce a second variable: a failure could then
    be the chip, or it could be something the template does that we removed. So the core
    stays conventional and the only unusual line is installWifiFIFO().

    The contrast with the real project is the whole reason this exists. Here the ARM7 is
    ours, idle, and running a normal main loop. Inside nds-bootstrap it belongs to the game,
    it is already carrying the cardengine's own hooks, and nothing may block. If WiFi cannot
    be made to work in this easy case, the hard case is not worth attempting.

    This file is part of nds-bootstrap and is licensed under the GPL-3.0,
    the same terms as the rest of the project.
*/

#include <nds.h>
#include <string.h>

#include "dsiwifi7.h"

static volatile bool exitflag = false;

static void VcountHandler(void) {
	inputGetAndSend();
}

static void VblankHandler(void) {
}

static void powerButtonCB(void) {
	exitflag = true;
}

int main(void) {
	/*
	    The extended TWL I/O, opened the same way the reference ARM7 core does it. This is
	    what puts the WiFi SDIO block at 0x04004A00 in reach at all -- and it is the same
	    SCFG state nds-bootstrap keeps open for its own reasons, which is why the question
	    this probe answers is worth asking of nds-bootstrap afterwards.
	*/
	if (isDSiMode()) {
		REG_SCFG_ROM = 0x101;
		REG_SCFG_CLK = (BIT(0) | BIT(1) | BIT(2) | BIT(7) | BIT(8));
		REG_SCFG_EXT = 0x93FFFB06;
		*(vu16*)(0x04004012) = 0x1988;
		*(vu16*)(0x04004014) = 0x264C;
		*(vu16*)(0x04004C02) = 0x4000;   /* power button IRQ, for Unlaunch 1.3 */
	}

	*(vu16*)(0x04004700) |= BIT(13);     /* 48kHz sound/mic */

	dmaFillWords(0, (void*)0x04000400, 0x100);
	REG_SOUNDCNT |= SOUND_ENABLE;
	writePowerManagement(PM_CONTROL_REG,
	                     (readPowerManagement(PM_CONTROL_REG) & ~PM_SOUND_MUTE) | PM_SOUND_AMP);
	powerOn(POWER_SOUND);

	readUserSettings();
	ledBlink(0);

	irqInit();
	initClockIRQ();
	fifoInit();
	touchInit();

	SetYtrigger(80);

	installSoundFIFO();
	installSystemFIFO();

	irqSet(IRQ_VCOUNT, VcountHandler);
	irqSet(IRQ_VBLANK, VblankHandler);
	irqEnable(IRQ_VBLANK | IRQ_VCOUNT | IRQ_NETWORK);

	setPowerButtonCB(powerButtonCB);

	/* The one line that is not template. Everything dsiwifi does on this CPU hangs off it. */
	installWifiFIFO();

	while (!exitflag) {
		const u32 keys = REG_KEYINPUT;
		if (0 == (keys & (KEY_SELECT | KEY_START | KEY_L | KEY_R))) {
			exitflag = true;
		}
		swiWaitForVBlank();
	}
	return 0;
}
