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
    Master switch, and it is off in every shipped build. Three values:

      **0** -- not built. The launcher is byte-for-byte what it was.

      **1** -- the diagnostic. Runs the whole ladder and *stops on a summary*, so a reading is a
      reading rather than something to catch before a game covers it. This is the mode every
      measurement in docs/retroachievements.md was taken in and it stays.

      **2** -- fetch, then boot. Same ladder, but it tears the radio down and returns, so the
      launcher goes on to start the game with the server's own achievement set already staged.

    Mode 1 halts for a second reason that mode 2 had to answer before it could exist. dsiwifi's
    bring-up blocks: wifi_card_wlan_init() spins in `while (1)` waiting for the firmware-ready
    flag and again in `while (!wmi_is_ready())`, with no timeout, inside a FIFO handler on the
    ARM7. If the chip does not come up that ARM7 is wedged -- and handing a wedged ARM7 to the
    bootloader, which is about to overwrite its code while a timer IRQ and an AUX IRQ are still
    live, is not something to do for a measurement.

    What makes mode 2 possible is that the teardown turned out to already exist. dsiwifi's
    DSiWifi_DisconnectAP() is an unimplemented sassert(false), but wifi_card_deinit() in its ARM7
    half does exactly the right four things: masks the SDIO card IRQ, disables the AUX IRQ,
    disables the ARM7's TIMER3, and clears the chip's own interrupt enables. The ARM9 half adds a
    TIMER3 of its own and a FIFO_DSWIFI handler, and both of those are ours to stop. See
    raWifiShutdown().

    So: `make RA_LAUNCHER_WIFI=1` measures, `=2` plays, and neither is a shipped build yet.
*/
#ifndef RA_LAUNCHER_WIFI
#define RA_LAUNCHER_WIFI 0
#endif

/* Mode 2 and above return to the launcher instead of halting on the summary. */
#define RA_WIFI_BOOTS_GAME (RA_LAUNCHER_WIFI >= 2)

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
    And where stage 12 writes the definitions it staged, verbatim, one per line.

    The first hardware run of that stage is why this exists. It reported 51 definitions in 6,791
    bytes and printed the first one as `1=1.300.` -- eight characters, which is *syntactically* a
    valid memaddr ("always true, 300 hits") and is not what a published achievement looks like.
    Six summary numbers cannot tell "the scanner works and that set has an odd first entry" apart
    from "the scanner is producing fragments", and no amount of arguing from the code settles it
    either.

    So the definitions become the artifact, the way dsiwifi's verbatim log is the artifact for the
    chip: this file can be read against the set's page on retroachievements.org, definition by
    definition. Same file format the launcher already *reads* from ra_achievements.txt, which
    makes it directly re-usable -- copy it there and the game boots with the server's own set.
*/
#define RA_DEFS_DUMP_PATH      "sd:/ra_definitions.txt"
#define RA_DEFS_DUMP_PATH_FAT  "fat:/ra_definitions.txt"

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
#define RA_WIFI_STAGE_PATCHED     12 /* r=patch put real definitions in the staging block */
#define RA_WIFI_STAGE_MAX         RA_WIFI_STAGE_PATCHED

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
	u8   patched;          /* r=patch produced at least one definition */
	u16  defsKept;         /* ...this many */
	u32  defsBytes;        /* and this much of the staging block */
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
    Step 3d's reader. `r=login` and `r=gameid` fit in a 2 K buffer; `r=patch` returns a whole
    achievement set, which for a big game is over 100 K -- more than the launcher's remaining
    heap and more than three times the staging block it is destined for. So it is never held:
    the bytes are handed to a sink as they arrive off the socket, and what survives the call is
    the definitions, not the reply.

    Two things sit between the socket and the sink, and both are why this is a struct rather
    than a callback:

      The HTTP headers have to be skipped, and their length is not known in advance -- so the
      boundary can fall anywhere, including between the `\r\n` and the `\r\n`.

      A 1.1 reply may be **chunked**, and a chunk header is a hex length written *into the byte
      stream*. The small replies this project has already read came back unchunked, which
      proves nothing about a 100 K one -- and a `1a2f\r\n` landing in the middle of a memaddr
      string would corrupt exactly one definition, silently. So the framing is undone here,
      where it can be tested on a host, rather than hoped about.
