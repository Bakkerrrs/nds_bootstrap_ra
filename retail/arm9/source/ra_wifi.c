/*
    Step two of the ladder, on the launcher's ARM9: ask nds-bootstrap's own ARM7 to bring
    the Atheros chip up, write down verbatim what it says, and stop on a summary.

    What this is *not* is a second copy of tools/wifiprobe/. The probe answered "does WiFi
    work on this console" and answered it completely, up to RetroAchievements replying over
    plain HTTP. This answers the one thing that run could not: whether dsiwifi comes up as a
    guest of nds-bootstrap's ARM7 rather than of the libnds template. Those differ in ways
    that all bear on the WiFi SDIO block --

      the launcher's ARM7 never writes SCFG; it inherits whatever launched it, where the
      probe sets REG_SCFG_EXT itself. If the extended TWL I/O is not open, the chip is not
      on the bus and nothing else matters, so that word is the first thing logged;

      it is already driving SD/eMMC through my_sdmmc at 0x04004800, one instance of the
      same IP block below the WiFi SDIO at 0x04004A00, with NDMA slot 0 taken;

      it owns the FIFO, and its idle loop is swiIntrWait rather than a main loop.

    So the ARM9 side here deliberately links nothing. dsiwifi's ARM9 half is lwip, and lwip
    is DHCP and sockets -- step three. Everything this step asks about happens on the ARM7,
    and all the ARM9 has to do is send one IPC message, read the narration, and count rungs.
    Two facts fall out of that, both useful: no library, and no 800K of lwip pools trying to
    fit in the launcher's 736K link region.

    The log is written to the card and it is meant to be diffed against /wifiprobe.log.
    That is the test design rather than a convenience -- "did the chip arrive in the same
    state under our boot path" is a text comparison between two runs of the same driver,
    which is a reading that cannot come out two ways.

    This file is part of nds-bootstrap and is licensed under the GPL-3.0,
    the same terms as the rest of the project.
*/

#include "ra_wifi.h"

#if RA_LAUNCHER_WIFI

#include <nds.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* dsiwifi's IPC contract, header-only. The library itself stays on the ARM7. */
#include "dsiwifi_cmds.h"

/* What the launcher's ARM7 stashed there at boot, before anything could change it. */
#define REG_SCFG_EXT7 *(u32*)0x02FFFDF0

/*
    The ARM7 says when installWifiFIFO() has returned. Without this the ARM9 could send
    INIT_IOP into a channel with no handler -- unlikely, since loadFromSD() has already
    spent hundreds of milliseconds on the card by the time we get here, but a race that
    would present as "the chip never answered" is exactly the kind this project has paid
    for before. It also diagnoses the honest mistake of rebuilding one CPU and not the
    other, which a bare timeout would blame on the hardware.
*/
#define RA_WIFI_ARM7_READY_CHANNEL FIFO_USER_04

/* Seconds. Generous, because the cost of being wrong is a run that reports the wrong rung. */
#define RA_WIFI_WAIT_ARM7   3
#define RA_WIFI_WAIT_CHIP   20
#define RA_WIFI_WAIT_LINK   40

static FILE*         logFile;
static raWifiVerdict verdict;

/*
    Everything goes to both the screen and the file, for the reason the probe found the hard
    way: the interesting line in a stack log is never the last one, and forty lines is where
    photographing a screen stops being reasonable.
*/
static void raWifiLog(const char* fmt, ...) {
	char    line[192];
	va_list args;

	va_start(args, fmt);
	vsniprintf(line, sizeof(line), fmt, args);
	va_end(args);

	iprintf("%s", line);
	if (logFile) {
		fputs(line, logFile);
		fflush(logFile);
	}
}

/*
    dsiwifi's narration, unedited. This is the channel that answers the question, so it is
    passed through rather than summarised -- the summary is derived from it afterwards and
    can be checked against it.
*/
static void raWifiLogRaw(const char* text) {
	iprintf("%s", text);
	if (logFile) {
		fputs(text, logFile);
		fflush(logFile);
	}
}

