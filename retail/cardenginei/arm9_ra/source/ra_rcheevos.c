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
extern int  ra_watch_add(u32 base, u8 size, u8 depth, const u32* offsets);
extern int  ra_watch_add_flags(u32 base, u8 size, u8 depth, const u32* offsets, u8 flags);
extern void ra_watch_clear(void);

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

/*
    ------------------------------------------------------------------------------------
    A stack of our own for rcheevos, which is the fix for two Data Aborts.

    Everything in this binary runs inside the game's VCOUNT interrupt handler -- see
    myIrqHandlerVcount() in cardenginei_arm9 -- so it runs on the game's IRQ stack, whose size is
    the game's business and not something we get to know. Measured on a host with
    -finstrument-functions:

        rc_runtime_do_frame()                 767 bytes
        rc_runtime_activate_achievement()   2,383 bytes

    The first has run every frame for many sessions without trouble, so the IRQ stack
    accommodates it. The second wants 3.1 times as much, and it is what the first real
    achievement set introduced: fifty-six parses instead of three. Both hardware crashes had a
    wild PC inside the *game's* memory with a null data address, which is what trampling the
    memory below an IRQ stack looks like from the outside.

    Why the three-definition builds worked is worth being honest about: they overflowed too. They
    did it three times, during boot, over memory the game had not started using. That is luck,
    and this document has a section about the last time luck was mistaken for a result.

    8 KB against a measured 2,383, and it costs nothing that matters -- the arena has 130 KB of
    margin with the set this large. The high-water mark is reported, so the next reading replaces
    the host's number with the console's.
    ------------------------------------------------------------------------------------
*/
#define RA_RC_STACK_BYTES   8192
#define RA_RC_STACK_WORDS   (RA_RC_STACK_BYTES / 4)
#define RA_RC_STACK_PATTERN 0x5A5A5A5AuL

/* 8-byte aligned because AAPCS wants sp 8-byte aligned at a public interface. */
static u32 raRcStack[RA_RC_STACK_WORDS] __attribute__((aligned(8)));
static u8  raRcStackReady;

typedef u8 (*raRcStep)(raSnapshot*);

/*
    Call fn(snapshot) with sp pointing at our stack, then put sp back.

    r4 and r5 hold the old sp and the target across the switch and are in the clobber list, which
    is what keeps the compiler from placing an input in either -- and that matters: an earlier
    shape of this took the function pointer in r0 and then loaded the argument into r0 before
    branching. `blx` because this is ARMv5TE and the callee may be either instruction set.
*/
#ifdef __arm__
static u8 ra_rc_on_stack(raRcStep fn, raSnapshot* snapshot) {
	u32 result;

	__asm__ volatile (
		"mov  r4, sp        \n"
		"mov  r5, %[fn]     \n"
		"mov  r0, %[arg]    \n"
		"mov  sp, %[top]    \n"
		"blx  r5            \n"
		"mov  sp, r4        \n"
		"mov  %[res], r0    \n"
		: [res] "=r" (result)
		: [fn] "r" (fn), [arg] "r" (snapshot),
		  [top] "r" ((char*)raRcStack + RA_RC_STACK_BYTES)
		: "r0", "r1", "r2", "r3", "r4", "r5", "r12", "lr", "cc", "memory"
	);
	return (u8)result;
}
#else
/*
    The host build calls straight through. tools/ra_reader_test.c therefore does *not* exercise
    the switch, which is worth stating rather than leaving implied -- what it does exercise is
    that everything reached through it still works when the stack is someone else's.
*/
static u8 ra_rc_on_stack(raRcStep fn, raSnapshot* snapshot) {
	return fn(snapshot);
}
#endif

/*
    Run one step on our stack and record how deep it went.

    The region is painted once and never repainted, so the mark is the high-water mark across the
    whole session rather than the last call's. Scanned from the low end: the first word that is
    still the pattern bounds everything that has ever been used above it.
*/
static u8 ra_rc_step(raRcStep fn, raSnapshot* snapshot) {
	u8  stage;
	u32 i;

	if (!raRcStackReady) {
		for (i = 0; i < RA_RC_STACK_WORDS; i++) {
			raRcStack[i] = RA_RC_STACK_PATTERN;
		}
		raRcStackReady = 1;
	}

	stage = ra_rc_on_stack(fn, snapshot);

	for (i = 0; i < RA_RC_STACK_WORDS; i++) {
		if (raRcStack[i] != RA_RC_STACK_PATTERN) {
			break;
		}
	}
	snapshot->rcStackUsed = (u16)((RA_RC_STACK_WORDS - i) * 4);
	/*
	    Saturating rather than wrapping would be wrong here: 8192 fits a u16 exactly, and a mark
	    *at* 8192 means the paint was consumed to the last word, which is the one reading that
	    would mean the stack is too small. It is reported as 8192 and read as "suspect".
	*/
	return stage;
}

