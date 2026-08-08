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
#include <unistd.h>

/*
    Step 3 links dsiwifi's ARM9 half after all -- see retail/dsiwifi9/, which rebuilds it with
    lwip's pools cut from 833K of .bss to 76K so it fits the launcher's link region.

    That changes who owns FIFO_DSWIFI. wifi_host_init() installs its own datamsg handler on
    that channel and drives the whole sequence itself, and two owners of one channel is not a
    thing -- so the probe stops speaking IPC and calls the same API tools/wifiprobe/ calls.
    Which is an improvement in its own right: the control and the measurement now go through
    the same entry points, so a difference between them cannot be ours.

    What is lost is that association and end-of-handshake used to arrive as the driver's own
    signals; now they are read out of its prose like everything else. See
    RA_WIFI_SAY_ASSOC in ra_wifi_verdict.c.
*/
#include "dsiwifi9.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

#define RA_HOST "retroachievements.org"
#define RA_PORT 80
/*
    A login for a user that does not exist, exactly as the probe does it. The reply is a 401
    with a JSON body, which proves DNS, TCP, HTTP and the API parsing our query without this
    program ever handling a real password. Over cleartext that distinction is worth keeping,
    and it is the reason step 3's first rung needs no credentials at all.
*/
#define RA_PATH "/dorequest.php?r=login&u=ndsbootstrap_probe&p=x"

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
#define RA_WIFI_WAIT_DHCP   30
#define RA_WIFI_WAIT_TAIL   8   /* after the summary: dsiwifi keeps narrating */

/*
    dsiwifi's narration is captured into RAM by the interrupt and written out by the main
    loop. Both halves of that split were learned from the first hardware run:

    The interrupt must not touch the card. On the DSi the SD is driven by the *ARM7*, over
    the FIFO -- so an fputs() inside a FIFO datamsg handler means FIFO traffic from inside a
    FIFO interrupt, against an ARM7 that is at that moment running a WiFi stack. The first
    run got away with it twice. That is not the same as it being safe, and step 4 puts the
    same code next to a game.

    16K because a full run of the probe narrates two or three. An overflow is counted and
    reported rather than silently wrapping: a log with a hole in it that does not say so is
    worse than a short one.
*/
#define RA_WIFI_TEXT_MAX 0x4000

static FILE*         logFile;
static raWifiVerdict verdict;

static char          textBuf[RA_WIFI_TEXT_MAX];
static volatile u32  textHead;      /* written only by the interrupt */
static u32           textTail;      /* read only by the main loop */
static volatile u32  textDropped;

/*
    Make what has been written survive a power-off, without closing the file.

    fflush() is not enough and the first hardware run proved it the expensive way: the run
    reached stage 5 of 5 on screen and left a **zero-byte** log. fflush() only pushes
    newlib's stdio buffer down into libfat's write(), which does write the data clusters --
    but a FAT file's length lives in its *directory entry*, and libfat only writes that from
    _FAT_syncToDisc(), reached from close() and fsync() and nothing else. This probe halts
    deliberately and never closes anything, so the bytes were on the card and the metadata
    said the file was empty.

    fsync() is therefore not a tidiness call, it is the fix. It also has to be per-write
    rather than at the end, because the thing most worth logging is a run that hangs:
    dsiwifi has two untimed loops in its bring-up, and a log that only becomes real at the
    end is exactly the log you do not get.
*/
static void raWifiSync(void) {
	if (logFile) {
		fflush(logFile);
		fsync(fileno(logFile));
	}
}

/*
    Move whatever the interrupt captured to the screen and the card. Main loop only.

    Copied out in bounded pieces because textBuf holds raw bytes with no terminator: the
    interrupt stores the string contents and not the NUL, so there is nothing to print until
    a piece is terminated here.
*/
static void raWifiDrain(void) {
	const u32 head = textHead;        /* snapshot: the interrupt may advance it as we go */
	char      piece[129];
	bool      moved = false;

	while (textTail < head) {
		u32 n = head - textTail;

		if (n > sizeof(piece) - 1) {
			n = sizeof(piece) - 1;
		}
		memcpy(piece, textBuf + textTail, n);
		piece[n] = 0;
		textTail += n;

		iprintf("%s", piece);
		if (logFile) {
			fputs(piece, logFile);
		}
		/*
		    Classified here rather than in the interrupt, so the handler stays a memcpy.
		    Reassembly across pieces is raWifiVerdictChunk()'s job either way, and the
		    boundaries it has to survive are the FIFO's, not this buffer's.
		*/
		raWifiVerdictChunk(&verdict, piece);
		moved = true;
	}

	if (moved) {
		raWifiSync();
	}
}

/*
    Our own lines. Drains first so the file reads in the order things happened rather than
    with the summary interleaved into whatever dsiwifi was mid-sentence about.
*/
static void raWifiLog(const char* fmt, ...) {
	char    line[192];
	va_list args;

	raWifiDrain();

	va_start(args, fmt);
	vsniprintf(line, sizeof(line), fmt, args);
	va_end(args);

	iprintf("%s", line);
	if (logFile) {
		fputs(line, logFile);
	}
	raWifiSync();
}

