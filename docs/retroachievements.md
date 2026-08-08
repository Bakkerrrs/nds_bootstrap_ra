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
| `ra_reader` | Read the game's RAM. Knows nothing about RetroAchievements. The watchlist lives in `cardenginei_arm9_ra`; the cardengine keeps the per-frame bridge and the snapshot. | phase 1 done, confirmed on hardware |
| `ra_overlay` | Show a notification over the running game. Knows nothing about RetroAchievements either. | proven, needs a real font |
| `ra_client` | Wrap `rcheevos`' `rc_client`; decide what to watch, evaluate, fire unlocks. | not started |
| `ra_net` | HTTP(S) transport to the RA servers. `rcheevos` ships no networking. | not started |

## Where this stands, and what to do next

Written as a handoff. Everything below the "Layering" table is background; this section
is what you need to pick the work back up.

### State

Phase 1 is done and confirmed on hardware across three sessions and two games. The
reader evaluates a watchlist with pointer chains, re-resolved from scratch every frame,
and every value predicted in advance has matched what the hardware showed.

`cardenginei_arm9_ra` — the separate ARM9 binary in DSi WRAM — is **built, loaded, called,
and confirmed on hardware**, with a working `malloc` over its arena. See
*`cardenginei_arm9_ra` — running on hardware* below for the readings.

**`rcheevos` is in, and it evaluates a real achievement definition** — on the host, where
the test now runs the achievement out to its target and watches the trigger fire. Four
hardware readings went into getting the ground under it solid, and each one closed a
question:

1. `ra_startup()` reported a dead heap as a working one on every frame after the first — a
   bug of mine, and the reason the next three readings were built to be unambiguous.
2. **newlib's `malloc` does not work in this window.** `_sbrk()` returned the predicted base
   address; `malloc` refused anyway, without ever calling it successfully.
3. **Our allocator does.** 2,728 bytes of real rcheevos allocations, `mallocProbe` exactly on
   `heapBase + 8`.
4. **The snapshot has no RetroAchievements console address.** Main RAM here is 16 MB and the
   map covers 4 MB, so the self-test moved to console address 0.

5. **It works.** `rcTriggered = 1` — an achievement unlocked on a 3DS running a DS game,
   with `rcLinesMax` at 1 scanline out of 263.

So the core of phase 2 is done and confirmed on hardware. What is missing now is the
*client*: the piece that knows which achievements a game has. That makes open question #1,
the network transport, the critical path.

The ARM9 cardengine has **436 bytes** left. It had 28 before the watchlist moved out, which
is the constraint behind almost every decision in this document.

### Phase 2's core question is answered: it works on hardware

Fifth reading, and every number predicted in advance matched:

| Field | Read | Meaning |
|---|---|---|
| `rcStage` | `06` | `RA_RC_FRAME` |
| `rcActivate` | `00` | `RC_OK` |
| `rcTriggerState` | `05` | `RC_TRIGGER_STATE_TRIGGERED` |
| **`rcTriggered`** | **`1`** | **an achievement unlocked on a 3DS running a DS game** |
| `rcPeeks` | `1` | one address per frame, exactly what the definition reads |
| `rcPeeksRejected` | `0` | nothing was refused |
| `rcInitLines` | `6` | the one-time parse, in scanlines |
| `rcLines` / `rcLinesMax` | `0` / `1` | the per-frame cost, out of 263 |
| `heapSize` | `0x2F048` | 192,584 — predicted to the byte |
| `heapUsed` | `2,728` | what rcheevos actually took |
| `mallocProbe` | `0x03750FC0` | `heapBase + 8`, the block header |

**The per-frame cost is the number that matters most, and it is negligible**: `rcLines` 0,
`rcLinesMax` 1 out of 263 scanlines. Activating an achievement costs 6 scanlines, once.
That answers open question #2 for rcheevos specifically — evaluating a definition every
frame inside a DS game's VCOUNT handler is affordable.

`rcEvents` read 255, which is the clamp. Those are `PROGRESS_UPDATED` events, one per frame
for 600 frames, and not a sign of anything wrong.

`rcMeasured` and `rcTarget` read `0`, which was expected once understood but is worth
recording: rcheevos reports measured progress only while a trigger is **active**, and
`TRIGGERED` is not active. They are latched now, so a reading taken after the unlock shows
the last active value — 599 of 600. One short of the target, because on the frame the count
reaches it the trigger fires and rcheevos has already stopped reporting. That is the honest
reading rather than an off-by-one.

### The next task: a real achievement, from a real game

Everything below the client is now proven on hardware. What is missing is the client: the
piece that knows *which* achievements a game has and what addresses they watch. That means
open question #1 — network transport — becomes the critical path rather than a side
question, because a definition has to come from somewhere.

The nearest useful step that needs no network: take a published DS achievement set, hard-code
one real definition, and confirm it unlocks by playing. That exercises the delta memref and
pointer-chain (`AddAddress`) paths the self-test deliberately does not, and it is the last
thing that can be checked before the transport decision has to be made.

### How you know it worked

Run `tools/ra_snapshot_addr.sh` for the snapshot address, point the RAM viewer at it, and
work down the chain. Each field names its own link, so a failure is located rather than
inferred:

| Offset | Field | Wanted | If not |
|---|---|---|---|
| `+0x1C` | `wramMagic` | `RAH1` (`52414831`) | the binary is not executing |
| `+0x20` | `wramTicks` | climbing | `.bss` in the window does not persist |
| `+0x24` | `wramState` | `02` | `00` the bootloader never set the flag; `01` the flag was set but the window holds no code, so the copy did not land |
| `+0x68` | `wramStage` | `04` | `00`–`03` names where startup stopped — see `RA_STAGE_*` |
| `+0x6C` | `rcStage` | `06` | see `RA_RC_*`, and `rcActivate` at `+0x6D` for a parse error |

The offsets move whenever the struct changes, which is what the pinned
`__builtin_offsetof` checks in `tools/ra_reader_test.c` are for: reorder `raSnapshot` and
the test fails rather than this table going quietly stale.

### Things that will cost you time if you do not know them

- **Build from the top level.** `make package-nightly`. Building a cardengine
  subdirectory directly needs `make CPP=arm-none-eabi-cpp`, because only
  `retail/Makefile` exports `CPP` — see the Building section.
- **`git clean -xfd` deletes untracked directories.** It ate
  `retail/cardenginei/arm9_ra/` once, mid-session. `git add` a new binary's directory
  before cleaning.
- **`rcheevos` is a git submodule.** A fresh clone needs
  `git submodule update --init --recursive`, or `retail/cardenginei/arm9_ra/rcheevos` is
  empty and the build fails on a missing header. `tools/ra_reader_test.sh` checks for this
  and says so rather than producing a wall of compiler errors.
- **`tools/ra_reader_test.sh`** runs the reader's logic *and rcheevos* on the host in
  seconds, with no devkitARM and no hardware. Use it before every flash cycle; it has
  already caught a wrong assumption that would have cost one.
- **The `arm9_ra` Makefile fails the build if the image exceeds
  `CARDENGINEI_ARM9_RA_IMAGE_MAX`.** That check exists because the loader copies a fixed
  length, so an oversized image would be copied *incomplete* — booting correctly, then
  failing inside code that is not there, with nothing at run time able to detect it. The
  build prints the budget on every success: `built ... (68024 of 131072 bytes, 63048
  spare)`.
- **`tools/ra_snapshot_addr.sh`** prints the snapshot address *and* the remaining space
  per variant. The address moves whenever the code around it changes, so re-run it after
  every build rather than reusing the last one.
- **The linker scripts assert `__bss_end <= __vram_top`.** If a build fails with
  "cardengine .bss overruns its window", that is the 28 bytes running out, not a mistake.

### Still open, and one of them is now the critical path

- **Open question #1, the network transport** — this is no longer "not on the critical
  path", it *is* the critical path, and desk research has narrowed it to one unknown: is the
  DSi's Atheros WiFi chip reachable from nds-bootstrap's ARM7 while a DS-mode game runs?
  Everything around it is settled — DS mode is WEP-or-open in hardware, WPA in a DS title is
  an open unassigned nds-bootstrap issue, and the deferred fallback turns out to be a
  supported RetroAchievements flow rather than a workaround. See the open questions
  section.
- **CI has never run on this repository.** The workflow exists and the build is verified
  to pass on the pinned toolchain, but Actions appears disabled for the fork, so the host
  test and the space-budget report added to it have never executed. Enabling it is a repo
  setting.

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

Three things that are easy to trip over:

- `lzss` is a **host** tool built from `lzss.c` in the repo root. Without it every
  `.lz77` target fails with `Error 127`.
- Build serially. `make -j` races: sub-makes link before their dependencies exist
  (`cannot find arm9mpu_reset.o`, `cannot find my_fat.o`).
