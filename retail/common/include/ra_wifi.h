/*
    Step two of the network ladder: bring the Atheros chip up from inside nds-bootstrap's
    launcher, and report how it came up.

    Step one -- tools/wifiprobe/ -- already reached RetroAchievements over plain HTTP from
    this console, as ordinary DSi-mode homebrew with an idle ARM7 of its own. What that run
    could not tell us is whether the same driver comes up as a *guest of nds-bootstrap's
    ARM7*, which is a different core: it is not the libnds template, it does not open SCFG
    itself, it is already driving the SD/eMMC controller one instance below the WiFi SDIO
    block, and it owns the FIFO the launcher talks over. See docs/retroachievements.md,
    "#1f -- the rest of the ladder".

    Deliberately stops short of lwip. Everything up to and including the WPA2 handshake
    happens on the ARM7 inside dsiwifi; DHCP and sockets are the ARM9's lwip, and lwip as
    dsiwifi configures it does not fit the launcher's link region (see the note in
    docs/retroachievements.md). Splitting there is not a compromise: it is where the
    question step two asks actually ends, and the ARM9 half of this probe therefore links
    no library at all -- only dsiwifi's IPC header.

    This file is part of nds-bootstrap and is licensed under the GPL-3.0,
    the same terms as the rest of the project.
*/

#ifndef RA_WIFI_H
#define RA_WIFI_H

#include <nds/ndstypes.h>
#include <stddef.h>   /* size_t, for the ra_net entry points below */

/*
    Master switch, and it is off in every shipped build.

    A build with this on is a *diagnostic build that does not boot games*. Two reasons, and
    the second is the one that decides it:

      The measurement wants the console to stop on a summary, so a reading is a reading
      rather than something to catch before the game covers it.

      dsiwifi's bring-up blocks. wifi_card_wlan_init() spins in `while (1)` waiting for the
      firmware-ready flag and again in `while (!wmi_is_ready())`, with no timeout, inside a
      FIFO handler on the ARM7. If the chip does not come up, that ARM7 is wedged -- and
      handing a wedged ARM7 to the bootloader, which is about to overwrite its code while a
      timer IRQ and an AUX IRQ are still live, is not something to do for a diagnostic.

    So: `make RA_LAUNCHER_WIFI=1` builds the probe, and nothing else should.
*/
#ifndef RA_LAUNCHER_WIFI
#define RA_LAUNCHER_WIFI 0
#endif

/*
    Where the launcher writes what it saw. Root of the card, next to NDSBTSRP.LOG and
    alongside the probe's own /wifiprobe.log -- because the two are meant to be *diffed*,
    and that is the whole test design. Both capture dsiwifi's narration verbatim, so
    "did the chip arrive in the same state under our boot path" is a text comparison
    rather than a judgement call.
*/
#define RA_WIFI_LOG_PATH      "sd:/ra_wifi_launcher.log"
#define RA_WIFI_LOG_PATH_FAT  "fat:/ra_wifi_launcher.log"

/*
    The ladder, and it only moves forward. Five rungs against the probe's six: rungs 1-3
    are the chip coming up, 4-5 are the link becoming usable, and the probe's stages 3-6
    (DNS, TCP, HTTP) are step three of the plan rather than this one.

    Each rung is named after the thing that proves it, so a stopped run says where it
    stopped instead of that it failed.
*/
#define RA_WIFI_STAGE_START       0  /* the ARM7 never said anything */
#define RA_WIFI_STAGE_CHIP        1  /* SDIO answered: the manufacturer/chip-ID read worked */
#define RA_WIFI_STAGE_FIRMWARE    2  /* the Xtensa core is running our firmware */
#define RA_WIFI_STAGE_WMI         3  /* WMI is up: the driver has a working command channel */
#define RA_WIFI_STAGE_ASSOCIATED  4  /* WMI_CONNECT_EVENT -- associated to the AP */
#define RA_WIFI_STAGE_READY       5  /* handshake done, keys installed, link usable */
#define RA_WIFI_STAGE_IP          6  /* DHCP gave us an address */
#define RA_WIFI_STAGE_RESOLVED    7  /* DNS answered for retroachievements.org */
#define RA_WIFI_STAGE_CONNECTED   8  /* TCP to port 80 */
#define RA_WIFI_STAGE_ANSWERED    9  /* the API replied, and the reply looks like the API */
#define RA_WIFI_STAGE_LOGGED_IN   10 /* r=login returned a token for the configured user */
#define RA_WIFI_STAGE_IDENTIFIED  11 /* r=gameid turned the ROM's hash into a GameID */
#define RA_WIFI_STAGE_MAX         RA_WIFI_STAGE_IDENTIFIED