/*
    dsiwifi's narration, unedited, captured from the interrupt. This is the channel that
    answers the question, so it is passed through rather than summarised -- the summary is
    derived from it afterwards and can be checked against it.

    No I/O here. See RA_WIFI_TEXT_MAX for why that matters more than it looks.
*/
static void raWifiCapture(const char* text) {
	u32 i;

	for (i = 0; text[i]; i++) {
		if (textHead >= sizeof(textBuf)) {
			textDropped++;
			return;
		}
		textBuf[textHead++] = text[i];
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

/* Drain once per frame, so a hang leaves everything up to it already on the card. */
static void raWifiIdle(void) {
	swiWaitForVBlank();
	raWifiDrain();
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
		raWifiIdle();
	}
	raWifiLog("\x1b[31mno %s after %ds\x1b[37m\n", what, seconds);
	return false;
}

static void raWifiReportIp(const char* what, u32 addr) {
	const u8* b = (const u8*)&addr;

	raWifiLog("%-16s %u.%u.%u.%u\n", what, b[0], b[1], b[2], b[3]);
}

/*
    DHCP, and it is lwip's first real test rather than a formality: everything below this
    point ran without an IP stack in step 2, so a failure here is the new code and not the
    driver.
*/
static bool raWifiWaitIp(int seconds) {
	int frames = seconds * 60;

	while (frames-- > 0) {
		const u32 addr = DSiWifi_GetIP();

		if (addr != 0 && addr != 0xFFFFFFFF) {
			verdict.ip    = addr;
			verdict.gotIp = 1;
			raWifiReportIp("IP", addr);
			return true;
		}
		raWifiIdle();
	}
	raWifiLog("\x1b[31mno IP after %ds\x1b[37m\n", seconds);
	return false;
}

/*
    One GET, and the reply is checked for *content*. A captive portal, a proxy or a Cloudflare
    interstitial all succeed at the socket level and mean nothing; the API's own error code
    coming back is what proves we reached RetroAchievements rather than something answering on
    its behalf. Same test the probe applies, for the same reason.
*/
static void raWifiHttpGet(void) {
	static char        response[2048];
	struct hostent*    he;
	struct sockaddr_in addr;
	char               request[320];
	int                sock;
	int                total = 0;

	he = gethostbyname(RA_HOST);
	if (!he || !he->h_addr_list[0]) {
		raWifiLog("\x1b[31mDNS failed for %s\x1b[37m\n", RA_HOST);
		return;
	}
	memcpy(&addr.sin_addr, he->h_addr_list[0], 4);
	verdict.dnsOk = 1;
	raWifiLog("resolved         %s\n", RA_HOST);
	raWifiReportIp("its address", addr.sin_addr.s_addr);

	sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0) {
		raWifiLog("\x1b[31msocket() failed\x1b[37m\n");
		return;
	}
	addr.sin_family = AF_INET;
	addr.sin_port   = htons(RA_PORT);

	if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
		raWifiLog("\x1b[31mconnect() to port %d failed\x1b[37m\n", RA_PORT);
		lwip_close(sock);
		return;
	}
	verdict.tcpOk = 1;
	raWifiLog("connected        port %d\n", RA_PORT);

	/* A user agent is mandatory on every Connect API call; without one it is our fault. */
	siprintf(request,
	         "GET %s HTTP/1.1\r\n"
	         "Host: %s\r\n"
	         "User-Agent: nds-bootstrap-ra-launcher/0.1\r\n"
	         "Connection: close\r\n"
	         "\r\n",
	         RA_PATH, RA_HOST);

	if (send(sock, request, strlen(request), 0) < 0) {
		raWifiLog("\x1b[31msend() failed\x1b[37m\n");
		lwip_close(sock);
		return;
	}
	raWifiLog("request sent\n");

	while (total < (int)sizeof(response) - 1) {
		const int got = recv(sock, response + total, sizeof(response) - 1 - total, 0);

		if (got <= 0) {
			break;
		}
		total += got;
	}
	response[total] = 0;
	lwip_close(sock);

	raWifiLog("%d bytes back\n", total);
	if (strstr(response, "invalid_credentials")) {
		verdict.apiOk = 1;
		raWifiLog("\x1b[32mthe API answered over plain HTTP\x1b[37m\n");
	} else if (total > 0) {
		raWifiLog("\x1b[31mreply is not the API\x1b[37m\n");
	}
	{
		const char* body = strstr(response, "\r\n\r\n");
		raWifiLog("body: %s\n", body ? body + 4 : response);
	}
}

