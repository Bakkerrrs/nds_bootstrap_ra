/*
    Bringing cardenginei_arm9_ra up far enough to run a C library.

    This binary is copied into DSi WRAM by the bootloader and jumped into. There is no
    crt0, because there is nothing here to run one: the loaded image arrives, and that is
    the whole of the startup this window gets. `.text`, `.rodata` and `.data` are inside
    the image and therefore correct. `.bss` is not in the image, so it arrives as whatever
    the previous occupant of this memory left behind -- and the bootloader's copy actively
    writes staging garbage over it, since it copies a fixed length rather than the exact
    image size.

    Everything written for this window so far coped with that by guarding on a magic and
    initialising by hand. `rcheevos` will not: it calls `malloc`, and newlib's allocator
    keeps state in `.bss` and assumes, like every other C library, that it starts zeroed.
    Handing it garbage would not fail cleanly, it would corrupt a heap.

    So this file is the crt0 the window does not have. It zeroes `.bss` exactly once per
    boot, then hands newlib a heap over what is left of the 256K window. The staging is
    reported through the snapshot as it goes, so a failure names its own stage rather than
    showing up as a game that stopped working.

    This file is part of nds-bootstrap and is licensed under the GPL-3.0,
    the same terms as the rest of the project.
*/

#include <stdlib.h>

#include "ra.h"

/*
    "Have we run yet" deliberately lives in .data rather than .bss, and the non-zero
    initialiser is what puts it there.

    .data is inside the loaded image, so the bootloader's copy sets it correctly on every
    boot -- which makes it a *reliable* flag rather than one that has to be distinguished
    from garbage. A .bss flag would also work, but only if written after the zeroing below
    clears it, and that is a subtle ordering dependency in code that runs once per boot
    inside an interrupt handler. Nobody would notice it breaking.

    It also means no magic has to be guessed at here: unlike every other claim in this
    project, this one is not a bet that garbage will not coincide.
*/
#define RA_STARTUP_FRESH 0x48534552  /* 'RESH' byte-wise -- the image's initial value */
#define RA_STARTUP_DONE  0x31424152  /* 'RAB1' */

static u32 startupState = RA_STARTUP_FRESH;

/*
    In .bss, and therefore garbage until the zeroing below has run. Set afterwards, never
    before -- the zeroing would clear them.
*/
static char* heapBreak;
static char* heapTop;
static char* heapBase;

/*
    newlib's malloc grows the heap through this. The arena is everything between the end of
    .bss and the top of the window -- about 240K of the 256K, since the binary and newlib's
    initialised data take a few.

    A null break means ra_startup() has not run, which is a refusal rather than something
    to paper over: allocating before .bss was zeroed is exactly the failure this file
    exists to prevent.
*/
void* _sbrk(int incr) {
	char* previous;

	if (heapBreak == 0) {
		return (void*)-1;
	}
	/* Signed, because newlib is allowed to hand space back with a negative increment. */
	if (incr > 0 && heapBreak + incr > heapTop) {
		return (void*)-1;
	}
	previous   = heapBreak;
	heapBreak += incr;
	return previous;
}

/*
    Bring the window up, once. Returns the stage reached, so the caller can report it even
    when it did not get all the way.
*/
u8 ra_startup(char* bssStart, char* bssEnd, char* windowTop) {
	if (startupState == RA_STARTUP_DONE) {
		return RA_STAGE_ALLOC;
	}
	startupState = RA_STARTUP_DONE;

	{
		u32* p   = (u32*)bssStart;
		u32* end = (u32*)bssEnd;
		while (p < end) {
			*p++ = 0;
		}
	}

	/* After the zeroing, never before: these three live in what was just cleared. */
	heapBase  = bssEnd;
	heapBreak = bssEnd;
	heapTop   = windowTop;

	/*
	    Prove the allocator rather than assume it. This is the first time anything in this
	    project has called into a C library from this window, and the failure mode if
	    newlib's state were still garbage is a silently corrupted heap -- which would
	    surface much later as `rcheevos` misbehaving for no visible reason.

	    Allocated, written across its whole length, read back at both ends, and freed. Not
	    a proof of much, but it is the difference between "malloc returned non-null" and
	    "the memory it returned is real".
	*/
	{
		volatile unsigned char* block = (volatile unsigned char*)malloc(1024);
		u32 i;
		bool ok = (block != 0);

		if (ok) {
			for (i = 0; i < 1024; i++) {
				block[i] = (unsigned char)(i & 0xFF);
			}
			ok = (block[0] == 0 && block[1023] == (unsigned char)(1023 & 0xFF));
			free((void*)block);
		}
		if (!ok) {
			return RA_STAGE_HEAP;
		}
	}

	return RA_STAGE_ALLOC;
}

/* Reported through the snapshot so the arena is visible rather than inferred. */
u32 ra_heap_size(void) {
	if (heapBase == 0) {
		return 0;
	}
	return (u32)(heapTop - heapBase);
}

u32 ra_heap_used(void) {
	if (heapBase == 0) {
		return 0;
	}
	return (u32)(heapBreak - heapBase);
}