- **Build from the top level**, not from a cardengine subdirectory. Each of the 40
  cardengine Makefiles generates its linker script with `$(CPP) -P $(INCLUDE) $< $@`,
  which needs `CPP` to be a preprocessor driver that accepts an output filename.
  `retail/Makefile` and `hb/Makefile` export `CPP := arm-none-eabi-cpp` for exactly
  that reason. devkitARM's own rules do not set it, so running
  `make -C retail/cardenginei/arm9` directly falls back to GNU make's default of
  `$(CC) -E` — and `gcc -E in out` treats `out` as a second *input*, failing with
  `linker input file not found: cardengine.ld`. Pass `CPP=arm-none-eabi-cpp` when
  building one variant in isolation, which is worth doing to read its `.map`.

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
  GSDD`, so `cardenginei_arm9_gsdd*` could link the reader and never install the
  handler. Since phase 1 they do not link it either: `RA_READER_ENABLED` defaults to 0
  for `GSDD`, and for `DLDI`, which has no room for it. See open question #4.
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

The cycle budget is **measured, and it is not the constraint**. `ra_reader_tick()`
samples `VCOUNT` on entry and again on exit and records the difference in `linesLast` /
`linesMax`. On hardware `linesLast` was 0 in every sample across two games, so a typical
tick costs under one scanline of 262. `linesMax` was 1 on one game and 11 on a busier
one, which is a ceiling on the *window* rather than on the reader: the work per tick is
fixed at a few hundred cycles and 11 scanlines is ~17,000, so the difference is the
machine being busy, not the reader. See the phase 1 measurements for the detail and for
what it would take to separate the two. Scanlines are a coarse
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

Phase 1 corrected this table in two ways, both worth recording, because the numbers
above are wrong.

**840 bytes was measured against the wrong symbol.** `__sp_usr` is where the user-mode
stack would start in a normal NDS program. Nothing in the cardengine installs it —
there is no reference to `__sp_usr`, `__sp_svc` or `__sp_irq` anywhere in
`retail/cardenginei/`. Injected code runs on the *game's* stack; those symbols are
inherited from the stock linker script this one was derived from and are vestigial.

The real ceiling is `__vram_top`, the end of the linker `MEMORY` region, and it sits
0x260 bytes higher. So the free space at phase 0 was not 840 bytes, it was **1,448** —
and by the time the overlay had been added it was 544, which is the figure phase 1 was
actually working against. `tools/ra_snapshot_addr.sh` now reports
`__vram_top - __bss_end`, and CI prints it on every build so the margin is visible
while it is still a margin.

**`cardenginei_arm9` is not one of eight, it is one of three.** Eight variants compiled
the reader; only three could ever run it, and the other five were paying for it in a
window they could not spare:

- The four `*_dldi` variants are nds-bootstrap running from a flashcard in DS mode,
  which the fork does not target — and they are the tightest of the eight. After phase
  1 `arm9_twlsdk_dldi` and `arm9_twlsdk3_dldi` would have had **176 bytes** left and
  `arm9_dldi` 208, against a reader costing about 550. They could not have carried it.
- The `GSDD` variants never tick at all: `hookIPC_SYNC()` is compiled out under
  `#ifndef GSDD`, so they linked the reader and never installed the handler.

`RA_READER_ENABLED` now defaults to 0 for both groups, which is why the space report
lists three variants rather than eight. Overridable on the command line.

**What is left.** `cardenginei_arm9` — the variant a plain retail DS game on a DSi or
3DS actually loads, and the one this fork tests on — has **44 bytes** free after phase
1. That is a real margin and not a negative one, but it is 44 bytes: the linker scripts
now assert `__bss_end <= __vram_top` so the next thing that does not fit fails the
build with a message saying what to do, and in practice the answer will not be to trim.
The two `twlsdk` variants have ~7,000 bytes and are not the constraint.

So the conclusion this section reached at phase 0 is not merely still true, it is now
quantified: **nothing else goes in the cardengine.** Not `rc_client`, not a font, not
the next 200 bytes of anything.

### #3 — pointer chains: **done**, see phase 1 below.

The reader walks a chain from scratch on every tick and validates every address in the
moment it is about to be used. Caching a resolved address was never an option: that is
the exact thing a pointer chain exists to avoid.

### #1 — network transport

Now the critical path, and desk research has narrowed it to a single unknown.

**What is settled: DS mode is WEP or open, full stop.** This is a hardware split, not a
software one. The DSi and 3DS carry two WiFi paths — the legacy Mitsumi-compatible core the
original DS used, which does WEP in hardware, and an Atheros AR6002/AR6013 that does
WPA/WPA2. The Atheros is a DSi-mode device. Nintendo's own documentation says DS
applications get WEP and Open only, and DSi-Enhanced titles running in DSi mode get WPA and
WPA2 as well; the DS-Homebrew wiki says the same, and nds-bootstrap inherits it — NTR games
need a WEP or open access point.

That matters more than it first looks. **WEP is the thing routers stopped offering.** An
architecture that requires the user to stand up a WEP network in order to earn achievements
is not one to build on if there is an alternative.

**What is settled: nobody has done WPA in a DS title.** `DS-Homebrew/nds-bootstrap` issue
#628, *DSi enhanced WIFI (WPA support) in DS title*, is open, unassigned, has no branch and
no pull request, and sits on the 2.0 milestone. Its proposed approach is patching the game's
WiFi functions using nocash's `wifiboot` as a base. So this is a known wish, not a solved
problem, and not something to build a phase on.

**What is settled: the driver exists, for DSi mode.** `shinyquagsire23/dsiwifi` is an
AR6012/AR6013 LwIP driver for DSi and 3DS, MIT licensed, and libnds gained DSi WiFi support
in 2.0 — which this fork cannot use, being pinned to 1.8.0 for the reasons in *Building*.
So there is prior art for talking to the Atheros chip, just not from inside a DS-mode game.

**The one open question.** All of the above is about *the game's* networking, reached through
the game's own code. Ours is different: we do not need to patch the game's WiFi functions, we
need our own client running alongside it. nds-bootstrap already keeps SCFG unlocked — that is
the whole reason this project has 256 KB of DSi WRAM to run in — so the question is whether
the Atheros chip is reachable from nds-bootstrap's ARM7 while an NTR game runs, and whether
there is room and CPU time there for a stack. **Nothing found online answers that**, and it
is the one thing genuinely worth asking.

Everything follows from it. If the answer is yes, phase 3 is live server contact. If it is
no, the fallback is deferred sync — and that fallback is in much better shape than it
looked, see below.

### #1a — the fallback is supported, not a hack

RetroAchievements documents a **standalone integration** path for non-emulator clients,
which is exactly what this is, and two details in it de-risk the deferred design:

- **There is a bulk unlock endpoint.** The Connect API can "unlock multiple achievements at
  once or resync all the user's unlocks in your system to RetroAchievements". That is
  precisely the primitive a queue-on-console, submit-later design needs — it is not
  something we would be smuggling past the API.
- **Softcore is a first-class parameter**, `h=0`, not a degraded mode.

Also worth recording: `rcheevos` ships no networking at all — clients fetch from RA and hand
the response down — so the transport is ours to write whichever way this goes. A user agent
header is mandatory on every Connect call, and game pages for standalones are set up by the
admin team on request.

What desk research did *not* settle on the RA side: whether plain HTTP still works against
`dorequest.php` or whether HTTPS is now mandatory. Every documented example uses HTTPS and
the docs never say plainly. This matters because a TLS stack is not going to fit here, so if
HTTPS is required, live contact is off the table even if the WiFi question comes back yes —
and the deferred path becomes the only one.

## Phase 1 — the watchlist and pointer chains

**Confirmed on hardware**, on a 3DS running *Space Invaders Extreme* through
`cardenginei_arm9`, and host-tested besides. The measured results are at the end of
this section.

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

Measured on two games: `linesLast` was **0** in every sample taken, so the typical tick
costs less than one scanline of 262. `linesMax` was **1** on *Space Invaders Extreme*
over 7,433 frames but **11** on a second, busier game over 9,247.

That spread is worth being careful about, because it is almost certainly not the
reader's own cost. The work per tick is fixed — three watches, two chain walks, about
thirty memory accesses, on the order of hundreds of cycles. Eleven scanlines is roughly
17,000. What the measurement actually captures is *elapsed time across the window*, and
on a DS that includes whatever else had the machine: DMA can halt the CPU, another
interrupt can land inside the window, main RAM has wait states under contention.

So `linesMax` is a ceiling on the window rather than a cost, and the honest form of the
answer to open question #2 is: the reader does not eat the frame, its typical tick is
under a scanline, and its worst observed window is 4% of a frame on a game that was
busy for reasons of its own. Separating the two would need a control measurement — an
empty window timed next to the real one, and subtracted. That costs about 20 bytes of
the 44 left, so it waits for the separate binary.

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

About 500 bytes of the cardengine window over phase 0, at `RA_WATCH_MAX` 4 and
`RA_CHAIN_MAX` 2 — 564 bytes of code and 128 of `.bss` on devkitARM r65, against 136
and 44 before. `cardenginei_arm9` had 544 bytes free and now has 44. That is most of
what was left, which is why:

- Both limits are knobs in `ra.h`, with the cost of a slot documented next to them.
- `ra_reader.c` alone is built `-Os`, with `noinline` on the two functions the `-O2`
  inliner duplicates — `ra_watch_eval()` into the tick loop and
  `ra_reader_watch_add()` into each of `claim()`'s three default installs, ~190 bytes
  for work that happens at most once per frame. Size is what is scarce in this file;
  the rest of the cardengine is untouched at `-O2`.
- The reader exposes `raSnapshotBuffer` directly instead of behind an accessor. In a
  module with tens of bytes of headroom, a function that only returns `&buffer` is not
  worth its own code, and there is nothing an accessor could add.
- There is no `ra_reader_watch_clear()`, though it is the obvious counterpart to
  `_add`. It has no caller until phase 2 and costs 44 bytes — as large as the entire
  remaining margin.
- It is not built at all for the `DLDI` and `GSDD` variants, which respectively cannot
  afford it and can never run it. See open question #4.
- The linker scripts now assert the window is not overrun, so the next thing that
  does not fit fails the build with a message rather than an address.

The direction of travel is unchanged and is now better supported: **code that grows
does not belong in the cardengine.** Phase 1 fits. The overlay's real font does not,
and `rc_client` is not close. That is the `cardenginei_arm9_ra` binary described under
phase 0.5, and it is still the next structural piece of work.

### Measured on hardware

Two games, both on a 3DS through `cardenginei_arm9`, snapshot at `0x027FEEE0`.

#### *Space Invaders Extreme*, after 7,433 frames (about two minutes of play)

| Field | Value | What it says |
| --- | --- | --- |
| `magic` | `RA1S` | the phase 1 buffer, at the address the tool reported |
| `ticks` | 7,433 | the VCOUNT handler is still firing every frame |
| `watchCount` / `resolved` | 3 / 3 | every default watch resolved on the frame that was sampled |
| `linesLast` / `linesMax` | 0 / 1 | the per-frame cost, under one scanline of 262 |
| `shows` / `denied` / `evicted` | 9 / 0 / 0 | the overlay got a layer and a block all nine times it asked |

And the watches themselves:

| Watch | `base` | `address` | `value` | `depth` / `size` / `status` |
| --- | --- | --- | --- | --- |
| 0, direct | `0x04001000` | `0x04001000` | `0x1110` | 0 / 2 / OK |
| 1, one indirection | `0x027FEF58` | `0x027FEEE4` | 7,433 | 1 / 4 / OK |
| 2, two indirections | `0x027FEF5C` | `0x027FEEE4` | 7,433 | 2 / 4 / OK |

