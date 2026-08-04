/*
    RetroAchievements support for nds-bootstrap -- game RAM reader (ARM9).

    Copies a window of the game's RAM into a snapshot buffer once per frame, and
    reports the memory map and MPU layout so a home can be found for the RA state
    that will never fit in the cardengine's 12K window. No RetroAchievements logic
    and no networking yet.

    This file is part of nds-bootstrap and is licensed under the GPL-3.0,
    the same terms as the rest of the project.
*/

#include <nds/arm9/cache.h>
#include "ndma.h"
#include "ra_reader.h"
#include "cardengine_header_arm9.h"

#if RA_READER_ENABLED

extern cardengineArm9* volatile ce9;

/* In ra_mpu.s -- MRC has no Thumb encoding. */
extern void ra_mpu_read_regions(u32* out);

/*
    Lives in the cardengine's .bss, which is inside the region reserved for the
    cardengine, so the game can never scribble on it. The bootloader copies only
    the loaded image, which ends where .bss begins, and there is no crt0 to zero
    what follows -- so every field is garbage until claim() says otherwise.

    Aligned to 16 because the in-game menu's RAM viewer can only jump to
    addresses that are a multiple of 0x10.
*/
/* Global so the link map names it; tools/ra_snapshot_addr.sh reads it from there. */
raSnapshot raSnapshotBuffer __attribute__((aligned(16)));
#define snapshot raSnapshotBuffer

static u32  dmaBuf[RA_PROBE_WORDS];
static bool mirrorTestDone;

static u32 watchAddress = RA_DEFAULT_WATCH_ADDRESS;
static u32 watchLength  = RA_SNAPSHOT_WINDOW;

/*
    Record what the cardengine can see of the memory map. Read every time rather
    than once, because the bootloader fills these in over the course of setup and
    an early caller would otherwise cache zeroes.
*/
static void sampleMemoryMap(void) {
	u32 ramTop;

	if (!ce9) {
		return;
	}
	snapshot.consoleModel   = ce9->consoleModel;
	snapshot.valueBits      = ce9->valueBits;
	snapshot.cacheAddress   = ce9->cacheAddress;
	snapshot.cacheEnd       = ce9->cacheAddress + ((u32)ce9->cacheSlots * (u32)ce9->cacheBlockSize);
	snapshot.romLocation    = ce9->romLocation;

	ramTop = (ce9->consoleModel > 0) ? RA_RAM_TOP_3DS : RA_RAM_TOP_DSI;
	snapshot.freeBytes = (snapshot.cacheEnd < ramTop) ? (ramTop - snapshot.cacheEnd) : 0;

	ra_mpu_read_regions(snapshot.mpuRegion);
}

/* One DMA word-read into dmaBuf, with the cache maintenance it needs. */
static u32 dmaPeek(u32 addr) {
	DC_FlushRange(dmaBuf, sizeof(dmaBuf));  /* clean, so nothing overwrites the result */
	ndmaCopyWords(RA_PROBE_NDMA_CHANNEL, (void*)addr, dmaBuf, sizeof(dmaBuf));
	DC_FlushRange(dmaBuf, sizeof(dmaBuf));  /* already clean, so this invalidates */
	return dmaBuf[0];
}

/*
    Is the memory above the ROM cache real, or just an alias of memory we already
    have?

    A DMA round-trip through it succeeded, which looked like proof the RAM is
    there. It is not: if main RAM ends at 16MB then that address mirrors one 16MB
    lower, and the round-trip only showed that writing and reading the same
    existing byte works. The MPU dump rules out the other explanation for the
    earlier Data Abort -- region 3 already spans 0x08000000 +128MB, covering both
    the cache, where stores work, and the faulting address -- so mirroring is what
    is left to check.

    Write the pattern at the target and watch the candidate alias. If it changes to
    match, the two are the same memory.

    Runs once, on the NDMA channel the card reads do not use.
*/
static void mirrorTest(void) {
	u32 target;
	u32 i;

	if (mirrorTestDone || !ce9 || ce9->consoleModel == 0 || ce9->cacheSlots == 0) {
		return;
	}
	target = (snapshot.cacheEnd + 0xFFF) & ~0xFFF;
	if (target < RA_MIRROR_SPAN + 0x02000000) {
		return;
	}
	mirrorTestDone = true;

	snapshot.aliasAddr   = target - RA_MIRROR_SPAN;
	snapshot.aliasBefore = dmaPeek(snapshot.aliasAddr);

	for (i = 0; i < RA_PROBE_WORDS; i++) {
		dmaBuf[i] = RA_PROBE_MAGIC + i;
	}
	DC_FlushRange(dmaBuf, sizeof(dmaBuf));
	ndmaCopyWords(RA_PROBE_NDMA_CHANNEL, dmaBuf, (void*)target, sizeof(dmaBuf));

	snapshot.targetReadBack = dmaPeek(target);
	snapshot.aliasAfter     = dmaPeek(snapshot.aliasAddr);
}

/*
    Initialise the buffer the first time anything touches it. Every entry point
    calls this, so the snapshot becomes readable as soon as any part of the
    cardengine runs -- not only once the per-frame hook is working.
*/
static void claim(void) {
	if (snapshot.magic[0] == RA_SNAPSHOT_MAGIC0
	 && snapshot.magic[1] == RA_SNAPSHOT_MAGIC1
	 && snapshot.magic[2] == RA_SNAPSHOT_MAGIC2
	 && snapshot.magic[3] == RA_SNAPSHOT_MAGIC3) {
		return;
	}

	snapshot.magic[0] = RA_SNAPSHOT_MAGIC0;
	snapshot.magic[1] = RA_SNAPSHOT_MAGIC1;
	snapshot.magic[2] = RA_SNAPSHOT_MAGIC2;
	snapshot.magic[3] = RA_SNAPSHOT_MAGIC3;

	snapshot.ticks      = 0;
	snapshot.cardReads  = 0;
	snapshot.irqEnables = 0;
	snapshot.srcAddress = 0;
	snapshot.length     = 0;

}

void ra_reader_set_window(u32 address, u32 length) {
	if (length > RA_SNAPSHOT_WINDOW) {
		length = RA_SNAPSHOT_WINDOW;
	}
	watchAddress = address & ~3;  /* keep the word copy below aligned */
	watchLength  = length & ~3;
}

const raSnapshot* ra_reader_snapshot(void) {
	return &snapshot;
}

void ra_reader_note_card_read(void) {
	claim();
	snapshot.cardReads++;
}

void ra_reader_note_irq_enable(void) {
	claim();
	snapshot.irqEnables++;
}

void ra_reader_note_hook(u32 irqTable, u32 vcountRef, u32 origVcount) {
	/* The hook chain is confirmed working on hardware; nothing left to record. */
	(void)irqTable;
	(void)vcountRef;
	(void)origVcount;
	claim();
}

void ra_reader_tick(void) {
	u32 length = watchLength;
	u32 i;

	claim();
	snapshot.ticks++;
	sampleMemoryMap();
	mirrorTest();
	snapshot.srcAddress = watchAddress;
	snapshot.length     = length;

	/*
	    Word-at-a-time so the copy stays cheap in the interrupt handler. The
	    source is volatile because it is the running game's memory and changes
	    underneath us; the window is deliberately not latched atomically, which
	    is fine for a per-frame sample.
	*/
	{
		const vu32* src = (const vu32*)watchAddress;
		u32* dst = (u32*)snapshot.data;
		for (i = 0; i < (length >> 2); i++) {
			dst[i] = src[i];
		}
	}
}

#endif /* RA_READER_ENABLED */
