/*
    Host-side test for the RA reader's watchlist and pointer-chain walker.

    Everything else in this fork has to be checked by flashing a card and reading hex
    out of the in-game menu's RAM viewer, which is slow enough that three separate
    bugs in the overlay each cost a flash cycle to find. The chain walker is pure
    logic over addresses, so it does not need the hardware -- and getting it wrong is
    a Data Abort inside the game's interrupt handler, i.e. a crash rather than a
    glitch. So it is tested here first.

    The trick that makes this a real test rather than a mock is the link address: the
    runner links this at 0x02100000 and maps a page at 0x04000000, so the reader's
    own globals really do sit inside the main RAM range it validates against and the
    I/O reads really do land on mapped memory. Nothing in ra_reader.c is stubbed or
    conditionally compiled -- the file is included verbatim, statics and all.

    Run it with tools/ra_reader_test.sh.

    This file is part of nds-bootstrap and is licensed under the GPL-3.0,
    the same terms as the rest of the project.
*/

#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include "../retail/cardenginei/arm9/source/ra_reader.c"

/* ra_reader.c reads these out of the overlay; here they are just storage. */
u32 raOverlayShows, raOverlayDenied, raOverlayEvicted, raOverlayDeniedNoLayer;

#define IO_BASE  0x04000000
#define IO_SIZE  0x2000
#define VCOUNT   ((vu16*)0x04000006)
#define DISPCNT  ((vu16*)RA_DEFAULT_WATCH_ADDRESS)

static int failures;

/*
    The reader has no watch_clear() of its own yet -- it had no caller and the
    cardengine had no room, see ra_reader.h. The test needs to start from an empty
    list, so it does what that function would have done.
*/
static void clear_watches(void) {
	int i;
	for (i = 0; i < RA_WATCH_MAX; i++) {
		raSnapshotBuffer.watches[i].status = RA_WATCH_UNUSED;
	}
	raSnapshotBuffer.watchCount = 0;
}

