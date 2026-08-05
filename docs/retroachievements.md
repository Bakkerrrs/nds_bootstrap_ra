# RetroAchievements on real DS hardware

This fork adds RetroAchievements support to nds-bootstrap, developed and tested on
a Nintendo 3DS running DS games natively in DS mode. The goal of the current work
is **softcore**: achievements that really unlock against the RetroAchievements
servers while playing on the console, with no emulator involved.

The architectural blueprint is odelot's `wii-ra-adapter`: run code alongside the
game, read the game's RAM every frame, and hand it to `rcheevos`. On the DS the
place where code already runs alongside the game is nds-bootstrap's **cardengine**,
which is injected into the game's own address space.

Licensed GPL-3.0, same as the rest of nds-bootstrap. `rcheevos` is also GPL.

## Layering

Three modules, kept strictly separate so the "brain" can later move somewhere else
without rewriting the reader:

| Module | Responsibility | Status |
| --- | --- | --- |
| `ra_reader` | Read the game's RAM. Knows nothing about RetroAchievements. | phase 1 done, hardware unconfirmed |
| `ra_overlay` | Show a notification over the running game. Knows nothing about RetroAchievements either. | proven, needs a real font |
| `ra_client` | Wrap `rcheevos`' `rc_client`; decide what to watch, evaluate, fire unlocks. | not started |
| `ra_net` | HTTP(S) transport to the RA servers. `rcheevos` ships no networking. | not started |

## Building

Upstream CI pins `devkitpro/devkitarm:20241104` — devkitARM r65 (gcc 14.2.0) with
libnds **1.8.0**. That pin matters: libnds 2.x (calico) removed
`nds/fifocommon.h`, `nds/fifomessages.h`, `nds/arm7/clock.h` and `sec_t`, all of
which nds-bootstrap uses, so it will not build against a current toolchain.

```sh
export DEVKITPRO=/opt/devkitpro
export DEVKITARM=$DEVKITPRO/devkitARM
export PATH=$PATH:$DEVKITPRO/tools/bin

gcc lzss.c -o /usr/local/bin/lzss   # host tool, required; CI does the same
make                                # serial -- see below
```

Two things that are easy to trip over:

- `lzss` is a **host** tool built from `lzss.c` in the repo root. Without it every
  `.lz77` target fails with `Error 127`.
- Build serially. `make -j` races: sub-makes link before their dependencies exist
  (`cannot find arm9mpu_reset.o`, `cannot find my_fat.o`).

Output is `retail/bin/nds-bootstrap.nds` and `hb/bin/nds-bootstrap.nds`.

## Phase 0 — reading the game's RAM every frame

Because the cardengine shares an address space with the game, reading game RAM is
just a pointer dereference — exactly what the in-game menu's RAM viewer does with
`address = 0x02000000`.

**Hook point.** The ARM9 cardengine already has a per-frame entry point:
`myIrqHandlerVcount()`, installed into the game's IRQ table at `ce9->irqTable + 2`
by `hookIPC_SYNC()` in `retail/cardenginei/arm9/source/misc.c`. Upstream only
installs it for the colour-LUT feature; this fork also installs it when the reader
is enabled, and `myIrqEnable()` forces `IRQ_VCOUNT` on to guarantee it fires.

In phase 0 `ra_reader_tick()` copied one fixed window of bytes into the snapshot
buffer once per frame. That window was pointed at game RAM, at the sub engine's
display registers and at the overlay's own VRAM in turn, which is how most of what is
written here was established. Phase 1 replaced it with a watchlist; the hook and the
snapshot buffer are unchanged.

The snapshot lives in the cardengine's own `.bss`, which is inside the region
reserved for the cardengine, so the game can never touch it. `.bss` is **not**
zeroed — an injected binary has no crt0 to do it — so the header is validated by
magic on every tick and nothing assumes a known initial state.

The snapshot doubles as the debug channel for everything in this document: it is
read with the in-game menu's RAM viewer, so its exact layout changes as diagnostics
come and go. See `raSnapshot` in `retail/common/include/ra.h` for the current one.

