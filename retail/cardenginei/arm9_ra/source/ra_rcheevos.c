/*
    rcheevos, running inside a DS game.

    This is the file where the project stops being a memory reader and starts being a
    RetroAchievements client: rcheevos is the same library the official emulator
    integrations use, and its runtime is what turns a definition string from the server
    into "this achievement just unlocked". Everything before it -- the WRAM window, the
    hand-written crt0, the heap -- existed to make this file possible.

    Three things about the DS shape the code here.

    The first is that achievement definitions come from the network. An address in a
    definition is a number someone else wrote, and on this platform dereferencing an
    address the console does not have is a Data Abort inside the game's interrupt handler.
    So peek() routes every read through the same ra_readable() the watchlist uses, and
    ra_rc_validate_address() is handed to rcheevos up front so an achievement that asks
    for memory this console cannot supply is disabled rather than evaluated against zeros.

    The second is that RetroAchievements addresses are console addresses, not DS
    addresses: the server's map puts DS system RAM at 0 and the emulator's frontend is
    expected to translate. We translate with our own three-line map rather than calling
    rcheevos' rc_console_memory_regions(), because that function's switch references the
    table for every console it supports -- forty-odd tables of regions and names -- and
    calling it would drag all of them into a 64K image to answer a question about one
    console.

    The third is that the ARM9 does not fault on an unaligned 32-bit load, it silently
    returns rotated data. Achievement authors write unaligned reads routinely and the
    server will happily serve them, so peek() assembles those from bytes. Refusing them
    would break real definitions and trusting the hardware would return plausible
    nonsense, which is worse.

    This file is part of nds-bootstrap and is licensed under the GPL-3.0,
    the same terms as the rest of the project.
*/

#include <stdlib.h>

#include "ra.h"
#include "locations.h"

#include "rc_runtime.h"
#include "rc_runtime_types.h"

/*
    A private header of the library, included for one reason: sizeof(rc_memrefs_t), which
    is the allocation rc_runtime_init() makes without checking the result. Probing for the
    real size rather than a guessed one means the check cannot drift out of date when
    upstream changes the structure -- and this is an in-tree build of rcheevos, so its
    private headers are as available as its public ones.
*/
#include "rcheevos/rc_internal.h"

/*
    retail/cardenginei/arm9_ra/source/cardengine.c -- the watchlist's own validation and
    read, reused here on purpose. rcheevos inheriting exactly the checks the walker uses
    is the point: there is one answer in this binary to "may this address be read", and
    a definition from the server does not get a weaker one than a hand-written watch.
*/
extern bool ra_readable(u32 addr, u32 len);
extern u32  ra_read(u32 addr, u8 size);

/* VCOUNT, read directly -- the game owns every hardware timer. See raSnapshot.linesLast. */
#define RA_VCOUNT            (*(const vu16*)0x04000006)
#define RA_SCANLINES_PER_FRAME 263

/*
    The RetroAchievements memory map for the Nintendo DS, which is the part of
    rcheevos' consoleinfo.c we actually need:

      console 0x0000000-0x03FFFFF   ->  0x02000000   system RAM (4M)
      console 0x0400000-0x0FFFFFF   ->  unused, padding to keep the DSi map aligned
      console 0x1000000-0x1003FFF   ->  data TCM

    Data TCM is deliberately not translated. Its base is not fixed -- it is whatever the
    game programmed into CP15 c9,c1 -- so there is no constant to map it to, and a guess
    would read real memory belonging to something else and produce values that look
    plausible. An achievement that reads DTCM is reported as unsupported instead, which
    shows up as rcPeeksRejected and as the achievement being disabled rather than as a
    wrong unlock.
*/
#define RA_DS_SYSTEM_RAM_BASE 0x02000000
#define RA_DS_SYSTEM_RAM_SIZE 0x00400000

/*
    Console address to DS address, or 0 for "this console does not have that". Zero is
    usable as the failure value because console address 0 maps to 0x02000000, so a real
    translation is never 0.
*/
static u32 ra_rc_translate(u32 consoleAddress, u32 len) {
	if (len <= RA_DS_SYSTEM_RAM_SIZE && consoleAddress <= RA_DS_SYSTEM_RAM_SIZE - len) {
		return RA_DS_SYSTEM_RAM_BASE + consoleAddress;
	}
	return 0;
}

static rc_runtime_t runtime;
static u32 peeksThisFrame;
static u32 peeksRejected;
static u32 triggeredCount;
static u32 eventCount;
static u8  rcStage;
static u8  linesMax;

/*
    The test achievement's id. Any non-zero number does; it is only how the runtime
    identifies the trigger back to us, and nothing here talks to the server yet.
*/
#define RA_TEST_ACHIEVEMENT_ID 1

