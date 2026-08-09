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

**The client-side half of an achievement is finished and proven on real hardware.**
Published RetroAchievements code notes, written as real memaddr definitions, evaluated by
rcheevos inside a retail DS game running natively on a 3DS, firing on the correct frames.
Nothing is emulated and nothing is stubbed. What is *not* done is telling the server —
nothing has ever been sent anywhere.

Confirmed on hardware, in order of when each was settled:

1. **Phase 1, the watchlist and pointer chains** — across several sessions and three games.
   Re-resolved from scratch every frame; every value predicted in advance has matched.
2. **`cardenginei_arm9_ra`**, the separate ARM9 binary in DSi WRAM — built, loaded, called,
   with a working allocator over its arena. newlib's `malloc` does **not** work in this
   window; ours does.
3. **rcheevos evaluating a synthetic definition** — `rcTriggered = 1`, `rcLinesMax` 1
   scanline of 263.
4. **rcheevos evaluating three real ones** — Super Mario 64 DS, `rcTriggered = 3`, including
   a guarded `AddAddress` chain and two delta memrefs. See *It fired* below.
5. **WiFi** — `tools/wifiprobe/` associated to WPA2-PSK and got RetroAchievements to answer
   over plain HTTP from this exact 3DS, stage 6 of 6.

6. **WiFi inside nds-bootstrap's launcher** — step 2 of the ladder, `reached stage 5 of 5` on
   a 3DS: chip, firmware, WMI, association and the WPA2 handshake, on nds-bootstrap's own
   ARM7 rather than the libnds template. The bring-up is **identical to the standalone
   control, value for value**, and the SCFG the launcher inherits is the exact word the probe
   writes by hand. Log at `docs/logs/ra_wifi_launcher-3ds.log`.
7. **The launcher reaching RetroAchievements** — step 3a, `reached stage 9 of 9`: lwip cut to
   fit, DHCP, DNS, TCP and one HTTP GET to `dorequest.php`, with the API's own
   `invalid_credentials` coming back. Log at `docs/logs/ra_wifi_launcher_http-3ds.log`.
8. **The ROM's RetroAchievements hash** — step 3b, and it is **the hash the server has**,
   checked against the set's page. `c3b1916756737f2c4117cc95c1d51ac7` for Super Mario 64 DS.
   Log at `docs/logs/ra_wifi_launcher_hash-3ds.log`.
9. **A real login from the launcher** — step 3c, `reached stage 10 of 10`: `ra.cfg` read,
   credentials percent-encoded, `r=login` answered with a token for a real account. Log at
   `docs/logs/ra_wifi_launcher_login-3ds.log`.
10. **The server recognising the ROM** — `reached stage 11 of 11`, `r=gameid` answered
    **`GameID 14856`** for `c3b1916756737f2c4117cc95c1d51ac7`. The hash question is closed by
    the server rather than by eye. Log at `docs/logs/ra_wifi_launcher_gameid-3ds.log`.

11. **The achievement set arriving from the server** — step 3d, `reached stage 12 of 12`:
    **87,747 bytes** of `r=patch` JSON streamed off the socket, **56 published definitions**
    staged for the cardengine, 3 unofficial ones filtered out, and **zero bytes of heap
    allocated** — `top 02329000` identical before and after. It took two runs: the first used a
    2 KB carry buffer against a set whose largest definition is **6,264 bytes** and lost five of
    them. Logs at `docs/logs/ra_wifi_launcher_patch{,2}-3ds.log`, and the set itself at
    `docs/logs/ra_definitions-14856.txt`, written by the console.
12. **The set fits, and rcheevos accepts all of it.** `28,585 of 32,759` bytes in the staging
    block, and `tools/ra_fit_test.c` activates **56 of 56** definitions through
    `rc_runtime_activate_achievement()` in **128,352 of the arena's 158,132 bytes** — 29,780 to
    spare. Both numbers were open questions until this ran.

14. **Fetched at boot, then played.** The console logs in, fetches the published set for GameID
    14856, tears the radio down — confirmed by dsiwifi's own `AR6014 deinitted`, not just by our
    acknowledgement — boots Super Mario 64 DS and plays. **About 15 seconds** from power-on to the
    game. The fetched set diffs clean against the earlier one and the snapshot is identical field
    for field, `rcEvents` aside. Log at `docs/logs/ra_wifi_launcher_boot-3ds.log`.
13. **The server's own set running inside the game.** Super Mario 64 DS boots and plays with all
    **56 of 56** published definitions active, `rcBadLine 0`, `rcPeeksRejected 0`, and the set's
    first definition unlocking first as predicted. It took three crashed runs to get there:
    everything in this project runs on the game's VCOUNT interrupt stack, and the parse needs far
    more of it than the evaluation, so rcheevos has its own 8 KB stack now. Measured on the
    console: **67 scanlines of 263 per frame**, 1,765 to load the set, 1,624 bytes of stack,
    110,472 of the arena's 149,288.

**Step 3 is finished.** The launcher logs in, identifies the ROM, fetches the published set and
stages it where the game will find it, allocating nothing. What has never been tried is *running*
those definitions — that is step 4, and the cheapest first move is below.

Everything from the game's RAM up to a fired trigger is done, and the launcher now reaches the
RetroAchievements API. **So open question #1 is closed for context A, and nothing in front of
the remaining work is a question about the platform** — the hash, `r=login` and `r=patch` are
code. What remains untested is context **B**, inside the game, where the ARM7 belongs to the
game; the plan does not need that to work (see *#1g*).

The ARM9 cardengine has **412 bytes** left. It had 28 before the watchlist moved out, which
is the constraint behind almost every decision in this document.

### Picking it up in a fresh session

The five things a new session needs that are not obvious from the source:

| | |
|---|---|
| Branch | `claude/fase-1-retroachievements-1phq4i` (was `claude/fase-1-avance-965hk5`) |
| Snapshot address | `tools/ra_snapshot_addr.sh` — currently `0x027FED50` for `cardenginei_arm9`, which is the variant a retail DS game loads on a 3DS. **Re-run it after every build**; it moves. |
| Magic to look for | ASCII `RA2S` (`52 41 32 53`). `RA1S`/`RA0S` means a stale address from an older build. |
| Host test | `./tools/ra_reader_test.sh` — no toolchain, no hardware, seconds. Builds and runs **three** binaries: the reader/watchlist, the launcher's pure logic, and `ra_fit_test` (a real 56-definition set against the cardengine's arena). Run it before anything. |
| Full build | `make` from the top level, **serially**, with `lzss` on `PATH`. See *Building*. |
| WiFi build | `make RA_LAUNCHER_WIFI=1` — the network diagnostic, 12 rungs: the chip, DHCP, DNS, HTTP, the ROM's hash, `r=login`, `r=gameid` and `r=patch`. **It does not boot games**; it stops on a summary and writes `/ra_wifi_launcher.log`. Needs `git submodule update --init`. |
| Fetch-and-play build | `make RA_LAUNCHER_WIFI=2` — the 13-rung ladder, then tears the radio down and boots the game with the server's set staged. See *Step 4, online half*. |
| RA config | `sd:/_nds/nds-bootstrap/ra.cfg`, odelot's format — copy `tools/ra.example.cfg`. Username and password, in the clear, by decision; see *Step 3c*. |

Two working habits this document was largely written by, both of which were learned by
paying for them:

- **A reading that cannot come out two ways is not a test.** Two rounds were spent on canaries
  whose value was the same whether the hypothesis held or not.
- **Measure before guessing.** Each guess costs a flash cycle and a play session; a watch line
  in `ra_achievements.txt` costs a text edit. The file exists for that reason.

The immediate next step is **step 4**, and it splits into two failures that are worth keeping
apart, because they have different fixes.

**4a and step 5 are both confirmed on hardware.** The block carries RetroAchievements' own ids —
`56 with, 0 without`, all distinct — and `rcFirstId` read **101000001**, matching line 1 of the dump
to the digit across eight hops.

**And `101000001` turned out to be the server talking to us.** Captured verbatim from the reply:
`"Title":"Warning: Unknown Emulator","Description":"Hardcore unlocks cannot be earned using this
emulator."` — RetroAchievements injects it as a fake always-true achievement because it does not
recognise this client's User-Agent. It is dropped now, on evidence rather than on a threshold. The
standing item it leaves is not code: **getting the client recognised by RetroAchievements** is a
conversation with them, and until it happens hardcore is off by the server's decision as well as
ours. See *It was the server talking to us* below.

**Both halves of the offline path are done, and 4a — fetch at boot, then boot — is confirmed on
hardware.** What remains is **4b**: `r=awardachievement` at the moment an achievement fires, which is
network *inside* the game. Two facts from the 4a run bear on it and point opposite ways — the radio is
still powered and associated when the game starts, and the launcher needed all of dsiwifi resident to
get there against the 18 KB of IWRAM context B leaves free. The nearer refinement is smaller: making
the 15-second ladder skippable from `ra.cfg`.

**The first half is done.** `docs/logs/ra_definitions-14856.txt` copied to
`sd:/_nds/nds-bootstrap/ra_achievements.txt` boots and plays with all 56 definitions active, at 67
scanlines of 263 per frame. What remains is the second half: the network beside a running game, and
then a build that both fetches and boots. The record of how the first half was reached:
It took **three crashed runs to find the cause, and it is not any of the things that were
suspected**: everything in this project runs inside the game's VCOUNT interrupt handler, on the
game's IRQ stack, and `rc_runtime_activate_achievement()` needs **2,383 bytes** where
`rc_runtime_do_frame()` — which has run every frame for many sessions — needs 767. The parse was
overflowing it. rcheevos has its own 8 KB stack now, and `rcStackUsed` (`+0x6A`) reports the
high-water mark. See *Second and third runs: it is the game's IRQ stack* below, including what the
earlier flat-depth measurement got right and what was wrongly concluded from it.
That is the server's own 56 definitions going through `loadRaDefinitions()` into the cardengine,
and it answers "can rcheevos run a real set on this hardware" on its own. Two numbers are open and
everything else is already answered on the host: **`rcInitTotal`** (`+0x9E`), the one-time parse,
and **`rcLinesMax`** (`+0x85`), the steady-state cost of 1,946 conditions per frame out of 263 —
three definitions cost 1. See *Step 4, offline half* for the full checklist, with a prediction
against every field, and for why `rcInitLines` had to be fixed before the run rather than after
it.

**Then the network beside a game**, which is the part *#1g* has always flagged as the real unknown:
in context B the ARM7 belongs to the game, and the launcher's ARM7 has 18 KB of IWRAM spare with
dsiwifi in it while the cardengine's own ARM7 hooks are also resident. `safe 61440` is the launcher
heap step 4 inherits — the static floor rose 35 KB across steps 3b–3d.

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

### Definitions come from a file now, not from a rebuild

Testing a definition against a running game is the slowest loop in this project: build,
flash, play, photograph. And a definition is exactly the kind of thing that is wrong the
first two times — a mistyped address, the wrong size, a condition that never becomes true.
Compiling one in would have meant a flash cycle per attempt.

So `cardenginei_arm9_ra` reads its definition from
**`sd:/_nds/nds-bootstrap/ra_achievements.txt`**. One line, the server's own memaddr syntax,
no rebuild. If the file is absent the binary falls back to its built-in self-test, and
`rcFromFile` at `+0x98` says which one is running — because a definition that does not
unlock is a very different problem depending on whether the file was picked up at all.

This is also phase 3's mechanism in miniature. When the launcher eventually logs in and
fetches a real set before the game boots, the definitions will travel exactly this path:
launcher → staging buffer → DSi WRAM. Building it now for a hand-typed file means the part
that has to work under a network later is the part already exercised.

**Where it lives.** The block sits at the *top* of the 256 KB window,
`0x03778000`–`0x03780000`, and the heap is shortened to stop below it. Putting it inside
the image the loader copies would have been free — the loader already copies 128 KB into a
68 KB image — but it would have landed in memory the allocator hands out. The cost is
`heapSize` going from 188 KB to **156 KB**, which is still room for well over a hundred
achievements at the ~1 KB each we measured.

**What is distrusted.** The file is the one input here that does not come from us, so it is
length-checked by the launcher before a byte is read — deliberately not through
`loadCardEngineBinary()`, which reads a whole file into its destination unbounded, fine for
a binary this project ships and not fine for a text file a user edits. It is terminated
again on the WRAM side regardless of what the file contained, trailing whitespace is
trimmed because a text editor adds a newline and rcheevos would reject it as syntax, and
the result goes to the same parser that will one day receive strings from the server. It
gets no more faith than those will.

### The definitions to try, and where they came from

The achievement set's logic is not public: `dorequest.php?r=patch` needs credentials and the
Web API needs a key. The game's **code notes** are, and they are better for this purpose —
documented addresses with their meanings, published by the people who wrote the set.

`tools/ra_achievements.example.txt` carries three definitions built from the notes for
*Space Invaders Extreme*, in increasing order of what they can prove. Whether the string
came from the server or from us changes nothing about what rcheevos has to do with it; what
matters is that the syntax is real and the memory is real.

```
0xX1593d0=0                                        current stage is 1
0x159992>d0x159992                                 a Red->Red round was just completed
I:0xW159164_0xX00009c=2_I:0xW159164_d0xX00009c!=2  entered Fever Time
```

The third is the one worth the session. `I:` is AddAddress — RetroAchievements' pointer
chain — and it reads the 24-bit game-state pointer at `0x159164`, then the 32-bit state at
`pointer + 0x9c`, where `0x02` is Fever. The pair says "Fever now, not Fever last frame",
which is the standard shape for an achievement that fires on a transition.

Two things in it reach code nothing else does. **The computed address is not a memref**, so
`rc_runtime_validate_addresses()` never sees it — which is exactly why `peek()` validates
every read as well, and this is the first test of that. And when the pointer is null or
garbage between scenes, the computed address gets refused and counted in `rcPeeksRejected`
rather than dereferenced, which on this platform is the difference between a false negative
and a Data Abort inside the game's interrupt handler.

24-bit is not a detail either. The console stores `0x0215xxxx`; dropping the top byte is
what turns a DS address into the console address RetroAchievements definitions are written
in, which is why the notes call these "24-Bit Pointers".

### First run with real definitions: the machinery works, the definitions do not

Every mechanical part passed on the first attempt, and nothing fired.

| Field | Read | |
| --- | --- | --- |
| `rcFromFile` / `rcActivated` / `rcBadLine` | `1` / `3` / `0` | all three parsed, including the pointer chain |
| `rcPeeks` | `4` | exactly the four distinct memrefs, so AddAddress is walked every frame |
| `rcPeeksRejected` | `0` | the computed address was always in range |
| `rcLines` / `rcLinesMax` | `2` / `2` | still two scanlines of 263 |
| `rcInitLines` | `37` | parsing three definitions, once |
| `rcTriggered` | **`0`** | after reaching Fever Time |
| `rcEvents` | `2` | two triggers went WAITING → ACTIVE |

That combination is informative rather than disappointing. `rcPeeks = 4` with
`rcPeeksRejected = 0` says the AddAddress chain resolves and reads real memory every frame,
which was the thing this run existed to test. `rcEvents = 2` says two of the three left
WAITING and are actively evaluating — the third, `stage == 0`, was true at activation and
rcheevos requires a trigger to be false once before it may fire, exactly as expected.

So the conditions are not being met, and that is a fact about this ROM rather than about
rcheevos. The definitions were written from published code notes; something in them does
not describe the copy being played.

**The response is not another guess.** A guess costs a play session. Reading the addresses
costs nothing extra, because the watchlist already resolves chains and reports values into
the snapshot — so the definitions file now also carries watches:

```
W:<address>:<size>[:<offset>[:<offset>]]
```

`W:159164:4:9c` resolves the same chain the Fever definition uses and puts the resolved
address and the value in `results[1]`. If the address looks like a plausible `0x02xxxxxx`
the pointer is real and the question is the offset or the meaning; if `status` is
`BAD_POINTER` the word is not a usable pointer at all. Either way the next session answers
"what does this memory hold" instead of "did my next guess work", which is the difference
between measuring and betting.

Any watch line replaces the built-in self-test watches, so the first four land in
`results[]` where a hex viewer can read them.

### The watches found it: RetroAchievements' DS pointers are 24-bit

Measuring instead of guessing paid immediately. Three of the four watches resolved and one
did not, and the pattern is the answer:

| | Resolved | Value | Status |
| --- | --- | --- | --- |
| Current Stage `0x1593d0` | `0x021593D0` | `0` — Stage 1 | OK |
| game state via `0x159164 → +0x9c` | — | — | **BAD_TARGET** |
| Stages Completed `0x1593c4` | `0x021593C4` | `0` | OK |
| Red→Red rounds `0x159992` | `0x02159992` | `0` | OK |

Three direct reads landing on plausible addresses with plausible values settles the first
question: **the code notes do describe this ROM**. The addresses are right.

The chain failing while `rcPeeksRejected` stayed `0` is what settles the second. Our walker
read the word at `0x02159164` as a 32-bit DS address, added `0x9c`, and got something
outside main RAM. rcheevos read the *same location* as 24 bits, added `0x9c`, and got a
console address it was happy with. Two readers disagreeing about one word is what
identified the model as wrong rather than the address.

**RetroAchievements' DS pointers are 24-bit console pointers, and the walker was treating
them as DS addresses.** That is not a quirk of this game; it is what the map means. The
notes say "[24-Bit Pointer]" precisely because the low 24 bits *are* the console address —
a 32-bit DS pointer of `0x02xxxxxx` would be past the 4 MB the map covers, so the top byte
is dropped by definition. Whatever this game keeps in that top byte, it is not part of the
pointer.

So the walker learned it: `RA_WATCH_FLAG_PTR24` masks each mid-chain pointer to 24 bits and
adds main RAM's base, and the file selects it with `W24:` instead of `W:`. rcheevos already
did this, which is why its side never complained — the fix brings our walker in line with
the library rather than working around it.

The host test now reproduces the hardware failure exactly: the same watch resolves with the
flag and returns `BAD_TARGET` without it, against a cell whose top byte is deliberately not
`0x02`. A regression test derived from an observation rather than from a guess.

### The zeros were the wrong cartridge, not the wrong walker

With the mask in, the chain resolved -- `status` went `BAD_TARGET` to `OK` -- and landed on
`0x0200009C`. That is main RAM's base plus the offset, which is exactly where a null pointer
lands, and a direct watch on the raw word confirmed it: `0x02159164` held **zero**. So did
Current Stage and Stages Completed, during active play.

Three zeros can be an uneventful moment. A null pointer during gameplay cannot. That
combination is the signature of reading the wrong memory, and the explanation turned out to
be the dullest available: **the code notes are for *Space Invaders Extreme 2*, and the
cartridge being played was the first game**, which has no achievement set at all. Right
memory map, wrong cartridge.

**And the 24-bit masking "fix" was reasoned wrongly, which the correct cartridge then
proved.** Running the same watches against Extreme 2, the raw word at `0x02159164` reads
`0x02159158` — a perfectly ordinary DS address with `0x02` in the top byte. Masking it to
24 bits and adding main RAM's base gives `0x02159158` again. **The mask changes nothing
here.**

So the disagreement that motivated it was never about masking semantics. It was about a
null pointer: unmasked, `0 + 0x9c` is `0x9c`, outside main RAM, and the walker correctly
refused; masked, `0 + 0x02000000 + 0x9c` is `0x0200009C`, inside main RAM, and the walker
happily resolved to nothing. **The mask turned a visible failure into a false success**, and
that is worth stating plainly rather than filing under "harmless".

The code stays, because it is what RetroAchievements means and what rcheevos does — a game
that stores flags in the top byte of a pointer would need it, and the DS map is
console-relative by definition. But it was added for a reason that turned out to be wrong,
and its only observed effect so far has been to hide a null pointer. The host test that
came with it still pins the semantics; what it does not do is justify the change on this
game's evidence, because this game does not need it.

This is where the file format earned itself. Three diagnostic rounds — the wrong cartridge,
the mask, and the correct cartridge — cost a text edit and ten minutes of play each.
None cost a build or a flash, which is what the definitions file was introduced to avoid,
one round before it turned out to be needed.

### The canary round, and knowing when to stop paying for a game

The mask round left one question open — do these notes describe this cartridge at all — and
the first attempt to answer it was a badly designed test. The canary chosen was *Extreme
Mode Unlocked*, and it read zero. Zero is what that address reads on a freshly downloaded
ROM with no save file, whether the addressing is right or wrong. **A test that returns the
same answer either way is not a test**, and asking for a hardware session to run it was the
mistake, not the reading it produced.

The replacement was `0x13c9c4`, *In Title Screen/Records Loop*: runtime state, no save
involved, and a state the console can be put into deliberately. Two readings, one on the
title screen and one during play, with three distinguishable outcomes — changed, both zero,
or unchanged nonsense.

The answer was the third, and two other watches agreed with it:

| | Title screen | In game | The notes say |
| --- | --- | --- | --- |
| Current Stage `0x1593d0` | `0x02` | `0x79` | `0x00`–`0x1d` |
| game state pointer `0x159164` | `0x02159158` | `0x02159158` | a pointer that moves between scenes |
| Main Menu information `0x155288` | `0` | `0` | non-zero on the menu |

`0x79` is outside the documented range of the field entirely, and a game-state pointer that
holds the identical word on the title screen and mid-run is not a game-state pointer. So the
memory is live — these are not unmapped reads, they return plausible-looking DS values — and
the fields are not the fields. **A systematic offset between the notes' addressing and this
dump**, which no further watch can correct.

The obvious next move was `md5sum` against the set's supported files. The user could not
find a dump that matched, and at that point the honest thing is to stop: every further round
costs a play session to re-confirm something already known. Changing games is cheaper than
chasing a hash.

### Super Mario 64 DS, chosen for the sentence at the top of its notes

> This Real Set has ONLY one ROM. It's EU ROM. All addresses are from EU ROM.

That single line is why this game replaces *Extreme 2*. One supported ROM means there is
exactly one right answer to "is this the dump the notes describe", and it can be settled
with a hash **before** the console is switched on. Three rounds were spent discovering
by measurement what a minute of arithmetic can now decide.

The canary improves too. The old one could only report *changed* or *did not change*, and
noise can produce a change. Screen ID at `0x8e43c` reports a **specific documented byte** —
`0x37` on the Main Menu, `0x38` on File Select — and noise cannot produce `0x37`. Map ID at
`0x9f2f8` gives a second predicted value from an unrelated address, and two independent
addresses agreeing is what rules out coincidence.

The character pointer at `0x9b450` is read **raw** rather than followed. Its value is the
evidence, and following it would hide a null — which is precisely what the 24-bit mask did
one round earlier. The chain still gets exercised, by rcheevos rather than by the walker,
through a guarded definition:

```
0xH08e43c=h38_d0xH08e43c!=h38          reached File Select
0xH09f2f8=h06_d0xH09f2f8!=h06          entered Bob-omb Battlefield
0xW09b450!=0_I:0xW09b450_0xX00005C!=0  Mario is loaded, and the pointer is real first
```

The first fires within seconds of boot, before a level is loaded — so if the addresses are
right, something unlocks almost immediately and the round is decided without playing. The
third is the one that reaches code nothing else does: the guard condition exists because a
null pointer plus `0x5C` still lands inside main RAM and reads whatever happens to be
sitting there, which would be a fire that means nothing.

### `h38` and not `0x38`: the example file now goes through the parser

Those three definitions were first written as `0xH08e43c=0x38`. In memaddr an operand
beginning `0x` is a **memory read**, not a hex constant — so that line compared Screen ID
against the 16-bit word at address `0x38`. Hex constants take an `h` prefix; bare digits are
decimal.

It would have parsed cleanly, activated cleanly, reported `rcActivated = 3` and
`rcBadLine = 0`, and never fired. Every field in the snapshot would have said the round was
working. That is the worst failure mode this system has, and it would have cost a play
session to not-quite-diagnose.

The file had been rewritten four times by then and had **never once been through the
parser**. It is the one document the user edits, and the only part of the system whose
errors are silent. So the host test now reads `tools/ra_achievements.example.txt` off disk,
stages it into the definitions block exactly as the bootloader would, and runs the real
`ra_rc_init()` over it: no rejected lines, three definitions activated from the file rather
than the built-in fallback, four watches installed, and the total still inside the
eight-line split limit. Seconds instead of an evening, and it fails on the syntax error that
motivated it.

### It fired. Three real definitions, on hardware, from published code notes

Two readings settled it, and every field agreed.

**Main menu.** `results[0]` resolved to `0x0208E43C` and read **`0x37`** — the exact byte the
notes predict for the Main Menu, from a set of addresses this project had never touched
before. That single value ended the question the previous three rounds could not answer.

| Offset | Field | Menu | Bob-omb Battlefield |
| --- | --- | --- | --- |
| `+0x34` | Screen ID `0x8e43c` | **`0x37`** — Main Menu | `0x3A` |
| `+0x40` | Map ID `0x9f2f8` | `0` — no map loaded | **`0x06`** — Bob-omb Battlefield |
| `+0x4C` | Coins `0x9f358` | `0` | `5` |
| `+0x58` | character pointer `0x9b450` | `0` — nobody loaded | **`0x02188A38`** |
| `+0x70` | `rcTriggered` | `0` | **`3`** |
| `+0x86` | `rcEvents` | `3` | `6` |