### Observing it on hardware

The buffer sits in `.bss`, so its address shifts whenever surrounding code
changes. After building, run:

```sh
./tools/ra_snapshot_addr.sh
```

Use the address for the variant your game actually loads — a plain retail DS game
on a DSi or 3DS uses `cardenginei_arm9`. Then open the in-game menu, go to the RAM
viewer, navigate to that address, and you should see:

- the ASCII bytes `52 41 31 53` (`RA1S`) -- the digit is the layout version, so a
  stale address from an older build announces itself as `RA0S`,
- a frame counter climbing once per frame,
- then the watch results described under phase 1 below.

Confirmed working on a 3DS running *Space Invaders Extreme* (`cardenginei_arm9`),
with the whole chain intact:

| Field | Value | Meaning |
| --- | --- | --- |
| `ticks` | 354 | the VCOUNT handler is firing every frame |
| `cardReads` / `irqEnables` / `hookCalls` | 89 / 128 / 1 | cardengine has control, the patched `irqEnable` ran, the install ran once |
| `irqTable` | `0x027E0000` | the game's IRQ table was found |
| `vcountRef` | `0x027FC348` | inside the cardengine (base `0x027FC000`) |
| `origVcount` | `0x02006BD8` | inside the game's ARM9 binary — the game had its own VCOUNT handler, so chaining is safe |

A note from that session worth keeping, because it is the trap anyone pointing a
watch at `0x02000000` will fall into: that address is where the game's ARM9 **code**
loads, so what came back was instructions that never change (`E7FFDEFF`, the ARM trap
encoding) and it looked as though nothing was being read. Liveness is proved by
`ticks` and by the watch statuses, not by a value that happens to hold still.

### Files

| File | Purpose |
| --- | --- |
| `retail/common/include/ra.h` | Shared definitions, snapshot layout, master switch |
| `retail/common/include/ra_reader.h` | Reader API |
| `retail/cardenginei/arm9/source/ra_reader.c` | Reader implementation |
| `retail/common/include/ra_overlay.h` | Notification API |
| `retail/cardenginei/arm9/source/ra_overlay.c` | Notification implementation |
| `tools/ra_snapshot_addr.sh` | Prints the snapshot address and the space left, from the link maps |
| `tools/ra_reader_test.c` / `.sh` | Host-side test for the watchlist and the chain walker |

`RA_READER_ENABLED` in `ra.h` is the kill switch. Set it to `0` and the cardengine
behaves exactly like upstream: no per-frame work, and no `IRQ_VCOUNT` forced on for
games that never asked for one.

### Caveats found while wiring this up

- **`colorLutBlockVCount` vetoes the hook.** Upstream already tracks games that
  misbehave when a VCOUNT interrupt is forced on. That flag now suppresses the
  reader's hook too, so those games are not newly broken — but on them the reader
  simply will not tick, and phase 1 will need another hook for them.
- **GSDD builds never tick.** `hookIPC_SYNC()` is compiled out under `#ifndef
  GSDD`, so `cardenginei_arm9_gsdd*` links the reader but never installs the
  handler.
- **Forcing `IRQ_VCOUNT` on is a real behaviour change** for games that did not
  enable it. This is the same thing the colour-LUT path does, so there is
  precedent, but it is the most likely source of regressions and is the first
  thing to suspect if a game misbehaves.
- Reads go through the ARM9 data cache, which is the CPU's own view of memory and
  therefore the correct one — the same view the RAM viewer shows.

## Open questions from the project brief

### #2 — is the VBlank hook the right point, and what is the cycle budget?

The ARM9 **VCOUNT** interrupt (line 0) is used rather than VBlank, because that is
the per-frame ARM9 hook the cardengine already owns. The ARM7 has an always-active
VBlank hook (`vblankHandler` in `retail/cardenginei/arm7/source/card_engine_header.s`)
but the ARM7's view of main RAM is not the ARM9's cached view, so it is the wrong
side to read from for RAM-watching accuracy.

