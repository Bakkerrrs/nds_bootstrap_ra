/*
    Host-side test for the RA watchlist and its pointer-chain walker.

    Everything else in this fork has to be checked by flashing a card and reading hex out
    of the in-game menu's RAM viewer, which is slow enough that three separate bugs in the
    overlay each cost a flash cycle to find. The walker is pure logic over addresses, so it
    does not need the hardware -- and getting it wrong is a Data Abort inside the game's
    interrupt handler, i.e. a crash rather than a glitch. So it is tested here first.

    It compiles cardenginei_arm9_ra's source verbatim -- statics and all, nothing stubbed
    or conditionally compiled. The trick that makes that a real test rather than a mock is
    the link address: the runner links this at 0x02100000 and maps a page at 0x04000000, so
    the snapshot it hands over really does sit inside the main RAM range the walker
    validates against, and the I/O reads really do land on mapped memory. The chain
    self-tests resolve for the same reason they do on hardware, not because a check was
    relaxed.

    It followed the code when the watchlist moved out of the cardengine and into DSi WRAM;
    that is the point of pointing it at a source file rather than at an interface.

    Run it with tools/ra_reader_test.sh.

    This file is part of nds-bootstrap and is licensed under the GPL-3.0,
    the same terms as the rest of the project.
*/

#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include "../retail/cardenginei/arm9_ra/source/cardengine.c"

#define IO_BASE  0x04000000
#define IO_SIZE  0x2000
#define DISPCNT  ((vu16*)RA_DEFAULT_WATCH_ADDRESS)

/*
    Stands in for the cardengine's raSnapshotBuffer. Aligned and, thanks to the link
    address, inside main RAM as far as the walker is concerned.
*/
static raSnapshot snapshot __attribute__((aligned(16)));

static int failures;

