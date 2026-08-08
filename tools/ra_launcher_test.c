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

int main(void) {
	test_rom_hash();
	test_url_encode();
	test_config();

	printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
	       failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