Watches 1 and 2 both resolved to `0x027FEEE4` — the snapshot address plus 4, which is
`ticks` — and both read 7,433, the live value of that field. **That is phase 1 working:
a chain walked through one indirection and a chain walked through two, re-resolved from
scratch that frame, reading memory that was changing underneath them.** The self-test
cells read back as `0x027FEEE0` and `0x027FEF58`, exactly the addresses the two chains
started from.

Two things worth noting about the numbers. Watch 0 read `0x1110` from the sub engine's
`DISPCNT`, but the in-game menu is drawn on that screen, so while the RAM viewer is
open that register describes the *menu*, not the game — the watch is doing its job
either way. And the overlay's 9 shows with 0 evictions is better than phase 0.5's 4 and
1 on the same game, which is what more play time in one session looks like rather than
a change in behaviour.

#### *Final Fantasy III*, sampled twice in one session

| Field | at 6,403 frames | at 9,247 frames |
| --- | --- | --- |
| `watchCount` / `resolved` | 3 / 3 | 3 / 3 |
| watch 1, one indirection | `0x027FEEE4` → 6,403 | `0x027FEEE4` → 9,247 |
| watch 2, two indirections | `0x027FEEE4` → 6,403 | `0x027FEEE4` → 9,247 |
| `shows` / `denied` / `evicted` | 5 / 4 / 0 | 9 / 4 / 0 |
| `linesLast` / `linesMax` | 0 / 10 | 0 / 11 |

The walker behaves identically on a second title, which is the point of running one:
both chains resolved to `ticks` and read its live value, twice, 2,844 frames apart. The
game itself ran well throughout.

The new information is `denied` = **4**. This is the first time on hardware that the
overlay has asked for a layer and a block and been refused — 4 refusals against 13
attempts. It confirms the prediction phase 0.5 made from first principles: the overlay
is *opportunistic by nature*, a game that keeps its sub layers busy leaves nowhere to
draw, and unlocks will have to queue until a slot frees rather than being dropped. That
is no longer a design argument, it is a measurement. `evicted` stayed 0, so when it did
get a block it never had to hand it back mid-notification.

#### The two-watch layout, after the bridge

A third confirmation, on the build where `RA_WATCH_MAX` had dropped to 2 to pay for the
bridge into `cardenginei_arm9_ra`. Every value predicted in advance matched what the
hardware showed, field for field: the magic, `watchCount` and `resolved` at 2, watch 0's
base and address at `0x04001000`, watch 1's base at `&raSelfCellPtr`, its offsets at 0 and
4, its resolved address at `S+4`, its value equal to `ticks` (0x6AE, 1710), the status
bytes, and both self-test cells.

Two things it established beyond the layout. Losing the depth-1 self-test did not cost the
walker anything — the two-step chain still resolves against live memory. And `wramState`
read `00`, not `01` or `02`, so the bridge correctly recognised that the separate binary
was not loaded and did not jump into an empty WRAM window. That was the point of the
reading: not proving something new works, but proving the bridge is inert before writing
the loader that will make it live.

Also `linesMax` = **1** on this run, where *Final Fantasy III* showed 11. Consistent with
the reading that 11 was contention from a busy machine rather than the reader's own cost,
though 28 seconds of session makes it an indication rather than evidence.

### Four things the field report taught us that the counters could not

Playing a real game for real found things no snapshot field was going to.

**The notification does not have a screen.** It appeared on the top screen in the field
and on the bottom in battles. That is not a bug and not a choice — the overlay draws on
the *sub* engine, and which physical panel the sub engine feeds is bit 15 of `POWCNT1`,
the display-swap bit, which belongs to the game. *Final Fantasy III* flips it by
context. Nothing in `ra_overlay.c` reads `POWCNT1`, so the overlay has no idea where its
own text is coming out.

For a feasibility proof that does not matter. For a real unlock notification it is a
decision to make deliberately: either read the swap bit and accept whichever screen the
sub engine is on, or be prepared to borrow from the main engine too so the notification
can be put somewhere predictable.

**The denials line up with the fades.** The notification went missing while moving
between maps, which is where the game fades the screen to black — plausibly by enabling
every sub background layer to do it, which would leave `chooseLayer()` nothing to take.
`denied` could not confirm that because it counted both failure modes as one, so it is
now split: `deniedNoLayer` counts the times no layer was switched off, and
`denied - deniedNoLayer` the times no VRAM block was spare. The next session can stop
guessing which one is happening.

**A palette bug the counters were blind to.** A graphical fault appeared on the title
screen at the moment the notification came up. `draw()` was saving and whitening all
sixteen entries of palette bank 15, but the glyphs are drawn entirely in colour index 1
— it ORs a nibble of 1 for every set pixel — so fifteen of those entries were being
overwritten for nothing. Any game using them saw them go white for the three seconds a
notification was up, and come back on hide: exactly a transient fault tied to the
toaster appearing.

Fixed to borrow the single entry the design actually needs, which also freed 30 bytes of
`.bss` — more than the new counter above cost, so the margin on `cardenginei_arm9` went
from 44 bytes to 104. One entry is still one the game may be using, since the text has
to be *some* colour, but that is the floor rather than fifteen times it.

### What the split counter answered, next session

Two more samples from *Final Fantasy III* with the palette fix and the split denial
counter in, snapshot at `0x027FEEA0`:

| Field | at 4,411 frames | at 7,782 frames |
| --- | --- | --- |
| `shows` | 5 | 7 |
| `denied` / `deniedNoLayer` | 1 / **1** | 4 / **4** |
| `evicted` | 3 | 3 |
| `linesLast` / `linesMax` | 11 / 11 | 0 / 11 |
| watch 1 / watch 2 | both `S+4` → 4,411 | both `S+4` → 7,782 |

**The fade hypothesis is confirmed, and completely.** `deniedNoLayer` equals `denied` in
both samples — every single denial was "no background layer was switched off", and VRAM
was never once the reason. That also clears the `surveyBlocks()` hole below of any
involvement in the denials. The fix is the unlock queue and nothing else; block
management does not need touching for this.

**The eviction path works, and this is the first time it has run.** `evicted` was 0
through every earlier session, so the code phase 0.5 wrote from a hardware lesson had
never actually executed. Here it fired three times: the game reclaimed the borrowed block
mid-notification, the overlay handed it back, and the reported graphical faults went
*down* rather than up.

**And a caveat about the cost measurement that these samples expose.** `linesLast` was 11
in the first sample — that tick really did take 11 scanlines — and the reading was taken
with the in-game menu open. The menu runs on the ARM9 and does substantial work, so it
is contending with the very thing being measured. Since `linesMax` is a running maximum,
menu frames feed into it too, and the only way to read it is to open the menu. It is
therefore entirely possible that the 11 is a menu artefact and the cost during actual
gameplay is nearer 0–1 lines. Nothing in the current instrumentation can separate them:
the observation perturbs the observed.

**A known interaction, deliberately not fixed.** The one graphical fault that remained
after the palette fix appeared at the moment of pressing X to open the in-game menu. That
is the menu taking over both screens; if the overlay holds a borrowed layer at that
instant, both are writing sub engine registers at once. It is an interaction by
construction rather than a new bug, and no achievement is going to unlock on the exact
frame the menu opens, so it stands as accepted. The clean answer, when the overlay is
rewritten, is for it to stand down while the menu is up.

### The RAM viewer will crash on a mistyped address — fixed

Not a bug in this fork's code, but a bug in the tool this fork's entire debug workflow
depends on, so it is fixed here.

The in-game menu's RAM viewer has no value search. What it has is a jump-to-address
screen where the address is edited one hex digit at a time, and then:

```c
u8 *ramPtr = arm7Ram ? arm7RamBuffer : (u8*)address;
```

Dereferenced with no bounds check anywhere in the file. Typing a value where an address
belongs — `52413153`, the snapshot magic, instead of `027FEF10`, where it lives — points
it at unmapped memory and takes a Data Abort straight to the red exception screen.

What makes it worse than one crash is the declaration:

```c
// For RAM viewer, global so it's persistant
vu32 *address = (vu32*)0x02000000;
```

Persistent by design, so the viewer reopens where you left it. Once poisoned, it faults
again on every re-entry before any keypress can correct it, and the only way out is
rebooting the game.

`clampAddress()` now runs before every read and on leaving the jump screen. An address
whose whole visible span is not inside a real region snaps back to `0x02000000`, which
also un-poisons the global. The range list is deliberately generous — main RAM, shared
and DSi WRAM, I/O, palette, VRAM, OAM, the GBA slot, and the extended RAM above
`0x0C000000` — because the point is to catch a typo, not to police where anyone looks.

It costs 176 bytes in a binary with 11.4 KB spare, so unlike everything else in this
document it was not a trade.

**A hole this exposed that is not fixed.** `surveyBlocks()` reads every enabled layer's
`BGCNT` as though it were a text background: character base in 16K units, screen base in
2K units, map size from bits 14-15. It never looks at the BG mode in `DISPCNT` or at the
colour-depth bit. For an affine or bitmap background those fields mean different things
— a bitmap's base is in 16K units, not 2K — so the survey can both miss blocks the game
is using and mark ones it is not. A title screen is a likely place for a bitmap
background. This was not the cause of the fault above, but it is a real way for the
overlay to pick a block that is in use, and it belongs with the overlay rewrite in the
separate ARM9 binary rather than with a patch here.

### What to look for on hardware

Build, run `tools/ra_snapshot_addr.sh` — it now lists three variants, not eight, and
for a plain retail DS game on a DSi or 3DS the one you want is `cardenginei_arm9` —
then point the in-game menu's RAM viewer at that address.

The layout, with the snapshot at `S`:

| Offset | Field | Expected |
| --- | --- | --- |
| `+0x00` | `magic` | `52 41 32 53` — `RA2S`. The digit is the layout version; an older build reads `RA1S` |
| `+0x04` | `ticks` | the cardengine's frame counter, climbing |
| `+0x08` / `+0x0C` / `+0x10` | `shows` / `denied` / `evicted` | the overlay's negotiation |
| `+0x14` | `deniedNoLayer` | of the denials, how many found no free layer |
| `+0x18` / `+0x19` | `watchCount` / `resolved` | `02` / `02` |
| `+0x1A` / `+0x1B` | `linesLast` / `linesMax` | the per-frame cost in scanlines |
| `+0x1C` | `wramMagic` | `52 41 48 31` — `RAH1` |
| `+0x20` | `wramTicks` | **the WRAM binary's own counter, kept in its own .bss** |
| `+0x24` | `wramState` | `02` — called |
| `+0x28` | `selfCell` | = `S` |
| `+0x2C` | `selfCellPtr` | = `S+0x28` |
| `+0x30` | `results[0]` | the direct watch |
| `+0x3C` | `results[1]` | the two-step chain |
| `+0x48`, `+0x54` | `results[2]`, `results[3]` | unused, zero |
| `+0x60` | `heapSize` | `78 B5 03 00` — 243,064, the arena in the WRAM window |
| `+0x64` | `heapUsed` | `04 00 00 00` — 4 bytes, see below; not the few KB you would expect |
| `+0x68` | `wramStage` | `04` — `RA_STAGE_WATCHES`, everything up |

Each result is 0x0C bytes: `address` at +0x00, `value` at +0x04, then `depth`, `size`,
`status`. So:

- **`results[0]`** (`S+0x30`), the direct read: `address` `0x04001000`, `depth` `00`,
  `size` `02`, `status` `02`. `value` tracks the sub engine's `DISPCNT`.
- **`results[1]`** (`S+0x3C`), two indirections: `address` = **`S+4`**, `value` = `ticks`,
  `depth` `02`, `size` `04`, `status` `02`.

`wramTicks` equalling `ticks` is the thing to check first now. That counter lives in the
WRAM binary's own `.bss` and is copied here each frame rather than incremented here, so
the two staying level is what proves state persists in that window between frames.
Everything built there from now on depends on it.

A `status` other than `02` says where a chain broke — `03` bad base, `04` bad pointer
mid-chain, `05` bad target, `06` misaligned. All three defaults resolve against memory
the reader owns, so anything else here is a bug in the walker rather than a game doing
something unexpected.

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

**Confirmed at phase 1**, and then pinned down: on *Final Fantasy III* the overlay was
refused across map transitions where the game fades the screen, and once the denial
counter was split, **every refusal was a "no free layer" one** — `deniedNoLayer` equalled
`denied` in both samples. VRAM was never the constraint. The queue is not a precaution,
it is required, and it is the only thing required.

The current version is a feasibility proof, not the finished notification: one fixed
message, glyphs stored in message order so there is no font and no lookup table.
That is what let it fit in the cardengine at all, and it is why the real one belongs
in the separate ARM9 binary described below.

### Hardware lessons, each of which cost a flash cycle

- **Borrow the minimum, not the convenient amount.** The palette code saved and
  whitened all sixteen entries of a bank when the glyphs only ever use one, so fifteen
  were stomped for free. It took playing *Final Fantasy III* to see it, as a transient
  fault on the title screen. "Ask for what is needed at the moment it is needed" applies
  to how *much* is borrowed, not only to when.
- **Handing the block back actually happens.** `evicted` sat at 0 through every session
  until *Final Fantasy III*, where it reached 3 -- so the give-it-back path, written from
  a hardware lesson at phase 0.5, had never once run before. When it did, the reported
  faults went down. Code written from a correct lesson can still sit unexercised for a
  long time; the counter is what said when it finally ran.
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

## Supported hardware: the 3DS family, and nothing else

Decided deliberately rather than drifted into. `consoleModel > 0` — the 3DS family,
including the New 3DS — is the only configuration this fork supports for
RetroAchievements. On a DSi, and on a DS through a flashcard, it behaves exactly like
upstream nds-bootstrap: no reader, no overlay, and no `IRQ_VCOUNT` forced on.

### Why

Development and testing happen on a 3DS, and nothing else has ever been tried. Two of
the things this fork does are real behaviour changes to a running game — forcing a
VCOUNT interrupt on for games that never enabled one, and borrowing a background layer
and a palette entry from the sub engine. Both are the kind of thing that shows up as an
intermittent oddity rather than a crash, which is exactly what a console nobody is
testing on cannot be trusted to reveal. Shipping them to a DSi on the strength of "it
should work" is not a trade worth making.

It also removes work that would otherwise be speculative. The DSi WRAM the separate
binary needs is guaranteed present on a 3DS with SCFG unlocked, so no fallback has to
be designed for the case where it is not.

### How it is enforced

`consoleModel` is not detected — it comes from `CONSOLE_MODEL` in the configuration
file, so the launcher, which knows the console, supplies it. It was already being passed
through to the ARM9 cardengine as `ce9->consoleModel`, so no new plumbing was needed.

Two checks, both necessary:

- `hookIPC_SYNC()` in `misc.c` will not install the VCOUNT hook for the reader's sake
  unless `ce9->consoleModel > 0`. This is the one that matters, because installing it is
  what forces `IRQ_VCOUNT` on.
- `ra_tick()` checks again, because the colour LUT installs the *same* handler and does
  run on a DSi — so being called is not proof the reader was wanted.

The gate costs 48 bytes of the cardengine's margin, taking it from 104 to 56. That is
what it is worth to leave an untested console running stock.

### What it does not change

- **DSi-enhanced games on a 3DS come along for free.** They load `cardenginei_arm9_twlsdk`
  or `_twlsdk3`, which already carry the reader and have ~7,100 bytes spare rather than
  56. Nothing extra is needed for them.
- **All eight variants still have to compile.** The gate is a runtime check, not a build
  configuration, so nothing can be deleted from the tree. The `DLDI` and `GSDD` variants
  remain compiled out via `RA_READER_ENABLED` for the separate reasons given under open
  question #4.
- **The cardengine's 12 KB window is the same on every console**, so the space pressure
  this document keeps returning to is unaffected.

## Where `cardenginei_arm9_ra` goes, and whether `rcheevos` fits

Researched before writing any of it, because the placement is hard to undo once the
bootloader plumbing exists. Everything below is measured on the pinned toolchain
(devkitARM r65, thumb, `-Os`) or read out of the bootloader source, not estimated.

### Only one region can host it

Three candidates, and two are already eliminated:

- **`0x0C`/`0x0D` extended main RAM** — ruled out earlier and worth restating, because it
  is the obvious choice by size. MPU region 3's *instruction* permission is `0x0`, so code
  can never execute there. Data stores do work inside the ROM cache, which matters below.
- **`0x02xxxxxx` main RAM** — the game's own address space. Taking a few hundred KB from a
  running game is not a thing that can be done safely.
- **DSi WRAM, `0x03700000`–`0x03780000` (512 KB)** — the only region with a *working
  precedent*: `cardenginei_arm9_colorlut` executes from `0x03732800` today. Requires
  `dsiWramAccess && !dsiWramMirrored`.

So it is DSi WRAM, and the colour LUT is the pattern to copy.

### Is that region available on the target hardware?

Yes, for a retail DS game on a 3DS with SCFG unlocked. From `retail/arm9/source/conf_sd.cpp`:

- `dsiWramAccess` is `true` outright when `REG_SCFG_EXT7 != 0`. Otherwise it is probed by
  writing a magic word to `0x03700000` and reading it back.
- `dsiWramMirrored` is set when `0x03700000` and `0x03708000` read back the *same* magic,
  meaning only the shared 32 KB exists rather than the full 512 KB. That is the flashcard
  case, which the fork does not target anyway.

Switching ownership of the region between CPUs needs the same IPC handshake the colour LUT
does (`arm9_stateFlag = ARM9_WRAMONARM7`, wait for `ARM9_READY`, copy, hand back).

### What the region is already spent on

The whole 512 KB is claimed, and the earlier assumption that the colour LUT leaves a gap
was wrong — the apparent hole is its stored-palette buffers:

| Range | Size | Used by |
| --- | --- | --- |
| `0x03700000` + `wramSize` | up to 512 KB | nitro file info preload / ROM-in-RAM headroom |
| `0x03732800` | 4 KB | colour LUT code |
| `0x03733800`, `0x0374B800`, `0x0374C000`, `0x0375C000`, `0x03760000`, `0x03764000`, `0x0376C000` | — | colour LUT stored palettes |
| `0x03770000` | 64 KB | the colour LUT table itself |

`wramSize` is computed in one place and duplicated as a literal in two more:

```c
u32 wramSize = (dsiWramAccess && !dsiWramMirrored) ? (colorLutEnabled ? 0x32800 : 0x80000) : 0;
```

`0x03700000 + 0x32800` is exactly `0x03732800`, so with the colour LUT on the preload
budget stops precisely where the LUT code starts. The LUT costs `0x4D800` (317 KB).

### What reserving space actually costs

Not the ROM cache — that is a separate thing in `0x0C`/`0x0D`. `wramSize` feeds two
consumers:

1. **`isROMLoadableInRAM()`**, where it is *added* to `romSizeLimit`. On a 3DS with a
   retail non-DSi-mode game that limit is `0xBE0000 + 0x1000000 + wramSize`:

   | Reserved for RA | ROM-in-RAM limit |
   | --- | --- |
   | nothing | 28.375 MiB |
   | 256 KB | 28.125 MiB |
   | all 512 KB | 27.875 MiB |

2. **`loadNitroFileInfoIntoRAM()`**, which preloads the ROM's filename and file-allocation
   tables and simply `return`s — skipping the optimisation, not failing — when they exceed
   the budget.

Both costs are **cliffs, not gradients**: a ROM either fits entirely in RAM or does not.
The band that changes hands is ~0.5 MiB out of ~28, and standard cart sizes cluster at
powers of two, so no common size sits inside it. The exposure is titles whose *trimmed*
size lands in that half-megabyte, which will be few but is not nothing.

### Does `rcheevos` fit? Measured, and comfortably

Cloned upstream and compiled for `armv5te` thumb `-Os`. 32 of 34 translation units build
unmodified; the two that do not are `rc_libretro.c` and `rc_validate.c`, neither of which
is needed at runtime.