#define CHECK(cond) do { \
	if (cond) { \
		printf("  ok    %s\n", #cond); \
	} else { \
		printf("  FAIL  %s\n", #cond); \
		failures++; \
	} \
} while (0)

static void expect_status(const char* what, int index, u8 want) {
	const u8 got = snapshot.results[index].status;
	if (got == want) {
		printf("  ok    %s: status %u\n", what, want);
	} else {
		printf("  FAIL  %s: status %u, wanted %u\n", what, got, want);
		failures++;
	}
}

/* Words inside main RAM the test can point chains at. */
static u32 target = 0xDEADBEEF;
static u32 targetPtr;
static u32 targetPtrPtr;

int main(void) {
	u32 offsets[RA_CHAIN_MAX];
	int i, slot;

	/*
	    Map the I/O page: the diagnostic watch reads the sub engine's DISPCNT, so that
	    has to be real memory here.
	*/
	if (mmap((void*)IO_BASE, IO_SIZE, PROT_READ | PROT_WRITE,
	         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED) {
		perror("mmap 0x04000000");
		return 2;
	}

	printf("link addresses\n");
	CHECK(ra_in_main_ram((u32)&snapshot, sizeof(snapshot)));
	CHECK(ra_in_main_ram((u32)&target, 4));

	/*
	    docs/retroachievements.md tells you to read specific offsets in the in-game menu's
	    RAM viewer. Those offsets are the interface, so they are pinned here: reorder the
	    struct and this fails instead of the instructions going stale.
	*/
	printf("\nthe documented snapshot offsets hold\n");
	CHECK(sizeof(raResult) == 0x0C);
	CHECK(__builtin_offsetof(raSnapshot, ticks) == 0x04);
	CHECK(__builtin_offsetof(raSnapshot, deniedNoLayer) == 0x14);
	CHECK(__builtin_offsetof(raSnapshot, watchCount) == 0x18);
	CHECK(__builtin_offsetof(raSnapshot, linesMax) == 0x1B);
	CHECK(__builtin_offsetof(raSnapshot, wramMagic) == 0x1C);
	CHECK(__builtin_offsetof(raSnapshot, wramTicks) == 0x20);
	CHECK(__builtin_offsetof(raSnapshot, wramState) == 0x24);
	CHECK(__builtin_offsetof(raSnapshot, selfCell) == 0x28);
	CHECK(__builtin_offsetof(raSnapshot, selfCellPtr) == 0x2C);
	CHECK(__builtin_offsetof(raSnapshot, results) == 0x30);
	CHECK(sizeof(raSnapshot) == 0x60);

	*DISPCNT = 0x1F40;

	printf("\nthe first tick claims .bss and installs the defaults\n");
	/* Neither side's .bss is ever zeroed on the target, so start from garbage. */
	memset(&snapshot, 0xA5, sizeof(snapshot));
	stateMagic = 0xA5A5A5A5;
	snapshot.ticks = 7;
	ra_wram_tick(&snapshot);
	CHECK(snapshot.watchCount == 2);
	CHECK(snapshot.resolved == 2);
	CHECK(snapshot.wramMagic == RA_WRAM_MAGIC);
	CHECK(snapshot.wramTicks == 1);

	printf("\nthe self-test cells point where the chain needs them to\n");
	CHECK(snapshot.selfCell == (u32)&snapshot);
	CHECK(snapshot.selfCellPtr == (u32)&snapshot.selfCell);

	printf("\ndefault watch 0 reads a register directly\n");
	expect_status("direct", 0, RA_WATCH_OK);
	CHECK(snapshot.results[0].address == RA_DEFAULT_WATCH_ADDRESS);
	CHECK(snapshot.results[0].value == 0x1F40);
	CHECK(snapshot.results[0].depth == 0);
	CHECK(snapshot.results[0].size == 2);

	printf("\ndefault watch 1 walks two indirections to ticks\n");
	expect_status("two steps", 1, RA_WATCH_OK);
	CHECK(snapshot.results[1].address == (u32)&snapshot.ticks);
	CHECK(snapshot.results[1].value == 7);
	CHECK(snapshot.results[1].depth == 2);

	printf("\nthe WRAM binary's own counter persists across ticks\n");
	*DISPCNT = 0x0100;
	for (i = 0; i < 9; i++) {
		snapshot.ticks++;
		ra_wram_tick(&snapshot);
	}
	CHECK(snapshot.wramTicks == 10);
	CHECK(snapshot.results[1].value == 16);   /* ticks went 7 -> 16 */
	CHECK(snapshot.results[0].value == 0x0100);

	printf("\nthe chains are re-walked every tick, not cached\n");
	snapshot.ticks = 0x1234;
	ra_wram_tick(&snapshot);
	CHECK(snapshot.results[1].value == 0x1234);

	printf("\nan emptied list resolves nothing\n");
	ra_watch_clear();
	ra_wram_tick(&snapshot);
	CHECK(snapshot.watchCount == 0);
	CHECK(snapshot.resolved == 0);

	printf("\nmalformed requests are refused\n");
	CHECK(ra_watch_add((u32)&target, 3, 0, 0) == -1);   /* size not 1/2/4 */
	CHECK(ra_watch_add((u32)&target, 0, 0, 0) == -1);
	CHECK(ra_watch_add((u32)&target, 8, 0, 0) == -1);
	offsets[0] = 0;
	offsets[1] = 0;
	CHECK(ra_watch_add((u32)&target, 4, RA_CHAIN_MAX + 1, offsets) == -1);
	CHECK(ra_watch_add((u32)&target, 4, 1, 0) == -1);   /* depth without offsets */
	CHECK(watchCount == 0);

	printf("\nsized reads take the right width\n");
	target = 0x11223344;
	ra_watch_clear();
	CHECK(ra_watch_add((u32)&target, 1, 0, 0) == 0);
	CHECK(ra_watch_add((u32)&target, 2, 0, 0) == 1);
	CHECK(ra_watch_add((u32)&target, 4, 0, 0) == 2);
	ra_wram_tick(&snapshot);
	CHECK(snapshot.results[0].value == 0x44);      /* little-endian, as the DS is */
	CHECK(snapshot.results[1].value == 0x3344);
	CHECK(snapshot.results[2].value == 0x11223344);

	printf("\nthe list holds many more watches than the cardengine could\n");
	ra_watch_clear();
	for (i = 0; i < RA_WATCH_MAX; i++) {
		if (ra_watch_add((u32)&target, 4, 0, 0) != i) {
			printf("  FAIL  slot %d\n", i);
			failures++;
		}
	}
	CHECK(RA_WATCH_MAX == 16);
	CHECK(ra_watch_add((u32)&target, 4, 0, 0) == -1);   /* and then refuses */
	ra_wram_tick(&snapshot);
	CHECK(snapshot.watchCount == RA_WATCH_MAX);
	CHECK(snapshot.resolved == RA_WATCH_MAX);

	printf("\nonly the first RA_RESULT_MAX are mirrored into the snapshot\n");
	CHECK(RA_RESULT_MAX == 4);
	CHECK(snapshot.results[RA_RESULT_MAX - 1].status == RA_WATCH_OK);

	printf("\nan unreadable base is reported, not dereferenced\n");
	ra_watch_clear();
	ra_watch_add(0x09000000, 4, 0, 0);   /* GBA cart space: not in the list */
	ra_wram_tick(&snapshot);
	expect_status("bad base", 0, RA_WATCH_BAD_BASE);
	CHECK(snapshot.results[0].address == 0);

	printf("\nthe window this code runs from is not readable either\n");
	ra_watch_clear();
	ra_watch_add(CARDENGINEI_ARM9_RA_LOCATION, 4, 0, 0);
	ra_wram_tick(&snapshot);
	expect_status("own window", 0, RA_WATCH_BAD_BASE);

	printf("\na misaligned target is reported rather than silently rotated\n");
	ra_watch_clear();
	ra_watch_add((u32)&target + 1, 4, 0, 0);
	ra_watch_add((u32)&target + 1, 2, 0, 0);
	ra_watch_add((u32)&target + 1, 1, 0, 0);   /* bytes are never misaligned */
	ra_wram_tick(&snapshot);
	expect_status("misaligned word", 0, RA_WATCH_MISALIGNED);
	expect_status("misaligned halfword", 1, RA_WATCH_MISALIGNED);
	expect_status("byte", 2, RA_WATCH_OK);

	/*
	    The failure statuses say where the chain broke, which is the useful thing to know
	    from a RAM viewer: BAD_POINTER means a word the walker was about to dereference for
	    a *further* step was unusable, while a bad final address is BAD_TARGET or
	    MISALIGNED however many steps preceded it. So a one-step chain whose pointer goes
	    null reports BAD_TARGET -- there was no further step -- and reaching BAD_POINTER
	    takes at least two.
	*/
	printf("\na one-step chain that stops resolving reports its target\n");
	targetPtr = (u32)&target;
	ra_watch_clear();
	offsets[0] = 0;
	offsets[1] = 0;
	ra_watch_add((u32)&targetPtr, 4, 1, offsets);
	ra_wram_tick(&snapshot);
	expect_status("chain resolves", 0, RA_WATCH_OK);
	CHECK(snapshot.results[0].value == 0x11223344);

	targetPtr = 0;                      /* what a game does between scenes */
	ra_wram_tick(&snapshot);
	expect_status("null pointer", 0, RA_WATCH_BAD_TARGET);
	CHECK(snapshot.results[0].address == 0);

	targetPtr = 0xFFFFFFFC;             /* addr + len would wrap past the check */
	ra_wram_tick(&snapshot);
	expect_status("wrapping pointer", 0, RA_WATCH_BAD_TARGET);

	targetPtr = (u32)&target + 1;
	ra_wram_tick(&snapshot);
	expect_status("misaligned resolved address", 0, RA_WATCH_MISALIGNED);

	printf("\nit recovers when the pointer comes back\n");
	targetPtr = (u32)&target;
	ra_wram_tick(&snapshot);
	expect_status("recovered", 0, RA_WATCH_OK);
	CHECK(snapshot.results[0].value == 0x11223344);

	printf("\na pointer the walker would have to follow again is BAD_POINTER\n");
	ra_watch_clear();
	targetPtr    = (u32)&target;
	targetPtrPtr = (u32)&targetPtr;
	offsets[0] = 0;
	offsets[1] = 0;
	ra_watch_add((u32)&targetPtrPtr, 4, 2, offsets);
	ra_wram_tick(&snapshot);
	expect_status("two steps resolve", 0, RA_WATCH_OK);
	CHECK(snapshot.results[0].value == 0x11223344);

	targetPtrPtr = 0;                    /* step 0 yields a pointer step 1 must follow */
	ra_wram_tick(&snapshot);
	expect_status("null mid-chain", 0, RA_WATCH_BAD_POINTER);
	CHECK(snapshot.results[0].address == 0);

	targetPtrPtr = (u32)&targetPtr + 1;  /* unaligned, so unusable as a pointer */
	ra_wram_tick(&snapshot);
	expect_status("misaligned mid-chain", 0, RA_WATCH_BAD_POINTER);

	targetPtrPtr = 0x04000000;           /* readable, but no game pointer lives in I/O */
	ra_wram_tick(&snapshot);
	expect_status("mid-chain outside main RAM", 0, RA_WATCH_BAD_POINTER);

	printf("\nan unreadable base is BAD_BASE even on a chain\n");
	ra_watch_clear();
	ra_watch_add(0x09000000, 4, 2, offsets);
	ra_wram_tick(&snapshot);
	expect_status("bad base, depth 2", 0, RA_WATCH_BAD_BASE);

	printf("\noffsets are added at each step\n");
	{
		static u32 pair[2];
		pair[0] = 0;
		pair[1] = 0x0BADF00D;
		targetPtr = (u32)&pair[0];
		ra_watch_clear();
		offsets[0] = 4;             /* one word into pair[] */
		offsets[1] = 0;
		ra_watch_add((u32)&targetPtr, 4, 1, offsets);
		ra_wram_tick(&snapshot);
		expect_status("offset applied", 0, RA_WATCH_OK);
		CHECK(snapshot.results[0].address == (u32)&pair[1]);
		CHECK(snapshot.results[0].value == 0x0BADF00D);
	}

	printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
	       failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
