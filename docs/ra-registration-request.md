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

## Short version, for a first message

The full draft below is right for an email or for a thread once somebody is engaged. It is the wrong
length for a first message in a Discord channel, where a wall of text gets scrolled past. So there
are two, and the short one exists only to get a human to ask the second question.

> Hi — I maintain **nds-bootstrap-ra**, a fork of nds-bootstrap that adds RetroAchievements support.
> nds-bootstrap runs DS ROMs from an SD card on **real DS hardware**, usually a 3DS via TWiLight
> Menu++, so this is not an emulator: the game runs on the console's own CPUs.
>
> Softcore works end to end today — login, `r=patch`, rcheevos evaluating the set on the console,
> and unlocks the server records. It sends `User-Agent: nds-bootstrap-ra/0.1`, which you don't
> recognise, so it gets the *Unknown Emulator* notice and `h=1` is filed as softcore.
>
> I'd like to get the User-Agent recognised. Two things I'd rather raise than have you find:
> the console **has no network while a game is running**, so unlocks are queued to the SD card and
> submitted on a later boot with `o=` carrying the earn time; and I've gone through your hardcore
> rules against this loader (cheats, savestates, rewind, slowdown, memory editing) and can walk
> through each one.
>
> Who's the right person to talk to, and is deferred submission something you'd accept?

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
> For completeness, since I tested it rather than assuming: sending `h=1` from this client returns
> `Success:true` and files the unlock as **softcore**. The hardcore score is unchanged, the softcore
> score moves, `r=unlocks` returns it in the softcore list and `HardcoreUnlocks` stays empty. I am
> not reporting that as a bug — it is a reasonable way to enforce the notice — only saying that I
> know what the current behaviour is and am not going to keep poking at it while I wait.
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
> **3. Is there anything else about deferred submission I should be handling that I am not?**
>
> One thing I found by looking for it, and have already fixed, as an indication of the standard I am
> trying to hold this to. The queued record used to store the achievement and when it fired, but not
> whether the session was hardcore — the mode was read from configuration at submission time. That
> meant a player could earn unlocks in softcore, set hardcore afterwards, and have them submitted as
> hardcore, correctly signed and indistinguishable from the real thing at your end.
>
> The record now carries the mode it was earned in, `h=` and the signature are taken from the record
> rather than from configuration, and it works in both directions: an unlock earned in hardcore is
> still submitted as hardcore even if the player has since switched to softcore.
>
> If there are other consequences of an offline queue that you have seen go wrong in other
> integrations, I would rather hear them now.
>
> **What is in place for hardcore already**
>
> Your hardcore rules forbid cheats, savestates, rewind and slowdown. Against this loader:
>
> - **Cheats** — nds-bootstrap has a cheat engine, and a hardcore session is refused whenever that
>   engine will run: it falls back to softcore rather than claiming hardcore. Worth being precise
>   about how that is determined, since it is not a setting. The loader has no cheats on/off
>   switch — the front-end writes the codes the player selected into a file, and the loader installs
>   whatever it is given. So "cheats are on" is measured from the three cheat inputs it can size,
>   and it is re-checked at the last moment before the game starts, when the loader knows whether
>   the engine actually got installed.
> - **Savestates** — nds-bootstrap has none. Not disabled: the feature does not exist in the
>   codebase.
> - **Rewind and slowdown** — likewise absent. The game runs on real hardware at its own speed.
> - **Memory editing** — the in-game menu has a RAM viewer that could write. In a hardcore session
>   it now refuses to enter edit mode; it will still display memory, which I hope is uncontroversial
>   since the rules are about modifying rather than reading. If you would rather it not display
>   memory either in hardcore, say so and I will close that too.
> - **Retroactive mode changes** — an unlock is submitted in the mode it was earned in, recorded
>   with the unlock itself rather than read from configuration when it is sent. See question 3.
>
> Happy to answer anything about the implementation, and happy to hand over source — it is public.
>
> Thanks for your time,

## When the answer comes back

Write it into `retroachievements.md` rather than only here, and specifically:

- If deferred submission is **not sanctioned**, the queue's design is what needs revisiting, and
  "In-game networking, reopened and then closed by measurement" is the section that has to be
  reopened with a different question. The record's mode field goes with it.
- If the User-Agent is **registered**, bump `RA_NET_CLIENT_VERSION` deliberately rather than
  incidentally — it is also `l=` on `r=startsession` — and record which string was registered.