Unlinked, by module:

| Module | text | rodata | total |
| --- | --- | --- | --- |
| `rcheevos` runtime — parse and evaluate conditions | 25,756 | 10,081 | **35 KB** |
| `rc_client` | 22,434 | 4,068 | 26 KB |
| `rapi` — request building, JSON | 18,248 | 6,364 | 24 KB |
| `rhash` — game identification | 26,188 | 8,639 | 34 KB |
| compat / util / version | 480 | 1,130 | 2 KB |
| **all of it** | 93,106 | 30,282 | **121 KB** |

Then linked for real, with `--gc-sections`, for the shape phase 2 needs — activate an
achievement from a definition string and evaluate it once per frame:

**48 KB** of `.text` + `.rodata` + `.data`, and **492 bytes** of `.bss`, *including*
everything newlib contributes to that path.

> **The real number is 68 KB, not 48.** This estimate was made against a standalone probe;
> the integration built into `cardenginei_arm9_ra` measures 68,024 bytes. The 20 KB
> difference is newlib's `printf` and softfloat, reached through
> `rc_update_richpresence()` — a call site that is statically reachable from
> `rc_runtime_do_frame()` even though it never executes. See *`rcheevos` in the window*
> below, which supersedes the figures in this section. The conclusion is unchanged: 68 KB
> against 256 KB still fits comfortably, and the sizing argument below was never close
> enough to the line for 20 KB to matter.

### Runtime state, also measured

`rc_runtime_activate_achievement()` mallocs per achievement. Wrapping `malloc` and feeding
it definition shapes RetroAchievements actually uses — a simple flag, a delta compare, a
pointer chain with an AND of several conditions, a reset condition:

| Definition | Bytes |
| --- | --- |
| `0xH00b8b1=1` | 1,912 (includes one-time runtime setup) |
| `d0xH0016c0<0xH0016c0_0xH0016c0>10` | 432 |
| `I:0xX0019c8_0xH000048=5_..._0xX00004c>1000` | 1,368 |
| six conditions plus a reset | 968 |

So roughly **1 KB per achievement** — 50–150 KB of heap for a real set of 50–150.

### The budget, end to end

| | Minimum viable | Full client, large set |
| --- | --- | --- |
| code | 48 KB | 121 KB |
| heap | ~50 KB | ~150 KB |
| **total** | **~100 KB** | **~270 KB** |

Against 512 KB of DSi WRAM, both fit. And the answer to the phase 0 question — *does
`rc_client` fit in the cardengine?* — is now quantified from the other direction too: 48 KB
is 460 times the 104 bytes the cardengine has left.

### Two consequences worth deciding on deliberately

**RA and the colour filter compete.** The LUT holds 317 KB, leaving 195 KB. The minimum RA
configuration (~100 KB) coexists with it; the full one (~270 KB) does not. So either RA runs
reduced when colour filters are on, or the two are mutually exclusive and the user picks. It
does not have to be decided now, but the code should not assume they can both be maximal.

**The RA binary needs a heap and a libc, unlike the cardengine.** `rcheevos` calls `malloc`,
`realloc`, `snprintf` — which drags in newlib's floating-point formatting — and `fmod`,
which pulls in soft-float doubles. The 48 KB figure already includes all of that, so it is
paid for rather than surprising. But it means `cardenginei_arm9_ra` has to be a properly
linked program with its own allocator over a reserved arena, not an injected blob in the
style of `cardenginei_arm9_colorlut`. That is the single biggest structural difference from
the pattern being copied, and it is worth knowing before starting rather than after.

## `cardenginei_arm9_ra` — running on hardware

Built, packed into the `.nds`, staged in main RAM, copied into DSi WRAM, recognised as
code, called once per frame, and executing. **Confirmed on hardware**, first attempt.

The reading that established it, snapshot at `0x027FEF10`:

| Field | Value | Meaning |
| --- | --- | --- |
| `wramMagic` `+0x4C` | `52414831` — `RAH1` | the separate binary wrote its own magic |
| `wramTicks` `+0x50` | 2,701 | its own frame counter |
| `wramState` `+0x54` | `02` | `RA_WRAM_CALLED` |
| `ticks` `+0x04` | 2,701 | the reader's counter — *identical* |

The two counters being equal is the strongest part. It means the WRAM binary was called on
every frame since boot with none dropped; an intermittent call would leave `wramTicks`
behind. And `linesMax` stayed at 1, so the jump into WRAM costs under a scanline.

### The two things it answered that no amount of reading could

**`0x02600000` really was free.** The staging address came out of reading the bootloader's
early clear list rather than the address constants, which is what caught `0x02700000` being
the FAT table cache. Getting that wrong would have corrupted the bootloader's own file
tables; instead the copy landed intact.

**The colour LUT's retail path works.** The block this mirrors hands WRAM to the ARM7 only
in DSi mode, so for a plain DS game the ARM7 writes `0x03740000` with no handover at all.
Whether anyone had ever exercised that path was the largest unknown in the design, and it
could only be settled by running it. It works — and the MBK mapping evidently survives into
gameplay too, since the ARM9 cardengine reaches the same window afterwards.

### The watchlist moved in, and the cardengine got its room back

First thing built there, and chosen first deliberately: it is the smallest useful payload,
and it tests the one property nothing had tested — whether `.bss` in that window survives
between frames. `wramTicks` is now the WRAM binary's own counter, kept in its own `.bss`
and copied into the snapshot each frame rather than incremented there. If the window does
not hold state, it sticks at 1 while `ticks` climbs. Finding that out with a watchlist
costs a flash cycle; finding it out with `rcheevos` half-integrated costs a week.

What moved: the descriptors, the pointer-chain walker, the range checks, `ra_watch_add()`.
What stayed: the snapshot, the per-frame entry point, the bridge, the overlay.

| | Before | After |
| --- | --- | --- |
| `ra_reader.o` text | 648 | **188** |
| `cardenginei_arm9` free | 28 | **476** |
| `RA_WATCH_MAX` | 2 | **16** |
| WRAM binary image | 48 bytes | 880 bytes |

The snapshot could not move with it. It is the only debug channel this project has, it is
read at a fixed cardengine address the RAM viewer is known to reach, and keeping it there
means the counters still work when the WRAM binary is absent — which is exactly when you
most want to see them. So the split is: the WRAM binary owns the watchlist and mirrors the
first `RA_RESULT_MAX` results into the snapshot, in a 12-byte form that drops `base` and
`offsets`. Those are static configuration; if they were wrong the address would not
resolve, which `status` already says.

One detail worth keeping. The self-test chain's cells used to live beside the watchlist,
but a pointer the walker follows must be a main RAM address and this binary runs from
`0x0374xxxx`. Relaxing that check to accommodate our own cells would have weakened it for
the game addresses it exists to guard, so the cells moved into the snapshot instead —
`selfCell` and `selfCellPtr` — and the WRAM binary fills them in, being the only side that
knows the address.

### Confirmed on hardware: the window holds state

Two readings at different times, snapshot at `0x027FED50`:

| Field | reading A | reading B |
| --- | --- | --- |
| `ticks` | 3,612 | 6,021 |
| `wramTicks` | **3,612** | **6,021** |
| `wramMagic` / `wramState` | `RAH1` / `02` | `RAH1` / `02` |
| `watchCount` / `resolved` | 2 / 2 | 2 / 2 |
| `selfCell` / `selfCellPtr` | `0x027FED50` / `0x027FED78` | same |
| `results[1]` | `S+4` → 3,612 | `S+4` → 6,021 |

**`wramTicks` tracks `ticks` exactly, twice.** That counter lives in the WRAM binary's own
`.bss` and is only copied into the snapshot, so if the window did not retain state between
frames it would sit at 1. It does retain it — `rcheevos` can keep its runtime there, which
was the open question this step existed to close.

The watchlist also survived the move intact: the two-step chain still resolves to `ticks`
and reads its live value.

### One number that changed, and why it is not a regression

`linesMax` went from 1 to **6**. The measurement's *scope* changed with this commit: it used
to time the reader alone, and now it wraps the overlay and the WRAM call as well. That is
the honest figure — it is what the game pays per frame for all of this — but it is not
comparable to the old one.

`linesLast` was `00` in both readings, so a typical tick still costs under a scanline; 6 is
a maximum, and 2.3% of a frame. What cannot be separated yet is how much of it is the wider
scope and how much is that calling into WRAM costs more than running from the cardengine —
cold code, different cache behaviour. That still needs the control measurement.

### A C library in the window: the crt0 it does not have

`rcheevos` calls `malloc`. That turned out to need a step nobody had had to take yet, and
it is worth spelling out because it is the kind of thing that fails silently.

There is no crt0 here. The bootloader copies the image and jumps in, and that is the whole
of the startup this window gets. `.text`, `.rodata` and `.data` are inside the image and so
arrive correct — which is why the binary's initialised data works. `.bss` is *not* in the
image: it arrives as whatever the previous occupant left, and the bootloader's copy writes
staging garbage over it besides, since it copies a fixed length rather than the exact image
size.

Everything written for this window so far coped by guarding on a magic and initialising by
hand. newlib will not. Its allocator keeps state in `.bss` and assumes, like every C
library, that it starts zeroed — and handing it garbage does not fail cleanly, it corrupts
a heap, which would surface much later as `rcheevos` misbehaving for no visible reason.

So `startup.c` is the crt0 this window lacks. It zeroes `.bss` once per boot, then gives
newlib a heap over the rest of the window through `_sbrk()`.

Measured: **5,836 bytes** for `malloc` alone in isolation, and the whole binary is now an
18,212-byte image against the 64K the loader copies, leaving a **237 KB arena**. newlib's
`.data` is the surprise at 5,636 bytes — that is the reentrancy structure — but it is in
the image, so it is paid for rather than a risk.

(Those are the numbers *before* rcheevos. With it the image is 68,024 bytes and the arena
is 193,148 — still ~189 KB, which was the point of measuring.)

Two details worth keeping:

**The "have we started" flag lives in `.data`, not `.bss`.** A `.bss` flag also works, but
only if written *after* the zeroing clears it, and that is a subtle ordering dependency in
code that runs once per boot inside an interrupt handler. Nobody would notice it breaking.
Putting it in `.data` — which the image initialises on every boot — makes it the one claim
in this project that is not a bet on garbage not coinciding with a magic.

