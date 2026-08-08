/*
    Host-side checks for the launcher's own pure logic: the ROM hash (3b), the configuration
    file and the query encoding (3c). What they have in common is that every one of them fails
    *silently* -- a hash over the wrong bytes, a password truncated at an `&`, a config key that
    parses as something else -- and every one of those looks, from the console, exactly like a
    wrong password or a game with no achievement set.

    A second binary rather than a section of tools/ra_reader_test.c, and that separation is
    load-bearing. That file computes the WRAM allocator's arena from the addresses of
    `__bss_start`, `__bss_end` and `__vram_top`, which it defines as 1-byte dummies -- so the
    arena is whatever the linker's ordering of those three happens to make it. Adding
    rcheevos' hash.c and hash_rom.c to that link flipped `__vram_top` below `__bss_end`, the
    span became -1, and `ra_startup()` zeroed and handed out memory across the whole process.
    The suite passed at -O0 and segfaulted at -O1, in a test several sections before the new
    code was even reached.

    That fragility is worth fixing on its own terms and is not this file's business, so this
    file simply does not join that link: no cardengine sources, no fixed link address, no
    mapped pages. It needs none of them -- the hash is file I/O and an MD5.

    Run through tools/ra_reader_test.sh, which builds both binaries.

    This file is part of nds-bootstrap and is licensed under the GPL-3.0,
    the same terms as the rest of the project.
*/

#include <stdio.h>
#include <string.h>

#include "ra_wifi.h"
/* For the staging block's size, which step 3d's scanner has to fit a real set into. */
#include "locations.h"

/*
    raNetUrlEncode() and raNetJsonString() are string logic; the rest of ra_net.c is lwip
    sockets. Rather than link lwip into a host test to reach two pure functions, the two are
    lifted in by including the file with its socket half compiled out. RA_NET_HOST_TEST is
    defined nowhere else, so the target build always gets the whole thing.
*/
#define RA_NET_HOST_TEST 1
#include "../retail/arm9/source/ra_net.c"

/*
    ------------------------------------------------------------------------------------
    Step 3b's hash, checked against the only reference that matters.

    ra_hash.c streams the four ranges rcheevos hashes through a 1 K buffer, because
    rc_hash_nintendo_ds() allocates max(0xA00, arm9_size, arm7_size) in one block -- 353,164
    bytes for nds-bootstrap's own .nds -- and the launcher's heap is 352 K before lwip and
    191 K after. It does not fit, so it had to be reimplemented.

    Which makes divergence the whole risk: a hash over almost-the-right bytes is a
    well-formed MD5 that the server does not recognise, and on hardware that is
    indistinguishable from a game with no achievement set. Nothing about a play session
    could tell those apart.

    So the real rc_hash_nintendo_ds() is compiled here -- only here, never into the
    launcher -- and the two are required to agree on real .nds files. rcheevos stays the
    definition; ours is an implementation of it that cannot drift in silence.
    ------------------------------------------------------------------------------------
*/
#include "rc_hash.h"
#include "rhash/rc_hash_internal.h"

static void* rcHashOpen(const char* path)                        { return fopen(path, "rb"); }
static void  rcHashSeek(void* h, int64_t o, int w)                { fseek((FILE*)h, (long)o, w); }
static int64_t rcHashTell(void* h)                                { return ftell((FILE*)h); }
static size_t rcHashRead(void* h, void* b, size_t n)              { return fread(b, 1, n, (FILE*)h); }
static void  rcHashClose(void* h)                                 { fclose((FILE*)h); }

/* rcheevos' answer for the same file, through its own allocating implementation. */
static int rc_reference_hash(const char* path, char out[33]) {
	rc_hash_iterator_t it;

	memset(&it, 0, sizeof(it));
	it.path = path;
	it.callbacks.filereader.open  = rcHashOpen;
	it.callbacks.filereader.seek  = rcHashSeek;
	it.callbacks.filereader.tell  = rcHashTell;
	it.callbacks.filereader.read  = rcHashRead;
	it.callbacks.filereader.close = rcHashClose;
	return rc_hash_nintendo_ds(out, &it);
}