static rc_runtime_t runtime;
static u32 peeksThisFrame;
static u32 peeksRejected;
static u32 triggeredCount;
static u32 eventCount;
/*
    Which definition unlocked first, and how expensive the one-time parse was. Statics rather
    than locals because the event handler has no user-data pointer and because the activation
    functions report through the snapshot they are handed rather than owning one.
*/
static u8  firstTriggered;
static u32 firstId;
static u8  initMaxLines;
static u32 initTotalLines;
static u8  rcStage;
static u8  linesMax;

/*
    128, raised from 8 when `r=patch` arrived.

    The 8 was right for what it was for: the definitions file was a hand-typed line or three,
    and a limit that small made the split obviously bounded. A real achievement set is a
    hundred definitions or more, so the number had to follow the source of the definitions
    changing from a person to a server.

    What it costs is 4 bytes of pointer each, and they are `static` below rather than on the
    stack for that reason -- `ra_rc_init()` is reached from the cardengine's own context, whose
    stack is not this binary's to spend 512 bytes of. It runs once, so static is not a
    compromise.

    The other half of the limit is the block itself: 32,760 bytes of text at
    CARDENGINEI_ARM9_RA_DEFS_MAX. 128 definitions therefore average 255 bytes each before the
    block runs out first, which is the constraint worth knowing about -- RA memaddr strings run
    from tens to a few hundred characters. tools/ra_reader_test.c pins the two numbers against
    each other so raising one without the other fails on the host.
*/
#define RA_DEFS_MAX_LINES 128

/*
    The split definitions, and how far activation has got through them.

    File statics rather than locals because activation is spread over frames now: ra_rc_prepare()
    fills these in once and ra_rc_activate_next() is called on later ticks. The pointers are into
    the staging block, which is not going anywhere -- ra_split_definitions() writes its
    terminators in place and never copies.
*/
static char* defLines[RA_DEFS_MAX_LINES];
/*
    Each line's RetroAchievements id, or RA_SYNTHETIC_ID_BASE + index when the line carried none.
    Parallel to defLines rather than packed with it because the pointers are into the staging block
    and the ids are not in it any more -- ra_take_id() consumes them on the way past.
*/
static u32   defIds[RA_DEFS_MAX_LINES];
static u8    defCount;
static u8    defIndex;
static u8    activatedCount;
static int   defFirstError;

/*
    The test achievement's id. Any non-zero number does; it is only how the runtime
    identifies the trigger back to us, and nothing here talks to the server yet.
*/
#define RA_TEST_ACHIEVEMENT_ID 1

/*
    rcheevos asks for memory through this, once per distinct address per frame.

    **num_bytes is not only 1, 2 or 4.** This comment used to say it was, on the reasoning that
    rc_peek_value() decomposes larger widths -- and the code below trusted that with an alignment
    mask of `numBytes - 1`. A definition using `0xW` asks for **three**, the mask becomes 2, and a
    32-bit load happens at an address that is 1 mod 4. The first achievement set this project did
    not write contains a `0xW`, which is how the assumption was found. Widths are handed to
    ra_read() unfiltered now and it assembles anything that is not a native aligned width.

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
	    Every width goes through the watchlist's own read now, including the odd ones, because
	    that is where the byte assembly belongs -- there is one answer in this binary to "read
	    these bytes" rather than two that can drift apart.

	    The test this replaced was `(address & (numBytes - 1)) == 0`, and it is worth recording
	    why it was wrong rather than just deleting it. It assumes numBytes is a power of two.
	    rcheevos asks for **three** when a definition uses `0xW`, and then the mask is 2 -- so an
	    address that is 1 mod 4 passes a test it should fail, and a 32-bit load happens at an odd
	    address. The first set this project did not write contains a `0xW`.
	*/
	return ra_read(address, (u8)numBytes);
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
		/*
		    The first one only, and recorded as a line number rather than an id so it can be
		    looked up in the file by eye. With one definition loaded a counter was enough; with
		    fifty-six, "something fired" is not a reading -- the set's first definition is
		    `1=1.300.` and should unlock about five seconds in, so this is what turns that into
		    a prediction that can be wrong.
		*/
		if (firstId == 0) {
			u8 i;

			firstId = runtimeEvent->id;
			/*
			    The line is looked up rather than derived. It used to be `id - base + 1`, which only
			    worked because every definition was numbered from RA_TEST_ACHIEVEMENT_ID in order;
			    with the server's own ids there is no arithmetic that recovers a line, and a search
			    over at most 128 entries costs nothing on the frame an achievement unlocks.
			*/
			for (i = 0; i < defCount; i++) {
				if (defIds[i] == runtimeEvent->id) {
					firstTriggered = (u8)(i + 1);
					break;
				}
			}
		}
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

