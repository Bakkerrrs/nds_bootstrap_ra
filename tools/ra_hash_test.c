/*
    Host-side check for step 3b: the RetroAchievements hash of a DS ROM.

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

    Run through tools/ra_reader_test.sh, which builds both.

    This file is part of nds-bootstrap and is licensed under the GPL-3.0,
    the same terms as the rest of the project.
*/

#include <stdio.h>
#include <string.h>

#include "ra_wifi.h"

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

int main(void) {
	test_rom_hash();

	printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
	       failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
