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
#include <malloc.h>

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
#include "locations.h"

#include "dsiwifi9.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

/* The host and port live in ra_wifi.h now, with the rest of ra_net's contract. */
/*
    A login for a user that does not exist, exactly as the probe does it. The reply is a 401
    with a JSON body, which proves DNS, TCP, HTTP and the API parsing our query without this
    program ever handling a real password. Over cleartext that distinction is worth keeping,
    and it is the reason step 3's first rung needs no credentials at all.
*/
#define RA_PATH_ANON "/dorequest.php?r=login&u=ndsbootstrap_probe&p=x"

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
/* Mode 2's teardown handshake. Must match retail/arm7/source/ra_wifi7.c. */
#define RA_WIFI_ARM7_STOP_CHANNEL  FIFO_USER_07
#define RA_WIFI_WAIT_STOP          3   /* seconds */

/* Seconds. Generous, because the cost of being wrong is a run that reports the wrong rung. */
#define RA_WIFI_WAIT_ARM7   3
#define RA_WIFI_WAIT_CHIP   20
#define RA_WIFI_WAIT_LINK   40
#define RA_WIFI_WAIT_DHCP   30
#define RA_WIFI_WAIT_TAIL   8   /* after the summary: dsiwifi keeps narrating */
/*
    Per-recv(), not for the whole reply. Long enough that a slow server is not mistaken for a
    lost FIN, short enough that the run still ends: the reply itself arrives in one packet.
*/
#define RA_WIFI_RECV_TIMEOUT 5

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

/* Drains happen once a frame; sync every eighth that moved bytes, so roughly 8 per second. */
#define RA_WIFI_SYNC_EVERY 8