static void raWifiMsgHandler(int bytes, void* userdata) {
	Wifi_FifoMsgExt msg;

	if (bytes < 4 || bytes > (int)sizeof(msg)) {
		fifoGetDatamsg(FIFO_DSWIFI, bytes > (int)sizeof(msg) ? (int)sizeof(msg) : bytes, (u8*)&msg);
		return;
	}
	fifoGetDatamsg(FIFO_DSWIFI, bytes, (u8*)&msg);

	switch (msg.cmd) {
		case WIFI_IPCINT_DBGLOG:
			msg.log_str[sizeof(msg.log_str) - 1] = 0;
			raWifiLogRaw(msg.log_str);
			raWifiVerdictChunk(&verdict, msg.log_str);
			break;

		case WIFI_IPCINT_CONNECT:
			/* WMI_CONNECT_EVENT: associated. The WPA2 handshake is still to come. */
			verdict.associated = 1;
			break;

		case WIFI_IPCINT_READY:
			/* Sent at the end of wmi_post_handshake(): keys installed, link usable. */
			verdict.linkReady = 1;
			break;

		case WIFI_IPCINT_PKTDATA:
			/*
			    Ignored, and deliberately not acknowledged. wifi_host.c writes the
			    F00FF00F free-marker six bytes below the buffer it is handed -- correct
			    when the ARM9 supplied that buffer through INITBUFS, which this probe
			    never does. With no buffer supplied the pointer is into the ARM7's own
			    mbox scratch, and stamping a marker into it would corrupt the driver's
			    packet header. Dropping inbound IP is free here: there is no IP stack to
			    drop it into, and the WPA2 handshake never comes this way -- EAPOL is
			    handled entirely on the ARM7.
			*/
			break;

		default:
			break;
	}
}

/*
    The SCFG words, first, because they decide whether the rest of the run means anything.
    Reported and not corrected: opening SCFG here would make this a measurement of a boot
    path nds-bootstrap does not have. If the bit is closed, that is the finding.
*/
static void raWifiReportScfg(void) {
	raWifiLog("isDSiMode        %s\n", isDSiMode() ? "yes" : "no");
	raWifiLog("SCFG_EXT  ARM9   %08lX\n", (unsigned long)REG_SCFG_EXT);
	raWifiLog("SCFG_EXT  ARM7   %08lX\n", (unsigned long)REG_SCFG_EXT7);
	raWifiLog("SCFG_CLK  ARM9   %08lX\n", (unsigned long)REG_SCFG_CLK);
	raWifiLog("SCFG_ROM  ARM9   %08lX\n", (unsigned long)REG_SCFG_ROM);

	/*
	    BIT(18) is the bit nds-bootstrap's own my_installSystemFIFO() tests, in a
	    commented-out guard, before installing the SD/MMC handlers -- so on this codebase's
	    reading it is the SD/MMC enable. The WiFi SDIO at 0x04004A00 is a second instance of
	    that controller 0x200 higher, which makes it *likely* to be the same gate and not
	    certain. Printed as the fact it is rather than as the conclusion it suggests: if
	    this reads set and the chip still never answers, the two are not the same gate, and
	    that is worth knowing rather than assuming either way.
	*/
	raWifiLog("SCFG_EXT7 BIT(18) %s\n", (REG_SCFG_EXT7 & BIT(18)) ? "set" : "clear");

	if (REG_SCFG_EXT7 == 0) {
		raWifiLog("\x1b[31mARM7 has no SCFG access at all.\x1b[37m\n"
		          "the extended TWL I/O is closed, so the\n"
		          "WiFi SDIO block is not on the bus.\n");
	}
}

static bool raWifiWaitArm7(void) {
	int frames = RA_WIFI_WAIT_ARM7 * 60;

	while (frames-- > 0) {
		if (fifoCheckValue32(RA_WIFI_ARM7_READY_CHANNEL)) {
			return fifoGetValue32(RA_WIFI_ARM7_READY_CHANNEL) != 0;
		}
		swiWaitForVBlank();
	}
	return false;
}

/*
    Wait for a rung, and give up rather than hang -- every step below can fail by never
    completing, and a probe that hangs teaches nothing. The point of the run is to come back
    with a number.

    Deliberately does *not* flush the classifier's partial line while it waits. The log
    arrives from an interrupt, so a flush here would land at an arbitrary point in whatever
    line is mid-delivery and cut it in two -- and every string that decides a rung is longer
    than the fragment either half would leave. That would present as a chip that never came
    up, which is the worst way to be wrong. The only flush is at the end, where nothing more
    is coming.
*/
static bool raWifiWaitStage(int wanted, int seconds, const char* what) {
	int frames = seconds * 60;

	while (frames-- > 0) {
		if (raWifiVerdictStage(&verdict) >= wanted) {
			return true;
		}
		swiWaitForVBlank();
	}
	raWifiLog("\x1b[31mno %s after %ds\x1b[37m\n", what, seconds);
	return false;
}