/*
    rcheevos asks for memory through this, once per distinct address per frame. num_bytes
    is only ever 1, 2 or 4 -- rc_peek_value() decomposes every other size into one of
    those before it gets here.

    There is no error channel: peek returns a value, so a read this console cannot serve
    has to return something. Zero is the right something. It makes the condition compare
    against zero and be false, which is "the achievement does not unlock" -- the safe
    direction. The refusal is counted rather than swallowed, so it is visible in the
    snapshot instead of being indistinguishable from a genuine zero in memory.
*/
static uint32_t ra_rc_peek(uint32_t consoleAddress, uint32_t numBytes, void* ud) {
	const u32 address = ra_rc_translate(consoleAddress, numBytes);

	(void)ud;
	peeksThisFrame++;

	if (address == 0 || !ra_readable(address, numBytes)) {
		peeksRejected++;
		return 0;
	}

	/*
	    Aligned reads go through the watchlist's own read. Unaligned ones are assembled
	    little-endian from bytes, because an unaligned LDR on the ARM9 returns the word
	    rotated rather than faulting -- so the hardware would answer, just wrongly.
	*/
	if ((address & (numBytes - 1)) == 0) {
		return ra_read(address, (u8)numBytes);
	}
	{
		u32 value = 0;
		u32 i;
		for (i = 0; i < numBytes; i++) {
			value |= (u32)(*(const vu8*)(address + i)) << (i * 8);
		}
		return value;
	}
}

/*
    Handed to rc_runtime_validate_addresses() once, after activation. Non-zero means the
    address is one this console can supply; rcheevos disables any achievement that
    references one that is not.

    This is the difference between checking a definition and checking every read it makes.
    Both happen -- peek() still validates, because a definition can compute an address at
    runtime through an indirection rcheevos calls AddAddress -- but doing it here as well
    means a definition that could never work says so on the frame it is loaded.
*/
static int ra_rc_validate_address(uint32_t consoleAddress) {
	return ra_rc_translate(consoleAddress, 1) != 0;
}

static void ra_rc_event_handler(const rc_runtime_event_t* runtimeEvent) {
	eventCount++;
	if (runtimeEvent->type == RC_RUNTIME_EVENT_ACHIEVEMENT_TRIGGERED) {
		triggeredCount++;
	}
}

/*
    The definition to evaluate, in the server's own syntax:

        M:0xH000000>=0.600.

    Read as: measured, the byte at console address 0 is at least zero, six hundred times.
    The comparison is always true, so it counts one hit per frame and unlocks after 600
    frames -- about ten seconds.

    It is anchored at console address 0, and that is the interesting part.

    The obvious anchor would have been the snapshot's own tick counter, and that is what
    this was until hardware said otherwise. It does not work, for a reason worth writing
    down: RetroAchievements maps 4M of DS system RAM, and on this hardware main RAM is 16M.
    The cardengine lives at 0x027FC000 -- eight megabytes in -- so the snapshot has no
    console address at all. It is not a mirror of 0x023FC000 either; that was tested
    directly, by writing a sentinel through one address and reading at the other, and they
    are separate memory.

    Console address 0 is the first word of the game's own RAM. Always mapped, always
    readable, never written by us.

    What this covers: a memref read, a comparison, a hit target, the measured flag, the
    trigger, and rc_runtime_do_frame() reaching memory every frame. What it does not cover
    is the delta memref, which needs a value that changes and therefore a game address
    nobody can name in advance. The first real achievement will exercise it.
*/
#define RA_TEST_DEFINITION "M:0xH000000>=0.600."

/*
    The definition actually evaluated: whatever the launcher staged from
    sd:/_nds/nds-bootstrap/ra_achievements.txt, or the built-in self-test if there is none.

    A file beats a constant here for one reason that matters more than flexibility: testing
    a definition against a running game costs a build, a flash, a play session and a
    photograph, and definitions are exactly the kind of thing that is wrong the first two
    times. Through a file, trying another one is an edit.

    Everything about the string is still distrusted. It is length-checked by the launcher
    before it is staged, terminated here regardless of what the file contained, and handed
    to rcheevos' parser -- which reports a bad definition as an error code rather than
    misbehaving. A definition from a text file gets no more faith than one from the server,
    because eventually it *is* one from the server.
*/
static const char* ra_definition(raSnapshot* snapshot) {
	const u32* block = (const u32*)CARDENGINEI_ARM9_RA_DEFS_LOCATION;

	if (block[0] != CARDENGINEI_ARM9_RA_DEFS_MAGIC) {
		snapshot->rcFromFile = 0;
		return RA_TEST_DEFINITION;
	}
	{
		u32   length = block[1];
		char* text   = (char*)(CARDENGINEI_ARM9_RA_DEFS_LOCATION
		                       + CARDENGINEI_ARM9_RA_DEFS_HEADER);

		if (length == 0
		 || length >= CARDENGINEI_ARM9_RA_DEFS_MAX - CARDENGINEI_ARM9_RA_DEFS_HEADER) {
			snapshot->rcFromFile = 0;
			return RA_TEST_DEFINITION;
		}
		/*
		    Terminated here rather than trusted. The launcher writes a terminator, but this
		    string is about to be walked by a parser and the cost of making sure is one
		    store.
		*/
		text[length] = 0;
		/*
		    Trailing newline and carriage return trimmed, because the file was typed by a
		    human in a text editor and rcheevos would reject the whitespace as syntax.
		*/
		while (length > 0 && (text[length - 1] == '\n' || text[length - 1] == '\r'
		                   || text[length - 1] == ' '  || text[length - 1] == '\t')) {
			text[--length] = 0;
		}
		snapshot->rcFromFile = 1;
		snapshot->rcDefLength = (u16)length;
		return text;
	}
}