All four watches reported `status = 2` (`RA_WATCH_OK`) in both readings, at the addresses
they were asked for. `rcFromFile = 1`, `rcActivated = 3`, `rcBadLine = 0`, `rcActivate = 0`
(`RC_OK`), `rcDefLength = 0x1291` — the file was read off the SD card, all three definitions
parsed, none was rejected.

**`rcTriggered = 3`: every definition fired, and each for its own reason.** Reaching File
Select, entering Bob-omb Battlefield, and the guarded pointer chain finding Mario loaded.
`rcEvents` going `3 → 6` is the corroboration: three triggers left WAITING at activation,
and three later reached TRIGGERED — two events each, no spurious ones.

The pointer chain is the part worth dwelling on. `rcPeeks = 4` with `rcPeeksRejected = 0` in
both readings says `AddAddress` was walked every frame and never once produced an address
outside the map. On the menu the pointer read `0` and the guard held the definition false;
in the level it read `0x02188A38` and the chain resolved. That is exactly the null the
24-bit mask hid one round earlier, now behaving correctly because the guard is in the
definition where it belongs.

Cost: `rcLinesMax = 2` scanlines of 263, unchanged from the self-test, and `heapUsed = 3128`
bytes for the runtime plus three achievements.

**What this proves and what it does not.** Published code notes → real memaddr syntax →
rcheevos evaluating against a running retail DS game on a 3DS → triggers firing on the right
frames. Delta memrefs and `AddAddress` both exercised against real game state, which the
self-test deliberately could not do. What it is *not* is an unlock: nothing has been sent
anywhere. The trigger fired locally, which is the entire client-side half of an achievement.

### The next task: telling the server

Everything below the client is now proven on hardware, and so is the evaluation itself. What
is missing is the two ends around it: knowing *which* achievements a game has, and reporting
that one fired. Both are network, so open question #1 — transport — is now the critical
path with nothing left in front of it.

The WiFi probe already reached RetroAchievements over plain HTTP from this exact 3DS
(stage 6/6, WPA2-PSK). What remains is moving that from a standalone DSi-mode homebrew into
nds-bootstrap's launcher, and deciding how a definition set travels from `r=patch` to the
definitions block that this round proved works.

### Step two, wired: the chip's bring-up moved into the launcher

`RA_LAUNCHER_WIFI=1` builds nds-bootstrap with dsiwifi's **ARM7 half** linked into the
launcher. The ARM9 sends one IPC message before the game boots, dsiwifi resets the Atheros
chip, launches its firmware, brings WMI up, associates and does the WPA2 handshake, and every
line it narrates is written verbatim to `/ra_wifi_launcher.log` next to the probe's
`/wifiprobe.log`. Then it stops.

Built on the pinned toolchain — devkitARM r65 / gcc 14.2.0 / libnds 1.8.0, from
`devkitpro/devkitarm:20241104`, the same pin upstream CI uses.

### Step two passed on hardware, and the log says the boot path changes nothing

**`reached stage 5 of 5` on a 3DS.** All five rungs: the chip answered on SDIO, the firmware
launched, WMI came up, it associated to the AP, and the WPA2 four-way handshake completed with
the GTK installed — **inside nds-bootstrap's launcher, on nds-bootstrap's own ARM7**. The full
log is committed at `docs/logs/ra_wifi_launcher-3ds.log`.

**The register that the whole question hung on:**

```
SCFG_EXT  ARM7   93FFFB06
SCFG_EXT7 BIT(18) set
```

`0x93FFFB06` is the **exact value `tools/wifiprobe/` writes into `REG_SCFG_EXT` itself**. The
launcher writes SCFG nowhere and inherits whatever launched it, and the concern was that the
extended TWL I/O might not be open on that path — so the WiFi SDIO block might not be on the
bus at all. It is open, to the bit, in both contexts. That concern is not mitigated; it is
gone, and by measurement rather than by inference.

**And the bring-up is identical to the control, value for value.** Every number the probe's
run recorded appears unchanged in the launcher's:

| | Control (`wifiprobe`) | Launcher (`RA_LAUNCHER_WIFI=1`) |
| --- | --- | --- |
| chip | `Mfg 02010271 Cid 0d000001 (AR6014)` | identical |
| arrival | `AR6014 needs firmware upload 0.` | identical — **cold** |
| reset cause | `00000002` | identical |
| BMI version | `2300006f` | identical |
| firmware | `609c0202 ready, handshaking...` | identical |
| device MAC | `04:03:d6:f9:36:52` | identical |
| AP | `MuMiMo24` / `00:5f:67:e9:f5:70` / `G TKIP P AES A PSK` | identical |
| handshake | `1/4` … `3/4` … `Added GTK 1` … `Done auth` | identical |

So the answer to the question step 2 was posed to ask — *does the chip arrive in a different
state under nds-bootstrap's boot path* — is **no, in every observable respect**. It arrives
cold both times, and `needs firmware upload 0` is the one line that says so.

Two honest limits on that comparison. The control column is the **excerpt recorded in this
document**, not a fresh full log — the probe was re-run but its log never made it back, so the
diff is one-sided: every value present in both matches, and lines the excerpt never captured
cannot be compared. And the launcher's log carries detail the excerpt does not (`Resetting
SDIO...`, `Rev: 11`, `HTC_MSG_READY`, `WMI_REG_DOMAIN_EVENT 80000188`, a 13-channel list).
Those are not differences in behaviour; they are lines nobody wrote down the first time.

**There is no IP address in the log, and that is correct.** DHCP is lwip's, lwip is the ARM9's,
and this side links no lwip — the ladder stopped exactly where it was designed to. The probe's
`IP 192.168.0.111` has no counterpart here by construction, not by failure.

`log lines 36`, and no dropped-characters warning, so the 16 K capture buffer and the FIFO
reassembly both held on a real run.

**Sharing the ARM7 also turned out not to matter.** The driver ran alongside `my_sdmmc` on the
neighbouring controller with NDMA slot 0 already taken, and reached a usable WPA2 link. The
contention argued about under *#1d* does not bite in the launcher.

That is the whole of what the ARM7 can do, working as a guest. **Live unlocks are reachable
from context A**, and open question #1 no longer has an unknown in front of it — only work.

#### The first two runs of this reached stage 5 and wrote a zero-byte log

Worth keeping, because the reasoning that produced it looked careful:

> `fflush()` does not make a file real on libfat. It pushes newlib's stdio buffer down into
> libfat's `write()`, which does write the data clusters — but a FAT file's **length lives in
> its directory entry**, and libfat writes that only from `_FAT_syncToDisc()`, reached from
> `close()` and `fsync()` and nothing else. This probe halts deliberately and never closes
> anything, so the bytes were on the card and the metadata said the file was empty.

The comment justifying the never-closed file was inherited from `tools/wifiprobe/`, where it
is correct — that program `fclose()`s when you press START, so its log has content. Carrying
the reasoning across without carrying the `fclose()` is what left a 40-line answer
unreadable. `fsync()` after every write is the fix, and it is better than a close: it makes
the log durable *line by line*, which is what a run that hangs in one of dsiwifi's untimed
loops actually needs.

**A second thing the run exposed, which had not failed yet.** The log was being written from
inside the FIFO interrupt handler. On the DSi the SD card is driven by the **ARM7**, over the
FIFO — so that was FIFO traffic from inside a FIFO interrupt, against an ARM7 that was at
that moment running a WiFi stack. It got away with it twice. The handler is now a `memcpy`
into a 16 K buffer and the main loop does all the I/O, which is both safer and the right
shape for step 4, where the same code runs next to a game.

With both fixed, the third run produced the log at the top of this section — 36 lines and a
summary, which is what turned "stage 5" from a screen reading into an account of *how* the
chip came up. Two hardware runs were spent on a bug in the instrument rather than in the
thing being measured, which is the cost of writing the log path as an afterthought to the
probe it serves.

#### What it actually asks, which is not quite what the plan said

The plan's step 2 was *"coming in through nds-bootstrap, is `WLANFIRM` already uploaded?"* —
and writing it made clear that the probe had already answered that, in a way that dissolves
the question: dsiwifi resets the chip into its BMI bootloader and relaunches the firmware
**every time, warm or cold**, and its firmware-upload path is `#if 0` besides. So `Reset
cause`, `BMI version` and `Launching!` prove nothing about how the chip arrived. Exactly one
line does — `%s needs firmware upload %lx`, printed only when the host-interest word at
`+0x58` reads zero — and on this console it was printed.

What was genuinely untested, and what this build was built to measure, is whether the same
driver comes up as a **guest of nds-bootstrap's ARM7** rather than of the libnds template. The
launcher is context A, but its ARM7 is not an ordinary one, and the differences all bear on the
WiFi SDIO block. Each row is why the build exists; the log above is the answer to all of them,
which is **none of them mattered**:

| | The probe's ARM7 | nds-bootstrap's launcher ARM7 |
| --- | --- | --- |
| SCFG | sets `REG_SCFG_EXT = 0x93FFFB06` itself | **inherits whatever launched it**; writes SCFG nowhere |
| SD/eMMC | untouched | already driving `my_sdmmc` at `0x04004800`, one instance below the WiFi SDIO at `0x04004A00` |
| NDMA | free | slot 0 taken by `driveInitialize()` |
| Timer 3 | free | free, but dsiwifi claims it on both CPUs |
| Idle loop | a normal `while` loop | `swiIntrWait(0, IRQ_FIFO_NOT_EMPTY)` |

The SCFG row is the one that could decide the run on its own, so the log opens with
`SCFG_EXT` from both CPUs before anything touches the chip. **It reports and does not
correct.** Opening SCFG here would have made this a measurement of a boot path nds-bootstrap
does not have; a closed bit would have *been* the finding. It came back `93FFFB06` — the value
the probe writes by hand — which is a better answer than any repair would have been, because
it means there is nothing to repair.

#### The ladder stops at the handshake, and that is where the question ends

Five rungs against the probe's six:

| Stage | Reached | What proves it |
|---|---|---|
| 0 | nothing | the ARM7 never narrated — see `SCFG_EXT` above it |
| 1 | chip | the SDIO manufacturer/chip-ID read answered (`Mfg ...`) |
| 2 | firmware | `Firmware ... ready, handshaking` — the Xtensa core is running |
| 3 | WMI | `... fully initialized!` — there is a working command channel |
| 4 | associated | `WIFI_IPCINT_CONNECT`, from `WMI_CONNECT_EVENT` |
| 5 | **link ready** | `WIFI_IPCINT_READY` — 4-way handshake done, GTK installed |

DNS, TCP and HTTP — the probe's stages 3 to 6 — are absent on purpose. Everything up to and
including the WPA2 handshake happens **on the ARM7 inside dsiwifi**; sockets are lwip on the
ARM9, and lwip is step 3. So the ARM9 side of this probe links no library at all, only
dsiwifi's IPC header — the whole thing is four of our own files and a Makefile switch.

**That split is not tidiness, and here is the number behind it** — measured from the linked
archive two sections down, not estimated. dsiwifi configures lwip with `PBUF_POOL_SIZE 512`
and `MEMP_MEM_MALLOC 0`, so the pools are static arrays, and the pbuf pool alone is **784,387
bytes** of `.bss`. The launcher's ARM9 is linked by
`retail/arm9/ds_arm9_ndsbs.mem` into `0x02280000`–`0x02338000` — **753,664 bytes total**, code
and heap included, and it cannot simply grow: `IPS_LOCATION` and `IMAGES_LOCATION` sit at
`0x02337000` and `0x02338000`. So lwip as
dsiwifi ships it **did not fit in the launcher**, which is a real piece of step 3 discovered by
writing step 2 -- and the reason step 2 was worth building separately rather than as the first
half of step 3. *Step 3a* above is what came of it.

#### Why the diagnostic build does not boot games

`wifi_card_wlan_init()` contains two **untimed** loops — `while (1)` waiting for the
firmware-ready flag, and `while (!wmi_is_ready())` — and they run inside a FIFO datamsg
handler on the ARM7. If the chip does not come up, that ARM7 is wedged, with a timer IRQ and
an AUX IRQ live. Handing that to the bootloader, which is about to overwrite the ARM7's code,
is not a thing to do for a measurement.

So the probe stops on its summary and `RA_LAUNCHER_WIFI` is off in every shipped build. That
also makes the reading unambiguous, which is the habit this document keeps paying for: a run
that halts on `reached stage N of 5` is a reading, not something to photograph before the
game covers it.

Two smaller decisions in the same spirit. `installWifiFIFO()` is called **after** the
launcher's own FIFO handshake completes, because installing it earlier would have let the
probe's own IPC message satisfy the `swiIntrWait()` that handshake waits on — and the ARM7
would then have run `SCFGFifoCheck()` before the ARM9 had sent `FIFO_USER_06`, silently
dropping the CPU-clock request. In a build that never boots a game that would not have been
visible; it would just have been wrong. And inbound IP packets are dropped **without**
acknowledging them: `wifi_host.c` stamps a free-marker six bytes below the buffer it is
handed, which is correct only for a buffer the ARM9 supplied through `INITBUFS`, and this
probe supplies none — so stamping would corrupt the ARM7's own mbox header. Nothing is lost,
because the WPA2 handshake is EAPOL and never comes that way.

### Step 3a — lwip in the launcher, and the pool that had to be cut

Step 3 is "the launcher fetches a definition set instead of reading one off the card", and it
has four parts: an IP stack, the RetroAchievements hash for a DS ROM, `r=login`, and `r=patch`
plus parsing. Only the first is a question about the platform; the other three are work. So it
goes first and alone, and the rung is deliberately the same one the standalone probe already
cleared: **reach the RA API over plain HTTP**. No credentials, no hash, no JSON — the request
logs in as a user that does not exist, and a well-formed `invalid_credentials` reply proves
DNS, TCP, HTTP and the API parsing our query.

The ladder therefore grows from 5 rungs to 9: `IP` (DHCP), `resolved`, `connected`,
`answered` on top of step 2's five. Everything at or below rung 5 is already proven on
hardware, which is what makes a failure above it unambiguous — it is the new code.

#### It reached the server. `stage 9 of 9`, from nds-bootstrap's launcher

```
IP               192.168.0.112
resolved         retroachievements.org
its address      104.26.2.251
connected        port 80
request sent
994 bytes back
the API answered over plain HTTP
body: {"Success":false,"Status":401,"Code":"invalid_credentials", ...}
```

Full log at `docs/logs/ra_wifi_launcher_http-3ds.log`. **Open question #1 — the network
transport — is closed for context A, end to end**: DHCP, DNS, TCP and HTTP from inside
nds-bootstrap's launcher, to RetroAchievements' own API, with the API's own error code coming
back rather than a captive portal's idea of one.

`994 bytes back` is the same byte count the standalone probe got, which is the kind of
corroboration worth noticing: same endpoint, same reply, different program. The address differs
(`104.26.2.251` here, `104.26.3.251` there) because Cloudflare has more than one edge, and the
lease differs (`.112` against `.111`) because it is a different lease. Everything about the
chip is unchanged from the step-2 log — same `Mfg`, same cold arrival, same firmware version,
same MAC, same BSSID — from a build that had since had an entire IP stack linked into the same
ARM9.

Which means **3b, 3c and 3d are work and not questions.** Nothing above this line needs another
platform experiment: the hash, `r=login` and `r=patch` are code, and the block they feed is
already proven to fire real achievements. That reading held — all three are written, and the only
one of them that turned out to have a genuine unknown in it is `r=patch`, where the unknown is a
*size* rather than a platform behaviour.

#### The hang after `request sent`, which was the one place a probe could still block

The build that added 3b froze twice on the line after `request sent`, and the cause was not 3b.
It was an unbounded `recv()`.

The reply is 994 bytes, so the first `recv()` returns all of it and the loop asks again. That
second call waits for the server's FIN — and `Connection: close` is a request, not a promise. If
the FIN is late or lost, a blocking `recv()` with no timeout waits for it forever. The earlier
run reached stage 9 because its FIN arrived promptly; the next one did not. **The difference was
luck, not code**, which means the earlier `stage 9 of 9` was a real result obtained by a probe
that could have hung at any time.

Every other wait in this file is bounded — `raWifiWaitStage()`, `raWifiWaitIp()`,
`raWifiWaitArm7()` all count frames and give up — because *a probe that hangs teaches nothing*
is the rule this document keeps restating. The socket read was the one place that rule was not
applied, and it is the one place lwip can block indefinitely.

Fixed with `SO_RCVTIMEO`, which dsiwifi's lwipopts enables, plus a report of what each call
returned. A timeout with bytes already in hand is not a failure — it means the reply arrived and
only the close is missing, which the API's own error code being present settles — so the log now
distinguishes `peer closed after 994` from `recv stopped after 994` from `recv stopped after 0`.
Three different worlds that all used to look like a frozen screen.

The next run confirmed the diagnosis exactly: **`recv stopped after 1000`**, then `1000 bytes
back`, then the API's own JSON, then `stage 9 of 9`. The reply had arrived in full and only the
close was missing — so the freeze was never a network failure, and the run that hung and the run
that succeeded differed by nothing except whether one TCP FIN turned up.

The same build also reports free heap after the hash, once lwip is up, and after the HTTP
exchange. The first version of that report was itself misleading; see *Step 3b* for the
correction and for the ~96 KB that step 3d actually gets.

#### The one alarming line in the log, which is not ours and is not a problem

```
netif is not up, old style port?WMI_CONNECT_EVENT len ca
```

Worth chasing rather than shrugging at, and it turns out to be an operator-precedence bug
upstream. `wifi_host_tick()` reads:

```c
if (host_bLwipInitted && ath_netif.ip_addr.addr == 0xFFFFFFFF || ath_netif.ip_addr.addr == 0x0)
```

which groups as `(a && b) || c`. `ath_netif` is a zeroed static, so `addr == 0` is true **before
lwip has been initialised at all** — and the guard that was meant to prevent exactly that,
`host_bLwipInitted`, is on the wrong side of the `||`. The tick runs at 1 kHz from timer 3 with
a once-per-second gate, so about a second after `wifi_host_init()` it calls `dhcp_start()` on a
netif that was never added or brought up, and lwip's own `LWIP_ERROR` says so. That is exactly
where the line lands in the log: just before association.

Harmless: `dhcp_start()` validates and returns `ERR_ARG`, and once there is an address the
condition goes false, so the prodding upstream *intended* still works. Left alone rather than
patched, because it is in the submodule and it costs nothing — but pinned by the host test, so
if a bump ever fixes the precedence this note can go rather than quietly becoming wrong.

#### The pool was bigger than the region, so the pool was cut

The measured obstacle from step 2: dsiwifi configures lwip with `PBUF_POOL_SIZE 512` and
`MEMP_MEM_MALLOC 0`, so the pools are static arrays, and `memp_memory_PBUF_POOL_base` alone is
**784,387 bytes** of `.bss` against a **753,664-byte** link region for the entire launcher.

Those numbers are right for homebrew that owns all 16 MB. They are not a bug and not something
to argue with upstream — they are simply not this program's budget, which is one HTTP GET
before a game boots. So `retail/dsiwifi9/include/lwipopts_ndsbs.h` cuts them:

| | dsiwifi | ours |
| --- | --- | --- |
| `PBUF_POOL_SIZE` | 512 | 32 |
| `MEMP_NUM_PBUF` | 1024 | 64 |
| `MEMP_NUM_TCP_SEG` | 64 | 24 |
| `MEMP_NUM_NETCONN` | 32 | 8 |
| **static `.bss` in the library** | **833,410** | **75,674** |

Eleven times smaller. The sizing is not aggressive: 32 receive buffers against a `TCP_WND` of
two MSS is roughly an order of magnitude over what can be in flight, and running the *receive*
pool dry stalls a transfer rather than failing it — a bug that would present as "the network
is slow", which is the worst kind to design in.

**The other option was checked first and is not available.** Giving the launcher more room
would have needed no submodule work at all, but `retail/arm9/ds_arm9_ndsbs.mem` puts it at
`0x02280000`–`0x02338000` and it is boxed in on both sides by things the launcher itself uses:
`IMAGES_LOCATION` at `0x02338000`, where `conf_sd.cpp` decompresses the boot images the
bootloader later displays, and `CARDENGINE_ARM9_SLOT2HEAP_LOCATION_BUFFERED` at `0x0227F800`
just below. Growing the region upward would put the launcher's own `.bss` on top of the images
it writes.

With the pools cut, the ARM9 region goes from 401,312 bytes used to **562,608 of 753,664**,
leaving **191 KB of heap**. That is comfortable but no longer generous, and it is the number to
watch: lwip's send path allocates from `malloc` (`MEM_LIBC_MALLOC` is 1), so the heap is now
shared between libfat, the launcher's own allocations, and the network.

#### Two mechanisms worth writing down, because both cost an attempt

**No include path can override lwip's options.** `opt.h` does `#include "lwipopts.h"`, and for
a quoted include the compiler searches the including file's *own directory* first — where
dsiwifi's `lwipopts.h` sits, next to `opt.h`. `-I` loses. `-iquote` loses. What works is the
header guard: our file is force-included with `-include` ahead of every translation unit, pulls
in theirs by explicit relative path so `__LWIPOPTS_H__` gets defined, then overrides. When
`opt.h` later asks, it gets nothing and our values stand. Including their file rather than
copying it means everything unlisted still tracks the submodule.

**And the submodule's build offers no way in**, which is why this is a separate library in our
tree rather than a flag passed down. Its `CFLAGS` and `INCLUDES` are `:=` assignments, so a
command-line override replaces rather than extends them, and `release` builds both CPUs at
once so anything passed would hit the ARM7 half too. `retail/dsiwifi9/` compiles the same
sources with the same code-generation flags the submodule uses — the only difference between
what we build and what it builds is the sizing header.

#### Who owns FIFO_DSWIFI, and what that cost

Linking dsiwifi's ARM9 half means `wifi_host_init()` installs its own datamsg handler on
`FIFO_DSWIFI` and drives the sequence itself. Two owners of one channel is not a thing, so the
probe stopped speaking IPC directly and now calls `DSiWifi_SetLogHandler()` and
`DSiWifi_InitDefault(WFC_CONNECT)` — **the same two calls `tools/wifiprobe/` makes.** That is
an improvement in its own right: the control and the measurement now enter through the same
door, so a difference between them cannot be ours.

What it costs is that rungs 4 and 5 used to arrive as the driver's own signals —
`WIFI_IPCINT_CONNECT` and `WIFI_IPCINT_READY` — and are now read out of its prose like the
rest, matching `WMI_CONNECT_EVENT` and `Done auth`. `Done auth` rather than `Added GTK`
because the GTK line is WPA2-only and `wmi_post_handshake()` prints `Done auth` on the open and
WEP paths too; a rung that silently cannot be reached on some networks is worse than a
slightly weaker one.

The committed step-2 log validates that substitution for free: it was captured *before* this
change and still contains both lines, so the host test now asserts that the text-based reading
agrees with what the IPC-based one reported at the time. Same bytes, same verdict, different
mechanism.

### Step 3b — the ROM's hash, and rcheevos as the reference rather than the implementation

Without this the launcher cannot ask the server *which* set to fetch. It is the last part of
step 3 that needs no password, and the only one whose correctness is a single number that can
be checked against RetroAchievements' own site rather than by playing.

**Done, and verified against the server's own database.** The probe logs the hash of the ROM it
was pointed at, before it touches the network, and for Super Mario 64 DS it matches what
RetroAchievements has.

#### rcheevos defines it; we had to implement it anyway

`rc_hash_nintendo_ds()` is already in the vendored submodule, and the first version of this
simply called it. Then it got measured. It allocates `max(0xA00, arm9_size, arm7_size)` in
**one block** so it can hash each region from memory — 353,164 bytes for nds-bootstrap's own
`.nds`, 382,212 for a real game. That allocation succeeds and is exactly the problem: the
launcher's heap is not bounded by anything useful, so a block that size walks straight over the
boot images it has already written. See *The heap took three attempts to report* below, which is
where that was finally understood.

So `ra_hash.c` streams the same four ranges — 352 bytes of header, the ARM9 code, the ARM7
code, 2,560 bytes of zero-padded icon block, plus the 512-byte SuperCard skip — through one
1 K buffer. Fixed cost, any ROM size, and the right shape for the shipped feature where this
runs on every boot.

**Which makes divergence the entire risk**, and it is the nastiest failure mode in step 3: a
hash over almost-the-right bytes is a well-formed MD5 that the server does not recognise, and
on hardware that is *indistinguishable from a game with no achievement set*. No play session
could tell those apart.

So `tools/ra_hash_test.c` compiles the real `rc_hash_nintendo_ds()` — only there, never into
the launcher — and requires the two to agree on real `.nds` files, whichever ones the build
produced. rcheevos stays the definition; ours is an implementation of it that cannot drift in
silence. Both agree today:

```
retail/bin/nds-bootstrap-nightly.nds   d2f9350db41ccd1e821ed5a4420351c5
tools/wifiprobe/wifiprobe.nds          a100b64c97ebea15684372b31505ae82
```

Only rcheevos' `md5.c` is linked into the launcher — the hashing, not the file plumbing. The
whole of 3b costs about 6.5 K of the ARM9.

#### On a real game the number is 382,212, and rcheevos' own function could not have run

The first hardware run of 3b, on *Super Mario 64 DS* — log at
`docs/logs/ra_wifi_launcher_hash-3ds.log`:

```
hash             c3b1916756737f2c4117cc95c1d51ac7
arm9 / arm7      382212 / 150308 bytes
would malloc     382212 bytes
```

**382,212 bytes in one block** — and what that number means took two further attempts to get
right, which is its own section below. The short version: it would have allocated fine, because
there are 12.8 MB behind `fake_heap_end`, and then overwritten the launcher's own boot images by
294 K. Streaming is a requirement; the mechanism is not the one first written here.

**And the hash is the one RetroAchievements has**, confirmed against the set's page.

Worth recording that this was doubted on the wrong grounds. The file is named
`... Super Mario 64 DS (Europe) ... (patched).nds`, and the reasoning was that an AP patch
rewrites the ARM9 binary while the ARM9 binary is most of what the hash covers — so the hash
would not match, and `r=patch` would answer "unknown game" for a reason unrelated to 3b. That
inference was wrong: this dump is patched *outside* the four hashed ranges, so it boots patched
and still hashes as the supported ROM.

The general caution survives — a patch that touched the ARM9 binary would change the hash, and
nds-bootstrap applies AP patches at run time anyway, so a pre-patched file is never required.
But for this ROM the question is settled, and **3b is verified against the server's own database
rather than only against rcheevos.** That is the one check no amount of local testing could have
produced.

#### The heap took three attempts to report, and the truth is a hazard rather than a number

Both wrong versions were wrong in a *flattering* direction, which is the reason this gets a
section instead of a footnote.

**First version:** `heap after hash 8528 free of 96452`. Reads as almost out of memory, and is
not — `mallinfo().arena` is what newlib has claimed by `sbrk()` so far, not what is available.
`usmblks` is not filled in by this newlib either, so `largest 0` meant nothing.

**Second version:** `fake_heap_end - sbrk(0)`, which reported **13,438,976 bytes**. True, and
useless. libnds sets `fake_heap_end` from main RAM and knows nothing about nds-bootstrap: on
hardware it measured `0x02FF3794`.

**What actually bounds the heap is `IMAGES_LOCATION`, and nothing enforces it.** The launcher
decompresses the boot images to `0x02338000` for the bootloader to display, and
`ds_arm9_ndsbs.mem` ends the link region at exactly that address — but the enforcement libnds
would provide lives in `fake_heap_end`, and that is set **12.8 MB higher**. So malloc will grow
straight through the images, and then through the RA staging block at `0x02600000`, the
cardengine staging at `0x026F0000`, the ARM7's at `0x027B2000` and the NDS header at
`0x027FFE00`. Silently. The failure would surface as a corrupt boot screen, or a cardengine
that starts and dies inside code that was overwritten after it was staged.

Measured on the run at the top of this section:

| | |
| --- | --- |
| end of static data | `0x0230AF30` |
| heap top after the HTTP exchange | `0x02322794` (96,356 claimed) |
| `IMAGES_LOCATION` — the real ceiling | `0x02338000` |
| `fake_heap_end` — what libnds allows | `0x02FF3794` |
| **safe headroom** | **88,172 bytes** |

**Which retires the reason given earlier for streaming the hash and replaces it with a better
one.** `rc_hash_nintendo_ds()` wanting 382,212 bytes in one block would *not* have failed to
allocate — there are 12.8 MB behind `fake_heap_end`. It would have succeeded and overrun
`IMAGES_LOCATION` by **294,040 bytes**, quietly destroying the boot images the launcher had
just written. So streaming was necessary, and the earlier claim that it "does not fit" was
wrong about the mechanism while landing on the right decision.

**And ~88 KB is the budget step 3d gets**, which settles a design question before it is asked:
`r=patch` must stream its reply, not buffer the set.

#### The test harness had a landmine in it, and adding two files stepped on it

Putting the hash check inside `tools/ra_reader_test.c` made the suite **segfault at `-O1` and
pass at `-O0`**, in a test several sections *before* the new code ran. That is worth the
paragraph, because the cause is not the new code at all.

That file defines `__bss_start`, `__bss_end` and `__vram_top` as 1-byte dummies, because the
cardengine takes their addresses: `ra_startup(__bss_start, __bss_end, ...)` zeroes the range
between the first two and hands out an arena starting at the second. On the target those are
linker-script symbols spanning a real window. On the host they are whatever the linker decides,
and adding rcheevos' `hash.c` and `hash_rom.c` to that link put `__vram_top` **below**
`__bss_end` — a span of **−1 bytes** — so the allocator's arena became the entire process.

It had always been luck. `fakeArena[0x3B000]` sits in that file unused, which says the intent
was there and was never wired to the symbols.

The fix here is deliberately narrow: the hash check is a **separate binary** that joins none of
that link and needs none of it — no cardengine sources, no fixed link address, no mapped pages,
because a hash is file I/O and an MD5. Making the arena explicit is a real fix and a separate
change; it is recorded under *Still open* rather than smuggled into 3b.

#### And it runs before the network

Two reasons, and neither is cosmetic. It needs no network, so putting it first means a failure
cannot be blamed on one. And it reads the ROM off the card while the heap is still whole —
`ra_hash.c` streams, but libfat still wants buffers, and there is no reason to make it compete
with lwip for them.

### Step 3c — the config file, and the one function that can lie convincingly

`r=login` is small: one GET, one token out of the reply. What it needs around it is a place to
keep credentials and a way to put them in a URL without corrupting them, and the second of those
is where the whole step can go silently wrong.

**The format is odelot's**, from his MiSTer core — `key=value`, `#` comments, credentials at the
top — because that is the file this project's user already has, and a format someone can copy
from a working example beats a tidier one they have to learn. `tools/ra.example.cfg` ships it;
the launcher reads `sd:/_nds/nds-bootstrap/ra.cfg`.

His file carries a dozen keys about popups and leaderboards that describe an overlay this fork
does not have. Rejecting them would make his file unusable here for no reason; accepting them
in silence would make a typo indistinguishable from a feature that is simply not built. So the
ones we know are counted as recognised-but-not-yet, and only genuinely unknown keys are
reported.

**The password stays in the file.** That is a decision on the record: caching only a token after
one login was offered and declined, so `r=login` sends the password in the clear on every boot
and the file sits readable on the card. What the code does do is keep it out of the log —
`raConfigRedact()` turns any secret into `(set, N chars)`, and the token gets the same treatment,
because a token grants the same power over the account and the log's entire purpose is to be
sent to someone.

#### It logged in. `stage 10 of 10`, and the heap did not move

```
ra.cfg           found
username         Bakke
password         (set, 10 chars)
10 keys parsed but not acted on yet
...
logging in as    Bakke
1089 bytes back
logged in, token (set, 16 chars)
```

Full log at `docs/logs/ra_wifi_launcher_login-3ds.log`. **A real RetroAchievements account,
authenticated from inside nds-bootstrap's launcher**, with the config file odelot's format
describes and no unknown keys reported — so his file loads here unchanged, which was the point
of adopting it.

And a number worth more than the rung: **`top 02325000` in all four heap reports.** The heap
never grew once during the run. lwip coming up, DHCP, two HTTP exchanges and a login together
cost about **184 bytes** of net allocation, out of the 10 K already free inside the claimed
arena. So the whole network path is allocation-cheap, and the budget it leaves for 3d is the
full **87,896 bytes** — 77,824 of safe growth to `IMAGES_LOCATION` plus 10,072 free where it
already stands.

That matters because it removes a worry rather than confirming one: the concern was that lwip
would eat the heap and leave `r=patch` nothing. It does not. What is left is whether a whole
achievement set fits in 88 K, which is a question about `r=patch` alone.

#### It answered `GameID 14856`, and that changes what is left

```
asking about     c3b1916756737f2c4117cc95c1d51ac7
884 bytes back
GameID           14856
```

Log at `docs/logs/ra_wifi_launcher_gameid-3ds.log`. **The hash question is now closed by the
server**, not by a comparison against a web page: RetroAchievements has this dump, under an ID
that `r=patch` can be asked about. The heap stayed at `top 02326000` through this rung too, so
the budget for the last request is **85,656 bytes** — 73,728 of safe growth plus 11,928 free.

So the entire network path is proven end to end, and **the last piece of step 3 is not a network
question at all.** It is a size question, and it has two numbers:

| | |
| --- | --- |
| heap available while lwip is up | 85,656 bytes |
| the definitions block the set has to land in | **32,760 bytes of text** |
| definitions the WRAM binary will parse | **`RA_DEFS_MAX_LINES` was 8** |

The 8 was the one that mattered, and it was not a bug — it was chosen when the definitions file
was a hand-typed line or three, and `docs` has said so since. A real set is a hundred or more
achievements, so `r=patch` could not be written without raising it, which meant touching
`ra_rcheevos.c` — code that is proven on hardware and fires real achievements. It is **128** now;
see *Step 3d* below for what that cost and what it broke on the way.

The 32,760 is probably enough and has never been checked: RA `MemAddr` strings run from tens to a
few hundred characters, so a hundred of them is plausibly 20-25 K. The measurement to take is the
one `r=patch` itself provides, and stage 12 reports it as `wanted`.

#### `r=gameid`: the one question only the server can answer

`r=patch` needs a `GameID`, and it comes from `dorequest.php?r=gameid&m=<hash>` — but the reason
to make that its own rung is that it settles something nothing local can. Step 3b proved the hash
matches what rcheevos computes, and the user compared it against the set's page. Neither is the
server saying *"I know this dump"*, and the difference is exactly what a trimmed, translated or
differently-patched ROM produces: a hash RetroAchievements has never seen, which looks precisely
like a game with no achievement set.

Unauthenticated, so it does not depend on the login rung above it.

**A `GameID` of zero is an answer, not an error**, and that distinction is most of the code. It is
what the API returns for a hash it does not know, so the run reports "the server does not know
this hash — the dump is not one the set covers" and stops. The next move then is to find the
supported ROM, not to debug the network. `raNetJsonNumber()` is separate from
`raNetJsonString()` for the same reason: a matcher that accepted a quote would read the first
digits of `"GameID":"1448"` and name a game that is not the game, and the host test pins that,
along with zero, whitespace, absence and an overflowing value.

#### Percent-encoding is the part that would have been blamed on the user

A password is user text. An `&` ends the query parameter early, so the server sees a shorter
password; a `+` decodes as a space; a `%` opens an escape that is not there. Every one of those
builds a **well-formed request that comes back `invalid_credentials`** — which from the console
is indistinguishable from the password simply being wrong. The user would have been told their
password was wrong when it was not, and would have retyped it.

So `raNetUrlEncode()` exists as a function with a host test rather than as a `sniprintf`, and
`tools/ra_launcher_test.c` feeds it exactly those characters plus a non-ASCII one, and checks
that truncation is *reported* rather than swallowed — a silently shortened password being the
same bug in a different coat.

The config parser is tested against **odelot's example verbatim**, because "his file works here"
is the actual requirement and a fixture invented for the test cannot check it.

#### ra_net exists now, which the layering table has been promising since phase 0

The table at the top of this document has had an `ra_net` row — *"HTTP(S) transport to the RA
servers. rcheevos ships no networking"* — since before any of it was written. The GET lived
inside `ra_wifi.c` while there was exactly one request to make. With `r=login` there are two and
`r=patch` makes three, so it moved: `raNetHttpGet()`, `raNetBody()`, `raNetJsonString()`.

`raNetJsonString()` is deliberately not a JSON parser. It matches `"key":"` and copies to the
closing quote, which is enough for `r=login`'s flat object of scalars and knowingly not enough
for `r=patch`'s nested one. Dragging a parser in before something needs it would mean shipping
untested code; 3d is where it earns its place.

The ladder grows one rung to **10**, and the file the host test reads is still a 9-rung run —
which is the point of reading real logs rather than transcriptions: the fixture did not have to
be edited to stay true.

#### The ARM7 was supposed to be the wall. It is not, and the reason is a section name

The one thing this could not be reasoned about was space. dsiwifi's ARM7 half is ~13,000
lines, a good part of it mbedTLS for the WPA2 handshake, and the launcher's ARM7 links into
libnds' stock 96 KB of IWRAM. The honest expectation was that it would not fit.

It fits, and comfortably, because most of it never goes to IWRAM at all:

| Region | Baseline | With the probe | Of |
| --- | --- | --- | --- |
| ARM7 `iwram` `0x037F8000` | 17,856 | **80,300** | 98,304 — **18,004 spare** |
| ARM7 `twl_iwram` `0x03000000` | 1,660 | 43,100 | 262,144 — 219,044 spare |
| ARM9 `ewram` `0x02280000` | 350,940 | 356,572 | 753,664 — the rest is heap |

**dsiwifi names its DSi-only sources `*.twl.c`, and devkitARM compiles those into a `.twl`
section that libnds places in `twl_iwram` — 256 KB of DSi-only ARM7 WRAM at `0x03000000`.**
The SDIO driver, WMI and the whole crypto slice land there, not in the 96 KB everything else
shares. That is not a lucky accident; it is what the naming convention is for, and it is why
a driver this size was ever viable on the ARM7.

IWRAM still went from 18% to 82% full, so it is *tight* rather than roomy, and step 4 —
running this beside a game, where the cardengine's own ARM7 hooks are also resident — has 18
KB to work in, not 80. That is a number worth having before that step rather than during it.

The ARM9 grew by 5,632 bytes and is irrelevant, which is the point of putting no library on
that side.

#### And the lwip number is now measured rather than argued

The claim above was that dsiwifi's lwip does not fit in the launcher. Linked, it is not an
estimate: `memp_memory_PBUF_POOL_base` is **784,387 bytes** of `.bss` on its own, and the
probe's ARM9 — which does link lwip — carries **840,160 bytes** of `.bss` in total. The
launcher's whole link region is **753,664 bytes**, of which **352 KB** was free after its own
code and data.

So the pool alone is larger than the entire region, and step 3 could not simply link the
library and see. That measurement is what chose the fix: cutting `PBUF_POOL_SIZE` from 512 to
32 took the library's static `.bss` from 833,410 bytes to 75,674, which is what
*Step 3a — lwip in the launcher* above is about.

#### The one part a host can check, and it is checked

`raWifiVerdict` reads how the chip arrived out of dsiwifi's **printf text**, because the chip
string, the host-interest flag and the BMI version are all statics inside the ARM7 half and
none of them crosses the FIFO any other way. That is a coupling to a third-party library's
log strings, and its failure mode is the worst kind this project has: a renamed string does
not break a build, it reports the wrong world after a play session.

So it is pinned twice, and `./tools/ra_reader_test.sh` runs both in seconds:

- the classifier is fed **the log this console actually produced** — the lines quoted under
  *#1e* above, and only those, which is why the expected stage there is 2 and not 3;
  `fully initialized!` certainly happened on that console but is not among the lines the
  document kept, and a fixture that invents evidence is worse than a short one;
- and the runner **greps `libs/dsiwifi` for every format string** the classifier matches, so
  bumping the submodule fails here rather than on hardware.

The test that matters most is neither of those. dsiwifi ships its log over the FIFO in
**59-byte chunks**, so a single line arrives split — and the strings being matched are up to
21 characters, so a cut in the wrong place hides one completely. The reassembly is checked by
feeding the same log at chunk sizes 59 and 7 and asserting the verdict is identical to the
line-at-a-time feed. A per-chunk matcher would have passed every other test in the file and
reported "the chip did not come up".

#### Two things the ladder does not repeat, and one latent break it fixed

The probe reports the console's configured WiFi slots, to tell "no network configured" apart
from "the chip failed". This does not, deliberately: the probe already established that this
console has a WPA2-PSK DSi slot, and the log explains a failure without it. One fewer FIFO
channel and one fewer struct for a fact already in hand.

And one trap this build set for itself, found by walking into it. `RA_LAUNCHER_WIFI` changes
compiler flags and the library list, **not the source list**, so make cannot see it change:
flipping the switch and rebuilding reused every object and produced a `.nds` that looked
built and had no probe in it. Every line of the build log said it had worked. That is the
same failure shape as `0xH08e43c=0x38` — a thing that reports success and does nothing — so
it is fixed rather than written down: both launcher Makefiles stamp the mode into their build
directory and wipe it when the mode changes. Flipping the switch either way now rebuilds, and
that was checked by flipping it three times and looking for the symbol.

And `libs/dsiwifi` moved out of `tools/wifiprobe/`, because two builds consume it now. That
does not weaken the probe as a control — what a control must not share is *nds-bootstrap's*
code, so that a failure in it cannot be nds-bootstrap's fault. The driver being the same
driver is the entire point: if the two runs disagree, the difference is nds-bootstrap.

Moving it turned up a break that was already there. `make -C libs/dsiwifi release` does not
generate `include/dswifi_version.h` — only the submodule's `all` target does, and every
source that includes `dsiwifi7.h` needs it. A fresh clone therefore failed the probe's build
too, from inside a third-party Makefile, on a missing header. Both entry points now ask for
that file by name first.

### Step 3d — `r=patch`, and reading a reply that does not fit in memory

The last rung of the ladder, and the first one that leaves something behind. Every rung before
it was a measurement — the chip came up, the API answered, the server knew the ROM. This one
takes RetroAchievements' own definitions and writes them into the staging block the cardengine
already reads, which closes the loop the document has described since phase 0:
launcher → staging → DSi WRAM.

The interesting part is not the request. It is that **the reply cannot be held.**

| | |
| --- | --- |
| `r=patch` for GameID 14856 | over 100 KB of JSON |
| heap available with lwip up | 85,656 bytes (measured, *It answered `GameID 14856`* above) |
| the destination block | 32,760 bytes, of which 32,751 is text |

So there is no buffer to read it into, and a JSON parser needs the document. The reply is
therefore read *through*: `recv()` into a 1 KB static window, hand each window to a scanner,
forget it. Nothing accumulates and nothing is allocated — the largest reply in this project
costs **zero bytes of heap**, which the run's own heap line is there to check rather than assume.

#### Why a scanner is sound here and not merely convenient

The scanner looks for the eleven bytes `"MemAddr":"` and copies to the closing quote. That is
not a parser and does not pretend to be one, so the question worth answering is what it can get
wrong.

It cannot be fooled by a value, and the reason is a property of JSON rather than of RA: **a
quote inside a string must be escaped as `\"`**, so those eleven bytes cannot occur inside one.
An achievement whose title is literally `"MemAddr":"` arrives as `\"MemAddr\":\"` — byte nine is
a backslash where the needle wants a quote, and it does not match. `tools/ra_launcher_test.c`
carries exactly that achievement in its fixture rather than leaving the argument unchecked.

What it genuinely does not know is **which object the key belonged to**. Today `MemAddr` is an
achievement field and nothing else — leaderboards carry `Mem`, rich presence carries
`RichPresencePatch` — so this reads the achievement set and only that. If RA ever adds a
`MemAddr` elsewhere in the reply, this would read that too. That is a limitation, written down
rather than left to be found.

#### Chunked transfer encoding, which is the failure this step would have shipped

An HTTP/1.1 reply may arrive with its body cut into chunks, each prefixed by a **hex length
written into the byte stream**. Every reply this project has read so far came back unchunked —
the bodies in the logs start at `{` — but each of those was one packet, and whether a CDN chunks
is a decision about size. A `1a2f\r\n` landing inside a memaddr string would corrupt **exactly
one definition out of a hundred**, and nothing anywhere would say so: the set would load, most
achievements would work, and one would silently never fire.

That is the same failure shape as the zero-byte log and the `recv()` hang — something that
reports success and is wrong — so the framing is undone in code that a host can test rather
than hoped about. `raNetStreamFeed()` strips headers and chunk framing as a byte-fed state
machine, and the test feeds each fixture **at every one of its byte boundaries** and once a byte
at a time, requiring the same body every time. A boundary between the `\r` and the `\n` of a
chunk header is an ordinary split to a network and a silent corruption to code that assumes
otherwise.

#### Unofficial achievements, and why a definition is held before it is committed

RA sends unofficial achievements in the same array as published ones, distinguished only by
`"Flags"`: **3 is core, 5 is unofficial.** Counting them would inflate the one number this rung
exists to produce — "does a real set fit 32 KB" — with achievements no player is scored on.

`Flags` arrives *after* `MemAddr` in each object, so filtering means the definition cannot be
written the moment it is read. It waits in a 2 KB carry buffer until its flag arrives, or until
the next `MemAddr` or the end of the document says none is coming. Which in turn is why
`raPatchFinish()` exists as its own call: the last achievement in the reply is the one whose
`Flags` is followed by the end of the document instead of by another key, so without it a set is
always exactly one definition short — an off-by-one that a hundred-achievement set hides
perfectly.

Three more decisions, all of the same kind:

- **`\/` is unescaped back to `/`.** RA escapes forward slashes and memaddr syntax uses `/` as
  its division operator, so leaving the pair intact would make every divide a parse error.
  `\\` and `\"` are handled because JSON allows them; anything else keeps its backslash rather
  than inventing a decoder for an escape a memaddr cannot contain.
- **A definition longer than the carry buffer is dropped, never clipped.** A truncated memaddr
  is not a shorter achievement, it is a *different* one — rcheevos would either refuse it or,
  worse, parse the surviving prefix into a condition that triggers when it should not.
- **A reply cut off mid-value is counted separately from one that overran the buffer**, because
  the two say opposite things about what to do next: one is a buffer to enlarge, the other is a
  request to repeat.

Every outcome is counted and reported, because a set that lost thirty definitions to a full
block looks exactly like a set with thirty fewer achievements from the block alone.

#### `RA_DEFS_MAX_LINES`: 8 → 128, and the host-test landmine it set off

The reader would only ever split **eight** lines out of the 32 KB block. That was the right
number while the definitions came from a hand-typed file, and this document has said so since;
`r=patch` makes the server the source, so it had to follow. The 128 pointers are `static` rather
than automatic because `ra_rc_init()` is reached from the cardengine's own context and 512 bytes
of that stack is not this binary's to spend.

Raising it fired a landmine in `tools/ra_reader_test.c` for the **third** time, and this time it
is fixed rather than documented. That file defined `__bss_start`, `__bss_end` and `__vram_top`
as 1-byte dummies, and the cardengine takes their *addresses* — so the allocator's arena was
whatever the linker's ordering of three symbols happened to make it. Adding rcheevos' `hash.c`
to the link once made the span **−1**; 512 bytes of new statics in the same file did it again.
Both times the suite **passed at `-O0` and segfaulted at `-O1`**, in tests unrelated to the
change.

They now come from `--defsym` as absolute addresses inside the real WRAM window, which the test
mmaps in full instead of only the definitions block at the top of it. The arena is *chosen*
rather than inherited, and nothing added to that file can move it again. The scratch window the
direct `ra_startup()` tests use is one buffer with offsets for the same reason — `fakeBss` and
`fakeArena` were contiguous only by the linker's good manners.

Two smaller things fell out. The over-limit fixture was `char many[256]`, hand-sized for twelve
12-byte lines; at 128 it overflowed and glibc's fortify check caught it, so it is derived from
the constant now. And a fixture length miscounted by one in the new patch tests cut a
`"Flags":5` to `"Flags":`, turning a test about unofficial achievements into a test about
missing flags — which then failed for the right reason on the wrong grounds. Both are the same
lesson: **a fixture whose size has to be re-derived by hand is a fixture that will eventually
measure wrong.**

#### First hardware run of stage 12: the set arrived, and the buffer was wrong

`reached stage 12 of 12`, and the numbers that matter are not the rung:

```
body was         87747 bytes
definitions      51 kept, 3 unofficial
lost             0 full, 5 too long, 0 cut, 0 empty
block            6791 of 32759 used, 6791 wanted
longest memaddr  6264 of 2047
first            1=1.300.
heap after patch 69632 safe + 8184 free, top 02327000
```

Log at `docs/logs/ra_wifi_launcher_patch-3ds.log`. What is **settled** by it:

- **The whole path works.** 87,747 bytes of JSON streamed off the socket, 59 `MemAddr` keys found,
  51 definitions written into the staging block, magic set. The reply is 2.7× the block it fed and
  was never held anywhere.
- **It cost nothing.** `top 02327000` and `safe 69632` are byte-for-byte identical before and after
  the request. The largest reply in this project allocates zero bytes of heap, which is what the
  streaming design was for and is now measured rather than intended.
- **Unofficial filtering works on real data.** 3 of 59 came back `Flags 5` and are not in the block.

And what it **breaks**:

> **`longest memaddr 6264 of 2047` — five real achievements were dropped because the carry buffer
> was a third of the size it needed to be.**

