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

What has never been read is rcheevos actually evaluating. That is the whole of the next
step.

The ARM9 cardengine has **436 bytes** left. It had 28 before the watchlist moved out, which
is the constraint behind almost every decision in this document.

### The next task: read the rcheevos block

**Settled by three hardware readings.** newlib's `malloc` does not work in this window
(`_sbrk()` returned the predicted base; `malloc` refused anyway), so the allocator is ours
now — and it works: `heapUsed` 2,728 bytes of real rcheevos allocations, `mallocProbe`
exactly on `heapBase + 8`. And the snapshot has no RetroAchievements console address,
because main RAM here is 16 MB while the map covers 4 MB, so the self-test moved to console
address 0.

What is left is whether rcheevos evaluates. Read these, in this order:

| Address | Field | Expected |
|---|---|---|
| `0x027FEDBC` | `rcStage` | **`06`** = `RA_RC_FRAME` |
| `0x027FEDBD` | `rcActivate` | `00` = `RC_OK` |
| `0x027FEDBE` | `rcTriggerState` | `02`/`03`, **not** `07` (disabled) |
| `0x027FEDC4` | `rcMeasured` | climbing one per frame |
| `0x027FEDC8` | `rcTarget` | `58 02 00 00` = 600 |
| `0x027FEDC0` | `rcTriggered` | `00`, then **`01`** after about ten seconds |
| `0x027FEDCC` | `rcPeeks` | non-zero |
| `0x027FEDD0` | `rcPeeksRejected` | `00` |
| `0x027FEDD4` | `rcLines` | the per-frame cost, at last measurable |

`rcTriggered` going to `01` is an achievement unlocking on real hardware. `rcLines` beside
`rcInitLines` is the first honest per-frame cost — earlier readings could not produce one,
because the achievement was disabled and `do_frame` had nothing to do.

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
- [~] Phase 2: `rcheevos` — submodule pinned at v12.4.0, runtime compiled into
      `cardenginei_arm9_ra`, `peek()` routed through the watchlist's own validation, one
      real achievement definition parsed and evaluated, running on our own allocator.
      **Passing on the host; awaiting one hardware reading.** `rc_client` remains ruled
      out, see open question #4.
- [ ] Phase 3: real network, softcore unlocks
- [ ] Phase 4: rich presence, achievement list, login status