The cycle budget is now **instrumented rather than argued about**, though the figure
itself still needs hardware. `ra_reader_tick()` samples `VCOUNT` on entry and again on
exit and records the difference in `linesLast` / `linesMax`. Scanlines are a coarse
unit — one is roughly 1,600 ARM9 cycles — but they are the right unit for the question
being asked, which is whether the watchlist eats into the frame, and `VCOUNT` is the
only clock available: the game owns the hardware timers.

The measurement is deliberately of the reader alone, not of the overlay that runs
just before it in the same handler. The overlay's cost is fixed; the reader's is the
one that scales with configuration, so it is the one worth a knob and a number.

### #4 — does `rc_client` fit in the cardengine? **No.**

This is now answered, and it is the most important finding so far. The ARM9
cardengine is linked into a fixed 12 KB window:

```
MEMORY { vram : ORIGIN = CARDENGINEI_ARM9_LOCATION, LENGTH = 12K - 0x60 }
```

which is `0x027FC000`–`0x027FEFA0`. Occupancy with the phase 0 reader included was:

| | |
| --- | --- |
| Loaded image (`cardenginei_arm9.bin`) | 10,420 bytes |
| `.bss` end | `0x027FE9F8` |
| Bottom of the stacks (`__sp_usr`) | `0x027FED40` |
| **Free** | **840 bytes** |

`rc_client` plus its runtime state is tens of kilobytes. It cannot live here, and
no amount of trimming changes that by an order of magnitude. Phase 2 therefore has
to place the client somewhere other than the cardengine region — the expanded heap
nds-bootstrap already manages for ROM caching is the obvious candidate, with the
cardengine keeping only the reader and a small bridge. That decision should be
made before any `rcheevos` integration work starts.

Phase 1 sharpened this in two ways, both worth recording.

**840 bytes was never the real ceiling.** `cardenginei_arm9` is one of eight variants
that link the reader, and it is not the tightest. `arm9_dldi`, `arm9_twlsdk_dldi` and
`arm9_twlsdk3_dldi` all carry more code in the same or a smaller window. Whatever the
reader costs, it costs in the tightest of them, so that is the number that matters —
`tools/ra_snapshot_addr.sh` now prints it per variant, and CI prints it on every build
so the margin is visible while it is still a margin.

**Running out was silent.** The stacks are placed by subtracting from the top of the
window, so they sit directly above `.bss` with nothing in between. A `.bss` that grew
past `__sp_usr` produced no diagnostic at all: the region only overflows once `.bss`
passes the very top, and long before that the usr, svc and irq stacks are running
through `.data` and `.bss` on the first call. The symptom is a game that misbehaves
for no visible reason — which, in a project where the reader is *suspected first* any
time a game acts up, is the worst possible failure mode. The eight linker scripts now
assert `__bss_end <= __sp_usr`, so it is a build failure instead.

### #3 — pointer chains: **done**, see phase 1 below.

The reader walks a chain from scratch on every tick and validates every address in the
moment it is about to be used. Caching a resolved address was never an option: that is
the exact thing a pointer chain exists to avoid.

### #1 — network transport

Untouched. Can the cardengine reach the 3DS's ARM11 WiFi stack, or is dswifi the only
option? This still gates the phase 3 architecture and is worth asking the DS-Homebrew
/ RetroAchievements Discords before any hardware is bought.

## Phase 1 — the watchlist and pointer chains

**Implemented and host-tested; not yet confirmed on hardware.** Everything below is
true of the code and of `tools/ra_reader_test.sh`; the "confirmed on hardware" label
this document uses elsewhere has been earned by phases 0 and 0.5 and has not been
earned by this one yet. See *What to look for on hardware* at the end.

Phase 0 read one fixed window. Phase 1 reads a list of watches, each of which is
either a direct address or a **pointer chain**: read the word at the base, add an
offset, read the word there, add another offset, then read the value. `RA_WATCH_MAX`
watches with up to `RA_CHAIN_MAX` indirections each, both in `ra.h`.

### The chain is walked from scratch every frame