/*
    Split the staged text into lines, in place.

    One definition per line, because a hardware session is the scarce resource in this
    project and testing one definition per session wastes it. Several can be tried at once
    and the snapshot says how many parsed and how many fired.

    Blank lines and lines beginning with '#' are skipped, so the file can carry a note about
    what each definition is meant to do -- which matters when the answer arrives hours later
    as a photograph.
*/
static u8 ra_split_definitions(char* text, u32 length, char** lines) {
	u8  count = 0;
	u32 i     = 0;

	while (i < length && count < RA_DEFS_MAX_LINES) {
		char* start;

		while (i < length && (text[i] == '\n' || text[i] == '\r'
		                   || text[i] == ' '  || text[i] == '\t')) {
			i++;
		}
		if (i >= length) {
			break;
		}
		start = &text[i];
		while (i < length && text[i] != '\n' && text[i] != '\r') {
			i++;
		}
		text[i++] = 0;

		/* Trailing whitespace, because an editor leaves it and rcheevos rejects it. */
		{
			char* end = start;
			while (*end) {
				end++;
			}
			while (end > start && (end[-1] == ' ' || end[-1] == '\t')) {
				*--end = 0;
			}
		}
		if (*start && *start != '#') {
			lines[count++] = start;
		}
	}
	return count;
}

/*
    Hexadecimal up to the next delimiter, advancing the cursor. Returns whether anything
    was read, because an empty field and a zero are different mistakes.
*/
static bool ra_parse_hex(const char** p, u32* out) {
	const char* s     = *p;
	u32         value = 0;
	bool        any   = false;

	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		s += 2;
	}
	while (1) {
		u32 digit;
		if (*s >= '0' && *s <= '9')      digit = (u32)(*s - '0');
		else if (*s >= 'a' && *s <= 'f') digit = (u32)(*s - 'a' + 10);
		else if (*s >= 'A' && *s <= 'F') digit = (u32)(*s - 'A' + 10);
		else break;
		value = (value << 4) | digit;
		any   = true;
		s++;
	}
	*p   = s;
	*out = value;
	return any;
}

/*
    A watch line: `W:<address>:<size>[:<offset>[:<offset>]]`, addresses being console
    addresses like everywhere else in this file.

    This exists because of what the first hardware run showed. Every mechanical part worked
    -- three definitions parsed, the pointer chain walked four memrefs a frame, nothing
    refused -- and no achievement fired. That is not a bug to debug, it is a fact about the
    game that nobody has measured: the conditions were written from published code notes and
    something about them does not match this ROM.

    Guessing at another definition costs a session. Reading the addresses costs nothing extra,
    because the watchlist already resolves chains and reports values into the snapshot. So
    the same file that carries definitions can carry watches, and the next session answers
    "what does this memory actually hold" instead of "did my next guess work".

      W:1593d0:4        the 32-bit value at console 0x1593d0
      W:159164:4:9c     the 32-bit value at (24-bit pointer at 0x159164) + 0x9c

    Any watch line replaces the built-in self-test watches, so the first four land in the
    snapshot's results[] where they can be read.
*/
static bool ra_add_watch_line(const char* line, u8 flags) {
	u32 base;
	u32 size;
	u32 offsets[RA_CHAIN_MAX];
	u8  depth = 0;

	if (!ra_parse_hex(&line, &base) || *line != ':') {
		return false;
	}
	line++;
	if (!ra_parse_hex(&line, &size)) {
		return false;
	}
	while (*line == ':' && depth < RA_CHAIN_MAX) {
		u32 offset;
		line++;
		if (!ra_parse_hex(&line, &offset)) {
			return false;
		}
		offsets[depth++] = offset;
	}

	/*
	    Console address to DS address, the same translation peek() does. A watch written
	    beside a definition should mean the same thing the definition means.
	*/
	base = ra_rc_translate(base, size ? size : 1);
	if (base == 0) {
		return false;
	}
	return ra_watch_add_flags(base, (u8)size, depth, offsets, flags) >= 0;
}

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
    Take a leading `<digits>:` off a line and return the id, advancing the pointer past it.

    Zero means the line had no id, which is not an error -- a hand-written ra_achievements.txt is
    not expected to carry them and the set this project shipped as an artifact does not.

    The test is **digits then colon**, and it is exact rather than heuristic. Every memaddr prefix
    flag that ends in a colon is a letter (`A:`, `M:`, `N:`, `O:`, `P:`, `Q:`, `R:`, `T:`, `I:`,
    `K:`, `Z:`, `G:`, `C:`, `B:`), so a digit run before the first colon cannot be memaddr syntax.
    A definition may certainly *begin* with a digit -- the real set's first line is `1=1.300.` --
    which is why the colon is required and why tools/ra_reader_test.c feeds exactly that line.