**`ra_startup()` takes the arena bounds as arguments** rather than reading `__bss_start` and
friends itself. That is what lets the host test hand it a scratch buffer and exercise the
real zeroing and the real arena arithmetic, instead of stubbing the one function whose
failure mode is a corrupted heap.

**And it proves the allocator rather than assuming it.** The first allocation is written
across its whole length, read back at both ends and freed, and the stage only advances if
that worked. `wramStage` reports how far it got: `01` .bss zeroed, `02` arena measured,
`03` allocation verified, `04` watchlist running. A failure names its own stage.

### Confirmed on hardware

`wramStage` reads **`04`**, `heapSize` reads 243,064 exactly, and the watchlist still
resolves — so newlib runs in that window and the arena is real.

`heapUsed` came back as **4 bytes**, against a predicted few KB, and being wrong about it
was more useful than being right. `_malloc_trim_r` was in the linked symbols: when the
probe frees its 1 KB block, newlib hands the memory back through a *negative* `_sbrk`. So
the sequence was `sbrk(+4)` to align, `sbrk(+1052)` for the block, `sbrk(-1052)` on free —
leaving the alignment adjustment.

Which means the allocator does not just allocate, it releases. And that only works because
`_sbrk()` bounds-checks growth rather than any change:

```c
if (incr > 0 && heapBreak + incr > heapTop) {
```

The `incr > 0` was deliberate, with a comment saying newlib is allowed to hand space back.
Had the check ignored the sign, `trim` would have failed and the heap would have
fragmented quietly. It is the kind of thing that is invisible until a long session runs out
of memory it should have had.

Two smaller notes from the same reading. `wramTicks` was one behind `ticks`, which is
benign: the cardengine increments `ticks` early in the tick and the WRAM binary writes
`wramTicks` later, so a viewer reading a live buffer can land between them. Earlier
readings matched exactly by timing luck; what matters is that they track, not that a single
sample agrees.

And `linesMax` stayed at 6. Adding newlib and a heap cost nothing per frame — the startup
runs once and the per-frame path is unchanged.

### What this changes

The ARM9 cardengine's margin stops being the binding constraint on the project. Still
queued for the 256K window:

- `rcheevos`, since measured at 68K linked, for phase 2
- a real font for the overlay instead of eleven hand-drawn glyphs
- the overlay rewrite, and with it the `surveyBlocks()` bug and the menu stand-down
- a control measurement to separate the reader's own cost from machine contention

### What is in place

| Piece | State |
| --- | --- |
| `retail/cardenginei/arm9_ra/` — Makefile, linker script, entry point, a stub | done |
| Linked into DSi WRAM at `0x03740000`, 256K window | done |
| Built by `retail/Makefile` and packed as `nitro:/cardenginei_arm9_ra.bin` | done |
| `ra_tick()` calls it, gated on a flag and on the window containing code | done |
| `wramMagic` / `wramTicks` / `wramState` in the snapshot | done |
| Staging address chosen and justified (`0x02600000`) | done |
| Launcher reads the nitrofile into that buffer | **confirmed on hardware** |
| Bootloader ARM7 copies the buffer into WRAM and sets `b_raWramLoaded` | **confirmed on hardware** |
| `wramSize` reduced to `CARDENGINEI_ARM9_RA_WRAMSIZE` when it is loaded | done |

The stub does one thing: write `RAH1` into `wramMagic` on its first call and increment
`wramTicks` every frame. Useless on its own, and that is the point — the milestone is the
*chain*, not the payload. Each link reports separately so a failure names itself instead
of showing up as one silent absence.

### Why the call is gated twice

`ra_tick()` refuses to call the window unless the bootloader claims it is loaded *and*
the first halfword at `+2` reads `0xEA00`. The binary's first instruction is a branch by
construction, so that halfword is the signature — and DSi WRAM holds whatever its previous
occupant left, so an unloaded or half-copied window reads as plausible garbage rather than
as zeroes. Calling into it would be a jump into arbitrary data, inside an interrupt
handler, in the middle of a game. The colour LUT makes the same check on itself for the
same reason.

### What it cost, and the trade that is now unavoidable

The bridge needed **84 bytes** against the **56** the cardengine had. It was paid for by
taking `RA_WATCH_MAX` from 4 to 2 and dropping the depth-1 self-test watch. Two slots is
exactly what the two remaining defaults use, so there is no spare watch at all now, and
`cardenginei_arm9` is left with 28 bytes.

That is worth stating plainly rather than burying: **the cardengine is now full enough
that adding anything means taking something out.** The bridge was worth a watch slot
because the bridge is what ends the competition — once the loader works and the binary is
proven on hardware, the reader and the overlay move into the 256K window and stop fighting
over 12K. Until then every further byte here is a trade.

### Where the staging buffer can live: `0x02600000`

The launcher reads the nitrofile into main RAM and the bootloader's ARM7 copies it into
WRAM, the way `CARDENGINEI_ARM9_CLUT_BUFFERED_LOCATION` works for the colour LUT. Finding
a safe address for that turned out to hinge on a mechanism that is invisible from the
address list, so it is worth writing down.

**What decides it is the bootloader's early clear list.** Before anything else,
`main.arm7.c` blanks most of EWRAM:

```c
memset_addrs_arm7(0x02000000, 0x02000400);
memset_addrs_arm7(0x02000620, 0x02084000);
memset_addrs_arm7(0x02280000, IMAGES_LOCATION);
dma_twlFill32(0, 0, (u32*)0x02380000, 0x3F000);
dma_twlFill32(0, 0, (u32*)0x023C0000, 0x40000);
memset_addrs_arm7(0x02700000, BLOWFISH_LOCATION);   // 0x02700000-0x027B0C00
dma_twlFill32(0, 0, (u32*)0x027F8000, 0x8000);
memset_addrs_arm7(0x02800000, 0x02E80000);
```

Anything the launcher stages inside one of those ranges is wiped before the bootloader can
use it. That is *why* the existing staging addresses are where they are — `0x026F0000`
(the ARM9 cardengine, 64K), `0x027CE800` (the colour LUT), `0x027D0000` (its table) all sit
in gaps between the clears, and the comment on the `0x02700000` line even says so:
"except before ce7 and ce9 binaries".

So the ranges that survive, in the space above the game's own 4MB:

| Range | Size | State |
| --- | --- | --- |
| `0x02400000`–`0x02680000` | 2.5 MB | preserved, nothing claims it |
| `0x02680000`–`0x02700000` | 512 K | preserved; donor ROM, IGM extension, ARM9 cardengine staging |
| `0x02700000`–`0x027B0C00` | 700 K | **cleared on startup**; FAT table cache lives here transiently |
| `0x027B0C00`–`0x027F8000` | 285 K | preserved; blowfish, ARM7 staging, colour LUT staging, cache tables |
| `0x027F8000`–`0x02800000` | 32 K | **cleared on startup** |

`CARDENGINEI_ARM9_RA_BUFFERED_LOCATION` is therefore `0x02600000`, in the middle of the
2.5 MB preserved block and 512 K clear of the donor ROM above it, with a 256 K cap.

**A wrong turn worth recording**, because it is the trap this whole exercise was meant to
avoid. `0x02700000` looked free: it appears in no location constant, and the first
references a grep turns up are in `retail/bootloader/` — the B4DS path, not ours. Widening
the search showed `bootloaderi` clears that entire span on startup *and* puts the FAT table
cache there. Two truncated greps in a row would have produced a bootloader that corrupts
its own file tables. "Not in locations.h" does not mean free, and neither does "the first
few hits are in another path".

**And the requirement is smaller than it looked.** Only the loadable image passes through
the buffer, not the 256K window: the heap is allocated in WRAM and never copied. rcheevos
measures 68K linked (48K was the estimate; see the correction above), so 256K of cap is
generous rather than tight.

The staging strategy that follows from all of this: prove the load path with the 48-byte
stub that exists now. If the address is wrong after all, the blast radius is 48 bytes and
it shows up as a failed verification on hardware before anything grows into it.

## Known graphical limitations of the overlay (deferred)

These are all in `ra_overlay.c`, all found by playing real games, and all deliberately
left alone — including for a first public release. They are listed together so the
decision is on the record rather than implicit in what nobody got around to fixing.

The reason they are acceptable is the same in every case: the overlay's design rule is
that **a notification that corrupts the game is worse than no notification**, and it
holds that rule. Every item below is either cosmetic, transient, or a design choice.
None of them can crash a game or corrupt a save.

| # | Limitation | Severity | Observed? |
| --- | --- | --- | --- |
| 1 | Collides with the in-game menu on the frame it opens | cosmetic, transient | yes, once |
| 2 | Borrows one palette entry the game may be using | cosmetic, transient | not since the fix |
| 3 | `surveyBlocks()` mis-reads non-text backgrounds | **could corrupt graphics** | no |
| 4 | Cannot choose which physical screen it appears on | design decision | yes, by design |
| 5 | Silently skips notifications when no layer is free | design decision | yes, 5 of 18 attempts |

**1 — the menu collision.** Pressing X to open the in-game menu hands both screens to
the menu. If the overlay is holding a borrowed layer at that instant, both are writing
sub engine registers in the same frame. This produced the single remaining fault seen
after the palette fix. No achievement is going to unlock on the exact frame the menu
opens, so the exposure is close to nil. The clean fix is for the overlay to stand down
while the menu is up, which needs a way to know the menu is up.

**2 — the borrowed palette entry.** The text has to be *some* colour, so one entry of the
sub background palette is taken and restored on hide. Which entries a game is using is
not discoverable from the registers, so there is no way to pick a provably free one. One
entry is the floor this design has; it was fifteen until phase 1 found that the glyphs
only ever use one.

**3 — the only one that could actually corrupt something.** `surveyBlocks()` reads every
enabled layer's `BGCNT` as though it were a text background — character base in 16K
units, screen base in 2K units, map size from bits 14-15 — and never consults the BG mode
in `DISPCNT` or the colour-depth bit. For an affine or bitmap background those fields
mean different things; a bitmap's base is in 16K units, not 2K. So the survey can both
miss a block the game is using and mark one it is not, and picking an in-use block would
overwrite the game's tiles until it redrew them.