This is the whole point, and it is worth being explicit about because caching looks
so obviously right. A chain exists precisely because the thing it points at *moves*:
the game allocates a player structure, frees it on a scene change, allocates another
somewhere else. An address that resolved correctly last frame does not point at the
same field this frame — it points into the middle of whatever is there now, and reads
a plausible-looking number out of it. So the resolved address is never kept, only
reported.

### Nothing read out of the game is trusted

A pointer read from game RAM is whatever happens to be in that word right now,
including garbage while the game is tearing one scene down and building the next.
Following it blind takes a **Data Abort inside the game's own VCOUNT handler**, which
is not a bad reading — it is a crash.

So every address is range-checked immediately before it is used, and the checks are
narrow rather than permissive:

- A pointer the walker is about to follow must be word-aligned and in main RAM
  (`0x02000000`–`0x03000000`). A game pointer is always a main RAM address; one that
  is not has been read out of a structure that no longer exists.
- The final address must be readable and aligned for its size. Main RAM, plus I/O
  only because the diagnostic watch reads a display register. Anything else is
  refused rather than tried.
- Alignment is checked because an unaligned ARM9 load does not fault — it silently
  returns rotated data, which is worse than failing, because the value looks fine.

A chain that does not resolve is **not an error**. Games null their pointers between
scenes; that is normal, and the reader reports it and carries on. What it must never
do is fault or read a wrong value quietly.

### Every watch says how it resolved

`status` per watch, not one global flag, because "which watch stopped resolving, and
at which step" is the question you actually have from a RAM viewer:

| Status | Meaning |
| --- | --- |
| `RA_WATCH_UNUSED` / `PENDING` | free slot / added but not yet evaluated |
| `RA_WATCH_OK` | resolved and read this tick |
| `RA_WATCH_BAD_BASE` | the chain's first address is not usable |
| `RA_WATCH_BAD_POINTER` | a word the walker had to follow for a further step was not usable |
| `RA_WATCH_BAD_TARGET` | the resolved address is not readable |
| `RA_WATCH_MISALIGNED` | the resolved address is not aligned for its size |

The distinction between `BAD_POINTER` and `BAD_TARGET` is about *where* the chain
broke, and it is easy to get backwards: a one-step chain whose pointer goes null
reports `BAD_TARGET`, because there was no further step to take. Reaching
`BAD_POINTER` takes at least two indirections. The host test asserts exactly this —
it caught the author expecting the opposite.

### Proving it works without a game

There is a chicken-and-egg problem: demonstrating that the chain walker resolves and
reads live memory needs a known pointer inside a game, and finding one is phase 2's
job. So the reader carries its own:

- **Watch 0** reads the sub engine's `DISPCNT` directly. A real register that really
  changes — the overlay sets and clears a background-enable bit in it.
- **Watch 1** walks one indirection to `snapshot.ticks`, via a cell holding the
  snapshot's address.
- **Watch 2** walks two indirections to the same place, via a cell holding *that*
  cell's address.

`ticks` climbs every frame, so watches 1 and 2 showing that same climbing number is
proof the walker resolved a chain and read live memory through it — with no game
knowledge at all. Between them the three defaults exercise every success path in the
evaluator.

### The per-frame cost

`linesLast` and `linesMax` record how many scanlines a tick consumed, from `VCOUNT`
before and after. That answers open question #2 above with a number instead of an
argument, and it is the honest unit: the game owns the hardware timers, so `VCOUNT` is
the only clock the reader can read without taking something in use.

### Tested on the host, not just on hardware

`tools/ra_reader_test.sh` builds `ra_reader.c` **verbatim** — nothing stubbed, nothing
conditionally compiled — for the host and exercises the watchlist against real memory.
The trick that makes it a real test rather than a mock is the link address: it links at
`0x02100000` and maps a page at `0x04000000`, so the reader's own globals genuinely sit
inside the main RAM range it validates against, and the I/O reads genuinely land on
mapped memory. The chain self-tests resolve for the same reason they will on hardware,
not because a check was relaxed.

It covers the success paths, every failure status, sized reads, offset accumulation at
each step, recovery after a pointer comes back, and a full list refusing more watches.
CI runs it before the ARM build, so a logic regression fails in seconds.