*/
static u32 ra_take_id(char** line) {
	const char* at = *line;
	u32         id = 0;
	u32         digits = 0;

	while (at[digits] >= '0' && at[digits] <= '9') {
		digits++;
	}
	if (digits == 0 || at[digits] != ':') {
		return 0;
	}
	{
		u32 i;
		for (i = 0; i < digits; i++) {
			/* Clamped rather than wrapped: a truncated id names a different achievement. */
			if (id < 100000000u) {
				id = id * 10 + (u32)(at[i] - '0');
			}
		}
	}
	*line = (char*)(at + digits + 1);
	return id;
}

/*
    Bring rcheevos up and report how far it got. Two functions rather than one, and the split is
    the whole point of this build.

    ra_rc_prepare() runs once: it probes the arena, initialises the runtime, reads the staged
    definitions and installs any watch lines. ra_rc_activate_next() then activates **one**
    definition per frame until the set is in.

    Why: fifty-six definitions cannot be parsed inside a single interrupt. Each costs the same
    ~2.4 KB of stack -- measured on a host, and flat, so depth is not what scales -- plus its own
    slice of time, and the first run that tried all fifty-six in one VCOUNT handler ended in a
    Data Abort with the ARM9 executing the definition text as code. That total time is still
    unmeasured, which is exactly why it is the leading suspect and why the fix is to stop doing
    it rather than to reason about it further.

    The second reason is diagnostic and matters just as much. rcActivated is published *before*
    each activation is attempted, so a crash names the line it died on. A set that dies at the
    same definition every time is one definition's problem; a set that gets through all of them
    and dies later is the frame budget's. Those are different bugs and the old code could not
    tell them apart.
*/
static u8 ra_rc_prepare(raSnapshot* snapshot) {
	char* text;
	u8    i;

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

	text        = (char*)ra_definition(snapshot);
	defLines[0] = text;
	defCount    = 1;
	if (snapshot->rcFromFile) {
		defCount = ra_split_definitions(text, snapshot->rcDefLength, defLines);
	}
	/*
	    All of it reset, not just the index. On hardware this runs once, so it would never have
	    mattered there -- and tools/ra_reader_test.c prepares twice, which is how a static that
	    carried a previous run's count showed up. A function whose correctness depends on being
	    called only once is a function that will eventually be called twice.
	*/
	/*
	    Ids taken here, once, right after the split -- not at activation time. The pointers in
	    defLines are what everything downstream uses, so they have to already be past the id; doing
	    it later would mean every user of a line remembering to skip it.
	*/
	snapshot->rcDefsWithId = 0;
	snapshot->rcDefsNoId   = 0;
	for (i = 0; i < defCount; i++) {
		defIds[i] = ra_take_id(&defLines[i]);
		if (defIds[i]) {
			snapshot->rcDefsWithId++;
		} else {
			/*
			    Numbered far from anything real, because rcheevos identifies achievements by id and
			    reuses the trigger of one it has already seen. See RA_SYNTHETIC_ID_BASE.
			*/
			defIds[i] = RA_SYNTHETIC_ID_BASE + i;
			snapshot->rcDefsNoId++;
		}
	}

	defIndex       = 0;
	defFirstError  = RC_OK;
	activatedCount = 0;
	initMaxLines   = 0;
	initTotalLines = 0;

	snapshot->rcActivated = 0;
	snapshot->rcActivate  = 0;
	snapshot->rcInitLines = 0;
	snapshot->rcInitTotal = 0;

	/*
	    Watches first, and only clearing the defaults if the file actually supplies some -- a
	    file of definitions alone should still show the self-test watches, which are the thing
	    that says the reader is alive at all.

	    Still done in one go: a watch line is a handful of hex fields, not a parse.
	*/
	{
		bool anyWatch = false;

		for (i = 0; i < defCount; i++) {
			const char* rest  = 0;
			u8          flags = 0;

			/* `W:` is a plain chain; `W24:` masks each pointer to 24 bits. */
			if (defLines[i][0] == 'W' && defLines[i][1] == ':') {
				rest = defLines[i] + 2;
			} else if (defLines[i][0] == 'W' && defLines[i][1] == '2'
			        && defLines[i][2] == '4' && defLines[i][3] == ':') {
				rest  = defLines[i] + 4;
				flags = RA_WATCH_FLAG_PTR24;
			}
			if (!rest) {
				continue;
			}
			if (!anyWatch) {
				ra_watch_clear();
				anyWatch = true;
			}
			if (!ra_add_watch_line(rest, flags)) {
				snapshot->rcBadLine = i + 1;
			}
		}
	}

	return RA_RC_LOADING;
}