This has never been observed, and phase 1 produced positive evidence that it is not
firing in practice: every one of the 5 recorded denials was a missing *layer*, not a
missing block, so the block search was never even the deciding factor. It is a latent
correctness bug rather than an active one — but it is the one item here that is a bug and
not a trade-off, and it should be fixed when the overlay is rewritten rather than
patched in place.

**4 — no control over the screen.** The overlay draws on the sub engine; which physical
panel that feeds is `POWCNT1` bit 15, which belongs to the game. *Final Fantasy III*
flips it by context, so the notification appeared on the top screen in the field and the
bottom in battle. Putting it somewhere predictable means being able to borrow from the
main engine too.

**5 — skipped notifications.** Confirmed to be entirely a "no free layer" condition, and
to line up with the fades on map transitions. The answer is a queue that holds an unlock
until a layer frees, which is real work rather than a fix, and it belongs with the client
that will generate the unlocks.

### Disposition

Items 1, 2, 4 and 5 are not defects to fix but properties to design around, and 5 needs
the client to exist first. Item 3 is a real bug with no observed effect. None of them
blocks a release, and all of them belong with the overlay rewrite in
`cardenginei_arm9_ra` — which the overlay needs anyway for a font it can fit, so
patching them into a 104-byte margin first would be work done twice.

## `rcheevos` in the window

This is where the project stops being a memory reader. `rcheevos` is the library every
official RetroAchievements integration uses; its runtime is what turns a definition string
from the server into "this achievement just unlocked". Everything before it — the WRAM
window, the hand-written crt0, the heap — existed to make it possible.

It is in as a **git submodule** pinned to **v12.4.0**
(`2ad0b8672f68a48148620164510b963039e49eb1`), not vendored. A fork that tracks upstream is
worth the `--init` step: the definition syntax evolves on the server side, and a vendored
copy would silently stop understanding definitions that the website happily produces.

### Only the runtime is compiled

`retail/cardenginei/arm9_ra/Makefile` builds a whitelist, not the library:

```make
RCHEEVOS_RUNTIME := $(filter-out rc_validate.c, \
            $(notdir $(wildcard rcheevos/src/rcheevos/*.c))) \
            rc_util.c rc_compat.c rc_version.c \
            md5.c
```