#define CHECK(cond) do { \
	if (cond) { \
		printf("  ok    %s\n", #cond); \
	} else { \
		printf("  FAIL  %s\n", #cond); \
		failures++; \
	} \
} while (0)

static void expect_status(const char* what, int index, u8 want) {
	const u8 got = raSnapshotBuffer.watches[index].status;
	if (got == want) {
		printf("  ok    %s: status %u\n", what, want);
	} else {
		printf("  FAIL  %s: status %u, wanted %u\n", what, got, want);
		failures++;
	}
}

/* A word inside main RAM the test can point chains at. */
static u32 target = 0xDEADBEEF;
static u32 targetPtr;
static u32 targetPtrPtr;

int main(void) {
	u32 offsets[RA_CHAIN_MAX];
	int i, slot;

	/*
	    Map the I/O page. The reader reads VCOUNT to time itself and the default
	    watch reads the sub engine's DISPCNT, so both have to be real memory here.
	*/
	if (mmap((void*)IO_BASE, IO_SIZE, PROT_READ | PROT_WRITE,
	         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED) {
		perror("mmap 0x04000000");
		return 2;
	}

	printf("link addresses\n");
	CHECK(ra_in_main_ram((u32)&raSnapshotBuffer, sizeof(raSnapshotBuffer)));
	CHECK(ra_in_main_ram((u32)&target, 4));

	/*
	    docs/retroachievements.md tells you to read specific offsets in the in-game
	    menu's RAM viewer. Those offsets are the interface, so they are pinned here:
	    reorder the struct and this fails instead of the instructions going stale.
	*/
	printf("\nthe documented snapshot offsets hold\n");
	CHECK(sizeof(raWatch) == 0x18);
	CHECK(__builtin_offsetof(raSnapshot, ticks) == 0x04);
	CHECK(__builtin_offsetof(raSnapshot, shows) == 0x08);
	CHECK(__builtin_offsetof(raSnapshot, deniedNoLayer) == 0x14);
	CHECK(__builtin_offsetof(raSnapshot, watchCount) == 0x18);
	CHECK(__builtin_offsetof(raSnapshot, linesMax) == 0x1B);
	CHECK(__builtin_offsetof(raSnapshot, watches) == 0x1C);
	CHECK((u32)&raSnapshotBuffer.watches[1] - (u32)&raSnapshotBuffer == 0x34);
	CHECK((u32)&raSnapshotBuffer.watches[2] - (u32)&raSnapshotBuffer == 0x4C);

	*DISPCNT = 0x1F40;
	*VCOUNT = 0;

	printf("\nfirst tick claims the buffer and installs the defaults\n");
	memset(&raSnapshotBuffer, 0xA5, sizeof(raSnapshotBuffer));  /* .bss is garbage */
	ra_reader_tick();
	CHECK(raSnapshotBuffer.magic[0] == 'R' && raSnapshotBuffer.magic[1] == 'A');
	CHECK(raSnapshotBuffer.magic[2] == '1' && raSnapshotBuffer.magic[3] == 'S');
	CHECK(raSnapshotBuffer.ticks == 1);
	CHECK(raSnapshotBuffer.watchCount == 3);
	CHECK(raSnapshotBuffer.resolved == 3);

	printf("\ndefault watch 0 reads a register directly\n");
	expect_status("direct", 0, RA_WATCH_OK);
	CHECK(raSnapshotBuffer.watches[0].address == RA_DEFAULT_WATCH_ADDRESS);
	CHECK(raSnapshotBuffer.watches[0].value == 0x1F40);

	printf("\ndefault watches 1 and 2 walk one and two indirections to ticks\n");
	expect_status("one step", 1, RA_WATCH_OK);
	expect_status("two steps", 2, RA_WATCH_OK);
	CHECK(raSnapshotBuffer.watches[1].address == (u32)&raSnapshotBuffer.ticks);
	CHECK(raSnapshotBuffer.watches[2].address == (u32)&raSnapshotBuffer.ticks);
	CHECK(raSnapshotBuffer.watches[1].value == 1);
	CHECK(raSnapshotBuffer.watches[2].value == 1);

	printf("\nthe chains are re-walked every tick, so the values track ticks\n");
	*DISPCNT = 0x0100;
	for (i = 0; i < 9; i++) {
		ra_reader_tick();
	}
	CHECK(raSnapshotBuffer.ticks == 10);
	CHECK(raSnapshotBuffer.watches[1].value == 10);
	CHECK(raSnapshotBuffer.watches[2].value == 10);
	CHECK(raSnapshotBuffer.watches[0].value == 0x0100);

	printf("\nan emptied list resolves nothing\n");
	clear_watches();
	CHECK(raSnapshotBuffer.watchCount == 0);
	ra_reader_tick();
	CHECK(raSnapshotBuffer.resolved == 0);

	printf("\nmalformed requests are refused\n");
	CHECK(ra_reader_watch_add((u32)&target, 3, 0, 0) == -1);   /* size not 1/2/4 */
	CHECK(ra_reader_watch_add((u32)&target, 0, 0, 0) == -1);
	CHECK(ra_reader_watch_add((u32)&target, 8, 0, 0) == -1);
	offsets[0] = 0;
	offsets[1] = 0;
	CHECK(ra_reader_watch_add((u32)&target, 4, RA_CHAIN_MAX + 1, offsets) == -1);
	CHECK(ra_reader_watch_add((u32)&target, 4, 1, 0) == -1);   /* depth without offsets */
	CHECK(raSnapshotBuffer.watchCount == 0);

	printf("\nsized reads take the right width\n");
	target = 0x11223344;
	clear_watches();
	CHECK(ra_reader_watch_add((u32)&target, 1, 0, 0) == 0);
	CHECK(ra_reader_watch_add((u32)&target, 2, 0, 0) == 1);
	CHECK(ra_reader_watch_add((u32)&target, 4, 0, 0) == 2);
	ra_reader_tick();
	CHECK(raSnapshotBuffer.watches[0].value == 0x44);      /* little-endian, as the DS is */
	CHECK(raSnapshotBuffer.watches[1].value == 0x3344);
	CHECK(raSnapshotBuffer.watches[2].value == 0x11223344);

	printf("\nthe list fills up and then refuses\n");
	slot = ra_reader_watch_add((u32)&target, 4, 0, 0);
	CHECK(slot == RA_WATCH_MAX - 1);
	CHECK(ra_reader_watch_add((u32)&target, 4, 0, 0) == -1);

	printf("\nan unreadable base is reported, not dereferenced\n");
	clear_watches();
	ra_reader_watch_add(0x09000000, 4, 0, 0);   /* GBA cart space: not in the list */
	ra_reader_tick();
	expect_status("bad base", 0, RA_WATCH_BAD_BASE);
	CHECK(raSnapshotBuffer.watches[0].address == 0);

	printf("\na misaligned target is reported rather than silently rotated\n");
	clear_watches();
	ra_reader_watch_add((u32)&target + 1, 4, 0, 0);
	ra_reader_watch_add((u32)&target + 1, 2, 0, 0);
	ra_reader_watch_add((u32)&target + 1, 1, 0, 0);   /* bytes are never misaligned */
	ra_reader_tick();
	expect_status("misaligned word", 0, RA_WATCH_MISALIGNED);
	expect_status("misaligned halfword", 1, RA_WATCH_MISALIGNED);
	expect_status("byte", 2, RA_WATCH_OK);

	/*
	    The failure statuses say where the chain broke, which is the useful thing to
	    know from a RAM viewer: BAD_POINTER means a word the walker was about to
	    dereference for a further step was unusable, while a bad *final* address is
	    BAD_TARGET or MISALIGNED however many steps preceded it. So a one-step chain
	    whose pointer goes null reports BAD_TARGET -- there was no further step -- and
	    reaching BAD_POINTER takes at least two.
	*/
	printf("\na one-step chain that stops resolving reports its target\n");
	targetPtr = (u32)&target;
	clear_watches();
	offsets[0] = 0;
	offsets[1] = 0;
	ra_reader_watch_add((u32)&targetPtr, 4, 1, offsets);
	ra_reader_tick();
	expect_status("chain resolves", 0, RA_WATCH_OK);
	CHECK(raSnapshotBuffer.watches[0].value == 0x11223344);

	targetPtr = 0;                      /* what a game does between scenes */
	ra_reader_tick();
	expect_status("null pointer", 0, RA_WATCH_BAD_TARGET);
	CHECK(raSnapshotBuffer.watches[0].address == 0);

	targetPtr = 0xFFFFFFFC;             /* addr + len would wrap past the check */
	ra_reader_tick();
	expect_status("wrapping pointer", 0, RA_WATCH_BAD_TARGET);
	CHECK(raSnapshotBuffer.watches[0].address == 0);

	targetPtr = (u32)&target + 1;
	ra_reader_tick();
	expect_status("misaligned resolved address", 0, RA_WATCH_MISALIGNED);

	printf("\nit recovers when the pointer comes back\n");
	targetPtr = (u32)&target;
	ra_reader_tick();
	expect_status("recovered", 0, RA_WATCH_OK);
	CHECK(raSnapshotBuffer.watches[0].value == 0x11223344);

	printf("\na pointer the walker would have to follow again is BAD_POINTER\n");
	clear_watches();
	targetPtr     = (u32)&target;
	targetPtrPtr  = (u32)&targetPtr;
	offsets[0] = 0;
	offsets[1] = 0;
	ra_reader_watch_add((u32)&targetPtrPtr, 4, 2, offsets);
	ra_reader_tick();
	expect_status("two steps resolve", 0, RA_WATCH_OK);
	CHECK(raSnapshotBuffer.watches[0].value == 0x11223344);

	targetPtrPtr = 0;                   /* step 0 yields a pointer step 1 must follow */
	ra_reader_tick();
	expect_status("null mid-chain", 0, RA_WATCH_BAD_POINTER);
	CHECK(raSnapshotBuffer.watches[0].address == 0);

	targetPtrPtr = (u32)&targetPtr + 1;  /* unaligned, so unusable as a pointer */
	ra_reader_tick();
	expect_status("misaligned mid-chain", 0, RA_WATCH_BAD_POINTER);

	targetPtrPtr = 0x04000000;           /* readable, but no game pointer lives in I/O */
	ra_reader_tick();
	expect_status("mid-chain outside main RAM", 0, RA_WATCH_BAD_POINTER);

	printf("\nan unreadable base is BAD_BASE even on a chain\n");
	clear_watches();
	ra_reader_watch_add(0x09000000, 4, 2, offsets);
	ra_reader_tick();
	expect_status("bad base, depth 2", 0, RA_WATCH_BAD_BASE);

	printf("\noffsets are added at each step\n");
	{
		static u32 pair[2];
		pair[0] = 0;
		pair[1] = 0x0BADF00D;
		targetPtr = (u32)&pair[0];
		clear_watches();
		offsets[0] = 4;             /* one word into pair[] */
		offsets[1] = 0;
		ra_reader_watch_add((u32)&targetPtr, 4, 1, offsets);
		ra_reader_tick();
		expect_status("offset applied", 0, RA_WATCH_OK);
		CHECK(raSnapshotBuffer.watches[0].address == (u32)&pair[1]);
		CHECK(raSnapshotBuffer.watches[0].value == 0x0BADF00D);
	}

	/*
	    The cost measurement can only be partly checked here: VCOUNT does not advance
	    on its own on the host, so the interesting case -- a tick that starts near line
	    261 and finishes after the wrap -- needs the real register moving underneath
	    the reader. What can be checked is that a tick which consumes nothing reports
	    nothing, i.e. the counter is a delta and not an absolute scanline.
	*/
	printf("\nthe per-frame cost is a delta, not an absolute scanline\n");
	*VCOUNT = 200;
	raSnapshotBuffer.linesMax = 0;
	ra_reader_tick();
	CHECK(raSnapshotBuffer.linesLast == 0);
	CHECK(raSnapshotBuffer.linesMax == 0);

	printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
	       failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
