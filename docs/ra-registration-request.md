# Getting `nds-bootstrap-ra` recognised — the message to RetroAchievements

**Status: draft. Not sent.** Kept in the tree so that when it is sent, the questions asked and the
answers given end up in the same place as everything else this project has had to learn the hard
way. Update it when it goes out, and paste the reply underneath.

## Why this is here rather than in a commit

The server injects a `Warning: Unknown Emulator` notice for an unrecognised User-Agent, and its own
wording says it blocks **hardcore only**: *"Hardcore unlocks cannot be earned using this emulator."*
Softcore works today. Hardcore does not, and no amount of code in this tree changes that — see
"It was the server talking to us" in `retroachievements.md` for the capture.

Everything on our side that disqualified hardcore has now been dealt with. The client is not
asking to be taken on trust: the section below says exactly what it does, including the two things
that make it unusual and the one that is still wrong.

## Where to send it

RetroAchievements' developer channels — the Discord's integration/development channels or a
direct approach to the RAdmin/DevCompliance team. There is no documented registration form for a
client that is not an emulator, which is itself the first question.

## Draft

> **Subject: Registering a client User-Agent — nds-bootstrap-ra, a DS ROM loader on real hardware**
>
> Hello,
>
> I maintain a fork of nds-bootstrap called **nds-bootstrap-ra**. nds-bootstrap is the loader that
> runs DS ROMs from an SD card on real Nintendo DS hardware — most commonly on a 3DS through
> TWiLight Menu++. The fork adds RetroAchievements support. It is not an emulator: the game runs
> natively on the console's own ARM9 and ARM7.
>
> It sends `User-Agent: nds-bootstrap-ra/0.1`, which you do not recognise, so the server injects the
> `Warning: Unknown Emulator` notice and blocks hardcore. Softcore has been working end to end for
> some time — logins, unlocks the server records, and sets fetched and evaluated on the console.
>
> I would like to ask three things.
>
> **1. What is the process for registering a client that is not an emulator?**
>
> I have deliberately not borrowed another client's User-Agent. I would rather be recognised as what
> this is, and a client that misidentified itself would be both against your rules and useless as
> evidence about anything.
>
> For what it is worth technically: it uses **rcheevos v12.4.0** as a submodule, runtime only, and
> computes the ROM hash with rcheevos' own Nintendo DS algorithm, so hashes match your database
> without special-casing. It speaks `r=login`, `r=gameid`, `r=patch`, `r=startsession`,
> `r=awardachievement` and `r=unlocks`, with the standard signature, and sends `o=` so an unlock is
> dated when it was earned rather than when it was reported.
>
> **2. Is deferred submission acceptable?**
>
> This is the part that genuinely differs from every emulator integration, and I would rather raise
> it than have you discover it.
>
> **The console has no network while a game is running.** This is measured, not assumed: the DS
> WiFi hardware is driven by the ARM7, and once the game boots it owns that processor and the
> hardware the network stack needs. We tried keeping a connection alive into gameplay; it dies
> within seconds of the game starting.
>
> So the flow is: the loader connects at boot, logs in, fetches the set, and takes the radio down
> before starting the game. rcheevos then evaluates the set inside the running game with no network
> at all. When an achievement triggers, the unlock id and a timestamp from the console's clock are
> appended to a file on the SD card. **The next boot that has a network submits it**, with `o=`
> carrying the elapsed time so you date it correctly.
>
> In practice that is usually minutes later — the player quits the game and the loader submits on
> the way to the next one — but it can be a day or more if they do not go online.
>
> Is that acceptable for softcore? For hardcore? If it is not, I would rather know now than build
> further on it.
>
> **3. If deferred submission is acceptable, may a queued unlock be submitted in a mode chosen after
> it was earned?**
>
> I ask because mine currently can be, and I do not think it should be. The queued record stores the
> achievement and when it fired, but not whether the session was hardcore — the mode is read from
> configuration at submission time. A player could earn unlocks in softcore, switch to hardcore, and
> have them submitted as hardcore.
>
> The fix is small and I intend to make it: record the mode with the unlock and refuse to upgrade it.
> I have held off only because your answer to question 2 decides whether that is the right shape of
> fix at all.
>
> **What is in place for hardcore already**
>
> Your hardcore rules forbid cheats, savestates, rewind and slowdown. Against this loader:
>
> - **Cheats** — nds-bootstrap has a cheat engine. A hardcore session is refused if a cheat file is
>   loaded for the ROM, and it falls back to softcore rather than claiming hardcore.
> - **Savestates** — nds-bootstrap has none. Not disabled: the feature does not exist in the
>   codebase.
> - **Rewind and slowdown** — likewise absent. The game runs on real hardware at its own speed.
> - **Memory editing** — the in-game menu has a RAM viewer that could write. In a hardcore session
>   it now refuses to enter edit mode; it will still display memory, which I hope is uncontroversial
>   since the rules are about modifying rather than reading. If you would rather it not display
>   memory either in hardcore, say so and I will close that too.
>
> Happy to answer anything about the implementation, and happy to hand over source — it is public.
>
> Thanks for your time,

## When the answer comes back

Write it into `retroachievements.md` rather than only here, and specifically:

- If **deferred submission is sanctioned**, build the mode field on the queue record (question 3 in
  the draft; the record has 4 spare bytes of 48, so it fits without another migration) and the
  launcher's refusal to upgrade.
- If it is **not sanctioned**, the queue's design is what needs revisiting, not the record's fields,
  and "In-game networking, reopened and then closed by measurement" is the section that has to be
  reopened with a different question.
- If the User-Agent is **registered**, bump `RA_NET_CLIENT_VERSION` deliberately rather than
  incidentally — it is also `l=` on `r=startsession` — and record which string was registered.
