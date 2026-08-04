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
| `ra_reader` | Read the game's RAM. Knows nothing about RetroAchievements. | phase 0 done |
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

`ra_reader_tick()` copies `RA_SNAPSHOT_WINDOW` (256) bytes from
`RA_DEFAULT_WATCH_ADDRESS` (`0x02000000`) into a snapshot buffer once per frame.

The snapshot lives in the cardengine's own `.bss`, which is inside the region
reserved for the cardengine, so the game can never touch it. `.bss` is **not**
zeroed — an injected binary has no crt0 to do it — so the header is validated by
magic on every tick and nothing assumes a known initial state.

```c
typedef struct raSnapshot {
	u8  magic[4];    /* 'R','A','0','S' */
	u32 frame;       /* incremented once per captured frame */
	u32 srcAddress;
	u32 length;
	u8  data[RA_SNAPSHOT_WINDOW];
} raSnapshot;
```

### Observing it on hardware

The buffer sits in `.bss`, so its address shifts whenever surrounding code
changes. After building, run:

```sh
./tools/ra_snapshot_addr.sh
```

Use the address for the variant your game actually loads — a plain retail DS game
on a DSi or 3DS uses `cardenginei_arm9`. Then open the in-game menu, go to the RAM
viewer, navigate to that address, and you should see:

- the ASCII bytes `52 41 30 53` (`RA0S`),
- a frame counter climbing once per frame,
- then a live mirror of `0x02000000`.

The counters before `data[]` are the diagnostic: they mark each link the reader
depends on, so a buffer that never fills in still says where things stopped.

Confirmed working on a 3DS running *Space Invaders Extreme* (`cardenginei_arm9`),
with the whole chain intact:

| Field | Value | Meaning |
| --- | --- | --- |
| `ticks` | 354 | the VCOUNT handler is firing every frame |
| `cardReads` / `irqEnables` / `hookCalls` | 89 / 128 / 1 | cardengine has control, the patched `irqEnable` ran, the install ran once |
| `irqTable` | `0x027E0000` | the game's IRQ table was found |
| `vcountRef` | `0x027FC348` | inside the cardengine (base `0x027FC000`) |
| `origVcount` | `0x02006BD8` | inside the game's ARM9 binary — the game had its own VCOUNT handler, so chaining is safe |

Note that `data[]` looks static: `0x02000000` is where the game's ARM9 **code**
loads, so the mirror is full of instructions that never change (`E7FFDEFF`, the
ARM trap encoding). Liveness is proved by `ticks`, not by the contents. Watching
memory that actually changes needs the parameterised window, which is phase 1.

### Files

| File | Purpose |
| --- | --- |
| `retail/common/include/ra.h` | Shared definitions, snapshot layout, master switch |
| `retail/common/include/ra_reader.h` | Reader API |
| `retail/cardenginei/arm9/source/ra_reader.c` | Reader implementation |
| `tools/ra_snapshot_addr.sh` | Prints the snapshot address from the link maps |

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

The cycle budget is not yet measured — that needs hardware. The 256-byte word copy
is ~64 loads and stores, which is negligible; the question only becomes real at
phase 1 watchlist sizes.

### #4 — does `rc_client` fit in the cardengine? **No.**

This is now answered, and it is the most important finding so far. The ARM9
cardengine is linked into a fixed 12 KB window:

```
MEMORY { vram : ORIGIN = CARDENGINEI_ARM9_LOCATION, LENGTH = 12K - 0x60 }
```

which is `0x027FC000`–`0x027FEFA0`. Current occupancy with the phase 0 reader
included:

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

### #1 — network transport, and #3 — pointer chains

Untouched. #1 (can the cardengine reach the 3DS's ARM11 WiFi stack, or is dswifi
the only option?) still gates the phase 3 architecture and is worth asking the
DS-Homebrew / RetroAchievements Discords before any hardware is bought. #3 needs
the reader to resolve pointer chains freshly each frame, which the current fixed
window does not do — it is a phase 1 design requirement, not a retrofit.

## Status

- [x] Baseline: unmodified nds-bootstrap builds
- [x] Phase 0: per-frame game RAM snapshot — **confirmed on hardware**
- [ ] Phase 1: parameterised watchlist + pointer chains
- [ ] Phase 2: `rcheevos` / `rc_client` with mocked network
- [ ] Phase 3: real network, softcore unlocks
- [ ] Phase 4: on-console UX