A whitelist rather than a blacklist, because upstream's file set grows and a blacklist
would quietly start pulling things in. `rc_api_*` (server request/response building),
`rc_client` (the whole session-management layer, ruled out in open question #4), `rhash`
beyond `md5.c`, and `rc_validate.c` are all left out.

`-lm` is needed for exactly one symbol: `fmod()`, reached from `rc_typed_value_modulus()`.
No DS achievement is likely to use a float modulus, but the reference is unconditional, so
the library does not link without it.

### What it costs: 68 KB, and 20 KB of that is `printf`

The image went from 6.4 KB to **68,024 bytes**. The interesting part is where it went, and
it is not where you would guess:

| Symbol | Bytes | What it is |
|---|---|---|
| `_vfiprintf_r` | 8,860 | newlib's `printf` engine |
| `handles` (`.data`) | 4,096 | newlib stdio |
| `md5_process` | 2,660 | rcheevos md5s each definition |
| `_malloc_r` | 1,992 | newlib allocator |
| `rc_parse_condset` | 1,768 | rcheevos |
| `get_arg` | 1,516 | `printf` |
| `__ieee754_fmod` | 1,352 | `fmod`, plus ~3 KB of softfloat behind it |

Roughly **20 KB is `printf` and double-precision softfloat**, for code that can never
execute here. The chain that keeps it alive is worth writing down, because it is not
obvious and `--gc-sections` does not cut it:

```
rc_runtime_do_frame  ->  rc_update_richpresence  ->  rc_format_typed_value  ->  snprintf
```

`rc_update_richpresence` is guarded at run time by `if (self->richpresence && …)`, which is
never true for us — but the *call site* is statically reachable, so the linker keeps
everything downstream of it.

**It is being carried, not cut.** Cutting it means not compiling `richpresence.c` and
`format.c` and supplying our own `rc_update_richpresence`, which is a link-time
substitution of an upstream function that other upstream code calls — a real maintenance
hazard, and not something to introduce in the same change as first-light integration. There
is no pressure: the image is 68 KB inside a 128 KB budget inside a 256 KB window. If space
ever gets tight this is a known, measured 20 KB, and rich presence is meaningless on an
overlay that cannot yet draw arbitrary text anyway.

Raising `CARDENGINEI_ARM9_RA_IMAGE_MAX` from 64 KB to 128 KB was needed for this, and it
exposed a real gap: **nothing checked the image against the limit.** The loader copies a
fixed length, so a 68 KB image would have been copied truncated at 64 KB — booting
correctly, since the branch at +0 is intact, then failing inside code that simply is not
there, with no way to detect it at run time. The Makefile now fails the build instead, and
prints the budget on every success. That is the only place the two numbers can be compared.

### The three things the DS makes different

**1 — definitions come from the network.** An address in a definition is a number somebody
else wrote, and dereferencing an address this console does not have is a Data Abort inside
the game's interrupt handler. So `peek()` routes every read through the same
`ra_readable()` the watchlist uses. There is deliberately **one** answer in this binary to
"may this address be read": an address from the server does not get a weaker check than a
hand-written watch. `ra_readable()` and `ra_read()` stopped being `static` for this and
nothing else.

`peek()` has no error channel — it returns a value — so a refused read returns **zero**.
That is the safe direction: the condition compares against zero and is false, which means
the achievement does not unlock. The refusal is counted in `rcPeeksRejected` rather than
swallowed, so it cannot be confused with a genuine zero in memory.

On top of that, `rc_runtime_validate_addresses()` is handed `ra_rc_validate_address()` once
after activation, so an achievement referencing memory this console cannot supply is
**disabled up front** rather than evaluated against zeros forever. Both layers are needed:
per-read validation catches addresses a definition computes at run time through
`AddAddress`, which the up-front pass cannot see.

**2 — RetroAchievements addresses are console addresses.** The server's map puts DS system
RAM at 0, and the frontend translates:

```
console 0x0000000-0x03FFFFF  ->  0x02000000   system RAM (4M)
console 0x0400000-0x0FFFFFF  ->  unused, padding to align the DSi map
console 0x1000000-0x1003FFF  ->  data TCM
```

Translated with our own three-line map rather than by calling
`rc_console_memory_regions()`, because that function's `switch` references the table for
every console rcheevos supports — forty-odd tables of regions and names — and calling it
would drag all of them into the image to answer a question about one console.

**Data TCM is deliberately not translated.** Its base is whatever the game programmed into
CP15 `c9,c1`, so there is no constant to map it to. A guess would read real memory
belonging to something else and produce values that look plausible, which is worse than
refusing: an achievement that reads DTCM is reported unsupported instead.

**3 — the ARM9 does not fault on an unaligned 32-bit load, it silently rotates.** A
32-bit `LDR` one byte into `44 33 22 11 88 77 66 55` returns `0x44112233` — the aligned
word, rotated — where the correct answer is `0x88112233`. Achievement authors write
unaligned reads routinely and the server serves them, so `peek()` assembles multi-byte
reads from bytes when they are unaligned. Refusing them would break real definitions;
trusting the hardware would return plausible nonsense.

### A real definition, and where it has to be anchored

The test definition is real syntax, not a stub:

```
M:0xH000000>=0.600.
```

*Measured*, the byte at console address 0 is at least zero, six hundred times. The
comparison is always true, so it counts one hit per frame and unlocks after 600 frames —
about ten seconds. `rcMeasured` climbs one per frame toward `rcTarget`, so a hex viewer
shows rcheevos *evaluating* rather than merely having been initialised.

**Console address 0 is the anchor because the snapshot has none**, and that took a hardware
reading to establish. The obvious choice was the snapshot's own tick counter, and it does
not work: RetroAchievements maps **4 MB** of DS system RAM, and on this hardware main RAM is
**16 MB**. The cardengine lives at `0x027FC000` — eight megabytes in — so no console address
names it.

The first attempt at a fix assumed it was a 4 MB mirror, which a real DS would have made
true. It is not one here: a sentinel written through `0x027FED54` and read at `0x023FED54`
came back different, twice, which is separate memory rather than a mirror. So the mirror
machinery came out again and the definition moved to an address the map actually reaches.

This is worth keeping straight because it is a property of the platform, not of this build:
**the RetroAchievements address space covers only the first 4 MB.** Real achievements are
unaffected — a retail DS game lives entirely in that 4 MB, which is exactly why the map is
drawn that way. Only code reading *its own* memory, as the self-test wanted to, falls
outside it.

What the definition covers: a memref read, a comparison, a hit target, the measured flag,
the trigger firing, and `rc_runtime_do_frame()` reaching memory every frame. What it does
not cover is the delta memref, which needs a value that changes and therefore a game address
nobody can name in advance. The first real achievement will exercise it.

Anchoring at a constant also removed the small hex writer that used to build the definition
string: there is no address to format anymore, so the definition is a literal. Nothing in
this project ever builds a definition — real ones arrive as strings from the server.

`rc_runtime_init()` allocates the memref list and **does not check the result** before
writing through it, so an exhausted arena would be a null dereference inside the library
rather than a failure it reports. A `malloc(sizeof(rc_memrefs_t))` probe runs first and
turns that into `RA_RC_NO_MEMORY`. The size comes from the library's private
`rc_internal.h` rather than a guessed constant, so the check cannot drift when upstream
changes the structure.

### Tested on the host first

`tools/ra_reader_test.sh` now compiles rcheevos too, and `ra_rcheevos.c` is `#include`d
into the test like `cardengine.c` and `startup.c` — the translation and the peek path are
`static`, and they are exactly the parts worth testing. The expensive failures here are a
definition that does not parse and an address that translates wrongly, and both are pure
logic. Catching either on the host costs seconds; catching it on hardware costs a flash
cycle and a photograph of a hex viewer.

It covers: the definition parses (`rcActivate == RC_OK`) and is not disabled by the
validation pass; the translation is exact at both ends of system RAM and refuses DTCM, the
byte past the end, a length that would run off the end, and an address that would wrap the
check; unaligned reads assemble to `0x88112233` across a known two-word straddle; a refused
peek returns zero and increments the counter; and `rcMeasured` climbs one per frame *and
stops climbing when the value stops changing*, which is what makes it a test of the delta
memref rather than of the frame counter.

The one thing the host cannot check is the arena: glibc's `malloc` does not go through our
`_sbrk`, so `heapUsed` is meaningless there. That is what hardware is for.

### First hardware reading: `rcStage = 01`, and the bug it exposed

The first read said `rcStage = 01` — `RA_RC_NO_MEMORY`, rcheevos unable to allocate **32
bytes** (`sizeof(rc_memrefs_t)`, measured on the target) — next to `heapSize = 0x2F27C`,
*exactly* the 193,148 bytes predicted. Two numbers that cannot both be true.

`heapUsed` read `0`, and that is the tell. If `malloc` had ever gone through `_sbrk` it
would have left at least the top chunk behind, so the break never moved: the `malloc(1024)`
probe inside `ra_startup()` had **failed**. But `wramStage` read `04`, which requires that
probe to have succeeded.

Both readings are explained by one line. `ra_startup()` sets its "already ran" flag
*before* running the probe, and then returned `RA_STAGE_ALLOC` unconditionally on every
later call:

```c
if (startupState == RA_STARTUP_DONE) {
    return RA_STAGE_ALLOC;      /* every frame after the first, regardless */
}
startupState = RA_STARTUP_DONE; /* set before the probe below */
```

So a probe that failed on frame 1 was reported as a working heap from frame 2 onward, and
every stage above it read as healthy over a dead heap. It now remembers the stage it
actually reached, in `.data` beside the flag for the same reason the flag is there: written
once per boot, read on every call, must not depend on the `.bss` zeroing having happened.

**That is the bug, and it is fixed. Why the probe fails is still open.** The two are
separate: the lie is what made the failure unreadable, not what caused it.

The lesson generalises past this instance. Every other stage in this project fails
*forward* — `wramState`, the watch statuses, `rcStage` — and this one failed *backward*,
reporting success it had not achieved. A staged report is only worth having if the stages
cannot lie, and this one could, in the one direction that matters.

So the next reading is built to be decisive rather than ambiguous. `ra_startup()` now asks
`_sbrk()` directly before giving up, and the snapshot carries `_sbrk()`'s own two numbers
plus both probe results:

- **`sbrkProbe` non-zero beside `mallocProbe` zero** → the arena is fine and hands out
  memory that `malloc` refuses. The fault is in newlib.
- **both zero** → the arena bookkeeping is wrong, despite `heapSize` reading correctly.

Two smaller things came out of the same reading. The arena base is now rounded up to 8:
`__bss_end` is only guaranteed 4-aligned and landed on `0x03750D84`, so dlmalloc was
correcting the misalignment by asking `_sbrk()` for the difference — which is where
`heapUsed = 4` in an earlier session came from, a number that looked inexplicable at the
time. And `RA_RC_NO_MEMORY` no longer covers two different failures: `rc_runtime_init()`'s
own allocation failing is now `RA_RC_NO_MEMREFS`.

The host test covers the regression, which it could not before. The runner links with
`-Wl,--wrap=malloc`, so the probe can be made to fail on demand and the test asserts the
*second* call still reports `RA_STAGE_HEAP`. Confirmed to fail against the old code and
pass against the new — a regression test that was never run red is not yet a test.

### The allocator: ours, not newlib's

The second reading settled it. `_sbrk()` returned exactly the predicted base address and
`heapSize` matched the prediction to the byte, and `malloc(1024)` refused anyway without
ever calling `_sbrk()` successfully. Whatever newlib is unhappy about is inside newlib.

`retail/cardenginei/arm9_ra/source/ra_alloc.c` replaces it: a first-fit list with forward
coalescing over the whole arena, 8-byte aligned payloads because rcheevos stores 64-bit
values in its typed-value union. Four reasons that hold independently of the bug:

1. **It is testable.** newlib's allocator cannot be exercised by
   `tools/ra_reader_test.sh` — on the host, glibc's malloc is what runs. That is precisely
   why this failure cost two flash cycles to characterise. `ra_alloc.c` is tested on the
   host like everything else in this binary: allocation, alignment, non-overlap, coalescing,
   splitting, exhaustion, double free, out-of-arena pointers, `realloc` growth and
   preservation of the original on failure, `calloc` zeroing and overflow refusal.
2. **It is deterministic.** This runs in the game's VCOUNT handler, where a variable-time
   path is a dropped frame. dlmalloc trims and consolidates on its own schedule.
3. **It needs no crt0.** newlib's allocator keeps initialised state in `.data` and expects
   a startup this window does not have. Ours needs one call with two pointers.
4. **What rcheevos asks for is modest** — roughly 1 KB per achievement at load time, and
   nothing per frame. So the O(n) first-fit walk never happens inside the per-frame path.

`_sbrk()` now **refuses everything**, which matters: the arena has exactly one owner, and
two allocators sharing one range is how you get corruption that only appears under load. It
cannot simply be deleted, because newlib's `snprintf` is still linked — statically reachable
from rich presence — and through it newlib's `_malloc_r`, which references `_sbrk_r`.

**It did not save the 20 KB.** The image went 68,272 → 68,776 bytes. `_malloc_r`, `_free_r`
and `_vfiprintf_r` are all still in there, unreachable, pulled in by that same `snprintf`
reference. Cutting them is still one job — not compiling `richpresence.c` and `format.c` —
and it was never the allocator's to do. `malloc` is now a 4-byte thunk to
`ra_alloc_malloc`.

The probe in `ra_startup()` calls `ra_alloc_malloc()` directly rather than `malloc`, so the
host test exercises the same code the hardware runs. And the failure path is now reachable
without mocking anything: hand `ra_startup()` a window with no room past `.bss` and
`ra_alloc_init()` cannot take an arena, which is how the regression test for the lying
stage is driven now that `--wrap=malloc` is gone.

### What it read on hardware

Five readings, and the last one is the milestone: **`rcTriggered = 1`** — an achievement
unlocked on a 3DS running a DS game. Every number predicted in advance matched.

| Address | Field | Read |
|---|---|---|
| `0x027FEDBC` | `rcStage` | `06` — `RA_RC_FRAME` |
| `0x027FEDBD` | `rcActivate` | `00` — `RC_OK` |
| `0x027FEDBE` | `rcTriggerState` | `05` — `RC_TRIGGER_STATE_TRIGGERED` |
| `0x027FEDC0` | `rcTriggered` | **`1`** |
| `0x027FEDCC` | `rcPeeks` | `1` per frame, exactly what the definition reads |
| `0x027FEDD0` | `rcPeeksRejected` | `0` |
| `0x027FEDBF` | `rcInitLines` | `6` — the one-time parse |
| `0x027FEDD4` | `rcLines` / `rcLinesMax` | `0` / `1`, out of 263 |

**The per-frame cost is the number that matters most, and it is negligible.** Evaluating a
definition every frame inside a DS game's VCOUNT handler costs under one scanline; activating
an achievement costs six, once. That answers open question #2 for rcheevos specifically.

`rcEvents` read 255, which is the clamp — `PROGRESS_UPDATED` once per frame for 600 frames,
not a fault.

`rcMeasured` and `rcTarget` read `0`, which is correct rcheevos behaviour rather than a bug:
measured progress is reported only while a trigger is **active**, and `TRIGGERED` is not
active. They are latched now, so a reading taken after the unlock shows the last active
value — 599 of 600. One short, because on the frame the count reaches the target the trigger
fires and rcheevos has already stopped reporting. The host test pins it as `target - 1`
rather than rounding up: the latch should show what was reported, not what would look tidier.

`heapSize` at `+0x60` reads **`0x2F048`** (192,584 bytes, ~188 KB) — the window minus
the 68 KB image, its `.bss`, and the 8-byte alignment of the base. `heapUsed` at `+0x64` is
what rcheevos actually took, and is the first real answer to "how much of 256 KB does this
eat".

**`rcInitLines` is reported separately from `rcLines` on purpose.** Activating an
achievement mallocs, md5s the definition and parses it, all inside the game's VCOUNT
handler, so it is far more expensive than a frame of evaluation. Folded into `linesMax` it
would make the steady-state cost look an order of magnitude worse than it is. The first
frame will still show a large `linesMax` in the cardengine's own counter; `rcInitLines` is
what explains it rather than leaving it mysterious.

The watchlist keeps running alongside rcheevos rather than being replaced by it. The two
are independent readers of the same memory: the watchlist is the part that can be debugged
by eye, and keeping both means a disagreement between them is visible rather than a
question of which one to believe.

## Status

- [x] Baseline: unmodified nds-bootstrap builds
- [x] Phase 0: per-frame game RAM snapshot — **confirmed on hardware**
- [x] Phase 0.5: text notification over a running game — **confirmed on hardware**
- [x] Phase 1: parameterised watchlist + pointer chains — **confirmed on hardware**,
      on *Space Invaders Extreme* and *Final Fantasy III*. Known overlay limitations
      found along the way are catalogued and deferred, see above.
- [x] `cardenginei_arm9_ra`, a separate ARM9 binary in DSi WRAM — **confirmed on
      hardware**. Staged, copied, recognised, called every frame, executing, and
      reporting back. 256K of window with code execution, which retires the cardengine's
      12K as the project's binding constraint.
- [x] Phase 2 core: `rcheevos` evaluating a real achievement definition — **confirmed on
      hardware**. Submodule pinned at v12.4.0, runtime only, running on our own allocator
      in DSi WRAM, `peek()` routed through the watchlist's own validation. An achievement
      unlocked on a 3DS running a DS game, at a cost of under one scanline per frame.
      `rc_client` remains ruled out, see open question #4.
- [ ] Phase 2 rest: a real achievement set, which needs the client and therefore the
      network transport — open question #1 is now the critical path.
- [ ] Phase 3: real network, softcore unlocks
- [ ] Phase 4: rich presence, achievement list, login status
