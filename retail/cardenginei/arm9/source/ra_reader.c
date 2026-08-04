/*
    RetroAchievements support for nds-bootstrap -- game RAM reader (ARM9).

    Copies a window of the game's RAM into a snapshot buffer once per frame,
    reports the console's real memory map, and probes the memory above the ROM
    cache to see whether it can host the RA state that will never fit in the
    cardengine's 12K window. No RetroAchievements logic and no networking yet.

    This file is part of nds-bootstrap and is licensed under the GPL-3.0,
    the same terms as the rest of the project.
*/

#include <nds/arm9/cache.h>
#include "ndma.h"
#include "ra_reader.h"
#include "cardengine_header_arm9.h"

#if RA_READER_ENABLED

extern cardengineArm9* volatile ce9;

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

static u32 watchAddress = RA_DEFAULT_WATCH_ADDRESS;
static u32 watchLength  = RA_SNAPSHOT_WINDOW;

#define RA_PROBE_WORDS (RA_PROBE_BYTES / 4)

static u32  probeSrc[RA_PROBE_WORDS];
static u32  probeDst[RA_PROBE_WORDS];
static u32  probeCtl[RA_PROBE_WORDS];
static bool probeDone;

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
}

/*
    Decide whether the memory just past the ROM cache is real.

    A CPU store there took a Data Abort, which could mean the RAM is absent or
    merely unreachable through the MPU -- opposite conclusions. DMA tells them
    apart: it bypasses the MPU and cannot fault, so a transfer to memory that is
    not there just goes nowhere and the read-back comes back wrong.

    The control round-trip through known-good .bss runs first. Without it a
    failure at the target would be ambiguous all over again, since it could just
    mean the transfer itself never worked.

    Runs once. The NDMA hardware is shared with card reads, so there is no reason
    to keep poking it every frame.
*/
static void probeReservedRegion(void) {
	u32 base;
	u32 i;
	bool ok;

	if (!RA_PROBE_ENABLED || probeDone) {
		return;
	}
	if (!ce9 || ce9->consoleModel == 0) {
		return;  /* only the 3DS is claimed to have memory above 0x0D000000 */
	}
	if (ce9->cacheSlots == 0) {
		return;  /* no cache means cacheEnd is meaningless; could be ROM-in-RAM */
	}

	base = (snapshot.cacheEnd + 0xFFF) & ~0xFFF;
	if (base < RA_PROBE_FLOOR || base + RA_PROBE_BYTES > RA_PROBE_CEILING) {
		return;
	}

	probeDone = true;
	snapshot.probeBase = base;

	for (i = 0; i < RA_PROBE_WORDS; i++) {
		probeSrc[i] = RA_PROBE_MAGIC + i;
		probeDst[i] = 0;
		probeCtl[i] = 0;
	}
	/*
	    Clean the pattern out to RAM for the DMA to find, and clean the landing
	    buffers too -- a dirty line written back later would overwrite what the
	    DMA delivers.
	*/
	DC_FlushRange(probeSrc, RA_PROBE_BYTES);
	DC_FlushRange(probeDst, RA_PROBE_BYTES);
	DC_FlushRange(probeCtl, RA_PROBE_BYTES);

	ndmaCopyWords(RA_PROBE_NDMA_CHANNEL, probeSrc, probeCtl, RA_PROBE_BYTES);
	ndmaCopyWords(RA_PROBE_NDMA_CHANNEL, probeSrc, (void*)base, RA_PROBE_BYTES);
	ndmaCopyWords(RA_PROBE_NDMA_CHANNEL, (void*)base, probeDst, RA_PROBE_BYTES);

	/* Now clean, so these only invalidate and the CPU sees what the DMA wrote. */
	DC_FlushRange(probeCtl, RA_PROBE_BYTES);
	DC_FlushRange(probeDst, RA_PROBE_BYTES);

	ok = true;
	for (i = 0; i < RA_PROBE_WORDS; i++) {
		if (probeCtl[i] != RA_PROBE_MAGIC + i) {
			ok = false;
		}
	}
	snapshot.probeControlOk = ok ? 1 : 0;

	snapshot.probeReadBack = probeDst[0];
	ok = true;
	for (i = 0; i < RA_PROBE_WORDS; i++) {
		if (probeDst[i] != RA_PROBE_MAGIC + i) {
			ok = false;
		}
	}
	snapshot.probeDmaOk = ok ? 1 : 0;
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
	snapshot.probeBase      = 0;
	snapshot.probeDmaOk     = 0;
	snapshot.probeControlOk = 0;
	snapshot.probeReadBack  = 0;
	snapshot.srcAddress = 0;
	snapshot.length     = 0;

	probeDone = false;
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
	probeReservedRegion();
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