That is the buffer being wrong, not the reply. `RA_PATCH_MEMADDR_MAX` was 2048 because RA memaddr
strings "run from tens to a few hundred characters" — true of most of them and false exactly where
it hurts. A completionist achievement is one condition per collectable; 150 stars is 150
conditions, and a few kilobytes is normal for the achievements a player cares most about. It is
**8192** now, and `longest` is reported every run so the margin stays a measurement.

The consequence is that **the number this whole rung existed to produce is not yet known.** `wanted`
read 6,791 of 32,759 — but that is 51 definitions averaging 133 bytes, with the five largest
missing. Five definitions of the size actually observed put `wanted` somewhere between 22 K and
32 K, which straddles the block:

| if the five average | `wanted` becomes | of 32,759 |
| --- | --- | --- |
| 3,000 bytes | 21,791 | fits |
| 5,000 bytes | 31,791 | **fits by 968 bytes** |
| 6,000 bytes | 36,791 | **does not fit** |

So "does a real set fit the block" is still open, and the next run answers it. That it is open is
the finding — the first run's comfortable-looking `6791 of 32759` was comfortable because five
definitions were missing from it.

#### `1=1.300.` and why six numbers were not enough

The other thing the run printed is `first 1=1.300.`, and it is worth being precise about what that
is and is not. Eight characters. It is **valid memaddr syntax** — `.300.` is a serialized hit count,
so it reads as "always true, 300 hits" — and it is **not** what a published achievement looks like.

Which means the summary cannot distinguish two very different worlds: *the scanner works and that
set has an odd first entry*, or *the scanner is emitting fragments*. Arguing from the code cannot
settle it either, and the whole method of this document is that a reading which can come out two
ways is not a reading.

So the definitions themselves become the artifact, the way dsiwifi's verbatim log is the artifact
for the chip. Stage 12 now writes `sd:/ra_definitions.txt` — every staged definition, one per
line — and it can be read against the set's page on retroachievements.org line by line. The log
also prints the **first three** definitions with their true lengths rather than one, because three
consecutive entries are not ambiguous in the way one is.

The dump is written **last, after the summary has been fsync()'d**, and that ordering is
deliberate: it is tens of kilobytes of SD I/O with the WiFi link still up, which is the ARM7
contention *#1d* is about and which froze a run once already. A freeze there now costs the file and
keeps the reading. It is also the same format the launcher already *reads* from
`ra_achievements.txt`, so copying it to `sd:/_nds/nds-bootstrap/` boots the game with the server's
own set and no network at all — useful while step 4 is still being built.

#### Second run, with the 8 KB buffer: the set is whole, and it fits

```
body was         87747 bytes
definitions      56 kept, 3 unofficial
block            28585 of 32759 used, 28585 wanted
memaddr length   8 shortest, 6264 longest, of 8191
def 1     8      1=1.300.
def 2  1226      R:0xH09cab4>0_R:0xH09cab5>0_R:0xH09cab6>0_R:0xH09cab7>0_
def 3  6264      0xT0009caa8=0.100._P:0x 0017e874=64.1._P:0x 00189074=64.
definitions to   sd:/ra_definitions.txt
```

Log at `docs/logs/ra_wifi_launcher_patch2-3ds.log`, and the set itself at
`docs/logs/ra_definitions-14856.txt` — 28,585 bytes written by the console.

**56 + 3 = 59, the same 59 the first run found**, with no `lost` line at all this time: nothing
too long, nothing cut, nothing dropped for space. The five definitions the 2 KB buffer ate are
back, and the two largest are 6,264 bytes each.

**`block 28585 of 32759 used`** — the size question step 3 existed to answer. **It fits, with 4,174
bytes to spare**, and that is 87.3% full. Tight rather than roomy: a set 15% larger than this one
would not fit, and the counters that would say so (`dropped`, `wanted`) are the ones already in the
log.

And `1=1.300.` was real. The dump's first line is exactly those eight characters, its second line
begins `R:0xH09cab4>0` as a definition should, and the file's byte count matches `used` to the byte:

| | |
| --- | --- |
| bytes in `ra_definitions.txt` | 28,585 |
| `block ... used` in the log | 28,585 |
| lines | 56 |
| sum of line lengths + one newline each | 28,585 |

So no definition was split, spliced or lost. Which retires the ambiguity honestly: the scanner is
right, and that set genuinely opens with an eight-character achievement.

#### rcheevos agrees, which is the check that actually matters

Text that looks like memaddr is not the same as memaddr. A scanner that dropped one character,
decoded an escape wrongly, or joined two definitions would still produce plausible-looking lines —
and rcheevos is the only thing that can say they are *valid*.

`tools/ra_fit_test.c` hands all 56 to `rc_runtime_activate_achievement()`:

> **56 of 56 activated, 0 refused.**

That is what makes the streaming extraction trustworthy rather than merely well-tested against
fixtures written by the person who wrote the scanner.

The same test answers step 4's opening question, which is whether the set fits the *arena* rather
than the block:

| | |
| --- | --- |
| arena (`__bss_end` `0x0375164C` → the defs block at `0x03778000`) | 158,132 bytes |
| `rc_runtime_init()` | 1,480 |
| peak, 71 blocks + their 8-byte headers | **128,352** |
| **margin** | **29,780 bytes — 81.2% used** |

**The console later read 110,472 of 149,288** — this estimate is **14% high**, and the reason is the
measuring tool rather than the code: the counting wrappers use `malloc_usable_size()` and glibc
rounds every block up. Over-estimating is the safe direction for a question about whether something
fits, and it is worth leaving the figure here beside the real one rather than quietly replacing it.

**And a correction worth recording, because it pointed the wrong way.** The first attempt at this
measurement used `rc_trigger_size()` and reported **101.7% — that the set did not fit.** It is
wrong by 33 KB: `rc_trigger_size()` sizes a *standalone* trigger, so it counts memrefs inside
every definition, while `rc_runtime_activate_achievement()` passes the runtime's communal pool as
`existing_memrefs` and each trigger only pays for what is new. The path that runs on hardware is
the runtime one. The test measures that path, and prints the margin rather than asserting a
threshold, because the margin is a property of the set.

`tools/ra_fit_test.c` is a **third** host binary, and the reason is concrete rather than tidiness:
it replaces `malloc`, `realloc`, `calloc` and `free` for its whole link in order to count them,
while `tools/ra_reader_test.c` does the exact opposite on purpose — `RA_ALLOC_NO_LIBC_NAMES` is
there to keep the cardengine's allocator from ending up underneath `printf`. The two arrangements
cannot share a link. The arena's lower bound is read out of the built `.elf` with `nm` rather than
carried as a constant, because every byte the cardengine's `.bss` gains comes straight out of that
29,780.

#### What step 3 leaves for step 4

Copying `ra_definitions.txt` to `sd:/_nds/nds-bootstrap/ra_achievements.txt` makes a **normal**
build boot the game with the server's own 56 definitions and no network involved. That is the
cheapest possible first move for step 4: it separates "rcheevos runs a real set on hardware" from
"the network works beside a game", and those are two different failures with two different fixes.

What is not answered, and is honestly step 4's:

- **1,946 conditions evaluated per frame.** The host says they fit; nothing says they are fast
  enough. The measurement already exists — `rcLinesMax`, which read 1 scanline of 263 for three
  definitions.
- **The ARM7 beside a game.** The launcher's ARM7 has 18 KB of IWRAM spare with dsiwifi in it; the
  cardengine's own ARM7 hooks are resident too in context B.
- **`safe 61440`.** The launcher's static floor has risen 35 KB across steps 3b–3d and that is
  what step 4 inherits.

#### The static floor rose, and that is the cost worth naming

Steps 3b, 3c and 3d took the launcher's static side from **569,136 to 604,844** of 753,664 —
response buffers, the config, the hash's streaming window, and the scanner's 8 KB carry buffer.
Every one of those raises the floor the heap starts from, and the two stage-12 runs measured the
result: `safe` fell from **77,824** at `r=login` to **69,632** with the 2 KB buffer and **61,440**
with the 8 KB one. It does not matter for *this* rung, which allocates nothing — `top 02329000` is
identical before and after the largest request in the project. It matters for step 4, and the
number to watch is `safe`, not `free`.

#### The screen gets a heartbeat, and the card gets nothing

The `recv()` loop blocks, so nothing can report progress while a hundred kilobytes come down —
a run that stalls halfway would look identical to one that never started. So the sink prints a
dot every 8 KB, and **only to the screen**: `iprintf()` is writes to console memory, where
`raWifiLog()` is libfat plus an `fsync()` over the FIFO to the ARM7 that is at that moment
running the WiFi stack. That combination is what froze a v6 run, and #1d is where it is
discussed. A dot costs nothing and makes the difference visible.

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
- **`rcheevos` is a git submodule**, and so is `libs/dsiwifi`. A fresh clone needs
  `git submodule update --init --recursive`, or `retail/cardenginei/arm9_ra/rcheevos` is
  empty and the build fails on a missing header. `tools/ra_reader_test.sh` checks for this
  and says so rather than producing a wall of compiler errors. `libs/dsiwifi` is only needed
  for `RA_LAUNCHER_WIFI=1` and for `tools/wifiprobe/`, so the host test reports it missing
  and carries on rather than failing.
- **`RA_LAUNCHER_WIFI=1` builds something that is not a loader.** It stops on a WiFi
  diagnostic summary instead of booting the game, on purpose — see *Step two, wired*. Do not
  ship one, and do not spend a session wondering why the game will not start.
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

- **Open question #1, the network transport** — steps one, two and 3a have all **passed on
  hardware, and for context A it is closed.** `tools/wifiprobe/` reached RetroAchievements over
  plain HTTP standalone, so WPA2 works and TLS is not needed; `RA_LAUNCHER_WIFI=1` brought the
  chip up to a usable WPA2 link inside nds-bootstrap's launcher; and the same build then did
  DHCP, DNS, TCP and an HTTP GET to `dorequest.php` and got the API's own reply. What is
  untested is context **B**, inside the game, where the ARM7 belongs to the game — and #1g is
  the argument for why the plan does not need it. See *#1f — the rest of the ladder*.
- **The control has never been re-run in full.** Step 2's log is complete and matches every
  value the probe's *recorded excerpt* holds, but the probe's own log from a fresh run never
  came back, so the diff is one-sided. Cheap to close if it ever matters: run
  `tools/wifiprobe/` and keep `/wifiprobe.log` next to
  `docs/logs/ra_wifi_launcher-3ds.log`.
- **The heap has never been under pressure.** One GET is not a set: `r=patch` returns the whole
  thing, and lwip's send path allocates from the same `malloc` as libfat and the launcher's own
  strings. Streaming the reply rather than buffering it is the obvious precaution.
- **The launcher's heap is not bounded by anything.** `fake_heap_end` is 12.8 MB above
  `IMAGES_LOCATION`, so malloc will grow through the boot images and the whole staging map
  without complaint — measured, `0x02FF3794` against a real ceiling of `0x02338000`. The safe
  headroom is **88,172 bytes**, which is what step 3d gets and why `r=patch` must stream. Worth
  fixing properly by lowering `fake_heap_end` in the launcher's startup; not fixed.
- **The SD and the WiFi contend on the ARM7, and it has now been seen.** One run froze between
  the HTTP exchange and the summary — SD writes with the network live, which is exactly #1d's
  worry. Log syncs are throttled and the window is marked so a repeat locates itself, but
  neither is a fix, and step 4 inherits this directly.
- **The host test's fake WRAM arena is defined by luck** and it bit once already. See *Step 3b*
  — `__bss_end` and `__vram_top` are 1-byte dummies in `ra_reader_test.c`, and the arena is
  whatever the linker's ordering of them makes it. Worth fixing deliberately; it is not fixed.
- **Reporting an unlock has never been attempted.** Evaluation is done; `r=awardachievement`
  is not written, and neither is `r=patch`. Both wait on the transport above.
- **Game identification.** Nothing yet computes the RetroAchievements hash for a DS ROM, so
  the launcher cannot ask the server *which* set to fetch. Today's definitions came from a
  file the user edited by hand.
- **The overlay still has no font**, which blocks naming the achievement that unlocked, the
  overlay rewrite, and the `surveyBlocks()` bitmap-mode bug.
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

### The step-2 WiFi diagnostic

```sh
git submodule update --init --recursive   # libs/dsiwifi
make RA_LAUNCHER_WIFI=1                   # still serial, still needs lzss
```

Verified on `devkitpro/devkitarm:20241104` — devkitARM r65, gcc 14.2.0, libnds 1.8.0, the pin
upstream CI uses. Both modes link, and `hb/` ignores the variable: only the **retail**
launcher grows the probe.

`RA_LAUNCHER_WIFI` is a make variable, not a header switch, because it decides what gets
*linked* and not only what gets compiled: the retail ARM7 gains `libs/dsiwifi/lib/libdsiwifi7.a`
and both CPUs gain `-DRA_LAUNCHER_WIFI=1`. Set on the command line it reaches every sub-make
on its own.

Three details worth knowing:

- The submodule's include directory carries its own `netdb.h`, `sys/socket.h` and
  `netinet/`, and this ARM9 builds mbedtls and polarssl next door. Both Makefiles add it
  with **`-iquote`**, not `-I`, so `<sys/socket.h>` still means newlib's. That is the same
  choice `tools/wifiprobe/arm7/Makefile` makes.
- **It fits, and the numbers are in *Step two, wired*.** ARM7 IWRAM goes from 18% to 82%
  full; most of dsiwifi lands in `twl_iwram` instead, because its DSi-only sources are named
  `*.twl.c`. Worth knowing before step 4 rather than during it.
- **Flipping `RA_LAUNCHER_WIFI` no longer needs a manual clean**, but only because both
  launcher Makefiles stamp the mode and wipe their build directory when it changes. If you
  add a third mode-dependent flag, stamp it the same way — make cannot see a flag change on
  its own, and the failure is a binary that looks built and does nothing.

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
| `tools/ra_reader_test.c` / `.sh` | Host-side test for the watchlist, the chain walker, the example file and step two's log classifier |
| `tools/ra_hash_test.c` | Host-side test for step 3b: our ROM hash against rcheevos' own. Its own binary, for a reason worth reading |
| `retail/arm9/source/ra_hash.c` | The ROM's RetroAchievements hash, streamed rather than allocated |
| `retail/arm9/source/ra_cfg.c` | The `ra.cfg` reader, and the redaction that keeps secrets out of the log |
| `retail/arm9/source/ra_net.c` | `ra_net`: the HTTP GET, the query encoder, and just enough JSON |
| `tools/ra.example.cfg` | The config file to copy to the card |
| `retail/common/include/ra_wifi.h` | Step two: the `RA_LAUNCHER_WIFI` switch, the stage ladder, the verdict struct |
| `retail/arm9/source/ra_wifi.c` | Step two on the ARM9: one IPC message, the log, the summary |
| `retail/arm9/source/ra_wifi_verdict.c` | Reads how the chip arrived out of dsiwifi's log text. Host-tested |
| `retail/arm7/source/ra_wifi7.c` | Step two on the ARM7: `installWifiFIFO()`, and where it goes |
| `docs/logs/ra_wifi_launcher-3ds.log` | The step-two run that passed. Evidence, and a host-test fixture |
| `docs/logs/ra_wifi_launcher_http-3ds.log` | The step-3a run that reached the API. Same |
| `docs/logs/ra_wifi_launcher_hash-3ds.log` | The step-3b run: the hash, and the 382,212-byte figure that justifies streaming it |
| `docs/logs/ra_wifi_launcher_login-3ds.log` | The step-3c run: a real login, and a heap that never grew |
| `docs/logs/ra_wifi_launcher_gameid-3ds.log` | `GameID 14856`: the server's own verdict on the hash |
| `retail/dsiwifi9/` | dsiwifi's ARM9 half rebuilt with lwip's pools cut to fit the launcher |
| `retail/dsiwifi9/include/lwipopts_ndsbs.h` | The sizing, and why no `-I` path could have done it |
| `libs/dsiwifi` | The driver, a submodule, shared with `tools/wifiprobe/` |

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

### #1a — TLS is not required. Measured, not asked.

The question that would have decided everything on its own — is HTTPS mandatory for
`dorequest.php`? — did not need asking. It is testable, and the answer is **no**:

```
$ curl -A "..." "http://retroachievements.org/dorequest.php?r=login&u=...&p=x"
{"Success":false,"Status":401,"Code":"invalid_credentials","Error":"Invalid user/password..."}
```

Plain HTTP, port 80, no redirect, byte-identical JSON to the HTTPS request. The API answers
over cleartext today.

`rcheevos` anticipates this. `src/rapi/rc_api_common.c` defines
`RETROACHIEVEMENTS_HOST_NONSSL "http://retroachievements.org"` as a first-class host, and
`rc_api_set_host()` recognises it specifically — switching the image host to its non-SSL
counterpart so a client pointed at cleartext does not end up mixing schemes. That is an
affordance built deliberately for constrained clients, not an accident.

Two honest caveats. This is true *today*; RA could require HTTPS at any point, and a design
that cannot fall back would break. And cleartext means **credentials cross the network in
the clear** — the Connect API takes a username and password on `r=login` and returns a
token. That is a real cost to weigh, not a footnote, and it is the user's call to make
knowingly.

The rest of the RA side, from the standalone integration guide:

- **A bulk unlock endpoint exists** — "unlock multiple achievements at once or resync all
  the user's unlocks". Deferred sync would be a supported flow, not something smuggled past
  the API.
- **Softcore is a first-class parameter**, `h=0`, not a degraded mode.
- A user agent header is **mandatory** on every Connect call.
- Game pages for standalones are set up by the admin team on request (DM `RAdmin`).
- `rcheevos` ships no networking at all, so the transport is ours to write regardless.

### #1b — what the prior art actually does

odelot's adapters are this project's blueprint, and the thing to copy is not their transport
but their **split**:

| Project | Evaluation runs on | Networking runs on |
| --- | --- | --- |
| `nes-ra-adapter` | Raspberry Pi Pico on the cartridge bus | a separate **ESP32** |
| `wii-ra-adapter` | ESP32 memory card | the same ESP32's WiFi |
| MiSTer cores | the FPGA host | the MiSTer's Linux side |

Not one of them does networking from the constrained side. The evaluator watches memory and
hands results to something else that owns the network. odelot needs an ESP32 because a NES
has no second computer to borrow.

**We do.** That is what being 3DS-only actually buys — not DS-mode networking, but a second
environment on the same device. See below.

### #1c — being 3DS-only does not relax the WiFi constraint

Worth stating plainly, because it is an easy assumption to get backwards: a 3DS running a DS
game is in DS/TWL mode. Its own operating system, its ARM11 and its WiFi stack are **not
running**. In that state the console is, for our purposes, a DSi. Scoping the project to the
3DS family buys SCFG access and DSi WRAM — which this project already spends — and it does
**not** buy the 3DS's networking.

So the WiFi question is unchanged by the scope decision. What the scope decision does change
is the fallback: the companion that owns the network can be a **3DS-mode homebrew app on the
same console**, with WPA2 and TLS from the 3DS's own stack. No extra hardware, unlike every
adapter above.

### #1d — the one question left, and what it actually costs

Before this research, live server contact needed two things to go right: reachable WiFi
*and* a way around TLS. TLS turned out not to be in the way. So one unknown decides it:

> **Is the DSi's Atheros WiFi chip reachable from nds-bootstrap's ARM7 while an NTR game
> runs, and is there room and CPU time there for a stack?**

That question was put to people who know the hardware, and the answer came back "possible,
but at the edge of realistic". Three obstacles, none of which is CPU or RAM. Recorded here
with what this repository's own source could confirm or correct, marked as such, because a
second-hand answer about our own code is worth checking against our own code.

**The bus exists, and the same SCFG bit opens it.** The Atheros is not on the NTR WiFi ports
at `0x0480xxxx`; it is on an SDIO controller at ARM7 ports `0x04004A00`–`0x04004BFF`. That
block is part of the extended TWL I/O whose availability depends on exactly the SCFG bit
nds-bootstrap keeps open to give us the DSi WRAM window. The same state that pays for this
project's 256 KB is, on paper, the state that exposes the WiFi bus.

**Corrected: the SD card and the WiFi are not on the same controller.** The answer we got
warned that the WiFi SDIO shares its controller with SD/eMMC, and that nds-bootstrap is
already using that subsystem to serve ROM reads — making it the most likely source of
conflict in the whole project. Our own source says otherwise:
`retail/cardenginei/arm7/source/patcher/my_sdmmc.h` puts the card at
`SDMMC_BASE 0x04004800`, and the WiFi SDIO is at `0x04004A00`. Two instances of the same IP
block, `0x200` apart, not one controller with two consumers. The contention is for ARM7 time
and possibly DMA, not for the controller itself.

**Corrected: the NDMA slots do not collide either.** `driveInitialize()` in the ARM7
cardengine calls `sdmmc_set_ndma_slot(0)` — slot **0** — and has an `ndmaDisabled` path that
calls `sdmmc_lock_ndma_slot()` to fall back. libnds' WiFi uses NDMA channel 3. Different
slots.

**Still standing, and the real structural cost: the ARM7 is not ours.** nds-bootstrap does
not run beside the game's ARM7 as a clean second thread. Our code would hang off the
interrupt hooks nds-bootstrap already inserted into the ARM7 that is running the game. A
WiFi stack expects regular ARM7 attention, claims a hardware timer, and does its association
and WMI/SDIO work asynchronously — and none of that can block, because blocking the ARM7
means missing the game's frame pacing. A non-blocking state machine advancing a little per
VBlank is feasible, but it is a rewrite of how `dsiwifi` waits today, not an integration.

Worth noting what this costs against what we measured: rcheevos evaluates in under one
scanline of 263. A WiFi stack in the same hook is a different order of work entirely, and
the budget headroom that looks generous now is generous *for rcheevos*.

**Still standing, and the highest risk: the WLAN firmware may not be loaded.** The
AR6002/AR6013/AR6014 keeps no firmware in flash — the Xtensa core's code is uploaded to RAM
on every boot, and the system menu is what does the uploading. Booting through
ntrboot/nds-bootstrap does not necessarily pass through the TWL menu, so the chip's state on
arrival is unknown: it may be up and need only WMI init, or it may be cold and need the full
BMI bootloader plus a firmware upload out of eMMC. `dsiwifi` assumes a starting point. If our
boot path leaves the chip somewhere else, the stack will not simply connect.

**And the driver is least proven on exactly our hardware.** `dsiwifi` is confirmed on real
DSi; the 3DS's DWM-W028 / AR6014 is far less tested, the library is not in a finished state,
and it does not work with every router for reasons that are not fully understood. Some WMI
commands are remapped on DSi/3DS relative to the stock Atheros reference.

**One more thing this project did to itself.** `wramSize` feeds `isROMLoadableInRAM()`, and
taking 256 KB of DSi WRAM lowered that budget — see *What reserving space actually costs*.
Games that no longer fit in RAM read from SD during play, on the ARM7, which is precisely
where a network stack would want to live. We made the contention slightly worse before we
knew we would care about it.

### #1e — the probe's answer: WPA2 and plain HTTP both work

Step one passed, completely, on the first run:

```
resolved retroachievements.org
IP 104.26.3.251
connected, port 80
request sent
994 bytes back
the API answered over plain HTTP
body: {"Success":false,"Status":401,"Code":"invalid_credentials", ...}
reached stage 6 of 6
WMI_BSSINFO MuMiMo24 (WPA2-PSK)
   BSSID 00:5f:67:e9:f5:70
   G TKIP P AES A PSK
```

Three things fall out of that, and two of them were supposed to be the hard ones.

**WPA2 works.** `WMI_BSSINFO ... (WPA2-PSK)` with AES is the Atheros path doing the thing
the legacy Mitsumi core cannot. The WEP problem — the one that looked like it might make
live networking useless even if it worked, because routers stopped offering WEP — does not
apply. The chip associated to an ordinary modern home network.

**The firmware question is answered, and it dissolves rather than passes.** This was
supposed to be the highest risk in the plan: the AR6002/AR6013/AR6014 keeps no firmware in
flash, the Xtensa code is normally uploaded by the system menu, and booting through ntrboot
may skip that — so the chip might arrive cold and need the full BMI bootloader plus a
firmware image read out of eMMC. Reading eMMC is exactly where SD contention would bite
under nds-bootstrap.

The log says the chip *did* arrive cold:

```
Mfg 02010271 Cid 0d000001 (AR6014)
AR6014 needs firmware upload 0.
Reset cause: 00000002
BMI version: 2300006f
BMI finishing...
Launching!
Firmware 609c0202 ready, handshaking...
```

`needs firmware upload 0` is `dsiwifi` reporting that the host-interest word at `+0x58` —
the flag the system menu would have set — reads zero. And then it launched the firmware
anyway and the handshake succeeded.

The reason is in the source: **`dsiwifi`'s entire AR6014 firmware-upload block is inside
`#if 0`.** It is compiled out. No `ar6014_part*_bin` is written, nothing is read from eMMC,
nothing is read from anywhere. All the driver does is reset the chip into its bootloader,
poke a few BMI registers, and start what is already there — and on the AR6014 that works,
which is presumably why upstream disabled the block. Its own comment, still in the code,
reads *"TODO: Source AR6014 bins from SAFE_FIRM nwm? Or just leave them because they're
only 4KiB"*.