/*
    What dsiwifi's narration said about how the chip arrived.

    Read out of its log text rather than from the driver, because none of it is exposed:
    the chip string, the host-interest flag and the BMI version are statics inside
    libs/dsiwifi's ARM7 half. That is a coupling to a third-party library's printf strings
    and it is worth saying so plainly -- which is exactly why the verbatim log is the
    primary artifact and this struct is the convenience. The host test pins every string
    against the log a real console produced, so a submodule bump that renames one fails
    tools/ra_reader_test.sh instead of quietly reporting the wrong world.
*/
typedef struct raWifiVerdict {
	u8   chipSeen;         /* "Mfg ..." -- the chip answered on the SDIO bus */
	u8   coldStart;        /* "needs firmware upload" -- host-interest flag read 0 */
	u8   bmiSeen;          /* "BMI version:" -- the bootloader answered */
	u8   firmwareLaunched; /* "Launching!" */
	u8   firmwareReady;    /* "ready, handshaking..." */
	u8   wmiReady;         /* "fully initialized!" */
	u8   associated;       /* "WMI_CONNECT_EVENT" */
	u8   linkReady;        /* "Done auth" -- the handshake finished */
	u8   mboxAllocFailed;  /* "bad mbox alloc" -- the ARM7 heap could not spare 3K */
	u8   reserved[3];

	/*
	    Set by the probe rather than read out of the log: from step 3 on, the top of the
	    ladder is lwip's business and lwip answers in return values, not in narration.
	*/
	u8   gotIp;
	u8   dnsOk;
	u8   tcpOk;
	u8   apiOk;
	u8   loggedIn;         /* r=login returned a token */
	u8   identified;       /* r=gameid returned a non-zero GameID */
	u32  gameId;           /* ...this one. Zero means the server does not know the dump */
	u32  ip;               /* as DSiWifi_GetIP() returns it */
	char chip[16];         /* "AR6014", as dsiwifi named it */
	/*
	    The partial line being accumulated -- not a result. Larger than dsiwifi's own 0x7C
	    print buffer on purpose: several of its calls print without a trailing newline, so
	    fragments concatenate here until one arrives. Concatenation cannot hide a match;
	    a forced flush at the end of this buffer could, by cutting a string in two, so the
	    buffer is sized so that never happens for anything the bring-up prints.
	*/
	char line[256];
	u16  lineLength;
	u16  lines;            /* how many complete lines were classified */
} raWifiVerdict;