/*
    One definition, then out. Returns RA_RC_LOADING while any remain.

    Every line gets its own achievement id, numbered from RA_TEST_ACHIEVEMENT_ID, so the first
    keeps the id the measured-progress fields report on and the rest still count toward
    rcTriggered. rcActivate carries the *first* failure rather than the last -- a set with one bad
    line among fifty-six should say which, not be overwritten by whichever came last.
*/
static u8 ra_rc_activate_next(raSnapshot* snapshot) {
	int one;
	u16 startLine;
	u16 spent;
	u8  line;

	/* Skip watch lines; ra_rc_prepare() already dealt with them. */
	while (defIndex < defCount
	    && defLines[defIndex][0] == 'W'
	    && (defLines[defIndex][1] == ':' || defLines[defIndex][1] == '2')) {
		defIndex++;
	}

	if (defIndex < defCount) {
		line = defIndex;
		defIndex++;

		/*
		    Timed one definition at a time, which is also the only way the total can be right:
		    a single VCOUNT delta around the whole set is taken modulo 263, so a parse spanning
		    four frames reports the remainder and a slow init reads as a fast one. Per-definition
		    deltas sum correctly as long as no single activation exceeds a frame, and
		    initMaxLines is what says whether that held.
		*/
		startLine = RA_VCOUNT;
		one = rc_runtime_activate_achievement(
			&runtime, defIds[line], defLines[line], 0, 0);
		spent = (u16)((RA_VCOUNT - startLine + RA_SCANLINES_PER_FRAME)
		              % RA_SCANLINES_PER_FRAME);

		initTotalLines += spent;
		if (spent > initMaxLines) {
			initMaxLines = (u8)((spent > 255) ? 255 : spent);
		}
		snapshot->rcInitLines = initMaxLines;
		snapshot->rcInitTotal = (u16)((initTotalLines > 0xFFFF) ? 0xFFFF : initTotalLines);

		if (one == RC_OK) {
			activatedCount++;
		} else if (defFirstError == RC_OK) {
			defFirstError        = one;
			snapshot->rcBadLine  = (u8)(line + 1);
		}
		/*
		    Published after each definition rather than after all of them, which is what makes a
		    crash name its own line: rcActivated is the count that succeeded, so dying inside
		    definition k leaves k-1 here. Kept as a count rather than briefly holding the index
		    being attempted -- a field that means two things depending on when you read it is not
		    a reading, and rcActivate being 0 already says none of the k-1 failed.
		*/
		snapshot->rcActivated = activatedCount;
		snapshot->rcActivate  = (s8)defFirstError;

		return RA_RC_LOADING;
	}

	/*
	    A file of watches alone is legitimate -- measuring memory is a reason to boot -- so only
	    a file that offered definitions and had none parse is a failure. The self-test keeps the
	    runtime doing something either way.
	*/
	if (activatedCount == 0) {
		if (rc_runtime_activate_achievement(&runtime, RA_TEST_ACHIEVEMENT_ID,
		                                   RA_TEST_DEFINITION, 0, 0) != RC_OK) {
			return RA_RC_PARSE_BAD;
		}
		/*
		    Recorded in defIds too, so the measured-progress lookup and the line search below both
		    find it. The fallback used to be indistinguishable from a staged definition because both
		    were numbered 1; now it has to say so.
		*/
		defLines[0]           = (char*)RA_TEST_DEFINITION;
		defIds[0]             = RA_TEST_ACHIEVEMENT_ID;
		defCount              = 1;
		activatedCount        = 1;
		snapshot->rcActivated = 1;
	}

	/*
	    Ask rcheevos to check every address the set ended up referencing, now, against what this
	    console has. rcheevos disables any achievement that names one it cannot supply, so this
	    is what turns "an achievement silently never fires" into rcPeeksRejected.

	    Done once, here, and deliberately after the last definition rather than after each one:
	    it walks the whole memref pool, so per-definition it would be O(n squared) over a pool
	    that ends up hundreds long.
	*/
	rc_runtime_validate_addresses(&runtime, ra_rc_event_handler, ra_rc_validate_address);

	return RA_RC_ACTIVE;
}