So bringing the chip up needs **no external data at all**: no eMMC, no NAND, no SD. The
worst of the three obstacles was a concern about a code path that does not execute. That
also removes SD contention from the firmware question specifically — though not from the
rest of the ARM7, where nds-bootstrap serves ROM reads.

The rest of the log is a clean WPA2 association: a four-way handshake, a GTK, and DHCP.

```
WPA2 Handshake 1/4:  ...  WPA2 Handshake 3/4:  Added GTK 1  Done auth
Dev 04:03:d6:f9:36:52   AP 00:5f:67:e9:f5:70
IP 192.168.0.111
```

And the chip is an **AR6014** — the 3DS's DWM-W028, the variant `dsiwifi` is least tested
against and the one this project actually has. It worked on the first run.

**Plain HTTP to RetroAchievements works from the console**, not just from a PC. The API
returned its own `invalid_credentials` JSON, which is the check that distinguishes reaching
RetroAchievements from reaching a captive portal that answered on its behalf.

So of the three obstacles named before any of this was tested, two are gone and one is
untouched:

| Obstacle | Status |
| --- | --- |
| TLS required for `dorequest.php` | **Gone.** Plain HTTP, confirmed end to end from the console. |
| WEP-only, so unusable on modern routers | **Gone.** WPA2-PSK with AES, associated and DHCP'd. |
| Atheros firmware must be uploaded from eMMC | **Gone.** The upload path is `#if 0` and the chip starts from BMI alone. |
| SD/SDIO controller contention | **Gone in the launcher.** Step 2 ran the driver on nds-bootstrap's ARM7, alongside `my_sdmmc` and with NDMA slot 0 taken, and reached a WPA2 link. |
| The ARM7 belongs to the game | **Still untouched.** Step 2 is the launcher; no game exists yet. Context **B** only. |

That last row is now the whole of open question #1 — and per *#1g* the plan does not need it.
Everything above it has been settled on this console: the last row was the whole of it before
step 2, and step 2 moved the SDIO-contention row with it.

One thing the run improved about the probe itself. `dsiwifi` narrates asynchronously and
kept printing after the summary — the `WMI_BSSINFO` line arrived *after* "log written".
The log file was being closed at the summary, so exactly the lines that describe how the
chip came up were being dropped from the file. It now stays open until you exit.

### #1g — there are three network contexts, not two, and the middle one was invisible

Everything above was argued as a choice between two places to put the network: inside the
game, where the ARM7 belongs to the game, or outside on a separate 3DS-mode companion app.
That framing missed the obvious one.

**nds-bootstrap's launcher is ordinary DSi-mode homebrew.** `retail/arm9/source/main.cpp`
calls `consoleDemoInit()` and checks `isDSiMode()`; `retail/arm7/` is its own ARM7, not the
game's. It reads the SD card, parses configuration, loads the ROM and stages
`cardenginei_arm9_ra` — all before a single instruction of the game has run. It is, in
every respect that matters, the same context the probe just succeeded in.

So there are three:

| Context | WiFi | Contention |
| --- | --- | --- |
| **A — the launcher**, before the game boots | proven, this is what the probe is | none; no game exists yet |
| **B — inside the game**, cardengine + WRAM binary | unproven, hard | the ARM7 is the game's, nothing may block |
| **C — a 3DS-mode companion app** | full stack, TLS, WPA2 | none, but it is a second program the user must run |

The plan had been B or C. **A is better than both**, and it is where the work should go:

- It needs no companion app, so the user launches one thing.
- It has no ARM7 contention, because there is no game yet — which was the last obstacle
  standing.
- It is where the interesting network work naturally belongs anyway: identifying the game
  and fetching its achievement set are things you do *before* play, not during.
- The pipe already exists. `CARDENGINEI_ARM9_RA_BUFFERED_LOCATION` and its `SRA1` magic were
  built to carry a binary from the launcher into DSi WRAM; carrying achievement definitions
  the same way is the mechanism we already debugged.

What A cannot do is submit an unlock at the instant it happens, because by then the launcher
is gone. Unlocks get queued in WRAM during play and flushed afterwards — either by the ARM7
cardengine writing them to the SD card, which it already does for screenshots and saves, and
the launcher sending them on next boot; or through the in-game menu, which already exists.
The Connect API's bulk endpoint is built for exactly this.

That is deferred submission, but by minutes rather than by a separate program, and on one
console with one launch. **Live submission from inside the game becomes an optimisation
rather than a prerequisite** — worth doing later if the ARM7 question turns out well, and
not blocking anything if it does not.

### #1f — the rest of the ladder

The risk is concentrated in the firmware-state question, and that is also among the cheapest
things to test. So the order is not "build the client":

1. ~~**Outside the game entirely.**~~ **Done, and it passed — `tools/wifiprobe/`, stage 6
   of 6 on hardware.** See *The probe's answer* below.
2. **The chip's bring-up under our boot path** — **passed, stage 5 of 5, and identical to the
   control.** Superseded by 3a below, which goes further from the same build.
   `make RA_LAUNCHER_WIFI=1` links dsiwifi's ARM7 half into the launcher and brings the chip
   up to the WPA2 handshake before any game exists. The question turned out not to be
   `WLANFIRM` — the probe had already answered that, and dsiwifi relaunches the firmware
   every time regardless — but whether the driver works as a guest of *nds-bootstrap's* ARM7,
   which inherits SCFG rather than opening it and is already driving the SD controller one
   instance below the WiFi SDIO block. See *Step two, wired* above.
3. **The definitions block, filled from the network instead of by hand.** The launcher fetches
   a set and writes it where `ra_achievements.txt` is written today — a destination already
   proven end to end on hardware, since three real definitions came through it and fired. Four
   parts, and only the first is a question about the platform rather than work:

   - **3a, the IP stack — done, `stage 9 of 9` on hardware.** lwip in the launcher, cut to
     fit, reaching the RA API over plain HTTP with no credentials. See *Step 3a — lwip in the
     launcher*.
   - **3b, game identification — written, host-verified, not yet run.** The launcher computes
     the ROM's RetroAchievements hash and logs it. See *Step 3b* below.
   - **3c, `r=login` — done, `stage 10 of 10` on hardware.** Credentials from a config file,
     percent-encoded, and a token back for a real account. See *Step 3c* below.
   - **3d, `r=patch` and parsing.** Two requests, not one: `r=gameid&m=<hash>` turns the hash
     into a `GameID` — which is also the server's own verdict on whether it knows this dump —
     and then `r=patch&u=&t=&g=` returns the set. JSON in, memaddr strings out, into the block
     that already works. The reply must be **streamed**: the measured budget is 88 K.
