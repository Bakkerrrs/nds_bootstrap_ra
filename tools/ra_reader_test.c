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

#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>

/*
    The linker script supplies these on the target. Here they span a scratch buffer, so the
    .bss zeroing and the arena arithmetic are exercised for real rather than stubbed. The
    allocator probe inside ra_startup() runs against the host's malloc -- our _sbrk() is
    never reached, so heapUsed stays 0 here -- but the staging logic around it is the same
    code the hardware runs.
*/
static char fakeBss[4096];
static char fakeArena[0x3B000];
char __bss_start[1], __bss_end[1], __vram_top[1];   /* referenced by cardengine.c */

/*
    RA_ALLOC_NO_LIBC_NAMES keeps our malloc from replacing glibc's underneath printf and the
    test harness itself -- see the note above ra_alloc_malloc(). The allocator is exercised
    through its ra_ names instead, which is the same code.
*/
#define RA_ALLOC_NO_LIBC_NAMES
#include "../retail/cardenginei/arm9_ra/source/ra_alloc.c"
#include "../retail/cardenginei/arm9_ra/source/startup.c"
#include "../retail/cardenginei/arm9_ra/source/cardengine.c"
/*
    Included rather than linked, for the same reason as the two above: the translation and
    the peek path are static, and they are exactly the parts worth testing. rcheevos itself
    is a normal library and is linked -- see tools/ra_reader_test.sh for the file list.
*/
#include "../retail/cardenginei/arm9_ra/source/ra_rcheevos.c"

#define MIRROR_LOW   0x02000000u          /* console 0x000000 */
#define MIRROR_HIGH  0x02800000u          /* the same page, two 4M mirrors up */
#define APART        0x02C00000u          /* masks to the same console address, own memory */

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

