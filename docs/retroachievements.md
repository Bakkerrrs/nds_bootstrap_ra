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

### What to read on hardware

Three readings in, the chain below the achievement is all confirmed: the binary loads and
runs, `.bss` persists, the arena is measured correctly, and **our allocator works** —
`heapUsed` showed 2,728 bytes of real rcheevos allocations and `mallocProbe` landed exactly
on `heapBase + 8`, the block header. What is left untested is rcheevos evaluating.

`tools/ra_snapshot_addr.sh` for the current snapshot address; for the build in hand it is
`0x027FED50`.

`heapSize` at `+0x60` should read **`0x2F048`** (192,584 bytes, ~188 KB) — the window minus
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