*/
typedef void (*raNetSink)(void* ctx, const char* data, int length);

#define RA_NET_LINE_MAX 128

#define RA_NET_STREAM_STATUS  0  /* the response line */
#define RA_NET_STREAM_HEADER  1  /* header lines, until a blank one */
#define RA_NET_STREAM_BODY    2  /* identity encoding: everything left is body */
#define RA_NET_STREAM_SIZE    3  /* chunked: reading a hex chunk length */
#define RA_NET_STREAM_DATA    4  /* chunked: inside a chunk */
#define RA_NET_STREAM_CRLF    5  /* chunked: the CRLF that follows a chunk */
#define RA_NET_STREAM_TRAILER 6  /* the last chunk was empty; nothing more is body */

typedef struct raNetStream {
	raNetSink sink;
	void*     ctx;
	u8        state;
	u8        chunked;
	u16       status;      /* the HTTP status line's code, 0 if it did not parse */
	u32       chunkLeft;
	u32       bodyBytes;   /* how much reached the sink -- the body's real length */
	u16       lineLength;
	char      line[RA_NET_LINE_MAX];
} raNetStream;

/*
    Step 3d's scanner: pull every `"MemAddr":"..."` out of an `r=patch` reply, as it arrives,
    straight into the staging block the cardengine reads.

    Not a JSON parser, and the reason is not laziness -- it is that a parser for this reply
    needs the reply, and the reply does not fit. What makes the shortcut sound rather than
    lucky is that JSON escapes any quote inside a string as `\"`, so the eleven bytes
    `"MemAddr":"` cannot occur *inside* a value: an achievement titled `"MemAddr":"` arrives as
    `\"MemAddr\":\"` and does not match. The needle can therefore only be a key.

    What it does not do is know which object the key was in. Today `MemAddr` appears only on
    achievements -- leaderboards use `Mem`, rich presence `RichPresencePatch` -- so this takes
    the achievement set and nothing else. If RA ever adds a `MemAddr` elsewhere, this would
    take that too, and that is a limitation rather than a bug to find later.
*/
/*
    8192, and the number is a hardware measurement rather than a guess.

    It was 2048, chosen because RA memaddr strings "run from tens to a few hundred characters".
    That is true of most of them and false where it matters: the first run of stage 12 against
    GameID 14856 reported `longest memaddr 6264 of 2047` and **dropped five definitions** for
    exceeding the buffer. A completionist achievement is one condition per collectable -- 150
    stars is 150 conditions -- so a few kilobytes is normal for exactly the achievements a player
    cares most about.

    Raising it is cheap in the launcher (6 KB of .bss against 69,632 of measured headroom) and it
    is the only honest option: the alternative was already rejected in ra_patch.c, because a
    truncated memaddr is not a shorter achievement but a different one.

    8192 leaves 30% over the largest definition this project has seen. `longest` is reported on
    every run precisely so that margin stays a measurement -- another game will have another
    largest, and the log will say so instead of five achievements quietly not existing.
*/
#define RA_PATCH_MEMADDR_MAX 8192

#define RA_PATCH_SCAN   0
#define RA_PATCH_VALUE  1
#define RA_PATCH_FLAGS  2
#define RA_PATCH_ID     3

/*
    Each staged line is `<id>:<memaddr>`, and the id is the achievement's own number on
    retroachievements.org.

    Without it nothing downstream is possible: `r=unlocks` answers in ids, `r=awardachievement` is
    asked in ids, and "which achievement just fired" has no answer the server would recognise. The
    block carried none until now because nothing needed them, and adding them while the format has
    exactly one producer and one consumer is far cheaper than adding them later.

    **A leading run of decimal digits followed by a colon is an id, and nothing else is.** That is a
    property of memaddr syntax rather than a convention chosen here: every prefix flag that ends in a
    colon -- `A:`, `M:`, `N:`, `O:`, `P:`, `Q:`, `R:`, `T:`, `I:`, `K:`, `Z:`, `G:`, `C:`, `B:` -- is a
    letter. Checked against the real set for GameID 14856: of its 47 lines containing a colon, the
    character before the first one is `M`, `N`, `O`, `P`, `R` or `T`, never a digit. And a definition
    may certainly *begin* with a digit -- that set's first line is `1=1.300.` -- which is exactly why
    the test is digits-then-colon and not digits.

    A line with no id still works, and files written by hand are not expected to carry them. See
    RA_SYNTHETIC_ID_BASE.
*/