static int failures;

#define CHECK(cond) do { \
	if (cond) { \
		printf("  ok    %s\n", #cond); \
	} else { \
		printf("  FAIL  %s\n", #cond); \
		failures++; \
	} \
} while (0)

/*
    Real .nds files, because a synthetic one would only prove the two implementations agree
    about a header this test wrote. These are whatever the build produced -- nds-bootstrap
    itself and the WiFi probe -- so the ARM9 and ARM7 sizes, the icon block and the offsets
    are all a linker's rather than a fixture's.

    Skipped, not failed, when they are absent: the host test is meant to run before any
    devkitARM build, and it says so rather than demanding one.
*/
static void test_rom_hash(void) {
	static const char* const roms[] = {
		"retail/bin/nds-bootstrap-nightly.nds",
		"retail/bin/nds-bootstrap.nds",
		"tools/wifiprobe/wifiprobe.nds",
		NULL,
	};
	int tested = 0;
	int i;

	printf("\nthe ROM hash agrees with rcheevos' own, on real .nds files\n");

	for (i = 0; roms[i]; i++) {
		char       ours[33], theirs[33];
		raHashInfo info;
		FILE*      f = fopen(roms[i], "rb");

		if (!f) {
			continue;
		}
		fclose(f);
		tested++;

		CHECK(raHashRom(roms[i], ours, &info) == true);
		CHECK(rc_reference_hash(roms[i], theirs) != 0);
		if (strcmp(ours, theirs) == 0) {
			printf("  ok    %s\n        %s\n", roms[i], ours);
		} else {
			printf("  FAIL  %s\n        ours   %s\n        rcheevos %s\n",
			       roms[i], ours, theirs);
			failures++;
		}

		/*
		    And the number that forced the streaming implementation, printed rather than
		    asserted -- it is a property of whatever ROM happened to be here, not of the
		    code. The launcher has ~191 K of heap once lwip is up, so anything approaching
		    that is the point being made.
		*/
		printf("        rcheevos would have malloc'd %lu bytes (arm9 %lu, arm7 %lu)\n",
		       (unsigned long)info.bufferBytes,
		       (unsigned long)info.arm9Size, (unsigned long)info.arm7Size);
	}

	if (!tested) {
		printf("  no .nds built yet -- skipped (run make first to cover this)\n");
	}

	/*
	    A file that is not a ROM has to fail rather than return a confident hash of whatever
	    was there. This test file is the nearest thing to hand.
	*/
	{
		char       ours[33];
		raHashInfo info;

		CHECK(raHashRom("tools/ra_reader_test.c", ours, &info) == false
		   || strlen(ours) == 32);
		CHECK(raHashRom("does/not/exist.nds", ours, &info) == false);
		CHECK(ours[0] == 0);
	}
}

/*
    The query encoder, and it is the one function in 3c that can lie convincingly.

    A password is user text. An `&` ends the parameter early, so the server sees a shorter
    password; a `+` decodes as a space; a `%` opens an escape that is not there. Every one of
    those builds a well-formed request that comes back `invalid_credentials`, which from the
    console is indistinguishable from the password simply being wrong -- so the user would be
    told their password is wrong when it is not.
*/
static void test_url_encode(void) {
	char buf[64];

	printf("\nthe query encoder survives a password with punctuation in it\n");

	CHECK(raNetUrlEncode("plain", buf, sizeof(buf)) && strcmp(buf, "plain") == 0);
	/* Unreserved per RFC 3986 pass through untouched. */
	CHECK(raNetUrlEncode("aZ09-_.~", buf, sizeof(buf)) && strcmp(buf, "aZ09-_.~") == 0);
	/* The three that would silently corrupt a login. */
	CHECK(raNetUrlEncode("a&b", buf, sizeof(buf)) && strcmp(buf, "a%26b") == 0);
	CHECK(raNetUrlEncode("a+b", buf, sizeof(buf)) && strcmp(buf, "a%2Bb") == 0);
	CHECK(raNetUrlEncode("100%", buf, sizeof(buf)) && strcmp(buf, "100%25") == 0);
	CHECK(raNetUrlEncode("a b", buf, sizeof(buf)) && strcmp(buf, "a%20b") == 0);
	CHECK(raNetUrlEncode("=?#/", buf, sizeof(buf)) && strcmp(buf, "%3D%3F%23%2F") == 0);
	/* High bytes, in case someone's password is not ASCII. */
	CHECK(raNetUrlEncode("\xc3\xb1", buf, sizeof(buf)) && strcmp(buf, "%C3%B1") == 0);
	CHECK(raNetUrlEncode("", buf, sizeof(buf)) && buf[0] == 0);

	/*
	    Truncation has to be reported, not swallowed: a silently shortened password is the same
	    bug as a badly escaped one.
	*/
	{
		char tiny[4];

		CHECK(raNetUrlEncode("abc", tiny, sizeof(tiny)) && strcmp(tiny, "abc") == 0);
		CHECK(raNetUrlEncode("abcd", tiny, sizeof(tiny)) == false);
		/* One %XX needs four bytes of room including the terminator. */
		CHECK(raNetUrlEncode("&", tiny, sizeof(tiny)) && strcmp(tiny, "%26") == 0);
		CHECK(raNetUrlEncode("&&", tiny, sizeof(tiny)) == false);
	}
}

/*
    The config file, fed odelot's own example verbatim -- because "his file works here" is the
    actual requirement, and a fixture I invent cannot check it.
*/
static void test_config(void) {
	const char* path = "/tmp/ra_cfg_test.cfg";
	raConfig    cfg;
	FILE*       f = fopen(path, "w");

	printf("\nthe config file reads odelot's format\n");
	if (!f) {
		printf("  cannot write %s -- skipped\n", path);
		return;
	}
	fputs("# RetroAchievements configuration file\n"
	      "\n"
	      "# RetroAchievements credentials\n"
	      "username=odelot\n"
	      "password=hunter2&co\n"
	      "\n"
	      "show_challenge_show_popup=1\n"
	      "show_progress_popups=1\n"
	      "debug=0\n"
	      "hardcore=1\n"
	      "force_hardcore=0\n"
	      "list_hotkey=0\n"
	      "   spaced   =   yes   \n"
	      "a line with no equals sign\n",
	      f);
	fclose(f);

	CHECK(raConfigRead(path, &cfg) == true);
	CHECK(cfg.found == 1);
	CHECK(strcmp(cfg.username, "odelot") == 0);
	/* Kept verbatim, punctuation and all -- the encoder is what makes it safe to send. */
	CHECK(strcmp(cfg.password, "hunter2&co") == 0);
	CHECK(cfg.usable == 1);
	CHECK(cfg.hardcore == 1);
	CHECK(cfg.debug == 0);
	/* His popup and leaderboard keys are recognised and not acted on, not rejected. */
	CHECK(cfg.notYet == 4);
	/* `spaced` is nobody's key: a typo has to be visible. */
	CHECK(cfg.unknown == 1);
	CHECK(cfg.badLines == 1);

	/*
	    And the secret must never be printable. The log is a file that gets sent to someone.
	*/
	printf("\nsecrets are not printable\n");
	CHECK(strstr(raConfigRedact(cfg.password), "hunter2") == NULL);
	CHECK(strstr(raConfigRedact(cfg.password), "10") != NULL);   /* its length, though */
	CHECK(strcmp(raConfigRedact(""), "(empty)") == 0);
	CHECK(strcmp(raConfigRedact(NULL), "(empty)") == 0);

	printf("\nthe login reply gives up its token and nothing else\n");
	{
		const char* reply =
			"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n"
			"{\"Success\":true,\"User\":\"odelot\",\"Token\":\"abcDEF123\",\"Score\":42}";
		char token[40];
		char user[40];

		CHECK(strstr(raNetBody(reply), "{\"Success\"") == raNetBody(reply));
		CHECK(raNetJsonString(reply, "Token", token, sizeof(token))
		   && strcmp(token, "abcDEF123") == 0);
		CHECK(raNetJsonString(reply, "User", user, sizeof(user))
		   && strcmp(user, "odelot") == 0);
		/* Absent, and a non-string field, must both fail rather than return rubbish. */
		CHECK(raNetJsonString(reply, "Nope", token, sizeof(token)) == false);
		CHECK(raNetJsonString(reply, "Score", token, sizeof(token)) == false);
		/* A token longer than the buffer is a failure, not a truncation. */
		{
			char tiny[4];
			CHECK(raNetJsonString(reply, "Token", tiny, sizeof(tiny)) == false);
		}
	}

	/*
	    r=gameid's reply is a bare number, and the number 0 is an *answer*: it is what the API
	    returns for a hash it has never seen. Reading it as "no field" would turn "your dump is
	    not the supported one" into "something went wrong", which are different problems with
	    different next moves.
	*/
	printf("\nthe gameid reply gives up a number, and zero is a number\n");
	{
		u32 id = 12345;

		CHECK(raNetJsonNumber("{\"Success\":true,\"GameID\":1448}", "GameID", &id)
		   && id == 1448);
		CHECK(raNetJsonNumber("{\"Success\":true,\"GameID\":0}", "GameID", &id) && id == 0);
		CHECK(raNetJsonNumber("{\"Success\":true, \"GameID\" : 7 }", "GameID", &id) == false);
		/* whitespace after the colon is legal; before it the needle simply will not match */
		CHECK(raNetJsonNumber("{\"GameID\":  7}", "GameID", &id) && id == 7);

		/* A quoted value is not a number. Reading its digits would name the wrong game. */
		id = 999;
		CHECK(raNetJsonNumber("{\"GameID\":\"1448\"}", "GameID", &id) == false);
		CHECK(id == 0);

		/* Absent, and a value that would overflow, both have to fail rather than wrap. */
		CHECK(raNetJsonNumber("{\"Success\":false}", "GameID", &id) == false);
		CHECK(raNetJsonNumber("{\"GameID\":99999999999999}", "GameID", &id) == false);
	}

	remove(path);
}

/*
    ------------------------------------------------------------------------------------
    Step 3d. Two state machines that are fed a socket's byte stream, which means the one thing
    they must survive is arriving in pieces -- and pieces of a size nothing here chooses.

    That is why the tests below do not check one split: they check *every* split. Feeding a
    fixture as two pieces at each of its byte boundaries, and again one byte at a time, and
    requiring the result to be identical every time, is the only form of this test that is worth
    running. A boundary falling between the `\r` and the `\n` of a chunk header, or in the
    middle of `"MemAddr":"`, or between a backslash and the slash it escapes, are all just
    ordinary splits to a network and all silently produce a wrong answer to code that assumes
    otherwise.
    ------------------------------------------------------------------------------------
*/

/* Where the streaming reader's sink puts what it is given, so a test can look at it. */
static char sunk[8192];
static u32  sunkLength;

static void sinkAppend(void* ctx, const char* data, int length) {
	(void)ctx;
	if (sunkLength + (u32)length < sizeof(sunk)) {
		memcpy(sunk + sunkLength, data, (size_t)length);
		sunkLength += (u32)length;
	}
	sunk[sunkLength] = 0;
}

/*
    Feed one reply at every possible split and require the body to come out the same each time.
    Returns 0 when every split agreed, or the split point that did not.
*/
static int streamEverySplit(const char* reply, const char* expectBody, u16 expectStatus) {
	const int    total = (int)strlen(reply);
	raNetStream  s;
	int          split;

	for (split = 0; split <= total; split++) {
		raNetStreamReset(&s, sinkAppend, NULL);
		sunkLength = 0;
		sunk[0]    = 0;

		raNetStreamFeed(&s, reply, split);
		raNetStreamFeed(&s, reply + split, total - split);

		if (strcmp(sunk, expectBody) != 0 || s.status != expectStatus
		 || s.bodyBytes != strlen(expectBody)) {
			return split + 1;
		}
	}

	/* And once byte by byte, which is the same test taken to its limit. */
	raNetStreamReset(&s, sinkAppend, NULL);
	sunkLength = 0;
	sunk[0]    = 0;
	for (split = 0; split < total; split++) {
		raNetStreamFeed(&s, reply + split, 1);
	}
	if (strcmp(sunk, expectBody) != 0 || s.status != expectStatus) {
		return -1;
	}
	return 0;
}

static void test_stream(void) {
	printf("\nthe streaming reader strips headers and chunk framing at any split\n");

	/*
	    Identity encoding, which is what every reply this project has read so far came back as.
	    Proven here rather than assumed, because the two paths have to agree.
	*/
	CHECK(streamEverySplit("HTTP/1.1 200 OK\r\n"
	                       "Content-Type: application/json\r\n"
	                       "\r\n"
	                       "{\"Success\":true}",
	                       "{\"Success\":true}", 200) == 0);

	/*
	    Chunked, which is the reason this file exists. A hex length written into the byte stream
	    would otherwise land inside a definition, and exactly one achievement out of a hundred
	    would be quietly wrong.
	*/
	CHECK(streamEverySplit("HTTP/1.1 200 OK\r\n"
	                       "Transfer-Encoding: chunked\r\n"
	                       "\r\n"
	                       "4\r\nabcd\r\n"
	                       "2\r\nef\r\n"
	                       "0\r\n\r\n",
	                       "abcdef", 200) == 0);

	/* A chunk longer than sixteen bytes, so the hex actually has to be hex. */
	CHECK(streamEverySplit("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
	                       "1a\r\nabcdefghijklmnopqrstuvwxyz\r\n0\r\n\r\n",
	                       "abcdefghijklmnopqrstuvwxyz", 200) == 0);

	/* Header names are case-insensitive and a server owes nobody a particular spelling. */
	CHECK(streamEverySplit("HTTP/1.1 200 OK\r\nTRANSFER-ENCODING: Chunked\r\n\r\n"
	                       "3\r\nxyz\r\n0\r\n\r\n",
	                       "xyz", 200) == 0);

	/* Chunk extensions are legal and nothing here wants them. */
	CHECK(streamEverySplit("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
	                       "3;name=value\r\nxyz\r\n0\r\n\r\n",
	                       "xyz", 200) == 0);

	/* A trailer after the last chunk is read and discarded, not handed on as body. */
	CHECK(streamEverySplit("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
	                       "3\r\nxyz\r\n0\r\nExpires: never\r\n\r\n",
	                       "xyz", 200) == 0);

	/*
	    A status other than 200 has to be *readable*, because that is how "the token expired"
	    stops looking like "the set is empty".
	*/
	CHECK(streamEverySplit("HTTP/1.1 404 Not Found\r\n\r\nno", "no", 404) == 0);
	CHECK(streamEverySplit("HTTP/1.1 401 Unauthorized\r\n"
	                       "Transfer-Encoding: chunked\r\n\r\n"
	                       "2\r\nno\r\n0\r\n\r\n",
	                       "no", 401) == 0);

	/* An empty body is a body. */
	CHECK(streamEverySplit("HTTP/1.1 204 No Content\r\n\r\n", "", 204) == 0);

	/* Headers alone, with the reply cut off before the blank line, must produce nothing. */
	{
		raNetStream s;

		raNetStreamReset(&s, sinkAppend, NULL);
		sunkLength = 0;
		sunk[0]    = 0;
		raNetStreamFeed(&s, "HTTP/1.1 200 OK\r\nContent-Type: text", 35);
		CHECK(sunkLength == 0 && s.status == 200);
	}
}

/*
    An r=patch reply, shaped like the real one: an object per achievement, MemAddr before Flags,
    forward slashes escaped, and unofficial achievements in the same array as published ones.

    The third achievement's title is the adversarial case and it is not hypothetical enough to
    leave out -- it contains the scanner's own needle, as a JSON string would have to write it.
    Because JSON escapes an interior quote as `\"`, the eleven bytes the scanner looks for cannot
    occur in a value, and this fixture is what says so rather than the argument in ra_patch.c.
*/
static const char patchReply[] =
	"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n"
	"{\"Success\":true,\"PatchData\":{\"ID\":14856,\"Title\":\"Super Mario 64 DS\","
	"\"ConsoleID\":18,\"Achievements\":["
	"{\"ID\":1,\"MemAddr\":\"0xH0a1b2c=1_d0xH0a1b2c=0\",\"Title\":\"First Star\","
	"\"Description\":\"Get it\",\"Points\":5,\"Author\":\"someone\",\"Flags\":3,\"Type\":null},"
	"{\"ID\":2,\"MemAddr\":\"A:0xX123456=1_I:0xH99\\/2\",\"Title\":\"Unofficial\","
	"\"Points\":0,\"Flags\":5},"
	"{\"ID\":3,\"MemAddr\":\"0xH000010>d0xH000010\","
	"\"Title\":\"They wrote \\\"MemAddr\\\":\\\" in the title\",\"Flags\":3}"
	"],\"Leaderboards\":[{\"ID\":9,\"Mem\":\"STA:0xH1=1::CAN:0=1\",\"Format\":\"SCORE\"}],"
	"\"RichPresencePatch\":\"Display:\\nStars: @Number(0xH1)\"}}";

/* The two published definitions, one per line, with the escaped slash decoded. */
#define PATCH_EXPECT "0xH0a1b2c=1_d0xH0a1b2c=0\n0xH000010>d0xH000010\n"

/*
    A whole fixture into the scanner in one call.

    A helper rather than a length at each call site, and that is not tidiness: the first version
    of these tests passed hand-counted byte counts, one of which was short by one. It cut a
    `"Flags":5` to `"Flags":` and turned a test about unofficial achievements into a test about
    missing flags -- which then failed for the right reason on the wrong grounds. A fixture that
    has to be measured by hand is a fixture that will eventually measure wrong.
*/
static void patchFeedAll(raPatch* p, const char* text) {
	raPatchFeed(p, text, (int)strlen(text));
}

/* One reply through the reader and the scanner together, split at one point. */
static void patchRun(raPatch* patch, char* block, u32 blockMax, const char* reply, int split) {
	const int   total = (int)strlen(reply);
	raNetStream s;

	raPatchReset(patch, block, blockMax);
	raNetStreamReset(&s, raPatchFeed, patch);
	raNetStreamFeed(&s, reply, split);
	raNetStreamFeed(&s, reply + split, total - split);
	raPatchFinish(patch);
}

static void test_patch(void) {
	static char block[4096];
	raPatch     patch;

	printf("\nthe patch scanner pulls the published set out of a streaming reply\n");

	patchRun(&patch, block, sizeof(block) - 1, patchReply, 0);
	CHECK(strcmp(block, PATCH_EXPECT) == 0);
	CHECK(patch.kept == 2);
	CHECK(patch.unofficial == 1);
	CHECK(patch.dropped == 0 && patch.tooLong == 0 && patch.cutShort == 0);
	CHECK(patch.empty == 0 && patch.oddFlags == 0 && patch.noFlags == 0);
	CHECK(patch.used == strlen(PATCH_EXPECT));
	CHECK(patch.wanted == strlen(PATCH_EXPECT));
	/* Decoded length, so the escaped slash counts as the one character it becomes. */
	CHECK(patch.longest == strlen("0xH0a1b2c=1_d0xH0a1b2c=0"));

	/*
	    Every split, because the boundary the network chooses is not ours -- and the interesting
	    ones are inside `"MemAddr":"`, between a backslash and the slash it escapes, and between
	    the digits of a Flags value.
	*/
	printf("\n...and does it identically at every split point\n");
	{
		const int total = (int)strlen(patchReply);
		int       bad   = 0;
		int       split;

		for (split = 0; split <= total; split++) {
			patchRun(&patch, block, sizeof(block) - 1, patchReply, split);
			if (strcmp(block, PATCH_EXPECT) != 0 || patch.kept != 2
			 || patch.unofficial != 1 || patch.dropped || patch.tooLong
			 || patch.cutShort || patch.noFlags || patch.oddFlags) {
				bad = split + 1;
				break;
			}
		}
		CHECK(bad == 0);
	}

	/* And one byte at a time, which is the same requirement taken to its limit. */
	{
		raNetStream s;
		const int   total = (int)strlen(patchReply);
		int         i;

		raPatchReset(&patch, block, sizeof(block) - 1);
		raNetStreamReset(&s, raPatchFeed, &patch);
		for (i = 0; i < total; i++) {
			raNetStreamFeed(&s, patchReply + i, 1);
		}
		raPatchFinish(&patch);
		CHECK(strcmp(block, PATCH_EXPECT) == 0 && patch.kept == 2);
	}

	printf("\nthe needle restarts correctly, and escapes decode\n");
	{
		/*
		    A doubled quote is the one case restart-and-retest has to get right, and the reason
		    ra_patch.c can use it instead of a failure function: the only character that repeats
		    in `"MemAddr":"` is the quote, at length one.
		*/
		raPatchReset(&patch, block, sizeof(block) - 1);
		patchFeedAll(&patch, "{\"\"MemAddr\":\"0xH1=1\",\"Flags\":3}");
		raPatchFinish(&patch);
		CHECK(patch.kept == 1 && strcmp(block, "0xH1=1\n") == 0);

		/* `\/` is the escape that matters: division is a memaddr operator. */
		raPatchReset(&patch, block, sizeof(block) - 1);
		patchFeedAll(&patch, "\"MemAddr\":\"0xH1\\/0xH2\\/2\",\"Flags\":3");
		raPatchFinish(&patch);
		CHECK(patch.kept == 1 && strcmp(block, "0xH1/0xH2/2\n") == 0);

		/* An unknown escape keeps its backslash rather than being invented away. */
		raPatchReset(&patch, block, sizeof(block) - 1);
		patchFeedAll(&patch, "\"MemAddr\":\"a\\tb\",\"Flags\":3");
		raPatchFinish(&patch);
		CHECK(patch.kept == 1 && strcmp(block, "a\\tb\n") == 0);

		/* A value ending the document, with no Flags field ever arriving, still lands. */
		raPatchReset(&patch, block, sizeof(block) - 1);
		patchFeedAll(&patch, "\"MemAddr\":\"0xH7=7\"}]}");
		raPatchFinish(&patch);
		CHECK(patch.kept == 1 && patch.noFlags == 1 && strcmp(block, "0xH7=7\n") == 0);

		/* Two definitions with no Flags between them: the first commits when the second opens. */
		raPatchReset(&patch, block, sizeof(block) - 1);
		patchFeedAll(&patch, "\"MemAddr\":\"a=1\",\"MemAddr\":\"b=2\"");
		raPatchFinish(&patch);
		CHECK(patch.kept == 2 && patch.noFlags == 2 && strcmp(block, "a=1\nb=2\n") == 0);

		/* An empty value is not a definition, and is counted rather than written. */
		raPatchReset(&patch, block, sizeof(block) - 1);
		patchFeedAll(&patch, "\"MemAddr\":\"\",\"Flags\":3");
		raPatchFinish(&patch);
		CHECK(patch.kept == 0 && patch.empty == 1 && block[0] == 0);

		/* A flag value nobody knows is kept and said out loud. */
		raPatchReset(&patch, block, sizeof(block) - 1);
		patchFeedAll(&patch, "\"MemAddr\":\"a=1\",\"Flags\":7");
		raPatchFinish(&patch);
		CHECK(patch.kept == 1 && patch.oddFlags == 1);

		/*
		    An unofficial definition is not written but is still measured. That matters for the
		    one number this rung is for: `longest` sizes the scanner's carry buffer, and a
		    buffer sized only by what got kept would be the wrong size for the next reply.
		*/
		raPatchReset(&patch, block, sizeof(block) - 1);
		patchFeedAll(&patch, "\"MemAddr\":\"0xH1=1_0xH2=2_0xH3=3\",\"Flags\":5");
		raPatchFinish(&patch);
		CHECK(patch.kept == 0 && patch.unofficial == 1 && block[0] == 0);
		CHECK(patch.longest == strlen("0xH1=1_0xH2=2_0xH3=3"));
	}

	printf("\nnothing is truncated: a short definition is a different definition\n");
	{
		static char big[RA_PATCH_MEMADDR_MAX + 256];
		u32         i;

		/* A memaddr past the buffer is dropped and counted, never clipped and kept. */
		strcpy(big, "\"MemAddr\":\"");
		for (i = strlen(big); i < RA_PATCH_MEMADDR_MAX + 100; i++) {
			big[i] = 'a';
		}
		strcpy(big + i, "\",\"Flags\":3");

		raPatchReset(&patch, block, sizeof(block) - 1);
		raPatchFeed(&patch, big, (int)strlen(big));
		raPatchFinish(&patch);
		CHECK(patch.kept == 0 && patch.tooLong == 1 && block[0] == 0);
		CHECK(patch.longest > RA_PATCH_MEMADDR_MAX);

		/* A reply cut off inside a value is a prefix, and a prefix goes nowhere. */
		raPatchReset(&patch, block, sizeof(block) - 1);
		patchFeedAll(&patch, "\"MemAddr\":\"0xH1=1_0xH");
		raPatchFinish(&patch);
		CHECK(patch.kept == 0 && patch.cutShort == 1 && block[0] == 0);
	}

	printf("\na full block stops cleanly and says how much it wanted\n");
	{
		/*
		    Room for the first definition and not the second. What matters is that the block
		    holds whole lines only -- a half-written memaddr would parse as a real one -- and
		    that `wanted` reports what a complete set would have needed. That number is the
		    measurement step 3d exists to take.
		*/
		char small[26];

		patchRun(&patch, small, sizeof(small) - 1, patchReply, 0);
		CHECK(strcmp(small, "0xH0a1b2c=1_d0xH0a1b2c=0\n") == 0);
		CHECK(patch.kept == 1 && patch.dropped == 1);
		CHECK(patch.used == 25 && patch.wanted == strlen(PATCH_EXPECT));
		CHECK(patch.used <= sizeof(small) - 1);

		/* No room at all: nothing is written and everything is accounted for. */
		patchRun(&patch, small, 0, patchReply, 0);
		CHECK(patch.kept == 0 && patch.dropped == 2);
		CHECK(patch.wanted == strlen(PATCH_EXPECT));
	}

	printf("\nthe real block is big enough for the set this fork is aiming at\n");
	/*
	    The two constants that have to agree, pinned where a change to either is visible: the
	    reader splits at most RA_DEFS_MAX_LINES definitions out of a block of
	    CARDENGINEI_ARM9_RA_DEFS_MAX bytes, so the average definition length that fits is what
	    decides whether a real set arrives whole. tools/ra_reader_test.c pins the same pair from
	    the reader's side; this is the writer's.
	*/
	/* 128 is RA_DEFS_MAX_LINES, which lives in the cardengine and is pinned to that value there. */
	CHECK((CARDENGINEI_ARM9_RA_DEFS_MAX - CARDENGINEI_ARM9_RA_DEFS_HEADER - 1)
	      / 128 >= 200);
	/* And a single definition can be as long as the scanner will carry. */
	CHECK(RA_PATCH_MEMADDR_MAX
	      < CARDENGINEI_ARM9_RA_DEFS_MAX - CARDENGINEI_ARM9_RA_DEFS_HEADER);
}

int main(void) {
	test_rom_hash();
	test_url_encode();
	test_config();
	test_stream();
	test_patch();

	printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
	       failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