#ifdef __cplusplus
extern "C" {
#endif

/*
    The classifier. Pure text in, flags out, no I/O -- so it is the one part of this that a
    host can check, and tools/ra_reader_test.c does.

    Fed a line at a time on purpose. dsiwifi's ARM7 ships its log over the FIFO in 59-byte
    chunks, so a single line can arrive split across two messages; matching per chunk would
    miss any string that straddles the boundary, and the strings that matter are long ones.
    raWifiVerdictChunk() reassembles, raWifiVerdictLine() is what does the deciding.
*/
void        raWifiVerdictReset(raWifiVerdict* v);
void        raWifiVerdictLine(raWifiVerdict* v, const char* line);
void        raWifiVerdictChunk(raWifiVerdict* v, const char* chunk);
void        raWifiVerdictFlush(raWifiVerdict* v);
int         raWifiVerdictStage(const raWifiVerdict* v);
const char* raWifiVerdictArrival(const raWifiVerdict* v);

/*
    Step 3c's configuration, read from sd:/_nds/nds-bootstrap/ra.cfg -- the same `key=value`
    shape odelot's MiSTer core uses, because that is the file this project's user already has.

    The password is kept in the file, knowingly: the alternative of caching only a token was
    offered and declined. It therefore goes over the wire in the clear on every boot. It must
    never reach the log -- see raConfigRedact().
*/
#define RA_CFG_PATH      "sd:/_nds/nds-bootstrap/ra.cfg"
#define RA_CFG_PATH_FAT  "fat:/_nds/nds-bootstrap/ra.cfg"

typedef struct raConfig {
	char username[33];   /* RA usernames are short; 32 is well past any real one */
	char password[65];
	u8   hardcore;       /* the API's h= parameter. Softcore is h=0 and is what this fork does */
	u8   debug;
	u8   found;          /* the file existed */
	u8   usable;         /* ...and both credentials are set */
	u16  notYet;         /* keys we recognise from odelot's file and do not act on yet */
	u16  unknown;        /* keys nobody knows: almost certainly a typo */
	u16  badLines;       /* non-comment lines with no `=` */
} raConfig;

/*
    Step 3c's transport. The layering table has had an `ra_net` row since before it existed;
    this is it. Negative returns so a failure names its own step instead of becoming a zero.
*/
#define RA_NET_HOST           "retroachievements.org"
#define RA_NET_PORT           80
#define RA_NET_RECV_TIMEOUT   5

#define RA_NET_BAD_ARGS       (-1)
#define RA_NET_NO_DNS         (-2)
#define RA_NET_NO_SOCKET      (-3)
#define RA_NET_NO_CONNECT     (-4)
#define RA_NET_REQ_TOO_LONG   (-5)
#define RA_NET_NO_SEND        (-6)

/* How far one request got, so the ladder can be filled in from a single call. */
typedef struct raNetProgress {
	u8  resolved;
	u8  connected;
	u8  sent;
	u8  closedByPeer;
	u32 address;
} raNetProgress;

/*
    Step 3b. What the launcher had to allocate to compute the hash, reported so the margin
    against the heap is a measurement rather than a hope: rc_hash_nintendo_ds() takes
    max(0xA00, arm9Size, arm7Size) in one block, and the launcher has ~352 K of heap before
    lwip starts and ~191 K after.
*/
typedef struct raHashInfo {
	u32 arm9Size;
	u32 arm7Size;
	u32 bufferBytes;
} raHashInfo;

#if RA_LAUNCHER_WIFI
/*
    The probe itself, on the ARM9 launcher. Never returns: it stops on a summary. See the
    switch above for why that is the design and not a shortcut.
*/
void raWifiProbe(bool sdFound, const char* ndsPath);

/*
    The RetroAchievements hash of a DS ROM, as rcheevos computes it. 32 lowercase hex digits
    and a terminator. False leaves hash empty and raHashLastError() explaining why.
*/
bool        raHashRom(const char* path, char hash[33], raHashInfo* info);
const char* raHashLastError(void);

/* Step 3c. The config file, and what may be said about a secret out loud. */
bool        raConfigRead(const char* path, raConfig* cfg);
const char* raConfigRedact(const char* secret);

/* Step 3c. The transport. */
bool        raNetUrlEncode(const char* in, char* out, size_t outSize);
int         raNetHttpGet(const char* host, const char* path, char* out, int outSize,
                         raNetProgress* p);
const char* raNetBody(const char* response);
bool        raNetJsonString(const char* json, const char* key, char* out, size_t outSize);
bool        raNetJsonNumber(const char* json, const char* key, u32* out);

/* The ARM7 half: hand this CPU to dsiwifi. One call, and where it goes matters. */
void raWifiInstall(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* RA_WIFI_H */