typedef struct raPatch {
	char* block;          /* where the definitions go, one per line */
	u32   blockMax;       /* bytes for text, terminator excluded */
	u32   used;

	u32   pendingId;      /* the "ID" seen most recently, waiting for its MemAddr */
	u16   withId;         /* definitions written with a RetroAchievements id */
	u16   withoutId;      /* ...and without one, which is a set we cannot report on */

	u16   kept;           /* written to the block */
	u16   unofficial;     /* dropped: Flags 5, not part of the published set */
	u16   oddFlags;       /* kept, but Flags was neither 3 nor 5 */
	u16   noFlags;        /* kept, and no Flags field ever arrived */
	u16   tooLong;        /* dropped: longer than RA_PATCH_MEMADDR_MAX */
	u16   cutShort;       /* dropped: the stream ended in the middle of the value */
	u16   empty;          /* dropped: the value was "" */
	u16   dropped;        /* dropped: the block was full */
	u32   wanted;         /* bytes the kept and block-full definitions needed between them */
	u32   longest;        /* longest memaddr seen, decoded -- including ones too long to keep */
	u32   shortest;       /* shortest one that belonged in the block: a fragment as a number */

	/* Scanner state. Carried across chunks, which is the whole point of it being here. */
	u8    state;
	u8    memAt;
	u8    flagsAt;
	u8    idAt;
	u8    escape;
	u8    pendingOpen;    /* a value has started, so there is something to commit */
	u8    pendingBad;     /* ...and it overran RA_PATCH_MEMADDR_MAX */
	u8    flagsSeen;
	u32   flags;
	u32   pendingSeen;    /* decoded length, whether or not it was stored */
	u32   pendingLength;  /* how much of it is in pending[] */
	char  pending[RA_PATCH_MEMADDR_MAX];
} raPatch;

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

/*
    Step 3d. The streaming half: reset, feed the socket's bytes in whatever sizes they arrive,
    and the sink sees the body with the headers and any chunk framing already gone.
    raNetStreamFeed() has no I/O in it and is checked on a host at every split point.
*/
void raNetStreamReset(raNetStream* s, raNetSink sink, void* ctx);
void raNetStreamFeed(raNetStream* s, const char* data, int length);
int  raNetHttpGetStream(const char* host, const char* path, raNetSink sink, void* ctx,
                        raNetProgress* p);

/* Step 3d. The scanner. Also pure, also fed in arbitrary pieces. */
void raPatchReset(raPatch* p, char* block, u32 blockMax);
void raPatchFeed(void* p, const char* data, int length);
void raPatchFinish(raPatch* p);

/* The ARM7 half: hand this CPU to dsiwifi. One call, and where it goes matters. */
void raWifiInstall(void);

/*
    ...and take it back. Called from the ARM7's idle loop rather than from the FIFO handler that
    asks for it, because wifi_card_deinit() writes SDIO registers and polls for the controller to
    answer -- a bounded wait, but not one to take inside an interrupt on a CPU that is about to be
    overwritten. The handler sets a flag; this does the work.

    A no-op until asked, so the loop pays a compare per FIFO wake-up.
*/
void raWifiPoll(void);

/*
    retail/arm9/source/conf_sd.cpp -- stage sd:/_nds/nds-bootstrap/ra_achievements.txt into the
    definitions block. Called once during loadFromSD(), and again by stage 12 when a fetch fails,
    because the fetch streams into that same block and destroys the file's text on its way.
*/
void loadRaDefinitions(void);

/*
    Mode 2's teardown, on the ARM9: stop dsiwifi's TIMER3 and its FIFO handler, ask the ARM7 to run
    wifi_card_deinit(), and wait a bounded time for it to say it did.

    Returns false if the ARM7 never answered, which is the one case where booting the game is worse
    than not booting it -- an ARM7 still taking SDIO interrupts when the bootloader overwrites its
    code. The caller decides; this only reports.
*/
bool raWifiShutdown(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* RA_WIFI_H */