void raWifiProbe(bool sdFound) {
	int stage;

	logFile = fopen(sdFound ? RA_WIFI_LOG_PATH : RA_WIFI_LOG_PATH_FAT, "w");

	iprintf("\x1b[33mnds-bootstrap RA WiFi, step 2\x1b[37m\n");
	iprintf("launcher context, no game running\n\n");
	if (logFile) {
		iprintf("logging to %s\n", sdFound ? RA_WIFI_LOG_PATH : RA_WIFI_LOG_PATH_FAT);
	} else {
		iprintf("\x1b[33mno log file; screen only\x1b[37m\n");
	}

	raWifiVerdictReset(&verdict);

	raWifiLog("\n-- stage 0: the bus --\n");
	raWifiReportScfg();

	raWifiLog("\n-- the ARM7 half --\n");
	fifoSetDatamsgHandler(FIFO_DSWIFI, raWifiMsgHandler, 0);

	if (!raWifiWaitArm7()) {
		raWifiLog("\x1b[31mthe ARM7 never reported installWifiFIFO().\x1b[37m\n"
		          "if only the ARM9 was rebuilt, rebuild both:\n"
		          "make RA_LAUNCHER_WIFI=1\n");
		goto done;
	}
	raWifiLog("installWifiFIFO() is in\n");

	/*
	    One message starts everything. wifi_card_handleMsg() turns dsiwifi's own logging on
	    and calls wifi_card_device_init(), which does the SDIO reset, BMI, the firmware
	    launch, the WMI handshake and the scan -- all on the ARM7, all narrated back here.
	*/
	raWifiLog("\n-- stage 1-3: bring the chip up --\n");
	{
		Wifi_FifoMsg msg;
		memset(&msg, 0, sizeof(msg));
		msg.cmd = WIFI_IPCCMD_INIT_IOP;
		fifoSendDatamsg(FIFO_DSWIFI, sizeof(msg), (u8*)&msg);
	}

	if (!raWifiWaitStage(RA_WIFI_STAGE_WMI, RA_WIFI_WAIT_CHIP, "WMI")) {
		goto done;
	}

	/*
	    Association is the AP's business as much as ours, and dsiwifi scans first, so this
	    is the slow rung. It is also the one the probe already passed on this console and
	    this network, which is what makes a failure here informative rather than ambiguous.
	*/
	raWifiLog("\n-- stage 4-5: associate and authenticate --\n");
	raWifiWaitStage(RA_WIFI_STAGE_READY, RA_WIFI_WAIT_LINK, "link");

done:
	raWifiVerdictFlush(&verdict);
	stage = raWifiVerdictStage(&verdict);

	raWifiLog("\n-- summary --\n");
	raWifiLog("chip             %s\n", verdict.chip[0] ? verdict.chip : "unidentified");
	raWifiLog("arrived          %s\n", raWifiVerdictArrival(&verdict));
	raWifiLog("BMI / launch     %s / %s\n",
	          verdict.bmiSeen ? "yes" : "no",
	          verdict.firmwareLaunched ? "yes" : "no");
	raWifiLog("firmware / WMI   %s / %s\n",
	          verdict.firmwareReady ? "ready" : "no",
	          verdict.wmiReady ? "ready" : "no");
	raWifiLog("associated       %s\n", verdict.associated ? "yes" : "no");
	raWifiLog("link ready       %s\n", verdict.linkReady ? "yes" : "no");
	if (verdict.mboxAllocFailed) {
		raWifiLog("\x1b[31mthe ARM7 could not allocate its mboxes\x1b[37m\n");
	}
	raWifiLog("log lines        %u\n", verdict.lines);

	raWifiLog("\n\x1b[33mreached stage %d of %d\x1b[37m\n", stage, RA_WIFI_STAGE_MAX);
	raWifiLog(stage >= RA_WIFI_STAGE_READY
	          ? "the chip comes up inside the launcher.\n"
	          : "stopped here -- see the log above.\n");

	/*
	    The file stays open, for the reason the probe learned by losing exactly the lines
	    that mattered: dsiwifi narrates asynchronously and keeps talking after the last rung
	    is decided. The first hardware run of the probe printed the line naming the access
	    point and its security mode *after* its summary.
	*/
	raWifiLog("\nthis build does not boot games.\n");

	/*
	    And it stops here. The ARM7 may be sitting in one of dsiwifi's untimed `while`
	    loops -- there are two in wifi_card_wlan_init(), waiting for the firmware-ready flag
	    and for WMI -- with a timer IRQ and an AUX IRQ live besides. Handing that to the
	    bootloader, which is about to overwrite the ARM7's code, is not something to do for
	    a measurement. See RA_LAUNCHER_WIFI in ra_wifi.h.
	*/
	while (1) {
		swiWaitForVBlank();
	}
}

#endif /* RA_LAUNCHER_WIFI */