/*
    One frame of evaluation, as a step so it can be run on the private stack like the rest.

    Returns a stage only to fit raRcStep; the caller keeps using RA_RC_FRAME. A wrapper rather
    than an asm call to rc_runtime_do_frame() directly, because the callbacks it needs are static
    to this file and the trampoline takes one argument.
*/
static u8 ra_rc_frame_step(raSnapshot* snapshot) {
	(void)snapshot;
	rc_runtime_do_frame(&runtime, ra_rc_event_handler, ra_rc_peek, 0, 0);
	return RA_RC_FRAME;
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

	/*
	    Coming up, spread over frames: prepare on one tick, then one definition per tick until
	    the set is in. Nothing evaluates until it is -- do_frame on a half-loaded runtime would
	    make rcLinesMax a measurement of a moving target.

	    An error stage matches neither branch and is left alone, so a failure stays reported
	    rather than being retried every frame forever.
	*/
	if (rcStage < RA_RC_ACTIVE) {
		if (rcStage == RA_RC_NONE) {
			rcStage = ra_rc_step(ra_rc_prepare, snapshot);
		} else if (rcStage == RA_RC_LOADING) {
			rcStage = ra_rc_step(ra_rc_activate_next, snapshot);
		}
		snapshot->rcStage = rcStage;
		if (rcStage < RA_RC_ACTIVE) {
			return;
		}
	}

	peeksThisFrame = 0;

	/*
	    On our stack too, and not only because 767 bytes is a lot to borrow: one stack for all of
	    rcheevos means rcStackUsed is the high-water mark for everything the library does rather
	    than for the parse alone, and it means there is one thing to reason about instead of two.
	    peek() is called from in here, so it runs on our stack as well -- which it should, since
	    it is rcheevos that decides how deep to call it from.
	*/
	startLine = RA_VCOUNT;
	ra_rc_step(ra_rc_frame_step, snapshot);
	lines = (RA_VCOUNT - startLine + RA_SCANLINES_PER_FRAME) % RA_SCANLINES_PER_FRAME;

	if (lines > 255) {
		lines = 255;
	}
	if ((u8)lines > linesMax) {
		linesMax = (u8)lines;
	}

	rcStage = RA_RC_FRAME;

	/*
	    Reported for the *first staged definition*, whatever its id turned out to be, rather than for
	    the constant 1. Those were the same thing while every definition was numbered from
	    RA_TEST_ACHIEVEMENT_ID; with real ids they are not, and asking for 1 would have quietly
	    reported on an achievement that does not exist.
	*/
	{
		const u32 firstId = defCount ? defIds[0] : RA_TEST_ACHIEVEMENT_ID;

		rc_runtime_get_achievement_measured(&runtime, firstId, &measured, &target);
		trigger = rc_runtime_get_achievement(&runtime, firstId);
	}

	snapshot->rcStage         = rcStage;
	snapshot->rcTriggerState  = trigger ? trigger->state : RC_TRIGGER_STATE_INACTIVE;
	snapshot->rcTriggered     = triggeredCount;
	snapshot->rcFirstTriggered = firstTriggered;
	snapshot->rcFirstId        = firstId;
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