4. **Only after those**, live submission from inside the game — context **B**, behind the
   VBlank hook as a non-blocking state machine, watching for ARM7 contention with the SD
   path. Queue-and-flush from the launcher (see #1g) makes this an optimisation rather than
   a prerequisite, which is why it is last rather than second.

If 1 or 2 goes badly, the deferred design is the sensible answer rather than the consolation
one: a 3DS-mode companion has an ARM11, a mature network stack, WPA2, TLS, and no contest for
the game's ARM7. Unlocks get written to a log on the SD card during play and synced
afterwards. It loses "live" and gains weeks.

**The honest read:** live unlocks are reachable, but probably three to four times the work
that having rcheevos already running would suggest — and the risk sits almost entirely in
the WLAN firmware state under our boot path, which is the cheapest thing on the list to test.

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

## Step 4, offline half: running the server's own set inside the game

Step 3 ends with 56 published definitions staged where the cardengine reads them. Step 4 runs
them, and it stacks two things that have never been tried: rcheevos evaluating a real set inside a
retail game, and WiFi in context B where the ARM7 belongs to the game. Those fail for unrelated
reasons and have unrelated fixes, so they are separated — and the first one needs no network at
all.

**The mechanism is not a new path.** `ra_definition()` decides whether the block is real by the
magic at `CARDENGINEI_ARM9_RA_DEFS_LOCATION` and nothing else, so the cardengine cannot tell a
file from an `r=patch`. Same address, same header, same `rcFromFile = 1`, same
`ra_split_definitions()`. Copying `docs/logs/ra_definitions-14856.txt` to
`sd:/_nds/nds-bootstrap/ra_achievements.txt` therefore exercises *the same code* with the network
removed as a variable.

There is also a hard reason and not only a methodological one: **`RA_LAUNCHER_WIFI=1` does not boot
games**, by design — dsiwifi's bring-up has two untimed `while` loops on the ARM7 and handing a
possibly-wedged ARM7 to a bootloader that is about to overwrite its code is not something to do for
a measurement. So no build today both fetches a set and boots a game. Building that one is step 4;
the file is the bridge until it exists.

### `rcInitLines` could not measure this, so it was fixed first

The one-time parse used to be timed as a single `VCOUNT` delta around `ra_rc_init()`, taken modulo
263. That is correct for three definitions and **silently wrong for fifty-six**: a parse spanning
four frames reports whatever remainder it lands on, so a slow init reads as a fast one. And there
is no way to count frames from inside a handler that is not being re-entered.

Why it matters more than tidiness: rcheevos deduplicates memrefs by scanning a list that grows as
definitions are added, so ~1,946 conditions across two parse passes is plausibly **tens of
millions of cycles at 67 MHz — ten frames or more, spent inside the game's VCOUNT handler**. That
is a hazard to the game, not a detail, and a number that wraps would have hidden it.

So the measurement moved down a level. Each activation is timed on its own and the results are
published *as the loop runs*, so a hang shows how far it got:

| Field | Offset | What it is |
| --- | --- | --- |
| `rcInitLines` | `+0x6F` | the **slowest single** activation, clamped at 255 |
| `rcInitTotal` | `+0x9E` | scanlines **summed** over all of them, clamped at 0xFFFF |

Per-definition deltas sum correctly as long as no single activation exceeds one frame, and
`rcInitLines` is what says whether that assumption held: at 255 it saturated and the total is a
lower bound.

`rcFirstTriggered` (`+0x9D`) went in beside them. With one definition a counter was enough; with
fifty-six, `rcTriggered` climbing says something fired and nothing about what. Both fields fit in
`reserved4[3]` — one `u8` and one aligned `u16` at an odd offset — so **every offset above them
keeps its address** and the checklist below stays valid for every reading anyone has photographed.

### The predictions, made before the run

Snapshot at **`0x027FED50`** (`cardenginei_arm9`, which is what a retail DS game loads on a 3DS).
Re-run `tools/ra_snapshot_addr.sh` after any rebuild.

| Field | Offset | Expected | What it being otherwise means |
| --- | --- | --- | --- |
| `rcFromFile` | `+0x98` | **1** | 0 = the file was not picked up; the built-in self-test is running and nothing below is about the set |
| `rcDefLength` | `+0x9A` | **`6FA9`** (28,585) | anything else = the file was truncated or edited |
| `rcActivated` | `+0x99` | **`38`** (56) | fewer = a definition the host accepted was refused on hardware, and `rcBadLine` says which |
| `rcBadLine` | `+0x9C` | **0** | non-zero = the first line that failed to parse |
| `rcStage` | `+0x6C` | `RA_RC_FRAME` = **7** (was 6 before `RA_RC_LOADING` was inserted). **5 = still loading**, and then `rcActivated` says which definition it is on | `RA_RC_NO_MEMORY` = the arena measurement was wrong |
| **`rcFirstTriggered`** | **`+0x9D`** | **1** | the set opens with `1=1.300.` — always true, 300 hits — so **line 1 should unlock about five seconds in.** Any other line first means a definition is reading memory it should not, which is a bug rather than a success |
| `rcTriggered` | `+0x70` | ≥ 1 within ~5 s | 0 after a minute = `do_frame` is not reaching memory; check `rcPeeks` |
| `rcPeeksRejected` | `+0x80` | **0** | non-zero = a definition asked for an address this console cannot supply. **This is the field most likely to be non-zero**, and it is why `rc_runtime_validate_addresses()` was put in before there was a real set to need it |
| **`rcInitTotal`** | **`+0x9E`** | **unknown — this is the reading** | 263 per frame. Under ~500 is a non-event; several thousand is ten-plus frames inside the game's VCOUNT handler, and then the parse has to be amortised across frames |
| **`rcStackUsed`** | **`+0x6A`** | **near 2,383** — the host's figure, on hardware for the first time. 8192 exactly means 8 KB is not enough either |
| `rcInitLines` | `+0x6F` | < 255 | 255 = one activation alone exceeded a frame, and `rcInitTotal` is a floor rather than a total |
| **`rcLinesMax`** | **`+0x85`** | **unknown — the other reading** | steady-state cost of 1,946 conditions per frame, out of 263. Three definitions cost **1** |

Two of those are genuinely open, and they are the point of the run: `rcInitTotal` and `rcLinesMax`.
The host has already answered everything else — 56 of 56 parse, and the set fits the arena with
29,636 bytes to spare.

`rcPeeksRejected` deserves its own sentence. The addresses in this set run up to `0x00189074`, and
the reader translates a console address by adding `0x02000000` — so they land in the game's main
RAM and should all be readable. Should. Nothing has ever tested that with addresses this project
did not choose, and a rejection is *better* than the alternative: it used to mean a Data Abort.

#### First run: a Data Abort, with the ARM9 executing the definition text

```
Error: Data Abort!
PC: 0377EEEA   ADDR: 0377EEF2
lr: 0377EE05
```

Both `PC` and `lr` are inside `CARDENGINEI_ARM9_RA_DEFS_LOCATION`. `0x0377EEEA` is **offset 28,386
into the definition text**, and `0x0377EE04` — the Thumb target `lr` came from — is 21 bytes into
line 52, mid-string. So this is not rcheevos returning an error: **control flow left the code and
the ARM9 has been decoding the achievement set as instructions.** `ADDR` is `PC + 8`, unaligned,
which is just where the wandering happened to fault.

That is the whole of what the photograph proves, and it is worth saying so plainly before the
theories.

##### Three hypotheses, measured rather than argued

**Stack depth.** rcheevos' parser was the obvious suspect — a 6,264-byte definition with 416
conditions, inside an interrupt handler with a small stack. Measured on a host by compiling
rcheevos with `-finstrument-functions` and recording the deepest frame:

| | |
| --- | --- |
| deepest definition in the set | **2,383 bytes** |
| shallowest | 2,079 bytes |
| `M:0xH000000>=0.600.` — the self-test that has always worked | **2,383 bytes** |
| `I:0xM09cab4_0xH09f2f8=6` — the shape of the three that fired on hardware | **2,383 bytes** |

Flat. The parser is iterative, so depth does not scale with the definition, and **the definitions
that already worked use exactly as much stack as the ones that crashed.** Hypothesis dead — which
is the value of measuring it, because it was the most plausible one.

**The arena overrunning the block.** The definitions sit at `0x03778000` and the arena runs up to
it from `__bss_end`. 128,352 bytes from `0x037516DC` reaches `0x03770B7C` — **29 KB short**. Not
that either.

**The linker script.** This one is real but is not the crash. `cardengine.ld.in` inherited
`__sp_irq`, `__sp_svc`, `__sp_usr`, `__irq_flags`, `__irq_flagsaux` and `__irq_vector` from the
libnds script it was copied from, every one computed down from `__vram_top` — so all six land at
`0x0377F...`, **inside the definitions block.** A stack seeded at `__sp_usr` would grow straight
down through the achievement set. Nothing references them (this binary has no crt0 and runs on the
caller's stack, which the crash screen confirms: `sp` is in main RAM), so they were harmless by
accident. They are deleted rather than relocated: an unused symbol pointing at live data gets used
eventually by someone with no reason to suspect it, and absent means a link error, which is the
failure to want.

##### What was found instead, and it is a real bug

`ra_read()` fell through to a 32-bit load for any size that was not 1 or 2. Correct for a watch
line, which `ra_watch_add_flags()` restricts to 1, 2 and 4. rcheevos is not restricted: **`0xW` is
a 24-bit read, and line 39 of this set contains one** — `I:0xW0009b450`.

So the first achievement set this project did not write immediately falsified a comment in
`ra_rcheevos.c` that said `num_bytes` is "only ever 1, 2 or 4". Two things followed from that
belief:

- a 24-bit read answered with **32 bits**, so the value carried a fourth byte that is not part of
  it — and because that `0xW` is an `AddAddress`, the extra byte becomes part of a **pointer**, and
  the read after it lands at an address the definition never named;
- the alignment test was `(address & (numBytes - 1)) == 0`, which assumes a power of two. With
  three the mask is 2, so an address that is 1 mod 4 **passes a test it should fail** and a 32-bit
  load happens at an odd address — where the ARM9 returns the word rotated rather than faulting.

Every width now goes through `ra_read()`, which assembles anything that is not a native aligned
width from bytes. **This is not claimed to be the Data Abort** — the faulting address is in the
definitions block, not in game RAM, so it was not a peek. It is a bug that would have made
achievements silently never fire, found because a real set exercised a path our own definitions
never did.

##### The fix that makes the next run diagnose itself

What is left unmeasured is the one thing that scales: **total time inside the game's VCOUNT
handler.** Fifty-six parses in one interrupt, on a retail game that is booting, with
nds-bootstrap's card hooks live. `rcInitTotal` was added to measure exactly that and the run died
before it could be read.

So activation is spread over frames — `RA_RC_LOADING`, one definition per tick, roughly one second
for the set. That removes the suspected cause, and if it was not the cause it removes it from the
list of suspects, which is worth as much.

The second half matters more: **`rcActivated` is published after every definition**, so a crash
names the line it died on. A set that dies at the same definition every time is one definition's
problem; a set that gets through all fifty-six and dies later is the frame budget's. Those are
different bugs with different fixes and the previous build could not tell them apart — it published
nothing until all fifty-six were in, which is exactly the value you do not get from a crash.

`RA_RC_LOADING` was inserted at 5, which moved **`RA_RC_ACTIVE` to 6 and `RA_RC_FRAME` to 7.** Every
guard in the cardengine reads `rcStage < RA_RC_ACTIVE` and needed no change, but readings
photographed before this build read one lower — the `rcStage 06` in the checklist above is now
`07`. Renumbered rather than parked above `RA_RC_ACTIVE`, because a loading state that sorts after
active would invert every one of those guards.

The host test drives the state machine the way hardware does — prepare, then one call per
definition, bounded — so a machine that never reports `ACTIVE` fails in milliseconds instead of
wedging a console. Two smaller things fell out of that: `ra_rc_prepare()` now resets every static
it uses, because the test prepares twice and a count carried over from the first run showed up
immediately; and a test asserting `wramTicks == 10` became `before + 9`, since the absolute number
encoded how many ticks happened earlier in the file.

#### Second and third runs: it is the game's IRQ stack

The build that spread activation over frames crashed too, and so did a set trimmed to almost
nothing:

| Run | Set | Crash |
| --- | --- | --- |
| all 56, one tick | 1,946 conditions, 28,585 bytes | `PC 0377EEEA` (in the definition text), `ADDR 0377EEF2` |
| all 56, one per frame | same | `PC 023C39B8`, **`ADDR 00000000`** |
| **48 light lines** | **311 conditions, 5,311 bytes** | `PC 023C3C58`, **`ADDR 00000000`** |

The third run is the one that settles it. `A-liviano` is lines 9-56 — no 6,264-byte monsters, 16% of
the conditions, and **27,664 bytes of the arena out of 158,644**. It still crashes, with the same
signature. That kills three hypotheses at once: the frame budget, total memory, and any particular
heavy definition.

And `0x023C3C58` is not ours. Nothing nds-bootstrap places in DSi mode lives at `0x023Cxxxx` —
those addresses in `locations.h` are B4DS — and SM64DS's ARM9 binary ends at `0x0205D544`. So both
of the last two crashes have the ARM9 executing **the game's own data** with a null data address,
which is what corrupted game memory looks like from the outside.

##### The measurement that names it

`ra_tick()` is called from `myIrqHandlerVcount()` in `cardenginei_arm9`. Everything in this project
runs **inside the game's VCOUNT interrupt handler, on the game's IRQ stack**, whose size is the
game's business and was never checked. Measured on a host with `-finstrument-functions`:

| | |
| --- | --- |
| `rc_runtime_do_frame()` | **767 bytes** |
| `rc_runtime_activate_achievement()` | **2,383 bytes** |

`do_frame` has run every frame for many sessions without trouble, so the IRQ stack accommodates
767 bytes plus the cardengine's own frames. **The parse wants 3.1 times as much**, and the parse is
what a real achievement set multiplied: three of them became fifty-six.

Note what the earlier stack measurement did and did not prove. It showed depth is *flat* — 2,079 to
2,383 across the set, and the same 2,383 for the definitions that have always worked — and that
correctly killed "the big definitions recurse deeper". It was read as exonerating the stack
entirely, and that was the wrong conclusion from a right measurement: **flat and too large is still
too large.** What changed between working and crashing was not the depth of one excursion but how
many times it was taken, and over memory the game was by then using.

Which makes the three-definition builds luck rather than a result. They overflowed too — three
times, during boot, over memory the game had not started using yet. This document has a section
about the last time luck was mistaken for a result, and it now has two.

##### The fix, and the number that will confirm it

rcheevos gets **a stack of its own**: 8 KB in the cardengine's window, against a measured 2,383,
with the arena still holding 130 KB of margin at this set size. `ra_rc_on_stack()` switches `sp`
around each step and puts it back — `r4` and `r5` carry the old `sp` and the target and are in the
clobber list, which is what stops the compiler placing an input in either. `do_frame` runs on it
too, so there is one stack to reason about rather than two and the high-water mark covers
everything rcheevos does.

That mark is reported as **`rcStackUsed`** (`+0x6A`, `0x027FEDBA`), measured the way the host test
could not: the region is painted once and the deepest word that changed bounds every excursion of
the session. It went into `reserved2`, so no offset above it moved.

The reading to take is therefore a *prediction*: **`rcStackUsed` should land near 2,383** — the
host's figure, on real hardware, for the first time. Well under it would mean ARM frames are
tighter than x86-64's, which is plausible and worth knowing. **8192 exactly** would mean the paint
was consumed to the last word, and then 8 KB is not enough either.

The host build calls straight through instead of switching stacks, so `tools/ra_reader_test.sh`
does not exercise the switch. What it does exercise is that everything reached through it still
works — which is worth saying plainly rather than leaving implied.

##### Confirmed on hardware: it runs

Both sets boot and play with the private stack in place — `A-liviano` (48 definitions) and
`C-todo-56` (the whole published set). Super Mario 64 DS reaches its file-select screen and plays.

That is the diagnosis confirmed rather than a fix that merely stopped a symptom: the only thing
that changed between the crashing builds and this one is *which stack rcheevos runs on*. The set,
the arena, the definition count and the per-frame work are all identical to the run that produced a
Data Abort.

The three numbers the run was for — `rcStackUsed`, `rcInitTotal` and `rcLinesMax` — have not been
read yet. Everything on this screen is in one place, so a single hex-viewer photograph settles all
of them:

| | |
| --- | --- |
| RAM Viewer at | **`0x027FEDB0`** |
| covers | `0x027FEDB0`–`0x027FEDEF`, which is the whole rcheevos half of the snapshot |
| `rcStackUsed` | `0x027FEDBA`, 2 bytes — **predicted near 2,383** |
| `rcInitTotal` | `0x027FEDEE`, 2 bytes |
| `rcLinesMax` | `0x027FEDD5`, 1 byte, out of 263 |
| `rcFirstTriggered` | `0x027FEDED`, 1 byte — **predicted 1**, about five seconds in |
| `rcPeeksRejected` | `0x027FEDD0`, 4 bytes — the field most likely to be non-zero |

##### It runs, and here is every number

Super Mario 64 DS boots and plays with all 56 published definitions active, and the snapshot at
`0x027FED50` was read out of the in-game RAM viewer. The whole rcheevos half, decoded:

| Field | Read | |
| --- | --- | --- |
| `rcFromFile` | **1** | the staged file was used, not the built-in self-test |
| `rcDefLength` | **28,584** | 28,585 minus the trailing newline the reader trims |
| `rcActivated` | **56** | every definition |
| `rcActivate` / `rcBadLine` | **0 / 0** | none refused |
| `rcStage` | **7** | `RA_RC_FRAME` |
| `rcTriggerState` | **5** | `RC_TRIGGER_STATE_TRIGGERED` |
| **`rcFirstTriggered`** | **1** | **the prediction. Line 1 is `1=1.300.` and it unlocked first** |
| `rcTriggered` | **1** | |
| `rcEvents` | 88 | 87 of them not unlocks — primes, unprimes, activations |
| **`rcPeeksRejected`** | **0** | **every address in a real server set translates** |
| `rcPeeks` | 69 | distinct addresses read per frame |
| **`rcStackUsed`** | **1,624** | of the 8,192 given |
| **`rcInitTotal`** | **1,765** scanlines | 6.7 frames to parse the set |
| **`rcInitLines`** | **210** of 263 | the slowest single definition |
| **`rcLinesMax`** | **67** of 263 | steady-state, 1,946 conditions |
| `heapSize` / `heapUsed` | **149,288 / 110,472** | 74%, 38,816 free |
| `heapBreak` / `heapTop` | `0x037538D8` / `0x03778000` | the top is exactly the definitions block |

The snapshot is internally consistent — `heapTop - heapBreak` equals `heapSize` to the byte — which
is worth checking before trusting any of the rest of it.

##### What the readings say, including where the host was wrong

**`rcFirstTriggered = 1`.** The set's first definition is `1=1.300.` — always true, three hundred
hits — and it is what unlocked, about five seconds in, exactly as predicted before the run. Any
other line here would have meant a definition reading memory it should not.

**`rcPeeksRejected = 0`.** This was the field most likely to be non-zero: hundreds of addresses
written by other people, up to `0x00189074`, none of them chosen by this project. Every one
translates and every one is readable. `rc_runtime_validate_addresses()` disabled nothing.

**`rcStackUsed = 1,624`, against the host's 2,383.** The host over-estimated by 47%, which is the
branch named in advance: ARM frames are tighter than x86-64's. So 8 KB is 5× the requirement. It is
also still more than twice what `rc_runtime_do_frame()` needs, which is why the parse was the thing
that overflowed the game's IRQ stack and the evaluation never did.

**`heapUsed = 110,472`, against the host's 128,352.** The host was **14% high**, and the reason is
in the measuring tool rather than in the code: `tools/ra_fit_test.c` counts
`malloc_usable_size()`, and glibc rounds every block up. The direction is the safe one for a
question about whether something fits, and the real figure is now the console's.

**`rcLinesMax = 67 of 263` — the set costs a quarter of a frame, every frame.** Three definitions
cost 1 scanline; 1,946 conditions cost 67. That is the answer to the question step 4 opened, and it
fits with room to spare.

**`rcInitTotal = 1,765 scanlines = 6.7 frames.** So the build that parsed all 56 in a single
interrupt held the game's VCOUNT handler for nearly seven frames. Spreading it was not a
precaution; it was necessary.

##### The new tightest constraint: one definition is 80% of a frame

`rcInitLines = 210 of 263`. A single `rc_runtime_activate_achievement()` on the 6,264-byte
definition costs **four fifths of a frame inside the game's interrupt handler**, and that is now the
number that bounds this design rather than the arena or the frame budget.

It is under a frame, which is why one-definition-per-tick works. What happens past a frame is worth
being precise about instead of alarming: the handler would simply not return before the next VCOUNT
was due, so the game would lose a frame rather than crash — `rcInitLines` would saturate at 255 and
say so. A game whose largest definition is twice this one would drop a frame or two while loading
and then run normally.

So the honest statement is that this scales gracelessly rather than dangerously, and the fix if it
ever matters is to parse in smaller pieces than one definition — which rcheevos does not offer, so
it would mean holding a partially parsed trigger across ticks. That is real work and there is no
reason to do it for a cost nobody has yet felt.

### What is deliberately not being changed yet

The self-test watches stay. `ra_definitions-14856.txt` contains no `W:` lines, so `anyWatch` stays
false and the four built-in watches remain — which is the designed behaviour and useful here, since
they are the thing that says the reader is alive at all independently of whether any achievement
fires.

And nothing amortises the parse across frames. That is the obvious fix if `rcInitTotal` comes back
large, and doing it before measuring would be guessing at a cost — the same mistake as the three
wrong heap lines in step 3.

## Step 4, online half: fetch at boot, then boot

The offline half proved the game runs the server's set. The online half is two separate problems that
have been conflated all along, and separating them is most of the work:

**4a — fetch in the launcher, then boot the game.** No network inside the game at all. The launcher
already does the whole ladder and stages the set; the only reason it never booted afterwards is that
mode 1 halts deliberately. This is the one that makes the thing usable.

**4b — network *inside* the game**, which is what `r=awardachievement` needs to tell the server an
achievement unlocked. That is context B, where the ARM7 belongs to the game, and it is the unknown
*#1g* has flagged from the beginning.

4a is done. 4b is not started.

### What made 4a possible: the teardown already existed

Mode 1 halts because handing a live radio to the bootloader is dangerous in a specific, nameable
way. Four things are running when the ladder finishes, and every one of them fires *after* the
bootloader has replaced the code it belongs to:

| | |
| --- | --- |
| ARM7 | `IRQ_WIFI_SDIO_CARDIRQ` on the AUX controller |
| ARM7 | `TIMER3`, dsiwifi's 100 ms SDIO tick |
| ARM9 | `TIMER3`, which drives `ath_lwip_tick()` |
| ARM9 | dsiwifi's datamsg handler on `FIFO_DSWIFI` |

`DSiWifi_DisconnectAP()` is an unimplemented `sassert(false, ...)`, so the obvious call is not there.
But **`wifi_card_deinit()` in dsiwifi's ARM7 half does exactly the right four things** — masks the
SDIO card IRQ, disables the AUX IRQ, disables `TIMER3`, and writes zero to the chip's
`F1_INT_STATUS_ENABLE` and CCCR `irq_enable`. It is declared in `arm_iop/source/wifi_card.h`, which
is not on the exported include path, so it is declared by hand the way dsiwifi's own `test_app`
does. The ARM9's two are ours to stop.

What none of it does is **power the chip down** — dsiwifi has no path for that. So the game boots
with the radio still associated to the access point and its interrupts masked. Said plainly rather
than glossed: nothing can poke either CPU and nothing is moving memory, but the chip is on. That is
also the most interesting thing 4b inherits.

#### Order, and the one refusal

The ARM7 goes first, because it is the one holding the chip: until its card interrupt is masked the
radio can still call it. Only then are the ARM9's timer and FIFO handler stopped — the other way
round and the ARM7's log messages would arrive at a channel with no handler while it was still
working.

The handshake is a round trip on `FIFO_USER_07` (01 through 06 are all spoken for; a channel with
two owners is the bug this project already paid for once with `FIFO_DSWIFI`). And
`wifi_card_deinit()` is called from the ARM7's **idle loop, not from the FIFO handler that asks for
it** — it writes SDIO registers and polls for the controller to answer, which is a bounded wait but
not one to take inside an interrupt on a CPU that is about to be overwritten.

**If the ARM7 never acknowledges, mode 2 halts exactly as mode 1 does.** A halt is a bad outcome and
it is the *better* bad outcome: the alternative is a crash somewhere inside the game, minutes later,
with nothing on the card explaining it.

### Confirmed on hardware: fetched at boot, then played

```
reached stage 12 of 12
the set is staged for the cardengine.
definitions to   sd:/ra_definitions.txt

-- giving the radio back --
AR6014 deinitted
ARM7            deinitted
ARM9            timer and FIFO handler stopped

radio down -- booting the game.
```

Log at `docs/logs/ra_wifi_launcher_boot-3ds.log`. **The console logged in, fetched the published set
for GameID 14856, tore the radio down, booted Super Mario 64 DS, and played.** About **15 seconds**
from power-on to the game, and the game felt normal.

`AR6014 deinitted` is worth pointing at: that line is **dsiwifi's own** `wifi_printlnf()`, from inside
`wifi_card_deinit()`. So the teardown is confirmed by the driver rather than by our acknowledgement —
the ARM7 really executed it, and the message made it back over the FIFO before the ARM9 removed the
handler. The three lines together also confirm the ordering the design depends on: the driver spoke,
then our ARM7 confirmed, then the ARM9's own two were stopped.

There is no `restored ra_achievements.txt` line, which is how the log says the set came off the
network rather than off the card.

#### The set is byte-identical, and so is the behaviour

`sd:/ra_definitions.txt` from this run **diffs clean** against
`docs/logs/ra_definitions-14856.txt`, the file the earlier fetch produced. And the snapshot is
identical field for field to the offline run:

| Field | From the file | From the network |
| --- | --- | --- |
| `rcStackUsed` | 1,624 | 1,624 |
| `rcInitLines` / `rcInitTotal` | 210 / 1,765 | 210 / 1,765 |
| `rcLinesMax` | 67 | 67 |
| `rcActivated` / `rcDefLength` | 56 / 28,584 | 56 / 28,584 |
| `rcFirstTriggered` | 1 | 1 |
| `rcPeeksRejected` | 0 | 0 |
| `rcEvents` | 88 | **132** |

Every number holds except `rcEvents`, and that one moved because the play session was longer — it
counts primes and unprimes, not unlocks. `rcTriggered` is 1 in both.

Identical is the right result and it is worth saying why it is not a tautology. The bytes are the
same bytes, so equal behaviour is expected — what the comparison actually establishes is that
**nothing about arriving over the network changed how the set behaves**: not the streaming scanner's
output, not the radio being left powered and associated, not the teardown happening between the
fetch and the game. A difference in any of those would have shown up here.

#### One thing the snapshot cannot tell you

`rcFromFile` reads 1 in both columns, because it means *"the staged block was used"* rather than
*"the definitions came from a file"* — a distinction that did not exist when it was named. And
`rcDefLength` cannot separate them either, since a successful fetch produces exactly the bytes the
file holds.

So provenance is a question only the log answers today. A byte for it is available — `reserved2` at
`+0x69` — but the launcher would have to get it *into* the block for the cardengine to see, and the
only channels are a second magic value or a spare bit in the length word. Both mean touching the
bootloader's magic check, which is proven code, for a diagnostic convenience the log already
provides. Left undone deliberately, and written down so the next person does not rediscover the
ambiguity from scratch.

#### What 4a costs, and what is left

15 seconds on every boot, and it is not optional in this build: the ladder runs before the game
regardless. That is the obvious next refinement — `ra.cfg` already exists as the place to say
"don't", and the failure paths already fall through to booting. Nothing about it is hard; it simply
has not been done.

**4b is untouched.** Unlocking an achievement on the server needs `r=awardachievement` at the moment
it fires, which is network *inside* the game, where the ARM7 belongs to the game. Two things this run
established bear on it, and they point in opposite directions: the radio is still powered and still
associated when the game starts, which is the encouraging half; and the launcher's ARM7 needed all of
dsiwifi resident to get there, against the **18 KB of IWRAM** the cardengine's own ARM7 hooks leave
free in context B.

### The regression 4a introduced, and the fix

Stage 12 invalidates the definitions block before it sends the request, so that a reply which never
arrives cannot leave stale bytes looking valid. In mode 1 that was free. In mode 2 it is not,
because **the block already contains the user's own `ra_achievements.txt`** — `loadRaDefinitions()`
stages it during `loadFromSD()`, well before the ladder runs. A console with no network would
therefore lose the definitions it would otherwise have played with, which is strictly worse than not
having tried.

Preserving it is not available: the reply is three times the size of the block, which is the whole
reason it is streamed into it. So the file is **re-staged** on every failure path that happens after
the invalidation — one file read, only on a path that already failed. `loadRaDefinitions()` stopped
being static for that, and says why in its own comment.

### Three modes now, and the stamp had to learn to count

`RA_LAUNCHER_WIFI` is 0, 1 or 2: not built, the diagnostic that halts, and fetch-then-boot. Mode 1
stays exactly as it was, because every measurement in this document was taken in it.

The build stamp that wipes objects when the mode changes was `$(if $(filter 1,...),on,off)` — which
cannot tell 1 from 2, and would have handed back objects built for the wrong mode. That is the same
silent-success failure the stamp was added to prevent, so it now records the raw value. Verified by
building 2, then 1, then 0 and checking for each mode's own strings in the `.nds`.

One thing worth checking that turned out to be a non-issue: mode 2 calls `myConsoleDemoInit()` and
then boots, which no build had done before. It is fine, and not by luck — `createRamDumpBin()` and
the pagefile creation both do exactly that on the first run of any game.

### What the run has to answer

| | |
| --- | --- |
| does the game boot at all after the radio is torn down | the whole question |
| `sd:/ra_wifi_launcher.log` ends with `radio down -- booting the game` | the teardown was clean |
| the definitions the game runs are the **server's**, not the file's | `rcDefLength` should be the fetched length, not 28,584 |
| how long the boot takes | the ladder is ~10-20 s, and it is on every boot |

And the failure that would be worth the most: a log ending at `the ARM7 never confirmed
wifi_card_deinit()`. That is mode 2 refusing to boot rather than crashing, and it would mean the
teardown needs more than dsiwifi's own four steps.

## Step 5: the block carries achievement ids

The staged definitions had no ids. Every line was a memaddr and nothing else, and the cardengine
numbered them 1..56 by position. That was invisible while nothing reported anything, and it blocks
everything that would:

| | |
| --- | --- |
| `r=unlocks` | answers in ids, so "which of these are already earned" has no answer |
| `r=awardachievement` | is *asked* in ids |
| a popup, a leaderboard, rich presence | all name an achievement |

So each line is now `<id>:<memaddr>`. Done here rather than later because the format has exactly one
producer and one consumer today, and both are in this tree.

### Digits then a colon, and why that is exact rather than heuristic

The discriminator has to survive a definition that *begins with a digit*, because the real set's
first line is `1=1.300.`. It does, and not by luck: **every memaddr prefix flag that ends in a colon
is a letter** — `A:`, `B:`, `C:`, `G:`, `I:`, `K:`, `M:`, `N:`, `O:`, `P:`, `Q:`, `R:`, `T:`, `Z:`.
Checked against the shipped set rather than asserted: of its 47 lines containing a colon, the
character before the first one is `M`, `N`, `O`, `P`, `R` or `T`, never a digit. And no line already
matches `<digits>:`.

A line with no id still works. `docs/logs/ra_definitions-14856.txt` — the artifact the console
produced before this step — has none, and a hand-written `ra_achievements.txt` is not expected to.

### The bug this found before it shipped: the first achievement inherited the game's id

The scanner tracks the most recent `"ID":` and attaches it to the next `MemAddr`. Ids arrive *before*
their MemAddr, which is the opposite of `Flags` and makes them much simpler — no deferral needed.

Except that the reply opens `{"Success":true,"PatchData":{"ID":14856,...` and only then reaches
`"Achievements":[{"ID":1,...`. So an achievement that arrived **without** an id of its own would
inherit the *game's*, and be counted as having one. That is worse than having none: a wrong id
reports an unlock for an achievement the player did not earn, where a missing id reports nothing.

The fix is one character: **clear the pending id at every `{`**, because an id belongs to the object
it was written in. Safe against a brace inside a string because of RA's field order — `ID` is first
in each object and `MemAddr` immediately after, so there is no text between them for a stray `{` to
sit in, and a brace in a Title lands after the id has already been consumed. The host test feeds
exactly the id-less-first-achievement case and requires the id *not* to appear.

The needle also carries its opening quote, which is what keeps `"GameID":` and `"ConsoleID":` from
matching — both contain `ID":` and neither has a quote before the `I`. Pinned as its own test.

### `RA_SYNTHETIC_ID_BASE`, which is not tidiness

A line without an id needs one anyway, because rcheevos identifies achievements *by id* and reuses
the trigger of one it has already seen. Numbering the id-less ones from 1 — as every build before
this did — was safe only while nothing carried a real id. The moment both appear in one file, a real
id of 3 collides with the third id-less line and two definitions become one achievement.

So id-less lines are numbered from **`0xF0000000`**. Real RA ids are six or seven digits and cannot
reach it.

Two smaller consequences of ids being real, both of which would have been silent:

- `rc_runtime_get_achievement_measured()` and `rc_runtime_get_achievement()` were asked about the
  constant `RA_TEST_ACHIEVEMENT_ID`. That was the first definition's id only while everything was
  numbered from it; now they ask about `defIds[0]`, whatever it turned out to be.
- the event handler derived a line number as `id - base + 1`. There is no arithmetic that recovers a
  line from a server-assigned id, so it searches `defIds` — at most 128 entries, on the frame an
  achievement unlocks.

### What to read

The snapshot grows from `0xA0` to `0xA8`, appended, so **every offset above keeps its address** and
the checklist stays valid.

| | | |
| --- | --- | --- |
| `rcFirstId` | `0x027FEDF0`, 4 bytes | the RA id of the first achievement to unlock — a number you can look up on the set's page |
| `rcDefsWithId` | `0x027FEDF4`, 2 bytes | staged lines that carried an id |
| `rcDefsNoId` | `0x027FEDF6`, 2 bytes | and lines that did not, which can never be awarded |

And in the log, stage 12 gains `ids   N with, M without`.

**Predictions:** `rcDefsWithId` 56 and `rcDefsNoId` 0 with a fetched set — RA sends an id per
achievement, and one short would mean one achievement that can never be awarded. `rcFirstId` should
be the id of whichever achievement `1=1.300.` is on the set's page, and `rcFirstTriggered` should
still read 1.

The block grows by the ids: 56 of about six digits plus a colon is **+392 bytes**, taking it from
28,585 to roughly **28,977 of 32,759 — 88.5% full**. Which moves the block-size question from
"comfortable" to "worth watching", and `wanted` reports it either way.

### Confirmed on hardware: the ids arrived, and one of them is worth a second look

```
definitions      56 kept, 3 unofficial
ids              56 with, 0 without
block            28943 of 32759 used, 28943 wanted
def 1    18      101000001:1=1.300.
def 2  1232      93121:R:0xH09cab4>0_R:0xH09cab5>0_...
def 3  6270      93119:0xT0009caa8=0.100._P:0x 0017e874=64.1._...
```

Log at `docs/logs/ra_wifi_launcher_ids-3ds.log`, set at `docs/logs/ra_definitions-14856-ids.txt`.
And from the snapshot:

| Field | Read | |
| --- | --- | --- |
| `rcDefsWithId` | **56** | every definition carried one |
| `rcDefsNoId` | **0** | |
| **`rcFirstId`** | **101000001** | and line 1 of the dump is `101000001:1=1.300.` |

`rcFirstId` matching the file to the digit is the end-to-end proof: server → streaming scanner →
staging block → bootloader copy → the cardengine's splitter → rcheevos' own identity for the
trigger → the event handler → the snapshot. Eight hops, one number.

All 56 ids are distinct, which matters more than it looks: rcheevos identifies achievements by id
and *reuses the trigger of one it has already seen*, so a duplicate would have silently merged two
achievements into one. The block landed at **28,943 of 32,759 — 88.4% full**, against a prediction
of 28,977; ids average 5.4 characters rather than the 6 assumed.

#### `101000001`

Every other id in this set is between **92,869 and 579,308**. That one is nine digits, its
definition is `1=1.300.` — always true, three hundred hits — and **it is the one that unlocks**,
about five seconds into every session.

What that is, I do not know, and it is not something to guess at: `retroachievements.org/achievement/101000001`
settles it in one click. What matters is the consequence, and it is concrete. **Step 6 would report
this unlock to the server on every single boot.** So the id needs identifying before anything reports,
and if it turns out not to be a published achievement then the set needs a filter this project does
not have yet — `Flags` 3 versus 5 does not separate it, since it arrived as core.

#### `101000001` is not a published achievement, and it is not being filtered

Two lookups settled it. `retroachievements.org/achievement/101000001` returns **NOT FOUND**, and the
set's own page — which redirects to `retroachievements.org/game/9983?set=6112` — lists **55**
achievements against the **56 core definitions** that arrive. The extra one is exactly that id, and
its definition is `1=1.300.`: always true, three hundred hits, unlocking five seconds into every
session.

**It is still staged, and that is deliberate.** One set and one id is not a rule. A filter on
"ids at or above 100,000,000 are not real" would be inferred from a single data point, and the first
time RetroAchievements numbers a genuine achievement differently it would drop something real —
silently, which is the failure this document keeps refusing to ship.

So what happens instead is that the reply's own bytes are captured. When an id at or above
`RA_ODD_ID_FROM` completes, the scanner copies the following **240 bytes of the reply verbatim** into
`oddContext` and the log prints them. The fields that identify an object — `Title`, `Points`,
`Flags`, `Type` — all follow the id inside it, so this is the one place they can be caught without
holding a reply that does not fit in memory. One example, the first, because one is what identifies a
shape.

The likely answer is already written down as a known limitation of the scanner: **it does not know
which object a `MemAddr` belonged to**, and this game has subsets. A flat scan over a reply
containing more than one set reads across all of them. If the captured context shows this id sitting
in a second set rather than in `Achievements`, the fix is a structural one — track which array the
scanner is inside — and not a threshold.

#### It was the server talking to us: `Warning: Unknown Emulator`

The capture came back with this, verbatim out of the reply:

```
"Title":"Warning: Unknown Emulator",
"Description":"Hardcore unlocks cannot be earned using this emulator.",
"MemAddr":"1=1.300.","Points":0,"Author":"","Modified":1786239426
```

Log at `docs/logs/ra_wifi_launcher_notice-3ds.log`.

**RetroAchievements injected it.** It is a message to the player wearing an achievement's clothes:
always-true after three hundred frames, so that a normal RA client pops it up about five seconds into
a session. Zero points, no author. The nine-digit id is the range the server uses for these.

Three things follow, and the first is that a guess was wrong.

**The subset theory is retired.** The suspicion was that a flat scan was reading across this game's
subsets — its page redirects to `game/9983?set=6112`, which made that plausible. It is not what
happened: the entry sits in `Achievements` with `Flags` 3 because the server put it there. The
scanner's "does not know which object a key belonged to" limitation is still real and still written
down; it is simply not the explanation here.

**It is dropped now, and the filter is against evidence rather than a threshold.** That was the whole
reason for capturing instead of filtering: the rule "ids at or above 100,000,000 are not
achievements" is the same line of code either way, but now it is justified by what the object *is* —
zero points, empty author, a Description addressed to a human — rather than by one id looking odd.
Staging it spent 19 bytes of an 88%-full block on an entry step 6 would have tried to award on every
boot. It is counted, and its context is still printed, so the server's message reaches the log even
though the definition no longer reaches the game.

**And the third is the interesting one: the server is gating hardcore on a User-Agent this project
never registered.** `ra_net.c` sends `User-Agent: nds-bootstrap-ra/0.1`, RetroAchievements does not
recognise it, and the consequence is exactly what the notice says. That is not a bug to fix in code —
it is a conversation with RetroAchievements about a client identifying itself, and it belongs on the
project's list rather than in a commit. It does retroactively justify `hardcore=0` in `ra.cfg`: this
fork chose softcore, and the server has independently decided the same thing.

Two smaller observations from the same run. The reply came back **82,811 bytes** where the previous
fetch was 87,747 — 4,936 fewer, so `r=patch` is not byte-stable between calls and nothing should
assume it is. And `safe` fell to **57,344**, from 61,440, which is the 240-byte capture buffer and the
statics around it; the fetch still allocates nothing.

**Prediction for the next run:** `definitions 55 kept`, matching what retroachievements.org lists
for the set exactly, with `1 server notice(s) dropped` beside it and the block at 28,924 bytes.

#### The bug the data walked into

`101000001` is nine digits, and the clamp guarding both id parsers was:

```c
if (id < 100000000u) { id = id * 10 + digit; }
```

written on the reasoning that "RA ids are six or seven digits today". That value survives it — but a
**ten-digit id, which a `u32` holds perfectly well, would have come out one digit short**. Silently,
and naming a different achievement. Which is exactly the failure mode called out one section above as
worse than having no id at all, reintroduced two paragraphs later by a lazy bound.

Both parsers refuse on overflow now instead of clamping, the same discipline `raNetJsonNumber()` has
had since step 3c: a value that will not fit is not an id. In the scanner the definition is counted in
`withoutId`; in the cardengine's `ra_take_id()` the prefix is **still stripped** even when the number
is refused, because a line left with `<digits>:` on the front is not memaddr syntax and rcheevos would
refuse the whole definition. Losing the ability to report one achievement beats losing the achievement.

Pinned with `101000001` from the real set, `4294967295` at the u32 boundary, and `99999999999` past it.

### Step 5 closed, and step 6a: `r=unlocks`

The run with the notice filtered read exactly what was predicted:

```
definitions      55 kept, 3 unofficial
ids              55 with, 0 without
1 server notice(s) dropped, first id 101000001
block            28924 of 32759 used, 28924 wanted
memaddr length   58 shortest, 6264 longest
```

Log at `docs/logs/ra_wifi_launcher_55-3ds.log`. **55 is the number retroachievements.org lists for
the set**, so the client and the site now agree on what the set contains. And `shortest` moving from
8 to 58 is the incidental confirmation: the 8 was the notice's `1=1.300.`, and with it gone the real
shortest definition is 58 characters.

One consequence worth stating before the next in-game reading, because it will look like a
regression: **the thing that unlocked five seconds into every session was the notice.** With it
filtered, `rcTriggered`, `rcFirstId` and `rcFirstTriggered` stay at 0 until a real achievement is
earned. That is correct and it removes the quick canary — what says the runtime is alive now is
`rcPeeks` at 69 a frame, `rcActivated`, and `rcStage`.

#### The new rung, and why it goes before the fetch

`r=unlocks&u=&t=&g=&h=0` answers with the ids the account already holds. It is **stage 12**, which
pushed the fetch to 13, and the order is the whole point: the block is 88% full with a set this size,
and a definition already earned is one that does not need to be in it. The arena and the per-frame
budget follow the block down.

**It fails open.** A request that does not answer leaves the skip list empty and every definition
stages, which is exactly the behaviour before this rung existed. What the code is careful about is the
difference between *"the account has earned nothing"* and *"we could not ask"* — both stage
everything, and only the second deserves a warning. `raNetJsonIdList()` returns **0 for an empty list
and −1 for a missing key** for that reason, and the host test pins both.

`h=0` because this fork is softcore, which `ra.cfg` says and the server independently agrees with —
see the `Warning: Unknown Emulator` notice.

#### Left out of the block, not staged and skipped

The filtering happens in the scanner, so an already-earned achievement never occupies a byte. A
player who has earned half of a set gets half the block back.

What that costs is that the cardengine cannot know those achievements exist, so it could not one day
show "30 of 55 earned". That is worth the space today and worth writing down, because the fix is a
format change rather than a flag.

Truncation is safe and still reported: the skip list holds `RA_WIFI_UNLOCKS_MAX` of 128, matching
`RA_DEFS_MAX_LINES`, and a set larger than that stages a few already-earned definitions again — block
space rather than correctness.

#### What to read

| | |
| --- | --- |
| `already earned    N` | how many the account holds, from the server |
| `  earned id      X` | the ids themselves, up to eight, labelled `(server notice)` past `RA_ODD_ID_FROM` |
| `already earned   M of N matched this set` | how many of those were in this set, printed even when M is 0 |
| `N named id(s) this set does not contain` | yellow, and only for a remainder the notices do not explain |
| `definitions      M kept` | should be 55 minus that |
| `block ... used` | should fall by the same definitions' worth |
| `reached stage 13 of 13` | the ladder grew a rung |

The rung numbers in this section are the ones this step shipped with. Step 6b later inserted
`r=awardachievement` at 12 and pushed these to 13 and 14 — the lines themselves are unchanged, and the
logs quoted above still say 12 and 13 because that is what they said.

A fresh account on this game reads `already earned 0` and changes nothing, which is the useful
control: the stage is proven by an account that *has* unlocks, so the number to compare against is
whatever retroachievements.org shows for the set.

#### The third socket lost a race inside lwip

The first run with `r=unlocks` in it never reached the fetch:

```
-- stage 11: does the server know this ROM --
asking about     c3b1916756737f2c4117cc95c1d51ac7
Assert "state!" failed at line 1411 in .../lwip/api/api_msg.c
gameid HTTP failed at step 4
```

Log at `docs/logs/ra_wifi_launcher_lwipassert-3ds.log`. Step 4 is `RA_NET_NO_CONNECT`, so
`connect()` returned an error, and lwip printed an assert on the way out.

`api_msg.c:1411` is the assert *after* the semaphore wait:

```c
msg->conn->current_msg = msg;
UNLOCK_TCPIP_CORE();
sys_arch_sem_wait(LWIP_API_MSG_SEM(msg), 0);
LOCK_TCPIP_CORE();
LWIP_ASSERT("state!", msg->conn->state != NETCONN_CONNECT);   /* <- 1411 */
```

The wait has no timeout, so it returning at all means the semaphore was signalled — and the state
being still `NETCONN_CONNECT` means the connect had *not* completed when it was. A semaphore that was
already signalled before the wait began produces exactly that, and netconns come from a static pool
of eight (`MEMP_NUM_NETCONN 8`), so a recycled netconn carrying a stale `op_completed` count is the
shape that fits.

**Nothing in stage 11's path changed.** `r=unlocks` is stage 12 and had not run; the only difference
in that build below stage 11 is 4.6 KB more `.bss`, which changes no behaviour. So this is a
pre-existing race that has now shown itself once, on the third socket of a run, after several dozen
runs that did the same thing and did not.

##### What the failure proves about the design

Worth saying before the fix, because it is the part that was designed rather than lucky:

- the error came back as a **named step** — `step 4`, `RA_NET_NO_CONNECT` — rather than as a hang or a
  wrong answer;
- stage 12 said `no GameID or token; staging the whole set` and stage 13 said `no GameID; the set
  cannot be asked for`, both of which are the **fail-open** paths;
- stage 13's early return happens *before* the block is invalidated, so the user's own
  `ra_achievements.txt` survived untouched;
- the radio came down cleanly and **the game booted**.

A random lwip race cost this run its achievement set and cost nothing else.

##### The fix is a retry, and that is a choice rather than a shrug

It is not fixed at the root. The honest reason: it is a race inside a vendored lwip, on a console with
no debugger, and the tooling to chase a semaphore lifecycle there does not exist in this project.
Claiming otherwise would be worse than saying so.

What a retry buys is real: a second attempt draws a **different netconn** from the pool, so a poisoned
one is stepped over rather than fatal. `raNetConnect()` now tries up to `RA_NET_CONNECT_TRIES` times
with `RA_NET_RETRY_FRAMES` between them — and the gap is doing work rather than marking time, because
lwip's own processing runs off a 100 ms TIMER3 in dsiwifi's ARM9 half, so waiting frames is what lets
a half-finished netconn finish and go back to the pool.

It also removed duplication that was already a small liability: DNS, socket and connect existed twice,
once in `raNetHttpGet()` and once in `raNetHttpGetStream()`, so a retry policy would have had to be
written twice and could have drifted.

**`attempts` is reported.** A retry that succeeds silently would turn a measurable race into an
impression, so every request prints `needed N connect attempts` when N is more than one. That is the
number to watch across runs: rare is a curiosity, common is a reason to look at lwip properly.

#### Hardware: the ladder reached 13, and the skip list matched nothing

The run with the retry in it walked the whole ladder. Log at
`docs/logs/ra_wifi_launcher_unlocks-3ds.log`:

```
-- stage 12: what has this account already earned --
931 bytes back
already earned    1
-- stage 13: fetch the set --
body was         87747 bytes
definitions      55 kept, 3 unofficial
ids              55 with, 0 without
1 server notice(s) dropped, first id 101000001
block            28924 of 32759 used, 28924 wanted
reached stage 13 of 13
```

Two readings, and they point opposite ways.

**The race did not recur.** No `needed N connect attempts` line appeared, on any of the three
requests, so all three sockets connected first try. That is consistent with what a race is and is not
evidence the retry works — the retry has still never been observed doing its job. What it does prove
is that the retry costs nothing when it is not needed: same three stages, same timings, no extra line.

**The skip list matched nothing.** `already earned 1` and yet `definitions 55 kept` — the same 55, the
same 28,924 bytes, the same block as the run before `r=unlocks` existed
(`docs/logs/ra_wifi_launcher_55-3ds.log`, byte for byte). The account holds one achievement in this
game and the scanner found no definition to leave out.

This was predicted as the outcome that would be a finding rather than a bug: it means the two requests
are not naming the same thing. It is not a filter that fired too hard — a filter that dropped a
matching id would still have counted it.

##### A count cannot say which, and *which* is the whole question

The old report printed `already earned N left out of the block` only when N was non-zero, so this run
produced **no line at all** — and absence is indistinguishable from a line that was never written.
That is a reading by silence, which this project does not accept anywhere else, so both halves now say
their numbers out loud:

- stage 12 prints the earned ids themselves, up to eight, then `...and N more`. Eight because the
  point is to identify a mismatch, not to dump an account.
- stage 13 prints `already earned M of N matched this set` **whenever there was a skip list at all**,
  and adds `the server named ids this set does not contain` when M is zero. `0 of 1` says what a
  silence only implies.

Three possibilities remain, and the id is what separates them:

| the earned id is | what that means |
| --- | --- |
| ≥ 100,000,000 | the account "earned" the `Warning: Unknown Emulator` notice, which this client filters — the mismatch is our own doing and harmless |
| a five- or six-digit id not among the 55 | it belongs to a **different subset** of game 9983; the site redirects to `?set=6112`, and `r=patch&g=14856` returns one subset's definitions while `r=unlocks&g=14856` may answer for another |
| a number in no plausible range | `r=unlocks` and `r=patch` do not share a numbering, and step 6b cannot be built on the assumption that they do |

The first is a curiosity. The second is a real constraint on submitting unlocks — the id you award has
to be the id the set defines. The third would mean rethinking step 6b entirely. Nothing in the count
distinguishes them, which is why the next run reports the id and not a tally.

The prediction for that run, stated before it happens: **`earned id 101000001`**, because the notice is
an achievement the server injects into this game's set and the account has been shown it on every boot.
If that is what the log says, the mismatch closes as an artefact of our own filter and step 6b proceeds
on the 55. If it is a five-digit id, the subset question becomes the next thing to answer.

##### It was the notice, and the mismatch closes

Log at `docs/logs/ra_wifi_launcher_earnedid-3ds.log`:

```
-- stage 12: what has this account already earned --
930 bytes back
already earned    1
  earned id      101000001
-- stage 13: fetch the set --
definitions      55 kept, 3 unofficial
already earned   0 of 1 matched this set
the server named ids this set does not contain
1 server notice(s) dropped, first id 101000001
```

The one id the account holds in game 14856 is `101000001` — the `Warning: Unknown Emulator`
pseudo-achievement, the same id stage 13 reports dropping in the very next line. The prediction was
exact, so the mismatch is an artefact of this client's own filter and nothing else: `r=unlocks` named
an achievement, the scanner had already refused it, and the skip list therefore had nothing to skip.

**`0 of 1` was structurally guaranteed, not unlucky.** In `raPatchCommit()` the `RA_ODD_ID_FROM` test
(`ra_patch.c:197`) runs *before* the skip-list search (`:215`) and returns, so an id past that boundary
can be counted as `oddIds` or as `alreadyDone` but never as both. An unlocks list containing only
notices can only ever read `0 of N`.

That makes the yellow warning wrong — it fires on the arithmetic being unexplained when this case is
fully explained. So the boundary is applied on both sides now:

- stage 12 labels the id, `earned id      101000001  (server notice)`, rather than leaving the reader
  to notice that the number matches one four lines further down;
- stage 13 subtracts the notices before deciding. `alreadyDone + unlockNotices < skipCount` is what is
  actually unaccounted for, and only that stays yellow; a notices-only remainder prints the plain
  `of those, 1 is the server's own notice`.

One honest gap: `ra_wifi.c` needs `nds.h` and dsiwifi, so it is not one of the three host binaries and
this arithmetic is not pinned by a test. Both sides read the same `RA_ODD_ID_FROM`, which is what keeps
them from contradicting each other, and that is the whole of the guarantee.

##### What this does *not* settle

The tempting conclusion is that `r=unlocks` and `r=patch` share a numbering. This run does not show
that. `101000001` is a synthetic id the server injects, and it matching itself across two requests says
nothing about whether the five-digit ids line up — the account holds no real achievement in this game,
so no real id has ever made the round trip.

That question is answered by the loop step 6b builds and not before it: earn one achievement in the
game, and see whether `r=unlocks` on the next boot returns the same `9XXXX` the set defines. Until then
the subset possibility — the site redirects game 9983 to `?set=6112`, and `g=14856` is what both
requests are given — stays open. It is cheap to keep open, because the first real unlock closes it as a
side effect.

The second reading is smaller and worth writing down: **the retry has now not fired for two full runs.**
Six sockets, all first-try. The lwip race has been observed exactly once and the mitigation for it has
never been observed working. That is the correct thing to say about it.

## Step 6b: closing the loop with `r=awardachievement`

An achievement earned while playing cannot be reported when it happens. The cardengine runs inside
the game, on the game's IRQ stack, with the radio torn down before the game ever started — there is no
socket to write to and no CPU free to bring one up. So the unlock is written to a file and the **next**
boot's launcher sends it. An unlock is late, not lost.

That splits the step in two, and the split is what makes it testable:

- **3a, the sending half.** Read a queue file, sign each id, submit it, report, clear. Touches the
  network and nothing else. Provable *today* by typing an id into a file with a text editor.
- **3b, the writing half.** The cardengine appends to that file when rcheevos fires. Needs a
  cross-CPU handoff that does not exist yet — see below.

3a is what is built. Doing it first is not laziness about 3b: the signature and the API's answers are
the unknowns, and finding out that the protocol is wrong should not require touching the ARM7
cardengine that every non-RA path in nds-bootstrap also uses.

### The rung goes *before* `r=unlocks`, and that is the whole design

The submit is **stage 12**, which pushed unlocks to 13 and the fetch to 14. The order is not
housekeeping.

Suppose the submit ran last. We award 93119, then fetch the set — which still contains 93119, because
the account's unlocks were read *before* the award landed. The definition stages, the game triggers it
again next session, it gets queued again, and it is awarded again on the boot after that. Forever.

Sending first means the server already knows about it when the next rung asks what the account holds,
so the scanner leaves it out of the block. The loop drains instead of spinning. Three consecutive rungs,
each genuinely depending on the one before it:

```
11  r=gameid            what game is this
12  r=awardachievement  here is what last session earned
13  r=unlocks           so what does the account hold now
14  r=patch             fetch, minus those
```

`tools/ra_reader_test.c` walks all three in order and pins them consecutive, because renumbering three
constants by hand is exactly the edit that leaves a gap — and a gap makes `reached stage N of 14` mean
nothing.

### The signature, which is the only part that is expensive to get wrong

`v=` is what makes the server believe an unlock came from an account rather than from anyone who knows
an id. RetroAchievements answers a wrong one with a generic refusal that says nothing about hashing —
so a mistake there is indistinguishable, from a console, from a wrong achievement id.

The formula was **read, not remembered**, out of the vendored copy at
`retail/cardenginei/arm9_ra/rcheevos/src/rapi/rc_api_runtime.c`:

```c
md5_init(&md5);
snprintf(buffer, sizeof(buffer), "%u", api_params->achievement_id);
md5_append(&md5, buffer, strlen(buffer));
md5_append(&md5, api_params->username, strlen(api_params->username));
snprintf(buffer, sizeof(buffer), "%d", api_params->hardcore ? 1 : 0);
md5_append(&md5, buffer, strlen(buffer));
```

So `v = md5(id ‖ username ‖ hardcore)`, decimal text, no separators. The two extra appends further down
that function only exist when a delegated unlock sends `o=` (seconds since the unlock), which this
client does not.

And it is pinned against an oracle this code had no part in producing:

```
printf '93119Bakke0' | md5sum   ->  d9ac96231a45f0f275747a84a4c9271d
printf '1Cheevos1'   | md5sum   ->  4787f01ee76713835a4f3bd5de506ec1
```

Both are `CHECK`s in `tools/ra_launcher_test.c`. The suite also pins that hardcore changes the digest —
otherwise the flag could be absent from the hash and nothing would notice — and that the digest is
always 32 lowercase hex.

#### The raw username, not the encoded one

`u=` in the URL is percent-encoded; the hash is over the raw name, because the server hashes what it
decoded. Getting that backwards breaks exactly the accounts with a space in the name and no others,
which is the kind of bug that ships. So the test signs `"two words"` and `"two%20words"` and asserts
they differ.

The name itself comes from the login reply's `User` field rather than from `ra.cfg`, because RA matches
logins case-insensitively and answers with the canonical spelling — and the hash has to be over the same
string that goes in `u=`. Two details there, both of which would have been quiet bugs:

- it is adopted **only when `raNetJsonString()` returns true**. That function leaves a partial copy
  behind when the value does not fit, so testing the buffer instead of the return value would adopt a
  *truncated* username — which signs every award wrong while looking entirely reasonable in the log;
- `raUser` is declared `sizeof(((raConfig*)0)->username)`, so the percent-encoded form always fits the
  caller's `3 * sizeof` buffer. A 64-byte name and a 99-byte encode buffer was the version before this.

### The file format, and the two constraints that pick it

`sd:/ra_unlocks.txt`, fixed 1,024 bytes, 64 records of 16.

**Fixed-size records, because of 3b.** The cardengine can only write into clusters that already exist
— that is how every file nds-bootstrap writes from inside a game works, `fileWrite()` against an
`aFile` the launcher pre-opened. So the launcher creates the file at full length and it never changes
length; record N is at offset `N * 16`, an offset the cardengine can compute without reading anything
first. Clearing is done **in place, zeros over the same length**, for the same reason: truncating could
hand the clusters back.

**ASCII decimal, because of 3a.** A human has to be able to type an id into it. That is what lets the
sending half be tested before the writing half exists, and it is how the next hardware run works.

Both are satisfied by the most forgiving parser in this project: every non-digit is a separator, **NUL
padding included**, so the cardengine's 16-byte records and a line typed in a text editor parse
identically. `#` to end of line is a comment, so a note left in the file cannot become a spurious
unlock — the test pins that `# 93119 was earned` awards nothing.

What it refuses rather than mangles is the same correction `ra_patch.c` and `ra_take_id()` both needed:
a run of digits that overflows u32 is **dropped and counted**, never truncated, because a shortened id
is a *different achievement*. `4294967295` survives; `4294967296` and `99999999999` are dropped. Zero
is dropped too — rcheevos refuses id 0 outright, and it is what a field of NUL padding would read as if
padding were ever mistaken for digits.

### An answer clears the record; silence keeps it

The rule is about who has seen the id, not about whether they liked it.

| what happened | the record is | why |
| --- | --- | --- |
| `Success:true` | cleared | it landed |
| the server answered and refused | cleared | it has seen the id; RA returns Success even for an already-held achievement, so a refusal is one it will keep refusing. Retrying forever would only spam it |
| the request never got an answer | **kept** | nothing was proved. It is still owed, and the next boot sends it |
| no token this boot | **kept**, none sent | a login that failed is not a reason to lose an unlock |

Every refusal is logged with the server's own `Error` string, and the body verbatim when there isn't
one. The whole point of a queue is that nothing disappears quietly.

`Success` is read by a third JSON reader, `raNetJsonTrue()`, rather than by a case in the other two.
The reply is `{"Success":true,...}` or `{"Success":false,"Error":...}` — five characters apart in an
otherwise identical body — so it matches `"Success":` and then requires the literal `true`. A
`strstr(body, "true")` would find the word inside an achievement title just as happily. A missing key
is false, which is the safe direction: refused-and-logged beats counted-as-awarded.

### What to read on the next run

`stage 12` is new and the fetch has moved to 14. The first boot with this build has an empty queue, so
what it proves is the plumbing:

| | |
| --- | --- |
| `queue            created, 1024 bytes, nothing owed` | first boot ever — the file did not exist and now does |
| `queue            empty (1024 bytes read)` | every boot after that, with nothing earned |
| `submitted        0 ok, 0 refused, 0 owed` | in the summary, printed even as three zeros |
| `reached stage 14 of 14` | the ladder grew a rung |
| `heap after award` | 2.3 KB more `.bss` than before; this line is where that shows |

Then the real test, and it needs no code: **put a real id in the file by hand.** Write `93119` into
`sd:/ra_unlocks.txt` and boot. That single run answers two questions at once —

- whether the signature and the request are right at all, which nothing local can establish;
- and **whether `r=unlocks` and `r=patch` share a numbering**, which the last run explicitly could not
  settle. If `93119` is accepted, the boot after it should read `already earned 2` with `earned id
  93119` next to the notice, and `definitions 54 kept` instead of 55 — the awarded achievement filtered
  out of the block by the rung below. That is the loop closing, observed rather than argued.

If instead it is refused, the server's `Error` string is in the log and says which of the two it was.

### Hardware: the plumbing run, then the real one

Two runs, exactly the two the section above asked for.

**The plumbing.** Log at `docs/logs/ra_wifi_launcher_queue-3ds.log`. Every new line read as designed:

```
-- stage 12: report what the last session earned --
queue            created, 1024 bytes, nothing owed
-- stage 13: what has this account already earned --
already earned    1
  earned id      101000001  (server notice)
-- stage 14: fetch the set --
already earned   0 of 1 matched this set
of those, 1 is the server's own notice
reached stage 14 of 14
```

No yellow warning, because the notice now accounts for the whole mismatch — which is the change from
the run before it working.

One prediction in that section was wrong and the log says so. I wrote "2.3 KB more `.bss`; this line is
where that shows". `heap after award` reports `45056 safe`, against `53248` before — an 8 KB drop, not
2.3 KB. `safe` is literally `IMAGES_LOCATION - heapTop`, so what moved is the heap top, from
`0232B000` to `0232D000`. The statics added come to about 2,370 bytes (`file[1024]`, `response[1024]`,
a `raQueue`, `raUser`), so **most of that 8 KB is not accounted for** and this document is not going to
pretend otherwise. What can be said: free space did not fall — `fordblks` went from 9,144 to 10,840,
and run-to-run variation on the same binary was already 9,144–9,592 — and 45 KB of headroom remains, so
nothing is tight. The number is logged every run and will be measured properly if it ever matters.

**The real one.** `93119` typed into `sd:/ra_unlocks.txt` with a text editor. Log at
`docs/logs/ra_wifi_launcher_awarded-3ds.log`:

```
-- stage 12: report what the last session earned --
queue            1 to send
  93119  awarded
awarded 1, refused 0, still owed 0
```

**The protocol works.** The signature, the parameter names, the token, and plain `GET` against
`dorequest.php` are all accepted — `Success:true` for a real achievement in this set. That is the whole
sending half proven, and it needed no code beyond what was already committed: a human typed an id into
a file.

#### But the loop did not close, and the next rung is where that shows

One rung later, in the same run, seconds after the award:

```
-- stage 13: what has this account already earned --
already earned    1
  earned id      101000001  (server notice)
...
definitions      55 kept
```

Still one unlock, still only the notice, still 55 definitions. **`Success:true` and the account holding
the achievement are not the same statement.**

Both endpoints were re-read from the vendored rcheevos before theorising, and neither is being called
wrong: `r=unlocks` sends `g=` and `h=` and returns `UserUnlocks` as a **flat array of numbers**, which
is exactly what `raNetJsonIdList()` parses; `h=0` is the softcore form. So the server really did answer
`[101000001]` right after accepting 93119.

What is left is a short list, and nothing in the current log distinguishes its entries:

| | |
| --- | --- |
| `r=unlocks` is cached or replicated late | the award landed and this query did not see it yet |
| the award was accepted and discarded | the standing User-Agent problem — RA does not recognise `nds-bootstrap-ra/0.1`, injects the `Warning: Unknown Emulator` achievement, and blocks hardcore. Whether it also drops softcore unlocks from an unregistered client is exactly the open question |
| something else about the request | `r=startsession` is a verb this client never sends, and rcheevos sends it when a game loads |

#### The blind spot this exposed, and the fix

For three runs `r=unlocks` reported a *parsed count* and never showed what it was parsing, and the award
reported one word. At the point where the parsed number is the thing in doubt, that is the wrong thing
to have logged.

So both replies are now logged verbatim — `award reply` and `unlocks reply`. `raWifiLog()`'s line buffer
is 192 bytes and these bodies are 900+, which is why a new `raWifiLogBody()` exists rather than a
`%s`: it chunks at 144 bytes and would have made this visible three runs ago.

It also **strips the session token** on the way out. No reply this client reads is supposed to echo the
token back, so that is a safety net and not a fix for a known leak — but this log is written to be sent
to someone else, the cost of being wrong once is an account, and the next endpoint added here does not
have to remember the rule. The `>= 8` length guard on it is load-bearing: `strncmp` against a
zero-length token matches at every position and would spin forever.

The reply carries `AchievementID`, `Score` and `AchievementsRemaining`. Those say whether anything was
recorded, and they are what the next run will show.

There is also a check that costs nothing and settles the biggest branch immediately: **look at the
account's page on retroachievements.org.** If 93119 shows as unlocked in softcore, the award landed and
`r=unlocks` is a caching question. If it does not, the client is being accepted and ignored, and the
User-Agent stops being a standing annoyance and becomes the blocker.

#### The site said 91467, and that answers a question I had been asking wrong

The account holds exactly one achievement in Super Mario 64 DS: **91467**, earned in **hardcore** on a PC
emulator. Not 93119.

Two readings, and the first one is free — it needed no console at all, only the definitions file already
archived at `docs/logs/ra_definitions-14856-ids.txt`:

```
ids in set 14856:  92869 92870 92871 92872 92873 92874 92875 92876 ... 579308
91467:             below all of them
93119:             present
```

**91467 is not in the set our ROM hashes to, and it is not merely absent — it is below the entire
range.** The set opens at 92869 and the first ids run consecutively, which is what a set authored in one
batch looks like; 91467 was created earlier, for a different set. So `r=unlocks&g=14856` was *right* to
not return it, and the "why doesn't the account's own unlock appear" half of the puzzle is closed with no
bug in it.

That is worth stating in its own right, because it is a property of this fork that users will hit: the
site redirects game 9983 to `?set=6112`, our hash resolves to **14856**, and those are different
achievement sets of the same game. **Achievements earned on a PC emulator against one set do not appear
for a ROM that hashes to another.** Nothing is wrong; RA subsets simply mean the ROM decides the set.

The second reading is the one that matters and it got worse, not better: 93119 was awarded with
`Success:true` and **is not on the account**. So the accept-and-discard branch is now the live one, and
"`r=unlocks` is just cached" is much weaker — the site is not a cache.

> **Both paragraphs above are wrong, and the run in the next section is what proved it.** 93119 *was*
> recorded — `r=startsession` returned it with a `When` matching the run that awarded it. And 91467 *is*
> associated with game 14856 by the server, so "not in the set" was not the reason `r=unlocks` withheld
> it; the reason is that 91467 is a **hardcore** unlock and `h=0` asks for the softcore list. They are
> left here rather than edited away because the next section is a correction and needs something to
> correct, but do not carry either conclusion forward.

### `r=startsession`, the verb this client never sent

Before blaming the User-Agent, there is a plainer candidate that was never eliminated because it was
never tried. rcheevos sends `r=startsession` when a game loads, before anything else game-specific. This
client sent `login`, `gameid`, `unlocks`, `patch` and `awardachievement` — and never opened a session at
all. "The server will not record an unlock without one" was a hypothesis with no evidence in either
direction, which is the worst kind to leave standing.

So it is **stage 12**, ahead of the award, which pushed the ladder to 15 rungs. Parameters read from
`rc_api_init_start_session_request_hosted()` rather than guessed: `g`, then `h` and `m` together, then
`l`.

It is not speculative work. A correct RA client sends this regardless of how the current question turns
out, and the reply is independently useful:

```json
{"Success":true,
 "Unlocks":[{"ID":93119,"When":1786243173}],
 "HardcoreUnlocks":[{"ID":91467,"When":1700000000}],
 "ServerNow":1786243200}
```

That is a **second source, in a different shape, for the thing currently in doubt** — and the two are
deliberately kept apart rather than merged. The skip list still comes from `r=unlocks`; the session's
counts are reported beside it. If they disagree, the log will say so instead of one silently winning.

The shape needed a new reader. `raNetJsonIdList()` reads `[93119,93120]` and would stop at the `{` here,
returning nothing and calling it an empty array — and an empty array is a *meaningful* answer from that
endpoint, so the two cannot share a reader. `raNetJsonObjectField()` walks from `"key":[` to the matching
`]` and takes `"field":<digits>` at brace depth 1 only, so a nested object cannot contribute an id from a
level it did not mean to read. Its limit is written down rather than left to be discovered: the brace
counting is not string-aware, which is safe for `Unlocks` and `HardcoreUnlocks` because those hold two
numbers and no strings, and would not be safe for an array with titles in it.

One subtlety the test pins because it would have been a silent wrong answer: `HardcoreUnlocks` **contains**
`Unlocks` as a substring. The needle is `"Unlocks":[` with the leading quote, and the character before
`Unlocks` in `"HardcoreUnlocks"` is `e`, so the first lookup cannot land inside the second.

#### The User-Agent, and what this project will not do about it

`nds-bootstrap-ra/0.1`, now defined once in `ra_wifi.h` instead of written out twice in `ra_net.c` —
because a value under investigation should not exist in two copies.

RetroAchievements identifies clients by it, does not recognise this one, injects the
`Warning: Unknown Emulator` achievement into every set it serves us, and blocks hardcore. Whether it also
declines to record softcore unlocks is exactly the open question.

The fix, if that is the cause, is **to ask RetroAchievements to recognise the client** — not to send a
known emulator's string. That would be against their rules, and it would also destroy the evidence: a
client that lies about what it is cannot answer this question. The honest name stays.

### The loop closed, and it closed the argument too

Log at `docs/logs/ra_wifi_launcher_session-3ds.log`. `93120` typed into the queue by hand, one boot after
`93119`. Every open question in the two sections above is answered by these three replies, which is what
logging them verbatim was for.

```
-- stage 12: start a play session --
session reply:
  {"Success":true,"ServerNow":1786244358,
   "HardcoreUnlocks":[{"ID":91467,"When":1786166850}],
   "Unlocks":[{"ID":93119,"When":1786243172},{"ID":101000001,"When":1786244358}]}
session started
session unlocks  2 soft, 1 hard

-- stage 13: report what the last session earned --
  93120  awarded
award reply:
  {"Success":true,"AchievementID":93120,"AchievementsRemaining":53,"Score":1126,"SoftcoreScore":597}

-- stage 14: what has this account already earned --
unlocks reply:
  {"Success":true,"GameID":14856,"HardcoreMode":false,"UserUnlocks":[93119,93120,101000001]}
already earned    3

-- stage 15: fetch the set --
definitions      53 kept, 3 unofficial
already earned   2 of 3 matched this set
block            16382 of 32759 used
```

#### The correction: nothing was ever discarded

`Unlocks` contains **93119, with `When` 1786243172** — the second the previous run awarded it. It was
recorded the whole time. My conclusion that the award had been accepted and thrown away was wrong, and
the reasoning behind it was wrong in a specific, avoidable way: the account's page was read as "the
account holds one achievement", when what it showed was one achievement *on the set the player had been
playing*. 93119 belongs to a different set of the same game. Inferring a server-side discard from a page
that was never going to list it was not a measurement.

The lesson is the same one this project keeps relearning, and it is worth the space: the reply is the
artifact. Three runs of `r=unlocks` reported a parsed count with the body unlogged, and I filled the gap
with an inference. One logged body ended the argument.

#### And `r=unlocks&h=0` is the softcore list, not the whole list

91467 is in `HardcoreUnlocks` and **not** in `Unlocks`, from the same reply, for the same game. So the
server does associate it with game 14856 — "it is not in this set" was not why `r=unlocks` withheld it.
It withheld it because `h=0` asks for softcore, and the two lists are tracked **independently**: a
hardcore unlock does not imply the softcore one here.

That makes a design decision that was made for a weak reason turn out right for a strong one. The skip
list still comes only from `r=unlocks`, with the session's hardcore count reported beside it and never
merged in — and merging it would now be an outright bug. In a softcore session, an achievement held only
in hardcore has *not* been earned yet, so it belongs in the block where the player can earn it. Counted,
not merged.

#### What the numbers cross-check

Two independent confirmations fell out that nothing was set up to produce:

- the server says **`AchievementsRemaining":53`** and the scanner staged **`53 kept`**. The server's own
  arithmetic about what is left agrees with what the block holds, from two different endpoints;
- the block went from **28,924 bytes to 16,382** — down 43% for two achievements out of 55, because 93119
  and 93120 were precisely the two 6,270-byte definitions the earlier logs printed as `def 2` and `def 3`.
  The reason to filter before staging was never about the count, and this is what it looks like when it
  pays.

`already earned 2 of 3 matched this set` with `of those, 1 is the server's own notice` — three ids, two
real ones matched, the notice accounted for, no yellow warning. Every line in that report now says a
true thing about a case it was written before.

#### The notice is re-awarded on every session

Small and worth writing down: `101000001`'s `When` is `1786244358`, which **is** `ServerNow` in the same
reply. The server does not remember having told us; it unlocks the `Warning: Unknown Emulator`
pseudo-achievement again at the start of every session. That is why it has appeared in every unlocks list
since the beginning, and it confirms the notice is a live statement about this client rather than a stale
row.

#### One thing this run does not settle

In the previous run, `r=unlocks` ran seconds *after* the award and did not report it. In this run, with
`r=startsession` ahead of it, `r=unlocks` reported `93120` immediately. That is consistent with the
session being what makes an unlock visible within the same run — and equally consistent with a short
cache that happened to expire differently. One run each is not enough to tell those apart, and it does
not matter enough to spend runs on: the award is recorded either way, and the next boot always sees it.

### 3b: the queue file is now always there, and the channel is mapped

Two things landed, and the third was stopped on purpose.

**The launcher creates the queue unconditionally**, in `conf_sd.cpp` beside `softResetParams.bin` and
for the same reason: the cardengine writes into clusters that already exist and cannot allocate any, so
the file has to be full length before the game boots. Not under `RA_LAUNCHER_WIFI` — the two halves are
independent, a build with no networking can still *record* an unlock, and gating it would make earned
achievements unrecordable on exactly the builds most people run. Rewritten only when the size is wrong,
because an existing queue holds ids that have not been sent.

**The producer side works and is validated.** See the previous section.

**The cluster plumbing was reverted**, and mapping it is the useful output. It is not the one-field
change the ce7 struct made it look like. `srParamsCluster`'s real path is:

```
main.cpp            stat() -> st_ino is the cluster
nds_loader_arm9.c   loader->srParamsFileCluster           (loadCrt0, a fixed-layout struct)
bootloaderi/main.arm7.c   extern u32 srParamsFileCluster   <- a linker-placed global, and
bootloader/main.arm7.c    extern u32 srParamsFileCluster      there are *two* bootloaders
hook_arm9.c         ce9->srParamsCluster = srParamsFileCluster
cardenginei/arm7    getFileFromCluster(&srParamsFile, ...)
```

So a new cluster means a field in `loadCrt0` whose layout two bootloaders read through fixed-offset
externs, plus the hook signatures, plus both bootloaders' call sites. That is the boot path of every
game nds-bootstrap runs, and a mistake in it does not fail loudly — it hands the ARM7 a wrong cluster
and writes 16 bytes into whatever file that is.

Doing it half-way and leaving it building was the other option and it was worse. The tree is clean, both
modes build, the suite passes, and the remaining work is now a known list rather than an unknown.

### What is not built, and what it needs

3b — the cardengine writing to the queue — is not started. Sizing it honestly, from reading the code
rather than guessing:

- `retail/cardenginei/arm9_ra/source/` contains **no** FIFO or `sharedAddr` use at all. It cannot talk
  to the ARM7 today.
- `fileWrite()` and the `aFile`/cluster machinery live in the **ARM7** cardengine
  (`retail/cardenginei/arm7/source/cardengine.c`), which gets clusters from the launcher through the
  ce7 struct — `srParamsCluster`, `ramDumpCluster` — and calls `getFileFromCluster()`.
- The channel between the two exists and is already used by the non-RA ARM9 engine: `sharedAddr[0..2]`
  for arguments, `sharedAddr[3]` as the command word.

So 3b is: a new cluster in the ce7 struct, a new command on `sharedAddr[3]`, and an `aFile` in the ARM7
engine. That is a small amount of code in a file that every game nds-bootstrap boots depends on, which
is why it is worth doing only once 3a has been shown to work end to end.

One known inaccuracy to write down now rather than discover later: unlocks are submitted **without
`o=`**, so RetroAchievements timestamps them at the moment they are sent, not the moment they were
earned. For a queue drained on the next boot that is usually minutes to days out. The fix is the `o=`
parameter plus the DSi's RTC read at both ends, and it changes the signature to the four-append form
in the code quoted above.

The sending half is now **confirmed on hardware end to end** — awarded, recorded, cross-checked against
the server's own `AchievementsRemaining`, and filtered out of the next boot's block. 3b is the only part
of the loop still missing, and it is the part with no network in it at all: the cardengine writing an id
into a file whose bytes are already allocated. Everything it needs to talk to has been measured.

## In-game networking, reopened and then closed by measurement

Open question #1 has always been settled for the launcher and open for the game, and the reason given
was size: dsiwifi's ARM7 half is 104,148 bytes against a cardengine region of 62,464 with 12,636 free
— 8.2× the free space and 1.7× the whole region. That number is right and it is not the whole story.

**Keeping the link alive is not the same as keeping the stack alive**, and this project's own teardown
turns out to be evidence for that. `wifi_card_deinit()` in
`libs/dsiwifi/arm_iop/source/wifi_card.twl.c:1557` does five things and every one of them is masking an
interrupt:

```c
wifi_sdio_enable_cardirq(REG_SDIO_BASE, false);
irqDisableAUX(IRQ_WIFI_SDIO_CARDIRQ);
irqDisable(IRQ_TIMER3);
wifi_card_write_func1_u32(F1_INT_STATUS_ENABLE, 0x0);
wifi_card_write_func0_u8(0x4, 0x0);          /* CCCR irq_enable */
```

No reset, no power-down, no WMI disconnect, nothing sent to the AP. And `DSiWifi_DisconnectAP()` is an
unimplemented `sassert(false)`, so nothing else does it either. The chip's firmware keeps running with
its WPA2 session and its association; what we switched off is the path by which it told us. The last two
lines are writes *to the chip*, so undoing them is two register writes rather than a driver.

That reframes the cost. Bring-up, scan and the WPA2 handshake are most of those 104 KB and are needed
**once** — the launcher already did them. What would have to be resident in-game is much smaller:
re-enable the two interrupt registers, a minimal WMI data path, and enough TCP for one outbound
connection.

### The experiment, which needs no code

The shipped teardown is already the non-destructive one, so the first measurement is free: boot a game,
leave it running, and look at the AP's client list for the console's MAC and IP — both of which the
launcher already logs (`Dev 04:03:d6:f9:36:52`, `IP 192.168.0.112`). Check again at 5, 15 and 30 minutes.

Do **not** ping the console. ICMP and ARP were answered by lwip on the ARM9, which no longer exists, so
a failed ping proves nothing. The AP's association table is the right instrument because it looks at the
802.11 layer, which is the layer in question.

### The prediction, before the run

It will appear, and then it will disappear — because the WPA2 supplicant is in **software on the ARM7**.
The launcher's log shows it doing the work (`WPA2 Handshake 1/4`, `3/4`, `Added GTK 1`), and APs rekey the
group key periodically, typically between 10 minutes and an hour. With the interrupts masked those EAPOL
frames are never processed, so the AP should eventually deauthenticate the station.

| what the router shows | what it means |
| --- | --- |
| gone at a suspiciously round interval | the GTK rekey. The link is reusable but time-limited, and the supplicant would have to stay resident too — materially more expensive than a data path |
| listed for the whole session | either the AP does not rekey or the chip handles it in firmware. The cheap path is open: two register writes plus minimal TCP |
| never listed | something else drops it, and the idea dies for the price of one session |

What it would buy if it survives: rich presence, unlocks reported at the moment they fire, and step 3b's
SD queue becoming unnecessary rather than merely late.

### The answer: it dies seconds after the game starts

The router listed the console during the ladder — MAC `04-03-D6-F9-36-52`, IP `192.168.0.112`, matching
the launcher's log exactly, which is the control. **Seconds after the game booted the row was gone, and
it never came back.**

The prediction above was wrong, and wrong in the informative direction. It said the link would survive
and then die at a group-rekey interval of ten minutes to an hour. Seconds rules the rekey out entirely:
this is not time passing, it is the game booting.

One hypothesis was worth killing with code before guessing further, and it also failed. The launcher logs
`SCFG_EXT7 BIT(18) set` — that bit enables the WiFi SDIO block — and `bootloaderi/main.arm7.c:1839`
writes `REG_SCFG_EXT = 0x93FFFB06`, which has BIT(18) set, the same value the launcher measured. **The
chip's host interface is not switched off.** That was the best available explanation and it is not the
one.

#### What is not known, and why it is not worth chasing

The mechanism is unmeasured. Plausible candidates are a "host lost" watchdog in the chip's firmware
disconnecting when nothing drains its mailbox, or something in the boot path cutting power by another
route. Neither was tested and naming one would be invention.

It does not matter which. Both leave the same requirement: the association would have to be established
*from inside the game*, which is bring-up, scan and the WPA2 handshake — the bulk of the 104 KB. No
further diagnosis changes a decision, so none is proposed.

#### What this settles

The cheap path is closed. The reframing that opened this section — keeping the link is not keeping the
stack — was right about the teardown, which verifiably does not disconnect, and wrong about the console,
where the game's boot does. So the original number stands with the shortcut removed:

```
ARM7 cardengine region :  62,464 bytes
  free                 :  12,636
would be needed        : 104,148   -- and the bring-up can no longer be left out
```

Live rich presence and same-moment unlock reporting are out of reach in this architecture. Not
impossible: they need a different memory home and a minimal stack written from scratch, which is a
project rather than an increment.

And it settles something in the other direction. **Deferred sync — queue to the SD, send on the next boot
— stops being a fallback chosen for convenience and becomes the measured answer.** That raises the
priority of finishing 3b, which is all that stands between the loop starting from play and the loop
starting from a text editor. The whole result cost one session and no code, which is what the experiment
was for.

## A second category of overlay failure: borrowed, drawn, invisible

Everything catalogued below is a *denial* -- the overlay wanted a VRAM block or a background layer,
found none free, and said so through `denied` or `deniedNoLayer`. Contra 4 is not that.

On Contra 4 the notification reports `shows 1` with `denied`, `evicted` and `deniedNoLayer` all zero.
It got its block, it got its layer, it held both for the full 180 frames, and it drew. And it is not
seen. **"Drawn" and "visible" are different claims**, and the instrumentation only ever measured the
first one -- which is why this went unnoticed for so long.

It went unnoticed, but not unobserved. The demo-timer notification that pulsed reliably in several
games **never appeared in Contra 4 either**, back in the phase 0.5 and phase 1 days, long before any
of the recent work. So this is a long-standing, game-specific display conflict rather than anything
the achievement path introduced -- and it also means the overlay itself is fine, since it is visible
elsewhere with the same code.

The leading explanation is one register bit. The glyph colour is written to standard palette RAM at
`0x05000400`, and with **BG extended palettes** enabled the sub engine does not read that for
backgrounds at all: the glyphs are drawn in whatever the game's extended palette holds at that index,
which is very likely nothing. That fits every number -- borrowed, never refused, never reclaimed,
invisible. `raSnapshot.overlayExtPal` records `SUB_DISPCNT` bit 30 at `show()` time so one run decides
it instead of an argument.

### The answer, and it is worse than invisible

The controlled pair was run, and the second half of it changed the problem. On Super Mario 64 DS the
demo notification pulses and is seen. On Contra 4 it is not seen -- **and part of the graphics at the
bottom of the screen flicker while it is up.**

So the overlay is not failing to draw. It is drawing into VRAM that Contra 4 is actively using: the
flicker is the game's own tiles being overwritten by glyphs for 180 frames and restored afterwards.
`surveyBlocks()` -- which reads the live BGCNT registers to decide which 16K character blocks are in
use -- is concluding that a block is free when it is not.

The counters agree that everything *else* is identical. The demo cycle is 180 frames held plus 60
waiting, first fire at tick 60, so `shows` should be `(ticks - 60) / 240 + 1`:

    Contra 4    273 ticks -> 1 show, and shows reads 1
    Mario 64   1291 ticks -> 6 shows, and shows reads 6

Both exact. The negotiation and the draw are the same code taking the same path in both games, with
`denied`, `evicted` and `deniedNoLayer` at zero on each. What differs is only whether the block it
chose was really free.

That reclassifies this from a cosmetic gap into a defect: for three seconds, on this game, the
notification corrupts the display of the game it is reporting on. Not showing a message is a missing
feature; damaging the game's graphics is a bug, and it is the one worth fixing first.

The extended-palette hypothesis is now secondary rather than dead -- it would still explain why the
glyphs themselves are not legible while their *effect* is -- but it is no longer the interesting
question. The interesting question is what `surveyBlocks()` mis-reads, and the next step is to capture
`SUB_DISPCNT` and all four `SUB_BGCNT` values at `show()` time, which says exactly what the survey saw
and what it should have seen.

Two errors of mine on the way to this, both worth recording because both wasted a run. The probe build
moves the snapshot -- `ra_overlay.o` grows with the demo compiled in -- so the address quoted for the
normal build was wrong and `overlayExtPal` sat off the bottom of both photographs. And the overlay
draws on the sub engine, which is the same screen the in-game menu occupies, so the notification cannot
be seen while the RAM viewer is open: a visibility check has to be made with the menu closed. I asked
for a reading that could not have produced one.

### The palette hypothesis is dead, and the notification appears

`overlayState` read **0x08** on Contra 4: bit 0 clear, so **BG extended palettes were off**. The
explanation this section was built around is wrong, killed by the one bit it was worth spending to
measure. The layer borrowed was 0 and the block was 1.

And in the same run the notification **appeared** -- in Contra 4, and in Super Mario 64 DS -- flickering
rapidly, without disturbing the game's graphics. So the earlier reading, where the message was absent
and the game's own tiles flickered instead, was not a property of the game. It is **scene-dependent**:
whether a character block is genuinely free depends on what the game is drawing at that moment. Calling
Contra 4 "a game where the overlay never appears" was too coarse a claim, and it came from a small
number of observations of one scene.

The counters keep agreeing on the arithmetic: `shows` read 2 at 313 ticks, against
`(313 - 60) / 240 + 1 = 2`.

The rapid flicker has a candidate that the same run supports rather than a new hypothesis.
`rearmDispstat` read 71, so Contra 4 rewrites the sub engine's display registers constantly -- and the
overlay's hold path re-asserts `SUB_BGCNT`, the scroll registers and the layer-enable bit once per
frame, from a VCOUNT handler that runs at line 0. A game that writes `SUB_DISPCNT` later in the same
frame wins that frame; we win the next. Visible on alternate frames is exactly a fast flicker, and it
predicts that re-asserting later in the frame -- or accepting a lower Y-trigger -- would steady it.

Not built on that, because it is one hypothesis with one supporting number. But it is testable and it
costs nothing to state before the run that would check it.

### The original plan for that pair, kept for the record

The decisive form is a *controlled* pair, and it costs ten seconds: run the `RA_OVERLAY_DEMO=60` probe
on Contra 4, where the message is known not to appear, and on a game where it is known to appear.
`overlayExtPal` differing between them is the answer; matching kills the hypothesis and sends the
search to layer priority or the tile map.

### The deferral worked, and it exposed the bug item 3 predicted

The build that holds a notification until the screen has stopped fading was run on hardware, and the
report was that the message still could not be read clearly and that **the whole of stage 2 of
*Contra 4* glitched**.

That is a worse outcome than before and it is the right kind of worse. Before the deferral every real
notification landed inside a fade: invisible, and — crucially — harmless, because a screen on its way to
black does not show what the overlay got wrong either. Deferring moved the draw to the middle of
gameplay, where the game is actively using its VRAM. The fade was not only hiding the notification. It
was hiding the corruption, which is why item 3 below could sit for so long as a bug with no observed
effect.

The false assumption was in `surveyBlocks()`:

```c
if (!(SUB_DISPCNT & (1u << (8 + i)))) {
    continue;  /* layer off, so its VRAM is not in use */
}
```

A layer that is off *this instant* was treated as owning nothing. But the survey runs once, at show
time, from a VCOUNT handler at line 0 — and the game turns that layer back on later, with its character
base still pointing at the block the overlay has just filled with glyphs. The block was never free; it
was momentarily unreferenced by an enabled layer, which is not the same claim.

There is direct evidence the game does this constantly rather than occasionally, and it was already in
the snapshot: `rearmDispstat` **clamped at 255**, counting how often the Y-trigger had to be written
back into the very register this survey trusts. A game that rewrites `SUB_DISPCNT` every frame is a
game whose enable bits say nothing about what it owns.

So the filter is gone: a block referenced by **any** layer counts as in use, enabled or not. Strictly
more conservative, and the trade is the right way round — it makes `denied` more likely and corruption
less. A notification that does not appear is a missing feature; a corrupted game is a bug.

The honest consequence is that *Contra 4* may now report `denied` and show nothing at all. That is a
correct denial, counted at `+0x0C denied` with `+0x14 deniedNoLayer` separating the two reasons, rather
than a silent trespass. It also says plainly what the real fix is: the overlay needs VRAM it does not
have to borrow, which is one more item on the list the rewrite in `cardenginei_arm9_ra` already owns.

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

This was not observed for a long time, and phase 1 produced what looked like positive
evidence that it was not firing in practice: every one of the 5 recorded denials was a
missing *layer*, not a missing block, so the block search was never even the deciding
factor.

**That evidence has since been invalidated, and the bug is active.** Deferring the
notification past the fade removed the very condition that was masking it — while every
notification landed inside a transition, the survey's mistakes had no visible
consequence. The first build that raised notifications on ordinary frames glitched the
whole of stage 2 of *Contra 4*. See "The deferral worked, and it exposed the bug item 3
predicted" above; the enable-bit half of the survey is now fixed, and the mode-blindness
described here is not.

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

`heapSize` at `+0x60` reads **`0x26EA8`** (159,400 bytes, ~156 KB) — the window minus the
68 KB image, its `.bss`, the 8-byte alignment of the base, and the 32 KB definitions block
reserved at the top. `heapUsed` at `+0x64` is
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
- [x] **The loop, closed on hardware.** Contra 4, stage 1 cleared: rcheevos fired
      inside the running game (`rcTriggered 1`, `rcFirstId 302329`, matching line 1 of
      the fetched set), the notification drew on the sub screen (`shows 1` with
      `denied`, `evicted` and `deniedNoLayer` all 0), the id crossed to the ARM7
      (`unlockSent 1`, nothing queued, nothing lost), and the cardengine appended
      `302329` to sd:/ra_unlocks.txt with no launcher and no network. Detect, notify,
      persist -- every link measured rather than assumed. The submission is deliberately
      not made: see `submit=0`.
- [ ] Phase 3: real network, softcore unlocks. Both halves are **confirmed on hardware**
      from the launcher: `login`, `gameid`, `startsession`, `awardachievement`, `unlocks`
      and `patch` all answer over plain HTTP, an unlock is recorded by the server and
      cross-checks against its own `AchievementsRemaining`, the already-earned ones are
      filtered out of the staging block, the radio comes back down, and the game boots.
      What is missing is the half with no network in it: the **cardengine writing an
      earned id into the queue file**, so that the loop starts from play rather than from
      a text editor. See step 6b.
- [ ] Phase 4: rich presence, achievement list, login status