void raWifiProbe(bool sdFound) {
	int stage;

	logFile = fopen(sdFound ? RA_WIFI_LOG_PATH : RA_WIFI_LOG_PATH_FAT, "w");

	iprintf("\x1b[33mnds-bootstrap RA WiFi, step 3\x1b[37m\n");
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

	if (!raWifiWaitArm7()) {
		raWifiLog("\x1b[31mthe ARM7 never reported installWifiFIFO().\x1b[37m\n"
		          "if only the ARM9 was rebuilt, rebuild both:\n"
		          "make RA_LAUNCHER_WIFI=1\n");
		goto done;
	}
	raWifiLog("installWifiFIFO() is in\n");

	/*
	    One call starts everything, and it is the same call tools/wifiprobe/ makes.
	    DSiWifi_InitDefault() installs dsiwifi's own handler on FIFO_DSWIFI, sends INIT_IOP,
	    and from there the ARM7 does the SDIO reset, BMI, the firmware launch, WMI and the
	    scan while the ARM9 half brings lwip up and starts DHCP. All of it narrates through
	    the log handler.
	*/
	raWifiLog("\n-- stage 1-5: bring the chip up and associate --\n");
	DSiWifi_SetLogHandler(raWifiCapture);
	DSiWifi_InitDefault(WFC_CONNECT);

	if (!raWifiWaitStage(RA_WIFI_STAGE_WMI, RA_WIFI_WAIT_CHIP, "WMI")) {
		goto done;
	}
	if (!raWifiWaitStage(RA_WIFI_STAGE_READY, RA_WIFI_WAIT_LINK, "link")) {
		goto done;
	}

	/*
	    And here step 3 begins: everything above this line was proven in step 2 without an IP
	    stack, so anything that fails from now on is lwip in the launcher -- which is the one
	    thing this round exists to find out.
	*/
	raWifiLog("\n-- stage 6: DHCP --\n");
	if (!raWifiWaitIp(RA_WIFI_WAIT_DHCP)) {
		goto done;
	}

	raWifiLog("\n-- stage 7-9: reach the API over plain HTTP --\n");
	raWifiHttpGet();

done:
	/*
	    Give the tail a chance before the summary rather than after it. dsiwifi narrates
	    asynchronously and keeps talking once the last rung is decided -- the probe's first
	    hardware run printed the line naming the access point and its security mode *after*
	    its own summary. Draining here means those lines land above the summary they inform,
	    and the summary is computed from a log that is actually finished.
	*/
	{
		int frames = RA_WIFI_WAIT_TAIL * 60;
		while (frames-- > 0) {
			raWifiIdle();
		}
	}
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
	if (verdict.gotIp) {
		raWifiReportIp("IP", verdict.ip);
	} else {
		raWifiLog("IP               none\n");
	}
	raWifiLog("DNS / TCP / API  %s / %s / %s\n",
	          verdict.dnsOk ? "ok" : "no",
	          verdict.tcpOk ? "ok" : "no",
	          verdict.apiOk ? "ok" : "no");
	if (verdict.mboxAllocFailed) {
		raWifiLog("\x1b[31mthe ARM7 could not allocate its mboxes\x1b[37m\n");
	}
	raWifiLog("log lines        %u\n", verdict.lines);
	if (textDropped) {
		raWifiLog("\x1b[31m%lu chars dropped: the log has a hole\x1b[37m\n",
		          (unsigned long)textDropped);
	}

	raWifiLog("\n\x1b[33mreached stage %d of %d\x1b[37m\n", stage, RA_WIFI_STAGE_MAX);
	if (stage >= RA_WIFI_STAGE_ANSWERED) {
		raWifiLog("the launcher can reach RetroAchievements.\n");
	} else if (stage >= RA_WIFI_STAGE_READY) {
		raWifiLog("the link is up; the IP stack is where it stopped.\n");
	} else {
		raWifiLog("stopped here -- see the log above.\n");
	}

	/*
	    Said explicitly because the first run could not say it, and the run was wasted for
	    exactly that reason: the log is fsync()'d, so it is complete on the card right now,
	    with the file still open. Powering off here does not lose it.
	*/
	raWifiSync();
	raWifiLog("\nlog written and synced -- safe to power off.\n"
	          "this build does not boot games.\n");

	/*
	    And it stops here. The ARM7 may be sitting in one of dsiwifi's untimed `while`
	    loops -- there are two in wifi_card_wlan_init(), waiting for the firmware-ready flag
	    and for WMI -- with a timer IRQ and an AUX IRQ live besides. Handing that to the
	    bootloader, which is about to overwrite the ARM7's code, is not something to do for
	    a measurement. See RA_LAUNCHER_WIFI in ra_wifi.h.

	    Kept draining rather than spinning idle, so a line dsiwifi emits a minute from now
	    still reaches the card. The file is deliberately never closed -- with fsync() per
	    write there is nothing a close would add, and closing would silently drop exactly
	    those late lines.
	*/
	while (1) {
		raWifiIdle();
	}
}

#endif /* RA_LAUNCHER_WIFI */