This matters here more than it would elsewhere. The alternative is a flash cycle per
attempt, and the overlay work cost three of them to find three bugs. The chain walker
is pure address logic; it does not need the hardware, and the part of it that fails
worst — following a bad pointer — is exactly the part a host test can pin down.

One thing it deliberately does not cover: `VCOUNT` does not advance on its own on the
host, so a tick spanning the end of the frame cannot be produced there. The wrap
arithmetic is written out explicitly rather than left to a mask, and it is unverified.

### What it costs

About 520 bytes of the cardengine window over phase 0, at `RA_WATCH_MAX` 4 and
`RA_CHAIN_MAX` 2 — roughly 580 bytes of code and 128 of `.bss`, against 136 and 44
before. That is a large fraction of what was left, which is why:

- Both limits are knobs in `ra.h`, with the cost of a slot documented next to them.
- `ra_reader.c` alone is built `-Os`, with `noinline` on the two functions the `-O2`
  inliner duplicates — `ra_watch_eval()` into the tick loop and
  `ra_reader_watch_add()` into each of `claim()`'s three default installs, ~190 bytes
  for work that happens at most once per frame. Size is what is scarce in this file;
  the rest of the cardengine is untouched at `-O2`.
- The reader exposes `raSnapshotBuffer` directly instead of behind an accessor. In a
  module with a few hundred bytes of headroom, a function that only returns `&buffer`
  is not worth its own code.
- The linker scripts now assert the window is not overrun, so the next thing that
  does not fit fails the build.

The direction of travel is unchanged and is now better supported: **code that grows
does not belong in the cardengine.** Phase 1 fits. The overlay's real font does not,
and `rc_client` is not close. That is the `cardenginei_arm9_ra` binary described under
phase 0.5, and it is still the next structural piece of work.

### What to look for on hardware

Build, run `tools/ra_snapshot_addr.sh`, point the in-game menu's RAM viewer at the
address for your variant, and check:

- `RA1S` at `+0x00`, and `ticks` at `+0x04` climbing once per frame.
- `watchCount` = 3 and `resolved` = 3 at `+0x14`.
- Watch 1 at `+0x30` and watch 2 at `+0x48`: `address` equal to the snapshot address
  plus 4, and `value` equal to `ticks`. **This is the phase 1 result** — chains
  resolving through one and two indirections against live memory.
- Watch 0 at `+0x18`: `value` tracking `DISPCNT`, changing when a notification is up.
- `linesMax` at `+0x17`: the per-frame cost, in scanlines, and the answer to open
  question #2.

## Phase 0.5 — on-screen notification

Brought forward ahead of the rest of the reader work: an unlock nobody can see is
worth little, and the alternative to a visible channel was reading hex out of the
in-game menu's RAM viewer for every test.

**Confirmed on hardware.** Text draws over a running DS game on a 3DS, without
pausing it, and the game keeps running normally underneath. That was the part
genuinely in doubt.

### How it works

The overlay borrows a background layer and a slice of VRAM from the sub engine for
the few seconds a notification is up, then gives them back:

1. At show time — not at boot — read the live registers and pick a background layer
   the game currently has switched off.
2. Survey which 16K blocks of sub BG VRAM the enabled layers use, for both character
   bases and maps, and pick a free one. Tiles go at its start, the map 2K in, so a
   single block covers both.
3. Draw, set the borrowed layer to priority 0 so it sits above the game's, and
   enable it.
4. Every frame, re-survey. If the game starts using the block, give it back
   immediately.
5. On hide, restore the layer's control register, its scroll registers, the DISPCNT
   bit and the palette bank.

If no layer is free, or no block is free, it stays quiet. **A notification that
corrupts the game is worse than no notification.**

Counters — `shows`, `denied`, `evicted` — go into the snapshot, so how often the
overlay gets what it asks for is measured rather than inferred from glitches. On
*Space Invaders Extreme*: 4 shows, 0 denied, 1 evicted.

### Why it negotiates

Every version that assumed a resource was the overlay's by right corrupted the
game's graphics, twice for the same underlying reason:

- The block chosen at boot stopped being free. The game moved its BG2 character base
  onto it mid-play, and a repair loop that rewrote the tiles each frame was
  destroying the game's own tiles underneath.
- BG0 was treated as the overlay's layer. This game enables its own sub BG0 at
  times, with a character base of its own, so taking it displaced a layer in use.

A game's layer and VRAM allocation **changes while it runs**. Anything the overlay
wants has to be asked for at the moment it is needed and handed back on demand.

### What this means for the real notification

It is **opportunistic by nature**. On a game that keeps all four sub layers busy
there is nowhere to draw, and no amount of engineering changes that short of
pausing the game — which is worse than not showing text. Unlocks will need queuing
until a slot frees up rather than being dropped.

The current version is a feasibility proof, not the finished notification: one fixed
message, glyphs stored in message order so there is no font and no lookup table.
That is what let it fit in the cardengine at all, and it is why the real one belongs
in the separate ARM9 binary described below.

### Three hardware lessons, each of which cost a flash cycle

- **DS VRAM ignores 8-bit writes.** The tiles were built a byte at a time and simply
  never got written, leaving every pixel at index 0 — transparent. A fully correct,
  fully configured layer drew nothing. Registers, map and palette all worked because
  they happened to use halfword writes.
- **The cardengine's `.bss` is never zeroed.** The bootloader copies only the loaded
  image and an injected binary has no crt0, so a `static bool` guard starts as
  whatever was in RAM. Guard state with a magic value.
- **Display resource ownership is dynamic**, as above.

### Where the overlay's code lives

Not in the cardengine: a font plus layout will not fit in ~840 bytes, and fitting
even this proof meant stripping the measurement scaffolding out.

Not in the in-game menu either — `loadInGameMenu()` backs the game's RAM up to a page
file and loads the menu *over* it, which is why the menu pauses the game, so its font
and `print()` are unreachable while a game runs.

The precedent that works is the colour LUT: a **separate ARM9 binary**, loaded by the
bootloader to its own address and called from the cardengine by function pointer.

```c
volatile void (*code)(bool) = (volatile void*)CARDENGINEI_ARM9_CLUT_LOCATION;
(*code)(processExtPalettes);
```

A `cardenginei_arm9_ra` binary following that pattern is where the overlay belongs —
and it is also the answer to the phase 2 blocker. `rc_client` did not fit in the
cardengine's 12K, which sent this work hunting for spare RAM; the answer was never to
find a block of RAM but to stop putting code in the cardengine. One mechanism covers
both.

### The RAM above the ROM cache (closed, unresolved)

Chased at length and worth recording so it is not chased again. On a 3DS the ROM
cache ends at `0x0DFCC000`, leaving 208K to the 32MB top. That memory is real and
distinct — a DMA write there read back intact and left the candidate mirror 16MB
lower untouched — but a **CPU store to it takes a Data Abort**, and none of the
MPU state explains why: region 3 spans `0x08000000` +128MB, covering both the
cache where stores work and the fault site, with data permission `0x1`
(privileged read/write). Cause undetermined.

It no longer matters. The separate-binary approach puts code and state in the
`0x02xxxxxx` space instead, which is required anyway: region 3's *instruction*
permission is `0x0`, so code could never have executed from `0x0C`/`0x0D`.

## Status

- [x] Baseline: unmodified nds-bootstrap builds
- [x] Phase 0: per-frame game RAM snapshot — **confirmed on hardware**
- [x] Phase 0.5: text notification over a running game — **confirmed on hardware**
- [x] Phase 1: parameterised watchlist + pointer chains — host-tested, **hardware
      confirmation outstanding**
- [ ] Next: `cardenginei_arm9_ra`, a separate ARM9 binary for RA code, following the
      colour LUT's pattern. Unblocks both a real font for the overlay and phase 2.
- [ ] Phase 2: `rcheevos` / `rc_client` with mocked network
- [ ] Phase 3: real network, softcore unlocks
- [ ] Phase 4: rich presence, achievement list, login status