/*
    True when a DS address has no console address in the RetroAchievements DS map. Stated
    here rather than in the reader because nothing in the shipped code needs to ask any
    more -- the self-test no longer points at our own memory -- but the property is what
    the anchor choice rests on, so it is worth pinning.
*/
static int ra_rc_console_unmapped(u32 dsAddress) {
	return !(dsAddress >= RA_DS_SYSTEM_RAM_BASE
	      && dsAddress <  RA_DS_SYSTEM_RAM_BASE + RA_DS_SYSTEM_RAM_SIZE);
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

	/*
	    A real main RAM mirror, set up before anything ticks because ra_rc_console_address()
	    reads through it. MIRROR_LOW and MIRROR_HIGH are one page of memory mapped twice, 8M
	    apart -- the same relationship 0x027FC000 has to 0x023FC000 on the DS. APART is a
	    separate page that masks to the same console address and must therefore be refused.

	    Both are far above where this binary links, so MAP_FIXED cannot land on it.
	*/
	{
		int fd = memfd_create("ra_mirror", 0);

		if (fd < 0 || ftruncate(fd, 0x1000) != 0
		 || mmap((void*)MIRROR_LOW,  0x1000, PROT_READ | PROT_WRITE,
		         MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED
		 || mmap((void*)MIRROR_HIGH, 0x1000, PROT_READ | PROT_WRITE,
		         MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED
		 || mmap((void*)APART, 0x1000, PROT_READ | PROT_WRITE,
		         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED) {
			perror("mmap main RAM mirror");
			return 2;
		}
	}

	/*
	    The definitions block the bootloader would have written at the top of the WRAM
	    window. Mapped so ra_definition() has something real to read; left with no magic, so
	    every test below runs against the built-in self-test definition unless it says
	    otherwise.
	*/
	if (mmap((void*)CARDENGINEI_ARM9_RA_DEFS_LOCATION, 0x1000, PROT_READ | PROT_WRITE,
	         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED) {
		perror("mmap definitions block");
		return 2;
	}
	*(u32*)CARDENGINEI_ARM9_RA_DEFS_LOCATION = 0;

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
	CHECK(__builtin_offsetof(raSnapshot, heapSize) == 0x60);
	CHECK(__builtin_offsetof(raSnapshot, heapUsed) == 0x64);
	CHECK(__builtin_offsetof(raSnapshot, wramStage) == 0x68);
	CHECK(__builtin_offsetof(raSnapshot, rcStage) == 0x6C);
	CHECK(__builtin_offsetof(raSnapshot, rcTriggered) == 0x70);
	CHECK(__builtin_offsetof(raSnapshot, rcMeasured) == 0x74);
	CHECK(__builtin_offsetof(raSnapshot, rcTarget) == 0x78);
	CHECK(__builtin_offsetof(raSnapshot, rcPeeks) == 0x7C);
	CHECK(__builtin_offsetof(raSnapshot, rcPeeksRejected) == 0x80);
	CHECK(__builtin_offsetof(raSnapshot, rcLines) == 0x84);
	CHECK(__builtin_offsetof(raSnapshot, heapBreak) == 0x88);
	CHECK(__builtin_offsetof(raSnapshot, heapTop) == 0x8C);
	CHECK(__builtin_offsetof(raSnapshot, mallocProbe) == 0x90);
	CHECK(__builtin_offsetof(raSnapshot, sbrkProbe) == 0x94);
	CHECK(__builtin_offsetof(raSnapshot, rcFromFile) == 0x98);
	CHECK(__builtin_offsetof(raSnapshot, rcActivated) == 0x99);
	CHECK(__builtin_offsetof(raSnapshot, rcDefLength) == 0x9A);
	CHECK(__builtin_offsetof(raSnapshot, rcBadLine) == 0x9C);
	CHECK(sizeof(raSnapshot) == 0xA0);

	*DISPCNT = 0x1F40;

	/*
	    Run before the real startup, because ra_startup() only does its work once per boot
	    and this needs to be that once.
	*/
	printf("\nthe allocator hands out the arena, and reclaims it\n");
	{
		static char pool[4096];
		void* a;
		void* bb;
		void* c;

		ra_alloc_init(pool, pool + sizeof(pool));
		CHECK(ra_alloc_size() == sizeof(pool));
		CHECK(ra_alloc_used() == 0);
		CHECK(ra_alloc_failures() == 0);

		a  = ra_alloc_malloc(100);
		bb = ra_alloc_malloc(100);
		CHECK(a != 0 && bb != 0);
		/* 100 rounds up to 104, plus an 8-byte header each. */
		CHECK(ra_alloc_used() == 2 * (104 + 8));
		/* Payloads 8-aligned, which rcheevos' 64-bit typed values require. */
		CHECK(((u32)a & 7) == 0 && ((u32)bb & 7) == 0);
		/* And they do not overlap. */
		CHECK((char*)bb >= (char*)a + 104);

		memset(a, 0xAA, 100);
		memset(bb, 0x55, 100);
		CHECK(((unsigned char*)a)[99] == 0xAA);   /* neither wrote into the other */
		CHECK(((unsigned char*)bb)[0] == 0x55);

		ra_alloc_free(a);
		CHECK(ra_alloc_used() == 104 + 8);
		ra_alloc_free(bb);
		CHECK(ra_alloc_used() == 0);
		/* Coalesced back into one block, or the next big request would fail. */
		CHECK(ra_alloc_largest() == sizeof(pool) - 8);

		c = ra_alloc_malloc(sizeof(pool) - 8);
		CHECK(c != 0);
		CHECK(ra_alloc_largest() == 0);
		ra_alloc_free(c);
		CHECK(ra_alloc_largest() == sizeof(pool) - 8);
	}

	printf("\nthe allocator refuses rather than overruns\n");
	{
		static char pool[512];
		void* a;

		ra_alloc_init(pool, pool + sizeof(pool));
		CHECK(ra_alloc_malloc(sizeof(pool)) == 0);          /* no room for the header */
		CHECK(ra_alloc_malloc(0xFFFFFFF0u) == 0);           /* would wrap the alignment */
		CHECK(ra_alloc_failures() == 2);

		a = ra_alloc_malloc(0);                             /* legal, and not a failure */
		CHECK(a != 0);
		ra_alloc_free(a);

		ra_alloc_free(0);                                   /* all no-ops, not corruption */
		ra_alloc_free(pool - 64);
		ra_alloc_free(pool + sizeof(pool) + 64);
		a = ra_alloc_malloc(64);
		ra_alloc_free(a);
		ra_alloc_free(a);                                    /* double free */
		CHECK(ra_alloc_used() == 0);
		CHECK(ra_alloc_largest() == sizeof(pool) - 8);        /* arena still whole */
	}

	printf("\nrealloc grows, copies, and leaves the original alone on failure\n");
	{
		static char pool[4096];
		void* a;
		void* grown;

		ra_alloc_init(pool, pool + sizeof(pool));
		a = ra_alloc_malloc(32);
		memset(a, 0x5A, 32);

		grown = ra_alloc_realloc(a, 64);
		CHECK(grown != 0);
		CHECK(((unsigned char*)grown)[0] == 0x5A && ((unsigned char*)grown)[31] == 0x5A);

		/* Already big enough: kept in place rather than moved. */
		CHECK(ra_alloc_realloc(grown, 40) == grown);

		/* Cannot be satisfied, so the original must survive. */
		CHECK(ra_alloc_realloc(grown, sizeof(pool) * 2) == 0);
		CHECK(((unsigned char*)grown)[0] == 0x5A);

		CHECK(ra_alloc_realloc(0, 16) != 0);        /* realloc(0, n) is malloc */
		CHECK(ra_alloc_realloc(grown, 0) == 0);     /* realloc(p, 0) is free */
	}

	printf("\ncalloc zeroes, and refuses an overflowing product\n");
	{
		static char pool[4096];
		unsigned char* a;

		ra_alloc_init(pool, pool + sizeof(pool));
		a = (unsigned char*)ra_alloc_malloc(64);
		memset(a, 0xFF, 64);
		ra_alloc_free(a);

		a = (unsigned char*)ra_alloc_calloc(8, 8);   /* same block back, previously 0xFF */
		CHECK(a != 0);
		CHECK(a[0] == 0 && a[63] == 0);
		CHECK(ra_alloc_calloc(0x10000000u, 0x40u) == 0);   /* product overflows 32 bits */
	}

	printf("\nan exhausted arena is a refusal, not a wild write\n");
	{
		static char pool[1024];
		int taken = 0;
		void* p;

		ra_alloc_init(pool, pool + sizeof(pool));
		while ((p = ra_alloc_malloc(64)) != 0) {
			memset(p, 0xC3, 64);   /* would corrupt the arena if it handed out too much */
			taken++;
			if (taken > 100) {
				break;             /* it should have refused long before here */
			}
		}
		CHECK(taken > 0 && taken <= sizeof(pool) / (64 + 8));
		CHECK(ra_alloc_failures() > 0);
		CHECK(ra_alloc_used() <= sizeof(pool));
	}

	printf("\na failed allocator stays failed, on every call\n");
	{
		/*
		    A window with no room past .bss, which is the one way the probe can fail without
		    mocking anything: ra_alloc_init() cannot take an arena, so the probe's
		    allocation is refused and ra_startup() must say RA_STAGE_HEAP -- and keep saying
		    it.

		    Run before the real startup, because ra_startup() only does its work once per
		    boot and this needs to be that once.
		*/
		char* const bssEnd = fakeBss + sizeof(fakeBss);

		CHECK(ra_startup(fakeBss, bssEnd, bssEnd) == RA_STAGE_HEAP);
		/*
		    The regression. This second call used to return RA_STAGE_ALLOC and claim a
		    working heap, because the "already ran" flag is set before the probe.
		*/
		CHECK(ra_startup(fakeBss, bssEnd, bssEnd) == RA_STAGE_HEAP);
		CHECK(ra_malloc_probe() == 0);
		CHECK(ra_sbrk_probe() == 0);   /* no arena was taken, which is why it failed */
		CHECK(ra_heap_size() == 0);

		/* Back to a fresh boot for the tests below. */
		startupState = RA_STARTUP_FRESH;
		startupStage = RA_STAGE_NONE;
	}

	printf("\nstartup zeroes .bss once and measures the arena\n");
	memset(fakeBss, 0xA5, sizeof(fakeBss));
	CHECK(ra_startup(fakeBss, fakeBss + sizeof(fakeBss),
	                 fakeBss + sizeof(fakeBss) + 0x3B000) == RA_STAGE_ALLOC);
	CHECK(fakeBss[0] == 0 && fakeBss[sizeof(fakeBss) - 1] == 0);
	CHECK(ra_heap_size() == 0x3B000);
	/* The probe allocated, wrote, read back and freed, so the arena is whole again. */
	CHECK(ra_heap_used() == 0);
	CHECK(ra_malloc_probe() != 0);
	/*
	    Second call must be a no-op. The flag that says so lives in .data, so the zeroing
	    cannot clear it -- which is the point of putting it there.
	*/
	fakeBss[0] = 0x42;
	CHECK(ra_startup(fakeBss, fakeBss + sizeof(fakeBss),
	                 fakeBss + sizeof(fakeBss) + 0x3B000) == RA_STAGE_ALLOC);
	CHECK(fakeBss[0] == 0x42);

	printf("\n_sbrk refuses everything, so the arena has one owner\n");
	{
		/*
		    ra_alloc.c owns the arena in one piece now. If _sbrk() still handed it out,
		    newlib's allocator and ours would be writing over each other -- which is the
		    kind of corruption that only appears under load.
		*/
		CHECK(_sbrk(16) == (void*)-1);
		CHECK(_sbrk(0) == (void*)-1);
		CHECK(_sbrk(-16) == (void*)-1);
		CHECK(ra_heap_used() == 0);   /* and nothing was handed out behind our back */
	}

	printf("\nthe first tick claims .bss and installs the defaults\n");
	/* Neither side's .bss is ever zeroed on the target, so start from garbage. */
	memset(&snapshot, 0xA5, sizeof(snapshot));
	stateMagic = 0;   /* startup zeroed .bss on the target; do the same here */
	snapshot.ticks = 7;
	ra_wram_tick(&snapshot);
	CHECK(snapshot.watchCount == 2);
	CHECK(snapshot.resolved == 2);
	CHECK(snapshot.wramMagic == RA_WRAM_MAGIC);
	CHECK(snapshot.wramTicks == 1);
	CHECK(snapshot.wramStage == RA_STAGE_WATCHES);
	CHECK(snapshot.heapSize == 0x3B000);

	/*
	    rcheevos, on the same tick. This is the first thing in the project that turns a
	    server-side definition string into an evaluated condition, and all of it is logic
	    a host can check: whether the definition parses, whether the console-to-DS
	    address translation lands on the right word, and whether the peek path refuses
	    anything it should not.
	*/
	printf("\nrcheevos parses the definition and evaluates it\n");
	CHECK(snapshot.rcActivate == 0);                  /* RC_OK */
	CHECK(snapshot.rcStage == RA_RC_FRAME);
	CHECK(snapshot.rcTriggerState != RC_TRIGGER_STATE_DISABLED);
	CHECK(snapshot.rcTarget == 600);
	/*
	    The definition reads one address, so a peek count of zero would mean do_frame
	    never reached memory -- which is the failure that would otherwise look like an
	    achievement that simply never unlocks.
	*/
	CHECK(snapshot.rcPeeks > 0);
	/*
	    Zero refusals is the whole point of routing peek() through ra_readable(): the
	    definition points at the snapshot, which is in main RAM, so nothing here should be
	    turned away. A non-zero count means the translation is wrong, not that the check is
	    too strict.
	*/
	CHECK(snapshot.rcPeeksRejected == 0);
	/*
	    Not disabled, which is what rc_runtime_validate_addresses() would have done had any
	    address in the definition been one this console cannot supply. So this asserts the
	    validation ran and passed, rather than that it was never wired up.
	*/
	CHECK(snapshot.rcTriggerState != RC_TRIGGER_STATE_DISABLED);
	CHECK(snapshot.rcTriggerState != RC_TRIGGER_STATE_INACTIVE);

	/*
	    The bug that disabled the achievement on the first hardware run. The cardengine
	    lives at 0x027FC000, inside main RAM's *second* 4M mirror, so subtracting the base
	    gives 0x7Fxxxx -- past the 4M RetroAchievements maps, and correctly refused. The
	    mask fixes it, but only where the region really is a mirror, so it is proved rather
	    than assumed.
	*/
	/*
	    The self-test definition is anchored at console address 0 -- the first word of the
	    game's own RAM -- because the snapshot has no console address on real hardware. Main
	    RAM there is 16M and RetroAchievements maps 4M, so the cardengine at 0x027FC000 is
	    eight megabytes past the end of the map, and not a mirror of 0x023FC000 either: that
	    was tested directly on hardware with a sentinel and they are separate memory.
	*/
	/*
	    The staged definition, which is how a real achievement gets tried without a rebuild.
	    Exercised here because the alternative -- finding out on hardware -- costs a flash
	    cycle per mistake, which is exactly what the file was introduced to stop.
	*/
	printf("\na staged definition is used, and a missing one falls back\n");
	{
		char* const  block = (char*)CARDENGINEI_ARM9_RA_DEFS_LOCATION;
		char* const  text  = block + CARDENGINEI_ARM9_RA_DEFS_HEADER;
		raSnapshot   probe;

		memset(&probe, 0, sizeof(probe));

		/* No magic: the built-in definition, and the snapshot says so. */
		*(u32*)block = 0;
		CHECK(strcmp(ra_definition(&probe), RA_TEST_DEFINITION) == 0);
		CHECK(probe.rcFromFile == 0);

		/* A staged definition is used verbatim. */
		strcpy(text, "0xH00c0fe=7");
		*(u32*)(block + 4) = strlen(text);
		*(u32*)block = CARDENGINEI_ARM9_RA_DEFS_MAGIC;
		CHECK(strcmp(ra_definition(&probe), "0xH00c0fe=7") == 0);
		CHECK(probe.rcFromFile == 1);
		CHECK(probe.rcDefLength == strlen("0xH00c0fe=7"));

		/*
		    A file typed in a text editor ends with a newline, and rcheevos would reject the
		    whitespace as syntax. Trimmed rather than blamed on the user.
		*/
		strcpy(text, "0xH00c0fe=7\r\n");
		*(u32*)(block + 4) = strlen(text);
		CHECK(strcmp(ra_definition(&probe), "0xH00c0fe=7") == 0);
		CHECK(probe.rcDefLength == strlen("0xH00c0fe=7"));

		/* Empty and oversized both fall back rather than being handed to the parser. */
		*(u32*)(block + 4) = 0;
		CHECK(strcmp(ra_definition(&probe), RA_TEST_DEFINITION) == 0);
		CHECK(probe.rcFromFile == 0);
		*(u32*)(block + 4) = CARDENGINEI_ARM9_RA_DEFS_MAX;
		CHECK(strcmp(ra_definition(&probe), RA_TEST_DEFINITION) == 0);

		/*
		    Several definitions in one file, because a hardware session is the scarce
		    resource and testing one per session wastes it. Comments and blank lines are
		    skipped so the file can say what each line is meant to do.
		*/
		{
			char*      lines[RA_DEFS_MAX_LINES];
			const char file[] =
				"# what this is for\n"
				"0xH0012a4=1\n"
				"\n"
				"  I:0xW159164_0xX00009c=2  \r\n"
				"0x159992>d0x159992\n";
			u8 count;

			memcpy(text, file, sizeof(file));
			count = ra_split_definitions(text, sizeof(file) - 1, lines);
			CHECK(count == 3);
			CHECK(strcmp(lines[0], "0xH0012a4=1") == 0);
			/* Leading and trailing whitespace and the CR all gone. */
			CHECK(strcmp(lines[1], "I:0xW159164_0xX00009c=2") == 0);
			CHECK(strcmp(lines[2], "0x159992>d0x159992") == 0);
		}

		/* A file with more lines than there are slots stops rather than overruns. */
		{
			char*  lines[RA_DEFS_MAX_LINES];
			char   many[256];
			int    n;
			u32    len = 0;

			for (n = 0; n < RA_DEFS_MAX_LINES + 4; n++) {
				len += sprintf(many + len, "0xH00%04x=1\n", n);
			}
			memcpy(text, many, len + 1);
			CHECK(ra_split_definitions(text, len, lines) == RA_DEFS_MAX_LINES);
		}

		/* Back to no file, so the tests after this see the built-in. */
		*(u32*)block = 0;
	}

	printf("\nthe self-test definition is anchored where the map reaches\n");
	CHECK(ra_rc_translate(0, 1) == RA_DS_SYSTEM_RAM_BASE);
	CHECK(ra_rc_validate_address(0) != 0);
	/*
	    And the reason it is anchored there: an address eight megabytes into main RAM --
	    where the cardengine actually lives on hardware -- has no console address at all.
	    The snapshot in this test links low enough to be inside the map, which the target's
	    never is.
	*/
	CHECK(ra_rc_console_unmapped(0x027FED54));
	CHECK(ra_rc_validate_address(0x027FED54 - RA_DS_SYSTEM_RAM_BASE) == 0);

	printf("\nthe definition's address translates to the snapshot's own ticks\n");
	CHECK(ra_rc_translate(0, 4) == RA_DS_SYSTEM_RAM_BASE);
	CHECK(ra_rc_translate((u32)&snapshot.ticks - RA_DS_SYSTEM_RAM_BASE, 4)
	      == (u32)&snapshot.ticks);
	/*
	    Console addresses this console does not have. The DS's 4M of system RAM is the
	    only region translated: the RetroAchievements map also lists data TCM at console
	    0x1000000, which has no fixed address on hardware and so is deliberately refused
	    rather than guessed at.
	*/
	CHECK(ra_rc_translate(RA_DS_SYSTEM_RAM_SIZE, 1) == 0);
	CHECK(ra_rc_translate(RA_DS_SYSTEM_RAM_SIZE - 1, 4) == 0);   /* would run off the end */
	CHECK(ra_rc_translate(RA_DS_SYSTEM_RAM_SIZE - 4, 4) != 0);   /* last aligned word fits */
	CHECK(ra_rc_translate(0x1000000, 1) == 0);                   /* data TCM */
	CHECK(ra_rc_translate(0xFFFFFFFC, 4) == 0);                  /* cannot wrap the check */

	/*
	    Unaligned multi-byte reads, assembled from bytes because the ARM9 would rotate the
	    word instead of faulting. Achievement authors write these and the server serves
	    them, so getting it wrong means plausible-looking wrong values rather than a crash.
	*/
	printf("\nunaligned reads are assembled little-endian, not rotated\n");
	{
		/*
		    Two adjacent words, so a read straddling the boundary has a known answer in
		    every byte. In memory that is 44 33 22 11 88 77 66 55, and a 32-bit read one
		    byte in must therefore be 0x88112233. An ARM9 LDR at that address would return
		    0x44112233 -- the aligned word, rotated -- which is why this is assembled from
		    bytes rather than left to the hardware.
		*/
		static u32 straddle[2];
		const u32 base = (u32)&straddle[0] - RA_DS_SYSTEM_RAM_BASE;
		straddle[0] = 0x11223344;
		straddle[1] = 0x55667788;

		CHECK(ra_rc_peek(base, 4, 0) == 0x11223344);
		CHECK(ra_rc_peek(base + 1, 2, 0) == 0x2233);
		CHECK(ra_rc_peek(base + 1, 4, 0) == 0x88112233);
		CHECK(ra_rc_peek(base + 3, 1, 0) == 0x11);
		CHECK(ra_rc_peek(base + 4, 4, 0) == 0x55667788);
	}

	printf("\na refused peek reads as zero and is counted, not dereferenced\n");
	{
		const u32 before = peeksRejected;
		CHECK(ra_rc_peek(0x1000000, 4, 0) == 0);   /* data TCM: unmapped here */
		CHECK(peeksRejected == before + 1);
	}

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

	/*
	    Last, because it advances snapshot.ticks and every assertion above about the
	    counters depends on how many times ra_wram_tick() has run.
	*/
	/*
	    Last, because it advances the frame count and every assertion above about the
	    counters depends on how many times ra_wram_tick() has run.
	*/
	printf("\nmeasured progress climbs one per frame, and triggers at the target\n");
	{
		const u32 startMeasured = snapshot.rcMeasured;
		for (i = 0; i < 5; i++) {
			ra_wram_tick(&snapshot);
		}
		/*
		    One hit per frame regardless of what memory does, which is what the always-true
		    comparison buys: the count is a property of rcheevos running, not of the game.
		*/
		CHECK(snapshot.rcMeasured == startMeasured + 5);
		CHECK(snapshot.rcTarget == 600);
		CHECK(snapshot.rcTriggered == 0);          /* still short of 600 */
		CHECK(snapshot.rcPeeks > 0);               /* and it really is reading memory */

		/* Run it out to the target and check the trigger actually fires. */
		while (snapshot.rcMeasured < snapshot.rcTarget && snapshot.rcTriggered == 0) {
			ra_wram_tick(&snapshot);
		}
		CHECK(snapshot.rcTriggered == 1);
		CHECK(snapshot.rcEvents > 0);
		/*
		    rcheevos stops reporting progress once a trigger fires, so these are latched.
		    Without that a snapshot read after the unlock shows two zeros and no evidence
		    of how it got there -- which is exactly what the first hardware reading showed.
		*/
		CHECK(snapshot.rcTarget == 600);
		/* The last value reported while active, which is the frame before it fired. */
		CHECK(snapshot.rcMeasured == snapshot.rcTarget - 1);
		/* Only the refusal forced by hand above; nothing in normal operation was denied. */
		CHECK(snapshot.rcPeeksRejected == 1);
	}

	printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
	       failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