static u32           syncPending;
/* Stage 0b computes it; stage 11 asks the server about it. */
static char          romHash[33];
/*
    Stage 10 gets it; stage 12 spends it. Hoisted out of raWifiLogin() for that reason and for
    no other -- and it is the one static in this file that must never be logged. See
    raConfigRedact(): a token is the account.
*/
static char          raToken[64];
/*
    Achievements the account already holds. 128 because RA_DEFS_MAX_LINES is 128 and a skip list
    longer than the set it filters would be pointless; a bigger set truncates and says so.
*/
#define RA_WIFI_UNLOCKS_MAX 128
static u32           unlockedIds[RA_WIFI_UNLOCKS_MAX];
static u16           unlockCount;
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

	/*
	    Synced when a line lands, but not more than a few times a second.

	    Every fsync() is an SD transaction, and on the DSi the SD is the *ARM7's* -- served
	    over the FIFO by the same CPU that is at that moment running dsiwifi's timer, its SDIO
	    interrupt and a WPA2 link. One v6 run froze immediately after the summary's heap line,
	    which is SD I/O with the network live, and the run before it did not. That is the
	    contention #1d worried about, seen for the first time, and it is the one hazard step 4
	    inherits directly.

	    Fewer transactions is not a fix and is not claimed as one -- a blocking libfat call
	    cannot be bounded by a frame counter. It is less exposure while the reading is being
	    taken, which is worth having until the contention itself is understood.
	*/
	if (moved && ++syncPending >= RA_WIFI_SYNC_EVERY) {
		syncPending = 0;
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

/*
    How much heap is left, at the moments it changes.

    Reported because it is the number the ladder keeps spending: lwip's send path allocates from
    the same malloc as libfat and the launcher's own strings, and the static side of the region
    has grown from 569,136 to 598,220 of 753,664 across steps 3b, 3c and 3d -- every static
    response buffer and the scanner's carry buffer raises the floor the heap starts from. Better
    to have the figure from a run that worked than to discover it from one that did not.

    Step 3d turned out to need none of it: the set is streamed into the staging block and the
    recv() window is static, so the largest reply in this project allocates nothing. That was
    the design intent and this line is how it gets checked rather than assumed.
*/
static void raWifiReportHeap(const char* when) {
	/*
	    Third attempt at this line, and the two wrong versions are worth keeping in view
	    because each was wrong in a way that mattered.

	    `mallinfo().fordblks of .arena` reads as almost-out-of-memory: `arena` is what newlib
	    has claimed by sbrk() so far, not what is available. Then `fake_heap_end - sbrk(0)`
	    reported **13.4 MB**, which is true and useless -- libnds sets `fake_heap_end` from
	    main RAM and knows nothing about nds-bootstrap's link region. It was measured at
	    0x02FF3794.

	    What actually bounds the heap here is neither: it is `IMAGES_LOCATION`. The launcher
	    decompresses the boot images to 0x02338000 for the bootloader to display later, and
	    `ds_arm9_ndsbs.mem` ends the link region at exactly that address -- but nothing
	    *enforces* it, because the enforcement libnds would do lives in `fake_heap_end` and
	    that has been set 12.8 MB higher. So malloc will grow straight through the images, and
	    then through the RA staging block at 0x02600000, the cardengine staging at 0x026F0000
	    and the ARM7's at 0x027B2000, silently, and the failure would appear as a corrupt boot
	    screen or a cardengine that starts and dies.

	    So the number reported is the distance to `IMAGES_LOCATION`, and `fake_heap_end` is
	    printed beside it precisely so the gap between the two is visible rather than
	    reassuring.
	*/
	extern char*          fake_heap_end;
	const struct mallinfo mi   = mallinfo();
	char* const           top  = (char*)sbrk(0);
	char* const           safe = (char*)IMAGES_LOCATION;

	raWifiLog("heap %-11s %ld safe + %lu free, top %08lX, limit %08lX\n", when,
	          (long)(safe - top), (unsigned long)mi.fordblks,
	          (unsigned long)top, (unsigned long)fake_heap_end);

	if (top >= safe) {
		raWifiLog("\x1b[31mthe heap has passed IMAGES_LOCATION\x1b[37m\n");
	}
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
    Say when a request needed more than one connect.

    Printed only when it happened, because the interesting reading is the frequency: a run of the
    ladder once had its third socket lose a race inside lwip -- see RA_NET_CONNECT_TRIES -- and a
    retry that succeeds silently would turn that into an impression instead of a number.
*/
static void raWifiReportAttempts(const char* what, const raNetProgress* p) {
	if (p->attempts > 1) {
		raWifiLog("\x1b[33m%s needed %u connect attempts\x1b[37m\n", what, p->attempts);
	}
}

/*
    Stage 7-9: reach the API, with no credentials at all.

    The request logs in as a user that does not exist, which is the same thing
    tools/wifiprobe/ does and for the same reason: a well-formed `invalid_credentials` proves
    DNS, TCP, HTTP and the API parsing our query, without the reading depending on the config
    file being right. So when the login below fails, the failure is the login.

    The reply is checked for *content*, not for bytes. A captive portal, a proxy or a
    Cloudflare interstitial all succeed at the socket level and mean nothing.
*/
static void raWifiReachApi(void) {
	static char   response[2048];
	raNetProgress p;
	int           got;

	memset(&p, 0, sizeof(p));
	got = raNetHttpGet(RA_NET_HOST, RA_PATH_ANON, response, sizeof(response), &p);

	verdict.dnsOk = p.resolved;
	verdict.tcpOk = p.connected;
	if (p.resolved) {
		raWifiLog("resolved         %s\n", RA_NET_HOST);
		raWifiReportIp("its address", p.address);
	}
	if (p.connected) {
		raWifiLog("connected        port %d\n", RA_NET_PORT);
	}
	raWifiReportAttempts("the probe", &p);
	if (p.sent) {
		raWifiLog("request sent\n");
	}
	if (got < 0) {
		raWifiLog("\x1b[31mHTTP failed at step %d\x1b[37m\n", -got);
		return;
	}
	raWifiLog("%s after %d\n", p.closedByPeer ? "peer closed" : "recv stopped", got);

	if (strstr(response, "invalid_credentials")) {
		verdict.apiOk = 1;
		raWifiLog("\x1b[32mthe API answered over plain HTTP\x1b[37m\n");
	} else if (got > 0) {
		raWifiLog("\x1b[31mreply is not the API\x1b[37m\n");
	}
	raWifiLog("body: %s\n", raNetBody(response));
}

/*
    Stage 10: r=login, with the credentials from ra.cfg.

    The password is percent-encoded rather than pasted in, and that is the one detail in this
    function that can fail convincingly: an `&` in a password ends the parameter early, a `+`
    decodes as a space, a `%` opens an escape that is not there. Each produces a well-formed
    request that comes back `invalid_credentials` -- so the user would be told their password
    is wrong when it is not. tools/ra_launcher_test.c pins the encoder against exactly those.

    Nothing secret is logged. Not the password, and not the token either: a token grants the
    same power over the account, and this log is a file whose purpose is to be sent to someone.
    A length answers the only question a log needs to answer.
*/
static void raWifiLogin(const raConfig* cfg) {
	static char   response[2048];
	char          user[3 * sizeof(cfg->username)];
	char          pass[3 * sizeof(cfg->password)];
	char          path[320];
	raNetProgress p;
	int           got;

	if (!cfg->usable) {
		raWifiLog(cfg->found
		          ? "\x1b[33mra.cfg has no username/password; login skipped\x1b[37m\n"
		          : "\x1b[33mno ra.cfg; login skipped\x1b[37m\n");
		return;
	}

	if (!raNetUrlEncode(cfg->username, user, sizeof(user))
	 || !raNetUrlEncode(cfg->password, pass, sizeof(pass))) {
		raWifiLog("\x1b[31mcredentials too long to encode\x1b[37m\n");
		return;
	}
	if (sniprintf(path, sizeof(path), "/dorequest.php?r=login&u=%s&p=%s", user, pass)
	    >= (int)sizeof(path)) {
		raWifiLog("\x1b[31mlogin request too long\x1b[37m\n");
		return;
	}

	raWifiLog("logging in as    %s\n", cfg->username);

	memset(&p, 0, sizeof(p));
	got = raNetHttpGet(RA_NET_HOST, path, response, sizeof(response), &p);
	raWifiReportAttempts("login", &p);
	if (got < 0) {
		raWifiLog("\x1b[31mlogin HTTP failed at step %d\x1b[37m\n", -got);
		return;
	}
	raWifiLog("%d bytes back\n", got);

	if (raNetJsonString(response, "Token", raToken, sizeof(raToken))) {
		verdict.loggedIn = 1;
		raWifiLog("\x1b[32mlogged in, token %s\x1b[37m\n", raConfigRedact(raToken));
		return;
	}

	/*
	    The API's own error, verbatim -- it distinguishes a wrong password from a banned
	    account from a malformed request, and guessing between those from a console is exactly
	    what this project does not do.
	*/
	{
		char code[64];

		if (raNetJsonString(response, "Code", code, sizeof(code))) {
			raWifiLog("\x1b[31mlogin refused: %s\x1b[37m\n", code);
		} else {
			raWifiLog("\x1b[31mno token in the reply\x1b[37m\n");
		}
		raWifiLog("body: %s\n", raNetBody(response));
	}
}

/*
    Stage 11: r=gameid, which turns the ROM's hash into the server's own verdict on it.

    `r=patch` needs a GameID and this is where it comes from, but the reason to do it as its own
    rung is that it answers a question nothing local can. Step 3b proved the hash matches what
    rcheevos computes, and the user checked it by eye against the set's page. Neither of those is
    the server saying "I know this dump", and the difference matters: a hash that RetroAchievements
    does not recognise is what a trimmed, translated or differently-patched ROM produces, and it
    looks exactly like a game with no achievement set.

    No credentials needed -- this request is unauthenticated -- so it is deliberately independent
    of whether the login above worked.

    **A GameID of zero is an answer, not an error.** It is what the API returns for a hash it does
    not know, and saying so plainly is the whole value of this rung.
*/
static void raWifiIdentify(void) {
	static char   response[1024];
	char          path[128];
	raNetProgress p;
	int           got;
	u32           gameId = 0;

	if (!romHash[0]) {
		raWifiLog("\x1b[33mno ROM hash; identification skipped\x1b[37m\n");
		return;
	}
	if (sniprintf(path, sizeof(path), "/dorequest.php?r=gameid&m=%s", romHash)
	    >= (int)sizeof(path)) {
		raWifiLog("\x1b[31mgameid request too long\x1b[37m\n");
		return;
	}

	raWifiLog("asking about     %s\n", romHash);

	memset(&p, 0, sizeof(p));
	got = raNetHttpGet(RA_NET_HOST, path, response, sizeof(response), &p);
	raWifiReportAttempts("gameid", &p);
	if (got < 0) {
		raWifiLog("\x1b[31mgameid HTTP failed at step %d\x1b[37m\n", -got);
		return;
	}
	raWifiLog("%d bytes back\n", got);

	if (!raNetJsonNumber(response, "GameID", &gameId)) {
		raWifiLog("\x1b[31mno GameID in the reply\x1b[37m\n");
		raWifiLog("body: %s\n", raNetBody(response));
		return;
	}

	verdict.gameId = gameId;
	if (gameId == 0) {
		/*
		    Not a failure of ours. The request worked, the API answered, and the answer is that
		    it has never seen this dump -- so there is no set to fetch and 3d would have nothing
		    to do. Said in full, because the next move is to find the supported ROM rather than
		    to debug any of this.
		*/
		raWifiLog("\x1b[33mthe server does not know this hash\x1b[37m\n"
		          "the dump is not one the set covers.\n");
		raWifiLog("body: %s\n", raNetBody(response));
		return;
	}

	verdict.identified = 1;
	raWifiLog("\x1b[32mGameID           %lu\x1b[37m\n", (unsigned long)gameId);
}

/*
    Stage 12: r=unlocks -- which of these has the account already earned.

    Runs before the fetch, because that is the only order in which it helps: the block is 88% full
    with a set of this size, and every definition already earned is one that does not need to be in
    it. The arena and the per-frame budget follow the block down.

    **Fails open.** A request that does not answer leaves the skip list empty and every definition is
    staged, which is exactly what happened before this stage existed. The distinction the code cares
    about is between "the account has earned nothing" and "we could not ask" -- both stage everything,
    and only the second is worth a warning.

    h=0 because this fork is softcore, which ra.cfg says and the server independently agrees with:
    see the `Warning: Unknown Emulator` notice.
*/
static void raWifiUnlocks(const raConfig* cfg) {
	static char   response[4096];
	char          user[3 * sizeof(cfg->username)];
	char          path[384];
	raNetProgress p;
	int           got;
	int           count;

	unlockCount = 0;

	if (!verdict.gameId || !raToken[0] || !raNetUrlEncode(cfg->username, user, sizeof(user))) {
		raWifiLog("\x1b[33mno GameID or token; staging the whole set\x1b[37m\n");
		return;
	}
	if (sniprintf(path, sizeof(path), "/dorequest.php?r=unlocks&u=%s&t=%s&g=%lu&h=0",
	              user, raToken, (unsigned long)verdict.gameId) >= (int)sizeof(path)) {
		raWifiLog("\x1b[31munlocks request too long\x1b[37m\n");
		return;
	}

	memset(&p, 0, sizeof(p));
	got = raNetHttpGet(RA_NET_HOST, path, response, sizeof(response), &p);
	raWifiReportAttempts("unlocks", &p);
	if (got < 0) {
		raWifiLog("\x1b[33munlocks HTTP failed at step %d; staging the whole set\x1b[37m\n", -got);
		return;
	}
	raWifiLog("%d bytes back\n", got);

	count = raNetJsonIdList(response, "UserUnlocks", unlockedIds, RA_WIFI_UNLOCKS_MAX);
	if (count < 0) {
		/*
		    No UserUnlocks key at all, which is a failure rather than an empty account -- the two
		    look identical from the skip list and mean different things about whether to trust it.
		*/
		raWifiLog("\x1b[33mno UserUnlocks in the reply; staging the whole set\x1b[37m\n");
		raWifiLog("body: %s\n", raNetBody(response));
		return;
	}

	unlockCount           = (u16)count;
	verdict.unlocksKnown  = 1;
	verdict.unlockCount   = unlockCount;
	raWifiLog("\x1b[32malready earned    %d\x1b[37m\n", count);

	/*
	    The ids themselves, and this line exists because a run reported `already earned 1` while the
	    fetch kept all 55 -- so the server named an achievement that is not in the set we staged. A
	    count cannot say which, and *which* is the only thing that distinguishes the possibilities:
	    an unofficial achievement we filter, one belonging to a different subset of this game, or a
	    numbering that does not match `r=patch`'s at all.

	    Eight at most, because the point is to identify a mismatch rather than to dump a full account.
	*/
	{
		int shown = (count < 8) ? count : 8;
		int i;

		for (i = 0; i < shown; i++) {
			raWifiLog("  earned id      %lu\n", (unsigned long)unlockedIds[i]);
		}
		if (count > shown) {
			raWifiLog("  ...and %d more\n", count - shown);
		}
	}
	if (count >= RA_WIFI_UNLOCKS_MAX) {
		/*
		    Truncation is safe and is still said out loud: a short skip list only means a few
		    already-earned achievements get staged again, which costs block space rather than
		    correctness.
		*/
		raWifiLog("\x1b[33mskip list full at %d; some will be staged again\x1b[37m\n",
		          RA_WIFI_UNLOCKS_MAX);
	}
}

/*
    Stage 13: r=patch -- fetch the real achievement set, and put it where the game will find it.

    This is the rung the whole ladder was for. Everything below it was a measurement; this
    produces the artifact, and it produces it into the same staging block a hand-written
    ra_achievements.txt already goes to -- CARDENGINEI_ARM9_RA_DEFS_BUFFERED_LOCATION, with the
    same header, written by the same rules. The bootloader copies that into DSi WRAM and the
    cardengine's reader splits it into lines. None of that path is new, which is the point of
    having built it for a file first.

    The reply is never held. See ra_patch.c: it is over 100 K for this game, the heap has about
    85 K left, and the destination is 32 K -- so the definitions are pulled out of the byte
    stream as it arrives and the rest is discarded going past.

    Two things this deliberately does not do. It does not log the reply, because logging what
    does not fit in memory to a card over a live WiFi link is the one thing #1d says not to do,
    and because the definitions themselves are the evidence. And it does not log the token: the
    request carries it, the log records that a request was made.
*/
/*
    The fetch failed, so put the user's own file back.

    Stage 12 streams the reply straight into the definitions block, which is where
    loadRaDefinitions() had already put sd:/_nds/nds-bootstrap/ra_achievements.txt -- so by the time
    a fetch fails, the file's text is already gone. In mode 1 that did not matter, because that build
    never boots a game. In mode 2 it means a console with no network would lose the definitions it
    would otherwise have run, which is strictly worse than not having tried.

    Re-staging rather than preserving, because the reply is three times the size of the block and
    there is nowhere else to scan it. Cheap: one file read, and only on the path that already failed.
*/
static void raWifiRestoreDefinitionsFile(void) {
	loadRaDefinitions();
	if (*(u32*)CARDENGINEI_ARM9_RA_DEFS_BUFFERED_LOCATION == CARDENGINEI_ARM9_RA_DEFS_MAGIC) {
		raWifiLog("restored         ra_achievements.txt (%lu bytes)\n",
		          (unsigned long)*(u32*)(CARDENGINEI_ARM9_RA_DEFS_BUFFERED_LOCATION + 4));
	}
}

/*
    A heartbeat on the screen while the set streams in, and screen *only*.

    The recv() loop cannot yield -- it blocks on the socket -- so nothing else can report
    progress while a hundred kilobytes come down, and a run that stalls halfway would look
    identical to one that never started. A dot every 8 K makes the difference visible without a
    single SD transaction: iprintf() here is writes to the console's own memory, where
    raWifiLog() would be libfat and an fsync() over the FIFO to the ARM7 that is at that moment
    running the WiFi stack. That combination is what froze a v6 run.
*/
static u32 patchProgress;

static void raWifiPatchSink(void* ctx, const char* data, int length) {
	raPatchFeed(ctx, data, length);

	patchProgress += (u32)length;
	if (patchProgress >= 8192) {
		patchProgress = 0;
		iprintf(".");
	}
}

static void raWifiFetchPatch(const raConfig* cfg) {
	static raPatch patch;
	char           user[3 * sizeof(cfg->username)];
	char           path[384];
	char* const    block = (char*)(CARDENGINEI_ARM9_RA_DEFS_BUFFERED_LOCATION
	                               + CARDENGINEI_ARM9_RA_DEFS_HEADER);
	const u32      blockMax = CARDENGINEI_ARM9_RA_DEFS_MAX
	                          - CARDENGINEI_ARM9_RA_DEFS_HEADER - 1;
	raNetProgress  p;
	int            got;

	if (!verdict.gameId) {
		raWifiLog("\x1b[33mno GameID; the set cannot be asked for\x1b[37m\n");
		return;
	}
	if (!raToken[0]) {
		/*
		    r=patch is the first request in this ladder that needs credentials. Said plainly
		    because the previous rung succeeds without them, so "the server knew the ROM but the
		    set did not arrive" would otherwise look like a server problem.
		*/
		raWifiLog("\x1b[33mno token; r=patch needs a login\x1b[37m\n");
		return;
	}
	if (!raNetUrlEncode(cfg->username, user, sizeof(user))) {
		raWifiLog("\x1b[31musername too long to encode\x1b[37m\n");
		return;
	}
	/*
	    The token goes in unencoded. RA's tokens are alphanumeric, and encoding one would be
	    fine -- but it would mean copying a secret into a second buffer for no reason, and this
	    file is careful about how many places hold one.
	*/
	if (sniprintf(path, sizeof(path), "/dorequest.php?r=patch&u=%s&t=%s&g=%lu",
	              user, raToken, (unsigned long)verdict.gameId) >= (int)sizeof(path)) {
		raWifiLog("\x1b[31mpatch request too long\x1b[37m\n");
		return;
	}

	raWifiLog("asking for       game %lu\n", (unsigned long)verdict.gameId);
	/*
	    Zeroed before the request rather than after it, so a reply that never comes leaves the
	    block invalid instead of leaving whatever was there looking valid.
	*/
	*(u32*)CARDENGINEI_ARM9_RA_DEFS_BUFFERED_LOCATION = 0;
	raPatchReset(&patch, block, blockMax);
	/*
	    Set after the reset, which zeroes the struct. Empty when r=unlocks did not answer, and then
	    the scanner behaves exactly as it did before that stage existed.
	*/
	patch.skipIds   = unlockedIds;
	patch.skipCount = unlockCount;
	patchProgress   = 0;

	memset(&p, 0, sizeof(p));
	got = raNetHttpGetStream(RA_NET_HOST, path, raWifiPatchSink, &patch, &p);
	raPatchFinish(&patch);
	iprintf("\n");
	raWifiReportAttempts("patch", &p);

	if (got < -1000) {
		raWifiLog("\x1b[31mthe server answered HTTP %d\x1b[37m\n", -got - 1000);
		raWifiRestoreDefinitionsFile();
		return;
	}
	if (got < 0) {
		raWifiLog("\x1b[31mpatch HTTP failed at step %d\x1b[37m\n", -got);
		raWifiRestoreDefinitionsFile();
		return;
	}

	raWifiLog("body was         %d bytes\n", got);
	raWifiLog("definitions      %u kept, %u unofficial\n",
	          patch.kept, patch.unofficial);
	/*
	    The reading step 5 exists for. A set where every definition carried its own achievement id is
	    a set that can be reported on; one id short is one achievement that can never be awarded, and
	    the block alone cannot say which of those happened.
	*/
	raWifiLog("ids              %u with, %u without\n", patch.withId, patch.withoutId);
	if (patch.skipCount) {
		/*
		    Printed whenever there was a skip list at all, matched or not. A missing line used to mean
		    "nothing matched", which is a reading by absence -- and absence is indistinguishable from
		    a line that was never written. `0 of 1` says the thing that a silence only implies.
		*/
		raWifiLog("already earned   %u of %u matched this set\n",
		          patch.alreadyDone, patch.skipCount);
		if (patch.alreadyDone == 0) {
			raWifiLog("\x1b[33mthe server named ids this set does not contain\x1b[37m\n");
		}
	}
	/*
	    The one this project cannot explain, with the reply's own bytes around it. Two lookups
	    established that the set publishes 55 achievements while 56 core definitions arrive, and that
	    the extra id returns NOT FOUND -- so what is left is to see what object it is in, verbatim,
	    rather than to infer a filter from a threshold. See RA_ODD_ID_FROM.
	*/
	if (patch.oddIds) {
		/*
		    The server's own message to the player, passed through rather than swallowed. The first
		    one read "Warning: Unknown Emulator -- Hardcore unlocks cannot be earned using this
		    emulator", which is RetroAchievements telling us it does not recognise the User-Agent
		    this client sends. Worth seeing on every run, because it is the server's answer to a
		    question this project has not asked it yet.
		*/
		raWifiLog("\x1b[33m%u server notice(s) dropped, first id %lu\x1b[37m\n",
		          patch.oddIds, (unsigned long)patch.oddId);
		raWifiLog("context: %s\n", patch.oddContext);
	}
	if (patch.dropped || patch.tooLong || patch.cutShort || patch.empty
	 || patch.oddFlags || patch.noFlags) {
		/*
		    Printed only when there is something to print, and printed in full when there is:
		    a set that lost thirty definitions to a full block looks exactly like a set with
		    thirty fewer achievements, from the block alone.
		*/
		raWifiLog("lost             %u full, %u too long, %u cut, %u empty\n",
		          patch.dropped, patch.tooLong, patch.cutShort, patch.empty);
		if (patch.oddFlags || patch.noFlags) {
			raWifiLog("odd              %u other flags, %u no flags\n",
			          patch.oddFlags, patch.noFlags);
		}
	}
	raWifiLog("block            %lu of %lu used, %lu wanted\n",
	          (unsigned long)patch.used, (unsigned long)blockMax,
	          (unsigned long)patch.wanted);
	raWifiLog("memaddr length   %lu shortest, %lu longest, of %d\n",
	          (unsigned long)patch.shortest, (unsigned long)patch.longest,
	          RA_PATCH_MEMADDR_MAX - 1);

	if (!patch.kept) {
		raWifiLog("\x1b[31mno definitions in the reply\x1b[37m\n");
		raWifiRestoreDefinitionsFile();
		return;
	}

	/*
	    The first three definitions, clipped, as the cheapest possible check that these are
	    memaddr strings and not 30 K of something else that happened to sit between quotes.

	    Three rather than one because of what the first run showed: a single `1=1.300.` is
	    ambiguous -- it is valid syntax for "always true, 300 hits" and it is also exactly what a
	    fragment would look like. Three consecutive entries are not ambiguous in the same way, and
	    ra_definitions.txt below settles it completely.
	*/
	{
		const char* at = block;
		int         n;

		for (n = 0; n < 3 && *at; n++) {
			char line[57];
			u32  full = 0;
			u32  i;

			/* The real length first, then the clip -- a clipped length would say nothing. */
			while (at[full] && at[full] != '\n') {
				full++;
			}
			for (i = 0; i < sizeof(line) - 1 && i < full; i++) {
				line[i] = at[i];
			}
			line[i] = 0;
			raWifiLog("def %d %5lu      %s\n", n + 1, (unsigned long)full, line);

			at += full;
			if (*at) {
				at++;
			}
		}
	}

	/*
	    Header last, and the magic last of all -- the same order loadRaDefinitions() uses, for
	    the same reason: the bootloader copies this block unconditionally and decides whether to
	    believe it by the magic alone, so a magic written before the length would make a
	    half-finished block indistinguishable from a finished one.
	*/
	*(u32*)(CARDENGINEI_ARM9_RA_DEFS_BUFFERED_LOCATION + 4) = patch.used;
	*(u32*)CARDENGINEI_ARM9_RA_DEFS_BUFFERED_LOCATION       = CARDENGINEI_ARM9_RA_DEFS_MAGIC;

	verdict.patched   = 1;
	verdict.defsKept  = patch.kept;
	verdict.defsBytes = patch.used;
	raWifiLog("\x1b[32mstaged %u definitions for the cardengine\x1b[37m\n", patch.kept);
}

/*
    Write the staged definitions out as text, so the set itself can be checked rather than its
    summary.

    Called last -- after the summary has been written and fsync()'d -- and that ordering is the
    whole reason it is a separate function. This is tens of kilobytes of SD I/O with the WiFi link
    still up, which is exactly the ARM7 contention #1d is about and which froze a run once
    already. Putting it after the summary means a freeze here costs the file and keeps the
    reading; putting it before would have risked the reverse.

    The file is the same format the launcher already *reads* from ra_achievements.txt, so it is
    not only evidence: copy it to sd:/_nds/nds-bootstrap/ and a normal build boots the game with
    the server's own achievement set, with no network involved. That is a useful thing to have
    while step 4 is still being built.
*/
static void raWifiDumpDefinitions(bool sdFound) {
	const char* const path = sdFound ? RA_DEFS_DUMP_PATH : RA_DEFS_DUMP_PATH_FAT;
	const char* const text = (const char*)(CARDENGINEI_ARM9_RA_DEFS_BUFFERED_LOCATION
	                                       + CARDENGINEI_ARM9_RA_DEFS_HEADER);
	const u32         length = *(u32*)(CARDENGINEI_ARM9_RA_DEFS_BUFFERED_LOCATION + 4);
	FILE*             out;

	if (*(u32*)CARDENGINEI_ARM9_RA_DEFS_BUFFERED_LOCATION != CARDENGINEI_ARM9_RA_DEFS_MAGIC
	 || length == 0) {
		return;
	}

	out = fopen(path, "w");
	if (!out) {
		raWifiLog("\x1b[33mcould not write %s\x1b[37m\n", path);
		return;
	}
	/*
	    Written and closed, not fsync()'d and left open like the log. The log is deliberately
	    never closed because a hang must not lose it; this one is finished when it is finished, and
	    close() is what makes libfat write the directory entry -- the same lesson the zero-byte log
	    taught.
	*/
	if (fwrite(text, 1, length, out) == length) {
		fclose(out);
		raWifiLog("definitions to   %s\n", path);
	} else {
		fclose(out);
		raWifiLog("\x1b[33m%s is short -- the card refused a write\x1b[37m\n", path);
	}
}

/*
    Give the radio back, so the bootloader can have both CPUs.

    This is the whole reason mode 2 can exist. Four things are live once the ladder has run, and
    every one of them is a thing that fires after the bootloader has replaced the code it belongs
    to:

      the ARM7's SDIO card interrupt and its TIMER3, which dsiwifi's own wifi_card_deinit() masks;
      the ARM9's TIMER3, which drives ath_lwip_tick() every 100 ms;
      and dsiwifi's datamsg handler on FIFO_DSWIFI.

    Order is deliberate. The ARM7 goes first, because it is the one holding the chip: until its
    card interrupt is masked the radio can still call it, and a call into overwritten code is the
    failure being avoided. Only then are the ARM9's own two stopped -- if it were the other way
    round, the ARM7's log messages would arrive at a channel with no handler while it was still
    working.

    Returns false when the ARM7 never acknowledges. That is the one outcome where booting is worse
    than not booting, and the caller is the one that decides.
*/
bool raWifiShutdown(void) {
	int frames = RA_WIFI_WAIT_STOP * 60;

	raWifiLog("\n-- giving the radio back --\n");

	fifoSendValue32(RA_WIFI_ARM7_STOP_CHANNEL, 1);
	while (frames-- > 0) {
		if (fifoCheckValue32(RA_WIFI_ARM7_STOP_CHANNEL)) {
			break;
		}
		raWifiIdle();
	}
	if (frames <= 0) {
		raWifiLog("\x1b[31mthe ARM7 never confirmed wifi_card_deinit()\x1b[37m\n"
		          "not booting: its card IRQ may still be live.\n");
		return false;
	}
	fifoGetValue32(RA_WIFI_ARM7_STOP_CHANNEL);
	raWifiLog("ARM7            deinitted\n");

	/*
	    The ARM9's two. timerStop(3) because dsiwifi's ARM9 half starts a 100 ms TIMER3 in
	    wifi_host_init() and nothing else in this launcher uses that timer; clearing the datamsg
	    handler because FIFO_DSWIFI is the channel dsiwifi drives its whole sequence over, and a
	    handler pointing into replaced code is the same hazard as a live interrupt.

	    The log handler goes too. raWifiCapture() only writes to a buffer, so it is harmless -- but
	    "harmless" is a property of this build rather than of the arrangement, and the drain below
	    is the last one there will be.
	*/
	timerStop(3);
	fifoSetDatamsgHandler(FIFO_DSWIFI, 0, 0);
	DSiWifi_SetLogHandler(0);

	raWifiDrain();
	raWifiLog("ARM9            timer and FIFO handler stopped\n");
	return true;
}

void raWifiProbe(bool sdFound, const char* ndsPath) {
	static raConfig config;
	int             stage;

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

	/*
	    Step 3b, and it runs here for two reasons. It needs no network, so putting it first
	    means a failure cannot be blamed on one. And it reads the ROM off the card while the
	    heap is still whole -- the launcher has roughly 352 K free at this point and 191 K once
	    lwip is up, and although ra_hash.c streams rather than allocating, libfat still wants
	    buffers and there is no reason to make it compete.

	    The hash is what 3c and 3d will ask the server about, so it is logged in full: it can
	    be checked against the game's page on retroachievements.org by eye, which is a cheaper
	    verification than any amount of code.
	*/
	raWifiLog("\n-- stage 0b: the ROM's RetroAchievements hash --\n");
	{
		raHashInfo hashInfo;

		raWifiLog("ROM              %s\n", ndsPath ? ndsPath : "(none given)");
		if (ndsPath && raHashRom(ndsPath, romHash, &hashInfo)) {
			raWifiLog("\x1b[32mhash             %s\x1b[37m\n", romHash);
		} else {
			raWifiLog("\x1b[31mhash failed: %s\x1b[37m\n", raHashLastError());
		}
		/*
		    Printed even on failure, because these three numbers are what say *why* the
		    streaming implementation exists: bufferBytes is what rcheevos' own function would
		    have had to allocate in one block.
		*/
		raWifiLog("arm9 / arm7      %lu / %lu bytes\n",
		          (unsigned long)hashInfo.arm9Size, (unsigned long)hashInfo.arm7Size);
		raWifiLog("would malloc     %lu bytes\n", (unsigned long)hashInfo.bufferBytes);
	}
	raWifiReportHeap("after hash");

	/*
	    Step 3c's half that needs no network. Read here, beside the hash, for the same reason:
	    a missing or malformed config file should be a line in the log before anything can be
	    blamed on the radio.

	    Reported in detail because every field is a way for a login to fail silently -- an empty
	    username, a `notYet` count that is really a typo, a `password=` line the user thought
	    they filled in. The secret itself is never printed: see raConfigRedact().
	*/
	raWifiLog("\n-- stage 0c: the RetroAchievements config --\n");
	raConfigRead(sdFound ? RA_CFG_PATH : RA_CFG_PATH_FAT, &config);
	raWifiLog("ra.cfg           %s\n", config.found ? "found" : "absent");
	if (config.found) {
		raWifiLog("username         %s\n", config.username[0] ? config.username : "(empty)");
		raWifiLog("password         %s\n", raConfigRedact(config.password));
		raWifiLog("hardcore         %s\n", config.hardcore ? "1" : "0");
		if (config.notYet) {
			raWifiLog("%u keys parsed but not acted on yet\n", config.notYet);
		}
		if (config.unknown) {
			raWifiLog("\x1b[33m%u unknown keys -- check for typos\x1b[37m\n", config.unknown);
		}
		if (config.badLines) {
			raWifiLog("\x1b[33m%u lines with no '='\x1b[37m\n", config.badLines);
		}
	} else {
		raWifiLog("put username= and password= in\n%s\n",
		          sdFound ? RA_CFG_PATH : RA_CFG_PATH_FAT);
	}

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

	raWifiReportHeap("with lwip up");

	raWifiLog("\n-- stage 7-9: reach the API over plain HTTP --\n");
	raWifiReachApi();
	raWifiReportHeap("after HTTP");

	raWifiLog("\n-- stage 10: log in --\n");
	raWifiLogin(&config);
	raWifiReportHeap("after login");

	raWifiLog("\n-- stage 11: does the server know this ROM --\n");
	raWifiIdentify();
	raWifiReportHeap("after gameid");

	/*
	    And the last rung, which is the first one that leaves something behind: the set is
	    written to the staging block the bootloader copies into DSi WRAM. This build still does
	    not boot a game -- see RA_LAUNCHER_WIFI -- so what it proves is that the definitions
	    arrive and fit, which is the question. Running them is step 4.
	*/
	raWifiLog("\n-- stage 12: what has this account already earned --\n");
	raWifiUnlocks(&config);
	raWifiReportHeap("after unlocks");

	raWifiLog("\n-- stage 13: fetch the set --\n");
	raWifiFetchPatch(&config);
	raWifiReportHeap("after patch");

done:
	/*
	    Give the tail a chance before the summary rather than after it. dsiwifi narrates
	    asynchronously and keeps talking once the last rung is decided -- the probe's first
	    hardware run printed the line naming the access point and its security mode *after*
	    its own summary. Draining here means those lines land above the summary they inform,
	    and the summary is computed from a log that is actually finished.
	*/
	/*
	    Marked at both ends on purpose. One v6 run froze somewhere in here -- after the HTTP
	    heap line and before the summary -- and with the window unmarked there was no way to
	    tell the tail drain from the summary's own writes. A repeat now says which.
	*/
	raWifiLog("\n-- draining the tail --\n");
	{
		int frames = RA_WIFI_WAIT_TAIL * 60;
		while (frames-- > 0) {
			raWifiIdle();
		}
	}
	raWifiSync();
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
	raWifiLog("logged in        %s\n", verdict.loggedIn ? "yes" : "no");
	if (verdict.identified) {
		raWifiLog("GameID           %lu\n", (unsigned long)verdict.gameId);
	} else {
		raWifiLog("GameID           %s\n", verdict.gameId == 0 ? "unknown to the server" : "not asked");
	}
	if (verdict.unlocksKnown) {
		raWifiLog("already earned   %u\n", verdict.unlockCount);
	} else {
		raWifiLog("already earned   unknown\n");
	}
	if (verdict.patched) {
		raWifiLog("definitions      %u in %lu bytes\n",
		          verdict.defsKept, (unsigned long)verdict.defsBytes);
	} else {
		raWifiLog("definitions      none\n");
	}
	if (verdict.mboxAllocFailed) {
		raWifiLog("\x1b[31mthe ARM7 could not allocate its mboxes\x1b[37m\n");
	}
	raWifiLog("log lines        %u\n", verdict.lines);
	if (textDropped) {
		raWifiLog("\x1b[31m%lu chars dropped: the log has a hole\x1b[37m\n",
		          (unsigned long)textDropped);
	}

	raWifiLog("\n\x1b[33mreached stage %d of %d\x1b[37m\n", stage, RA_WIFI_STAGE_MAX);
	if (stage >= RA_WIFI_STAGE_PATCHED) {
		raWifiLog("the set is staged for the cardengine.\n");
	} else if (stage >= RA_WIFI_STAGE_IDENTIFIED) {
		raWifiLog("logged in, and the server knows the ROM.\n");
	} else if (stage >= RA_WIFI_STAGE_LOGGED_IN) {
		raWifiLog("logged in from the launcher.\n");
	} else if (stage >= RA_WIFI_STAGE_ANSWERED) {
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

	/*
	    The definitions go out here, deliberately after the summary is already on the card. See
	    raWifiDumpDefinitions(): this is the largest SD write of the run and the link is still up.
	*/
	if (verdict.unlocksKnown) {
		raWifiLog("already earned   %u\n", verdict.unlockCount);
	} else {
		raWifiLog("already earned   unknown\n");
	}
	if (verdict.patched) {
		raWifiDumpDefinitions(sdFound);
		raWifiSync();
	}

#if RA_WIFI_BOOTS_GAME
	/*
	    Mode 2: give the radio back and return, and the launcher goes on to boot the game with the
	    set already staged. Nothing above this line is different from the diagnostic -- same ladder,
	    same log, same summary -- which is deliberate: the reading and the thing that plays are the
	    same code, so a difference between them cannot be ours.
	*/
	if (!raWifiShutdown()) {
		/*
		    The one refusal. An ARM7 that never confirmed the teardown may still be taking SDIO
		    interrupts, and the bootloader is about to overwrite the code those interrupts vector
		    into -- so this stops on the summary exactly as the diagnostic does, rather than handing
		    a live radio to a CPU that is about to forget how to answer it.

		    A halt is a bad outcome and it is the *better* bad outcome: the alternative is a crash
		    somewhere in the game, minutes later, with nothing on the card explaining it.
		*/
		raWifiSync();
		raWifiLog("\nstopped rather than boot with a live radio.\n");
		while (1) {
			raWifiIdle();
		}
	}

	raWifiSync();
	raWifiLog("\nradio down -- booting the game.\n");
	/*
	    Closed here, unlike the diagnostic's log, and for the opposite reason. There it is left open
	    because a hang must not lose the late lines; here the launcher is about to hand both CPUs to
	    the bootloader, so there are no late lines and close() is what writes the directory entry.
	*/
	if (logFile) {
		fclose(logFile);
		logFile = 0;
	}
	return;
#else
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
#endif
}

#endif /* RA_LAUNCHER_WIFI */