/*
    Bring rcheevos up, once, and report how far it got. Returns the stage reached.

    The malloc probe is not defensive habit. rc_runtime_init() allocates the memref list
    and does not check the result before writing through it, so an exhausted arena would
    be a null dereference inside the library rather than a failure it reports. Proving the
    allocation can be made before calling it turns that into RA_RC_NO_MEMORY.
*/
static u8 ra_rc_init(raSnapshot* snapshot) {
	int activate;

	{
		void* probe = malloc(sizeof(rc_memrefs_t));

		if (probe == 0) {
			return RA_RC_NO_MEMORY;
		}
		free(probe);
	}

	rc_runtime_init(&runtime);
	if (runtime.memrefs == 0) {
		return RA_RC_NO_MEMREFS;
	}

	activate = rc_runtime_activate_achievement(
		&runtime, RA_TEST_ACHIEVEMENT_ID, ra_definition(snapshot), 0, 0);

	snapshot->rcActivate = (s8)activate;
	if (activate != RC_OK) {
		return RA_RC_PARSE_BAD;
	}

	/*
	    Ask rcheevos to check every address the definition ended up referencing, now,
	    against what this console has. Nothing should fail here -- the definition above
	    points at the snapshot -- but a definition from the server will, and this is the
	    call that has to already be in place when it does.
	*/
	rc_runtime_validate_addresses(&runtime, ra_rc_event_handler, ra_rc_validate_address);

	return RA_RC_ACTIVE;
}

/*
    Called once per frame from ra_wram_tick(), after the watchlist. Runs inside the game's
    VCOUNT interrupt handler like everything else in this binary, so the cost is measured
    rather than assumed -- see rcLines.
*/
void ra_rc_tick(raSnapshot* snapshot) {
	const rc_trigger_t* trigger;
	unsigned measured = 0;
	unsigned target   = 0;
	u16 startLine;
	u16 lines;

	if (rcStage == RA_RC_NONE) {
		/*
		    The one-time parse, timed on its own so it does not masquerade as the
		    steady-state cost.
		*/
		startLine = RA_VCOUNT;
		rcStage   = ra_rc_init(snapshot);
		lines     = (RA_VCOUNT - startLine + RA_SCANLINES_PER_FRAME) % RA_SCANLINES_PER_FRAME;

		snapshot->rcInitLines = (u8)((lines > 255) ? 255 : lines);
		snapshot->rcStage     = rcStage;
		if (rcStage != RA_RC_ACTIVE) {
			return;
		}
	}
	if (rcStage < RA_RC_ACTIVE) {
		return;
	}

	peeksThisFrame = 0;

	startLine = RA_VCOUNT;
	rc_runtime_do_frame(&runtime, ra_rc_event_handler, ra_rc_peek, 0, 0);
	lines = (RA_VCOUNT - startLine + RA_SCANLINES_PER_FRAME) % RA_SCANLINES_PER_FRAME;

	if (lines > 255) {
		lines = 255;
	}
	if ((u8)lines > linesMax) {
		linesMax = (u8)lines;
	}

	rcStage = RA_RC_FRAME;

	rc_runtime_get_achievement_measured(&runtime, RA_TEST_ACHIEVEMENT_ID, &measured, &target);
	trigger = rc_runtime_get_achievement(&runtime, RA_TEST_ACHIEVEMENT_ID);

	snapshot->rcStage         = rcStage;
	snapshot->rcTriggerState  = trigger ? trigger->state : RC_TRIGGER_STATE_INACTIVE;
	snapshot->rcTriggered     = triggeredCount;
	/*
	    Latched at the last active reading rather than copied blindly. rcheevos reports
	    measured progress only while a trigger is active, so both of these go back to zero
	    the moment the achievement fires -- which would leave a snapshot taken after the
	    unlock showing a pair of zeros and no sign of how it got there -- which is what the
	    first successful hardware reading did show.

	    The latched value is the last one reported while the trigger was active, so it is
	    one short of the target: on the frame the count reaches it, the trigger fires and
	    rcheevos has already stopped reporting. 599 of 600 beside rcTriggered = 1 is the
	    honest reading, not an off-by-one.
	*/
	if (target != 0) {
		snapshot->rcMeasured = measured;
		snapshot->rcTarget   = target;
	}
	snapshot->rcPeeks         = peeksThisFrame;
	snapshot->rcPeeksRejected = peeksRejected;
	snapshot->rcLines         = (u8)lines;
	snapshot->rcLinesMax      = linesMax;
	snapshot->rcEvents        = (u8)((eventCount > 255) ? 255 : eventCount);
}
