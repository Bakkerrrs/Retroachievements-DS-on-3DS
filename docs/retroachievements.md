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

> ### Picking it up from here
>
> **Read `## Building` first, specifically "Build the deliverable with `tools/ra_release.sh`".** The
> single most expensive lesson on this branch is that `retail/bin/nds-bootstrap.nds` holds whichever
> `RA_LAUNCHER_WIFI` mode built last, and that handing over a `=0` build presents as an in-game memory
> fault rather than a build mistake. Use the script; never copy that file by hand.
>
> **The loop is closed end to end and confirmed on hardware.** An achievement fires inside the game,
> the notification names it, the id and its earn time reach the SD card with no network, the next boot
> submits it with `o=` so the server dates it correctly, and the in-game menu shows what is still
> waiting under `Achievements... → Sync Pending`.
>
> **The next three pieces of work are small, known and written up** in "What is left" item 3: the
> cardengine reads `ndsHeader` at unlock time and that memory is not stable during play, the queue file
> never migrates its length while it is empty, and `raPendingBlock`'s placement is justified by design
> rather than by measurement.
>
> **Two readings answer almost any in-game question**, and reaching for them early is worth more than
> reasoning about the code: `rcFromFile` at `+0x98` of the snapshot says whether the staged set
> arrived, and `defsMagic` at `+0xC4` says why not. Both are in the RAM viewer; the snapshot's address
> is not fixed across boots, so find the `RA2S` magic rather than jumping to a remembered address.

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
| Branch | `main` |
| Snapshot address | `tools/ra_snapshot_addr.sh` — currently `0x027FEA00` for `cardenginei_arm9`, which is the variant a retail DS game loads on a 3DS. **Re-run it after every build**; it moves. |
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

### Build the deliverable with `tools/ra_release.sh`, not by hand

**`retail/bin/nds-bootstrap.nds` holds whichever `RA_LAUNCHER_WIFI` mode built last.** The
pre-delivery checklist above says to compile at `=0` and `=2`; doing both and then copying that file
hands over whichever one happened to run second. Copy it after a `=0` build and the launcher has no
network at all.

This cost about a week, and the reason it did is that the symptom does not look like a build problem:

- No ladder runs, so nothing is staged for the cardengine.
- The bootloader finds no magic where the definitions should be, and clears the destination.
- `cardenginei_arm9_ra` falls back to its built-in self-test — **one** definition, carrying no id, so
  it is numbered `RA_SYNTHETIC_ID_BASE + 0` = `0xF0000000`.
- That fires seconds into the game and gets written to the queue. On screen it is the notification's
  heading with no achievement name under it.

So a wrong build reads as an in-game memory fault. The arena's top, the pending block's address and
the launcher's statics were each blamed and each exonerated, on evidence that was really *"did the
`=2` build happen to run last this time"*. Two of those wrong conclusions were written into commit
messages before being disproved.

**And the logs agree with each other rather than with the console.** A boot with no network never
rewrites `ra_wifi_launcher.log`, so the file still holds the *previous, working* boot's run — reporting
`staged 45 definitions` and `reached stage 15 of 15` for a boot that did neither. Comparing that log
against a snapshot taken on the failing boot produces a contradiction that cannot be resolved by
reasoning, because the two readings describe different boots. **Check the log's timestamp against the
run before trusting it.**

`tools/ra_release.sh` fixes the order rather than remembering it — `=0` first because it is only a
compile check, `=2` last because it is the deliverable — and then proves the result from the ELF
rather than from intent: a `=2` launcher links dsiwifi into its ARM7 and a `=0` launcher does not, so
counting those symbols separates them without trusting which `make` ran. It runs the host suite,
prints the snapshot budget, refuses to emit a file if any of it fails, and reports the commit, whether
the tree was dirty, and the md5.

**The reading that identifies this in seconds** is `raSnapshot.rcFromFile` at `+0x98`: 0 means the
staged set never arrived. `defsMagic` at `+0xC4` then says why — `RDA1` for arrived, `0` for the
bootloader looking and finding nothing staged, anything else for nothing having been copied at all.

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

### #2 — is the VBlank hook the right point, and what is the cycle budget? **Yes, and no.**

**The hook is VBlank, and it took a reversal to get there.** This section answered "VCOUNT, line 0"
for most of the project's life, on the reasoning that VCOUNT was the per-frame ARM9 hook the
cardengine already owned. Hardware overturned that: three conditions have to hold for a VCOUNT
interrupt to fire — the game's `irqTable[2]` entry, `IRQ_VCOUNT` in `REG_IE`, and the Y-trigger in
`REG_DISPSTAT` — and Contra 4 clears the third constantly. `ticks` reached 1,132 across a session of
many thousands of frames while `rearmDispstat` saturated at 255: the reader was running on about **8%
of frames**, and the overlay, which has to re-assert its borrowed registers every frame, was visible
about one frame in twelve.

VBlank has no third condition. A DS game keeps `IRQ_VBLANK` enabled and `irqTable[0]` pointed at
something of its own because it needs the interrupt itself, and hardware confirmed that directly:
`rearmIe` reads **0** across a three-minute session, so the game never once cleared it. `ticks` now
tracks the frame count one for one — 10,715 over about three minutes.

Two things follow that the old answer had wrong. A reader build no longer forces `IRQ_VCOUNT` on for
a game that never asked for one, which it used to do purely for the reader's sake. And every duration
in the overlay is honest again: 180 frames is three seconds rather than half a minute.

The ARM7 still has an always-active VBlank hook (`vblankHandler` in
`retail/cardenginei/arm7/source/card_engine_header.s`) and it is still the wrong side to read game RAM
from, for the original reason: the ARM7's view of main RAM is not the ARM9's cached view.

**The cycle budget is measured, and it is no longer negligible.** The reader samples `VCOUNT` on entry
and on exit and records the difference in `linesLast` / `linesMax`.

| | Phase 1, watchlist only | Now, with rcheevos and the overlay |
| --- | --- | --- |
| `linesLast` typical | 0 of 262 | **28–33** |
| `linesMax` | 1, and 11 on a busier game | **154–211** |

The steady-state figure is `rcLines`, 28–31, and that is rcheevos evaluating 45 definitions and 21
distinct addresses. It is now paid on **every** frame rather than on 8% of them, so the real per-frame
cost went up roughly twelvefold with the hook change.

What makes it affordable is where it is spent rather than how much: VBlank is 71 scanlines and this
uses about 30, so in steady state the work fits inside the blanking period and touches no visible
line at all. The old VCOUNT hook fired at line 0 and spent its scanlines on drawn pixels. `linesMax`
of 154–211 is the initialisation frames — `rcInitLines` 85–88 for the slowest single activation,
`rcInitTotal` about 1,080 summed — and those happen once per boot.

Scanlines are a coarse unit, roughly 1,600 ARM9 cycles, but they are the right unit for the question
being asked and `VCOUNT` is the only clock available: the game owns the hardware timers.

The measurement covers the reader, the WRAM call and the overlay together, which is the honest scope —
it is what the game pays per frame for all of this.

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

> **Fixed since.** It did move to the separate binary with the rewrite, and it is fixed there now.
> `raOverlaySurvey()` consults the BG mode, tells text from affine from extended, and reads a bitmap's
> base in 16K units with its depth from bit 2. See "The survey learns to read the BG mode" below.

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
and palette entries from the sub engine. Both are the kind of thing that shows up as an
intermittent oddity rather than a crash, which is exactly what a console nobody is
testing on cannot be trusted to reveal. Shipping them to a DSi on the strength of "it
should work" is not a trade worth making.

It also removes work that would otherwise be speculative. The DSi WRAM the separate
binary needs is guaranteed present on a 3DS with SCFG unlocked, so no fallback has to
be designed for the case where it is not.

### The test rig, stated once

**Every hardware run in this document was made with TWiLight Menu at its defaults**, on a
3DS, from the SD card. No colour LUT, no `PHAT_COLORS`, no per-game overrides. Stated
here because it is a standing fact about every reading in this file, not a detail of any
one of them, and because it is load-bearing more often than it looks.

Worked example, from the report of an unlock that RA accepted and the in-game menu still
listed as pending. The menu reads the tally out of `cardenginei_arm9_ra`'s DSi WRAM
window, so the first question was whether it had been shown a stale block — one the
bootloader never refreshed because the binary was not staged that boot. Everything that
decides that is console-level or global, never per-game:

| Gate | Where | Depends on |
| --- | --- | --- |
| `dsiFeatures() && !b4dsMode` | `conf_sd.cpp:1338` | the console |
| `!colorTable` | `conf_sd.cpp:1840` | the LUT selection and `PHAT_COLORS`, both global |
| `consoleModel > 0` | `conf_sd.cpp:1840` | the console |
| `dsiWramAccess && !dsiWramMirrored` | `main.arm7.c:2362` | measured by writing `0x03700000` |
| the staged image magic | `main.arm7.c:2363` | `cardenginei_arm9_ra.bin`, the same file every boot |

The per-game colour-LUT blacklists only ever turn `colorTable` **off**, so no game can
switch the filter on for itself. A notification had been seen on this console and this
card, which requires all five to have been true — and since none of them can differ
between two games on one boot-to-boot pair, they were equally true for the game that
misbehaved. The block was fresh. The queue file really did still hold the record, and a
whole branch of the investigation closed without a hardware run.

That deduction is only available because the rig is fixed. On a card where a player had
set a colour filter, the same symptom would have had a second, entirely different cause,
and the log would have been the only way to tell them apart.

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

### The gate was holding it, and the gate was reading the wrong bits

The build with the conservative survey was run on Contra 4 and showed nothing at all. The snapshot at
`0x027FEE80` said why, and it said it by what was *missing*:

| Field | Value | Reading |
|---|---|---|
| `rcTriggered` +0x70 | 1 | rcheevos fired |
| `rcFirstId` +0xA0 | 302,349 | and named the achievement |
| `unlockSent` +0xAC | 1 | the id crossed to the ARM7 |
| `unlockQueued` / `unlockLost` | 0 / 0 | nothing waiting, nothing dropped |
| `shows` +0x08 | **0** | |
| `denied` +0x0C | **0** | |
| `evicted` +0x10 | **0** | |
| `deniedNoLayer` +0x14 | **0** | |
| `overlayState` +0xB3 | 0x00 | |

`show()` always increments exactly one of `shows` or `denied`. All four at zero means it was **never
called** — so the survey fix was not even exercised, and the notification never got as far as asking for
VRAM. It was still owed, held by the fade gate, and nothing in the snapshot said so.

Two independent bugs in that gate, both found by reading it rather than by another run.

**The mode bits were never consulted.** `SUB_MASTER_BRIGHT` is a blend factor in bits 0-4 and a *mode* in
bits 14-15: 0 and 3 mean no effect at all, 1 blends toward white, 2 toward black. The test read only the
factor. A game that leaves a stale factor sitting there with the mode off has a perfectly normal screen
and a gate that will never open.

That also **weakens the conclusion of the previous section**, which was stated too strongly. `overlayState`
bit 5 coming back set was reported as "confirmed: the notification landed inside a fade". All it ever
proved was that bits 0-4 were non-zero. The fade may well have been real — a stage ending is a fade — but
that reading did not establish it, and calling it confirmed was over-reading a bit that could not carry
the claim.

**The bound was in ticks, and ticks are not frames.** The wait was capped at 600, described as ten
seconds. It is 600 *calls*, and in Contra 4 `ticks` reached 1,720 over a session long enough to score
43,425 points — the per-frame hook keeps getting torn out and is re-armed from `cardRead()`, so the reader
runs a small fraction of the frames. 600 of those is minutes. The bound is 90 now: in a game that ticks
normally any transition is over well inside it, and in a game that does not, the notification is late by a
second or two instead of lost.

**And the state had no field, which is why one run was spent on it.** Every bit of `overlayState` is
written by `draw()`, which only runs when a notification *is* raised — so the one state that needed
reporting was the one state nothing could report. Bit 7 now says "a notification is owed right now" and is
refreshed every tick.

The ARM9 cardengine window is down to **8 bytes free**, and it took three attempts to fit even this. A
word carrying the wait count and the raw brightness register overflowed it by 44 bytes; packing the two
into one word still overflowed it; a single spare bit in a byte that already existed fit. That is not a
tight budget any more, it is the end of one — and it is the strongest argument yet for the overlay moving
to `cardenginei_arm9_ra`, which the achievement-name notification needs anyway.

### The flicker was the tick rate, and the hook moved to VBlank

The deferral fix worked and the notification became visible — "RA UNLOCKED, but very subtle, like 1-2
frames, intermittent". `overlayState` read **0x4A** on Contra 4: layer 1, block 1, extended palettes off,
bit 5 clear so *not* inside a fade, **bit 6 set so it had been held back and released**, bit 7 clear so
nothing was still owed. `denied`, `evicted` and `deniedNoLayer` all 0. The whole chain confirmed in one
reading, and the same reading explains the flicker.

I had a hypothesis and it was the wrong one. It was: *the game writes `SUB_DISPCNT` later in the same
frame than our line-0 handler, so we win alternate frames.* The mechanism is **frequency, not phase**.

`ticks` reached **1,132** across a session long enough to score 43,050 points — thousands of frames — while
`rearmDispstat` saturated at **255**. Contra 4 clears `DISP_YTRIGGER_IRQ` constantly, each re-arm from
`cardRead()` buys roughly one tick, and the reader therefore ran on about **8% of frames**. The overlay has
to re-assert its borrowed layer every frame, because the game rewrites those registers every frame — so it
was visible about one frame in twelve. That is precisely an intermittent 1-2 frame flash.

Both hypotheses predicted a fast flicker; the tick count is what separates them, and it was already in
every snapshot taken.

**So the hook moved from VCOUNT to VBlank.** Three conditions had to hold for the old hook to fire — the
game's `irqTable[2]` entry, `IRQ_VCOUNT` in `REG_IE`, and the Y-trigger in `REG_DISPSTAT` — and the third
is the one a game destroys. VBlank has no third condition: a DS game keeps `IRQ_VBLANK` enabled and
`irqTable[0]` pointed at something of its own, because it needs the interrupt itself.

What that buys, beyond the flicker:

- **A tick is a frame again.** `OVERLAY_SHOW_FRAMES 180` is three seconds rather than half a minute, and
  the 90-frame fade bound is a second and a half rather than minutes. Both numbers were quietly wrong by
  an order of magnitude while the reader was on a hook that fired 8% of the time.
- **The reader stops forcing an interrupt on.** A build with `RA_READER_ENABLED` no longer switches
  `IRQ_VCOUNT` on for a game that never asked for one — that only ever happened for the reader's sake, and
  the VCOUNT hook is the colour LUT's alone again.
- **`rearmDispstat` retires at 0**, and reading 0 forever is the clearest available statement that the
  fragile condition is gone. `ticks` tracking the frame count is the other half of the same reading.

Two implementation notes worth keeping:

**Installing and re-arming are the same act**, so there is one function and no separate install. If
`irqTable[0]` is not ours, save what is there and put ours in — which is an install the first time
`cardRead()` runs and a repair every time after. `cardRead` is patched into the game's *code* rather than
its interrupt table, so it survives exactly the thing that kills interrupt hooks.

**Neither mirrored layout had to change.** The game's original handler is kept in a word inside
`card_engine_header.s` next to the stub, loaded PC-relative, rather than added to the `cardengineArm9`
header — so `sizeof(cardengineArm9)` is still 0x108 and the patch table is still at +0x120. The assembly is
guarded by `RA_VBLANK_HOOK` from `ASFLAGS` rather than `RA_READER_ENABLED`, which the assembler never sees;
the two are set in the same three Makefiles, and if they drift apart the link fails on `raVblankHandler`
instead of quietly producing a cardengine with no hook.

### Confirmed on hardware, and `rearmIe` is the number that proves the reasoning

Two runs on Contra 4 with the VBlank hook, and the flicker is gone — the message reads instead of
flashing.

| | VCOUNT hook | VBlank hook, opening minute | VBlank hook, ~3 min |
|---|---|---|---|
| `ticks` | 1,132 over several minutes | 2,293 | **10,715** |
| against wall clock | ~8% of frames | ~38 s of frames | ~178 s of frames |
| `rearmTable` | 1 | 2 | 2 |
| `rearmIe` | 0 | **0** | **0** |
| `rearmDispstat` | **255**, saturated | 0 | 0 |

`ticks` now tracks the frame count one for one. And `rearmIe` reading **0** is the prediction verified
directly rather than by consequence: the game never once cleared `IRQ_VBLANK`, because it needs that
interrupt for itself. That was the entire argument for moving, and it did not have to be inferred.

The unlock run read `shows 1` with `denied`, `evicted` and `deniedNoLayer` all 0, and `overlayState`
**0x4A** — layer 1, block 1, extended palettes off, bit 5 clear so not inside a fade, bit 6 set so it had
been deferred and released, bit 7 clear so nothing was still owed. Deferred, released, drawn on a clean
screen, onto VRAM the game was not using.

**A confirmation that came free.** `rcFirstId` was **302329** at `rcFirstTriggered` **1** — the stage 1
trophy, the very achievement that started this hunt by unlocking with nothing on screen. It is line 1 of
the set now. It had been 302349 at line 11, then line 12, because 302329 was the one being filtered as
already earned; deleting it from the site put it back at line 1 and shifted everything after it.
`rcActivated` 45 against 44, `rcDefLength` 7,415 against 7,332, `rcFirstTriggered` 1 against 12, and
`rcFirstId` naming the difference. Four independent numbers telling one story, and the already-earned
filter is now demonstrated in both directions rather than just asserted.

**What it costs, stated rather than buried.** `rcLines` is 28-31 per frame in steady state, and that is
now paid on every frame instead of on 8% of them — the real per-frame cost went up roughly twelvefold.
The placement makes it affordable in a way the old hook did not: VBlank is 71 scanlines and this uses
about 30, so in steady state the work fits entirely inside the blanking period and touches no visible
line at all. The old hook fired at line 0 and spent those 30 lines on drawn pixels. Only the
initialisation frames overrun (`rcInitLines` 85, `linesMax` 189), and those happen once per boot.

And `cardengineArm9` is now pinned the way `cardengineArm7` is — `raCe9OffsetsPinned` in `misc.c` asserts
five field offsets against what `nm` reports for the assembly labels. This file reads `irqTable` to install
an interrupt handler now: a field that shifted would not fail to build, it would write a wild pointer into
a running game's interrupt table. The check was verified by inserting a field and watching the build fail.

## The notification says what was earned

`RA UNLOCKED` was always a placeholder. It said something had happened and nothing about what — and
with 45 definitions armed, "something fired" stopped being a reading a long time ago.

Three pieces, and the interesting part is that the split went the opposite way to the one predicted.

### The record grew a title, and the delimiter is a tab

Each staged line was `<id>:<memaddr>`. It is now `<id>:<memaddr>\t<title>`.

A tab, and the choice is what makes this unambiguous rather than merely convenient:

- **Not another colon.** A title is whatever a person typed, colons very much included — *Chapter 1:
  Beginnings* is an ordinary achievement name.
- **Not another line.** The reader treats every line as a definition.
- **A tab works because a memaddr has no whitespace anywhere in its syntax**, and JSON cannot carry a
  raw tab inside a string: it arrives as `\t`, two characters, and the launcher writes unknown escapes
  through verbatim. So a tab can appear in exactly one place in the record and means exactly one thing.

Backward compatible in both directions. A block with no tabs parses as it always did, and a
hand-written `ra_achievements.txt` needs no title.

`RA_PATCH_TITLE_MAX` is 32 — 31 characters — and it is sized from the *display* rather than from what
RA sends. The strip is one row of a 32-tile background with a tile of margin, so 30 characters is what
can be shown; storing more would spend block space, the scarcest thing here at 88% full, on text
nothing could draw.

**And if the label is what does not fit, the label goes and the achievement stays.** Without that,
adding titles would have silently switched off achievements at the end of a large set — a definition
that fitted yesterday would be `dropped` today for the sake of thirty bytes of text. `titleNoRoom`
counts it, and the log prints `titles N with, M clipped, K dropped for room`.

### Two bugs this found, both older than the change

**A brace in a string cost an achievement its id.** The scanner cleared the pending id on every `{`,
justified by a comment reading "ID comes first in each object and MemAddr immediately after, so there
is no text between them for a stray brace to sit in". That is not the order RA sends. The one reply
this project has captured reads:

```
"ID":101000001,"Title":"Warning: Unknown Emulator","Description":"Hardcore unlocks cannot be
earned using this emulator.","MemAddr":"1=1.300.","Points":0,"Author":""
```

A `Description` sits between the id and the memaddr and an `Author` sits between the memaddr and the
`Flags` field that commits. A brace in either would have cleared a perfectly good id, leaving an
achievement that can never be reported to the server — silently, because an id-less definition still
evaluates and still unlocks. The scanner tracks quoting for this one purpose now; the needles still run
over every byte, which is what makes them safe against a key name appearing inside a value.

**The overlay could put a tilemap inside a block it had not borrowed.** `BGCNT` holds the character
base in four bits (16K units, so any of the eight blocks) and the screen base in **five** (2K units,
reaching 62K and no further). The offsets that would extend both — `DISPCNT` bits 24-29 — are main
engine only. So a map placed inside block 4 or above needs a screen base of 32 or more, the field
truncates, and two kilobytes of tilemap land in a low block the game may well be using. The survey
still examines all eight, because a block the *game* is using has to be detected wherever it is; only
the choice is narrowed to the first four.

### The font is in `arm9_ra`, and the overlay did not move

The prediction in `ra_overlay.h` was that the real notification "needs a font and layout code that will
not fit in the cardengine's remaining ~840 bytes, and belongs in a separate ARM9 binary". Half right.
The font did have to move — printable ASCII is 760 bytes and the ARM9 window had **24 free**, having
already turned down a four-byte debug field and a fifty-six-byte diagnostic mode. But the file itself
stayed.

The split is at the pixels. `cardenginei_arm9_ra` owns the font, the character lookup, the centring and
the bit expansion, and renders into a fixed 32×2 tile strip when an achievement fires — which is the
only moment the id exists, and the strip has to survive up to 90 frames of fade deferral before it is
drawn. The overlay is handed the address in `raSnapshot.overlayText`, range-checked by `ra_tick()` so
the drawing side needs no opinion about where WRAM is, and blits it.

What the overlay keeps is everything that negotiates with the game: choose a layer that is off, survey
which VRAM blocks are referenced, borrow, re-assert every frame, hand back on eviction, restore. That
was the expensive part to get right and there was no reason to move it.

**It came out ahead.** The ARM9 window went from 24 bytes free to **116** — the glyph table and the bit
expansion were bigger than the blit and the four-byte snapshot field that replaced them. On the other
side the arena margin fell from 19,676 bytes to 15,932, which is where a 2K strip and 760 bytes of font
should show up.

The font is generated from pixel art rather than typed as hex, and that is not a detail: a wrong nibble
in a hand-typed font is a letter that looks nearly right in one word and wrong in another, which is
exactly the class of mistake a photograph taken hours later cannot diagnose.

### Tested where a hardware run would otherwise have paid for it

The nibble order is the one thing here that cannot be checked on hardware without spending a session. A
4bpp DS tile stores the *leftmost* pixel in the low nibble; getting it backwards renders every glyph
mirrored, which on a photograph of a three-second toaster reads as "the font is wrong" rather than as an
ordering bug.

So it is pinned against a glyph whose shape is known. `L` is `.##.....` six times over a foot of
`.######.`: the upright spans nibbles 1-2 and reversing the order would move it to 5-6. Verified by
reversing it and watching the check fail — the foot alone would not have caught it, because `.######.`
is symmetric.

Also pinned: the strip is cleared whole between calls, so a shorter message after a longer one cannot
keep the tail of the previous *achievement name* — which would read as a plausible message rather than
as obvious corruption, and is the worst way for this to fail. And a character outside the font renders
as a space rather than as a box, because the text that reaches that path is a JSON escape nobody chose.

### The overlay is fine; the moment is not

Two hypotheses died in one session, and the tool that killed them cost no achievements.

**The probe.** `-DRA_OVERLAY_DEMO=60` raises the notification once a second with nothing unlocked. It
had stopped fitting -- 20 bytes over the ARM9 window -- and the cause was the direct `show()` call:
`show()` is static, and a second call site stops gcc inlining it. Raising it through `pending` instead
fits and is better, because the probe then takes the same path a real unlock does, deferral included.
The other half was that a pulse with nothing rendered draws a blank box, so `arm9_ra` renders a
placeholder strip during `ra_rc_prepare()` and `overlayText` is a valid address from the first frame.

**What it showed.** On Contra 4 the notification is **visible and legible**, both lines, on the title
screen *and* thirty seconds into stage 1 gameplay. `shows` 11 over 2,680 ticks, which is exactly
2,680 ÷ 240 (60 frames of interval plus 180 of display), with `denied`, `evicted` and `deniedNoLayer`
all zero across eleven borrow-and-return cycles. `overlayText` 0x037542B8, matching `nm`.

So the font, the blit, the two-line layout and the whole VRAM negotiation work — during gameplay, in
the game where a real unlock shows nothing.

That kills the leading hypothesis. The pulse got `overlayState` 0x08 (layer **0**) and the real unlock
got 0x4A (layer **1**), and the DS resolves equal priorities by layer number, so the game's BG0 would
be drawn in front of our BG1. Plausible, mechanically sound, and wrong: the pulse is visible during
gameplay, where the game certainly has BG0 enabled.

What is left is **when**. Achievement 302329 is *stage 1 clear* — it fires during the stage-clear
sequence, which is the one moment the game tears down and rebuilds its entire screen.

**A reading that does not exist yet.** `overlayDispcnt` and `overlayWindow` were added for exactly this
question and have never been captured at the moment of a real unlock: the build that has them has only
ever been run without one, and the run with the unlock predates them.

### It was the display, and the probe read the achievement's name back

Confirmed on hardware, and the confirmation arrived as a sentence rather than a hex dump. Through the
whole of stage 1 the pulse was visible and read `probe 0123456789`, the placeholder `arm9_ra` renders at
init. At the stage-clear transition it **went blank for a stretch**. At the start of stage 2 it came back
reading **"Welcome to the jungle"** — the real name of the achievement that had fired during the blank.

The order matters and is worth spelling out, because "the title appeared" and "the title appeared exactly
when it should have" are different claims. The placeholder was on screen right up to the unlock; the real
name replaced it from the unlock onwards. That also verifies the one part of the cross-binary contract
nothing had exercised: a re-render *mid-session* is picked up by the next show, so the strip is a live
channel rather than something written once at init.

So the notification had always been drawn correctly. It was drawn into a screen the game was not
displaying, and it spent its 180 frames there.

`shows` 43 against 10,339 ticks, which is exactly 10,339 ÷ 240, with `denied`, `evicted` and
`deniedNoLayer` all zero across forty-three borrow-and-return cycles.

**And the same reading exposes a liability the fix had just created.** The transition lasts well over a
second and a half. The pulse survived it only because another pulse arrived after the screen came back —
a single real notification would have hit the 90-frame bound *inside* the blank, been released into it,
and been thrown away. The fix would have looked like it worked in the probe and failed in practice.

So the bound is now applied only to what it was designed for:

| Condition | Treatment | Why |
|---|---|---|
| A fade (`MASTER_BRIGHT`) | bounded at 90 frames | A game can *sustain* one — a dark room, a pause menu, a brightness setting. "Might not be visible" needs a deadline or the notification is lost. |
| Forced blank or display off | **waits as long as it takes** | "Definitely not visible", and no game runs with its screen switched off for long, because it needs the screen. Counting those frames toward a deadline is how the notification gets released into the blank. |

`overlayState` bit 7 stays set throughout, so a notification owed indefinitely — a game that never uses
the sub engine at all — reads as owed rather than as nothing happening.

### Two conditions the gate never checked

Added on their own merits rather than on suspicion, because both are unambiguously right:

| `DISPCNT` | Meaning |
|---|---|
| bit 7 | **Forced blank.** The engine outputs white and displays nothing — not a layer, not a sprite. Games set it while rebuilding a scene. |
| bits 16-17 | **Display mode.** 0 is off, and no background appears in it however its own registers read. |

A notification drawn into a screen that is not being displayed spends its 180 frames invisible and is
gone, which is the same argument the fade gate is built on applied to the two conditions that outrank
a fade. Neither could ever have been detected by a counter here: the survey sees VRAM, `brightActive()`
sees brightness, and nothing saw whether the engine was displaying at all.

And this time it *was* the cause, confirmed before it was written down as one. See above: the probe went
blank exactly at the transition and came back with the achievement's own name. Five hypotheses were spent
on this bug; the two that held were the two that were measured before anything was built on them.

### It is only ever visible on BG0, and that is measured

`overlayDispcnt` finally came back, from a pulse during stage 1 gameplay: **0x00211E10**.

| Bits | Value | Meaning |
|---|---|---|
| 0-2 | 0 | BG mode 0 |
| 7 | 0 | no forced blank |
| 8 | **0** | **the game's BG0 is off** |
| 9, 10, 11, 12 | 1 | BG1, BG2, BG3 and OBJ all on |
| 13-15 | 0 | no window active |
| 16-17 | **1** | display mode is graphics |
| 30 | 0 | extended palettes off |

And `overlayState` for that pulse was **0x08** — layer **0** — visible and legible. Both real unlocks
read **0x4A** — layer **1** — and were never seen.

That is the answer, and it had been sitting in a hypothesis I killed on an assumption. The DS orders
backgrounds by `BGCNT` bits 0-1, lower value in front, and **breaks ties by layer number**. The overlay
takes priority 0, the strongest there is, so it beats any enabled layer at priority 1 or worse — and loses
to any *lower-numbered* enabled layer at priority 0, with nothing below 0 to reach for.

`chooseLayer()` was returning the first layer the game had switched off. On BG0 that is unbeatable. On BG1
it is behind the game's BG0. Free and hidden looked exactly like drawn-and-lost.

The same reading closes the other two doors it was added to test: no window is active, so `overlayWindow`
reading 0 is a fact rather than an accident; display mode is graphics; no forced blank. The gate conditions
were fine at that moment — they were only ever wrong during the transition, which is a separate and now
fixed problem.

**What changed.** `chooseLayer()` refuses a layer it knows would be covered:

```c
for (i = 0; i < 4; i++) {
    if (!(SUB_DISPCNT & (1u << (8 + i)))) return i;   /* free, nothing in front of it */
    if ((SUB_BGCNT(i) & 3) == 0) return -1;           /* enabled at priority 0: it and all after are hidden */
}
```

One loop rather than a nested search, and equivalent: walking in order, the first free layer is usable if
and only if no earlier enabled layer holds priority 0.

**What that buys, and what it does not.** It turns an invisible notification into a counted denial, which
is the trade this file makes everywhere else — `denied` and `deniedNoLayer` both move, and
`overlayDispcnt` says which layer blocked it. It does **not** make the notification appear at the moment
that matters, because at the start of stage 2 the game's BG0 is on.

### So it takes the layer it needs, and stops refusing

A change of policy, and the reason is that the honest refusal delivers nothing. At the start of stage 2 the
game's BG0 is on at priority 0, and that is exactly when the stage-1 achievement's notification is due. A
notification that is correct and never appears is not a notification.

`chooseLayer()` now returns the first layer that is either free-and-reachable or **enabled at priority 0**,
taking the second kind rather than giving up on it. What that costs is bounded and reversible, and both
halves matter:

- **The layer's VRAM is never touched.** The overlay points its character and screen bases at a block of
  its own, so the game's tiles and tilemap for that layer sit there untouched throughout.
- **Every register taken is put back** by `hide()` — `BGCNT`, both scroll registers, and the enable bit.

So the layer's content is intact the whole time and simply does not display for the three seconds the
notification is up. On Contra 4 that is part of the bottom-screen map.

Two consequences worth writing down before the next run rather than after it:

- **`deniedNoLayer` is retired at 0**, like `rearmDispstat`. `chooseLayer()` cannot fail any more, so a
  whole category of denial is gone; if that field ever moves again, something reintroduced a refusal.
- **The "who writes last" race comes back, and may show as flicker.** Displacing an *enabled* layer means
  the game is still writing that layer's `BGCNT` from its own code, while the overlay re-asserts it once
  per frame from VBlank. Whoever writes later in the frame wins it. This is the same mechanism that made
  the notification flash one frame in twelve on the old VCOUNT hook, and it is stated here as a prediction
  so the next reading can confirm or kill it rather than have it explained afterwards.

### It works, and the flicker prediction was wrong

`overlayState` **0x48** on the real unlock: layer **0**, block 1, extended palettes off, not inside a fade,
deferred and released. `shows` 1 with `denied`, `evicted` and `deniedNoLayer` all zero. `rcFirstId` 302329
at line 1, 45 definitions active. And on screen: the achievement's name, **stable, no flicker**.

The prediction in the previous section was that displacing an *enabled* layer would bring back the
"who writes last" race and show as flicker. It did not. Either Contra 4 does not rewrite that layer's
`BGCNT` during the three seconds, or the once-per-frame re-assert from VBlank wins. Written down before the
run precisely so it could be killed this way.

**And one thing was described worse than it is.** The cost was written up as "part of the bottom-screen
map". On Contra 4 the sub engine feeds the **top** screen — the one being played on — so what actually
happens is that a background layer of the gameplay screen goes missing for three seconds. Which panel the
sub engine drives is `POWCNT1` bit 15 and belongs to the game; see limitation 4 below.

Nothing is corrupted and nothing about the game's state changes: `evicted` 0, the layer's VRAM was never
touched, and `hide()` puts `BGCNT`, both scroll registers and the enable bit back, so it returns exactly.
The game keeps rendering that layer throughout; it simply is not displayed.

But as a finished state it is not good enough. Eating a piece of the scenery you are dodging bullets in is
not an acceptable toaster, and that is the honest reading of an otherwise working notification.

## Sprites: the path that costs the game nothing

An object at a given priority is drawn **above every background** at that priority, whatever the game is
doing with its layers. So it does not have to take a layer at all — and what it does need, a disabled OAM
entry and a range of object VRAM nobody references, it can *find* rather than borrow. The background path
stays as the fallback for when neither can be found.

### The move that had to come first

The ARM9 cardengine had 60 bytes free and had already refused a four-byte diagnostic field twice. OAM
negotiation, an object-VRAM survey and a gather blit were never going to fit, so the overlay moved to
`cardenginei_arm9_ra` — the move this document has predicted since phase 1.

The window went from 60 bytes free to **1,224**, and it cost nothing: that binary is called once a frame
from the same VBlank hook, so the overlay runs exactly as often as it did. Three things came free:

- The trigger count is **this** frame's rather than last frame's, so a notification is raised on the frame
  the achievement fires instead of the one after.
- The rendered strip is a neighbour's pointer instead of an address crossing a binary boundary, so the
  range check that guarded it is gone.
- **The snapshot address no longer moves between the normal and pulse builds** — 0x027FEA00 for both,
  because the code the demo flag changes is not in the cardengine any more. That removes a class of
  mistake this document records twice.

`RA_OVERLAY_DEMO` moved to `arm9_ra`'s Makefile with it. Left where it was it would have been a flag
reaching nothing, and the pulse probe would have stopped working silently — the exact failure its own
comment warns about.

### Eight objects of 32×16

256×16 pixels is exactly the 32×2 tiles of the strip. Eight rather than four 64×32 ones because 64×16 is
not a shape the DS has, and four of the larger size would reserve twice the object VRAM to leave half of
it blank.

Object VRAM is claimed in 2K units, one per notification, and **only the first 16K is ever considered**.
That is a mapping question, not tidiness: how much VRAM sits behind 0x06600000 depends on the game's
`VRAMCNT`, and a write past the end does not fail — it mirrors, onto tiles the game *is* using. 16K is the
smallest allocation a game using sub objects realistically makes.

### Confirmed: the notification appears and the game loses nothing

*"El aviso se ve y no falta nada — ni escenario ni balas ni personaje."*

Which is the whole point of this path, and it closes the question the overlay has been open on since
phase 1. Objects beat every background at the same priority, so no layer is taken; the OAM entries come
from where the game does not reach, so no sprite is taken; the object VRAM is surveyed and given back, so
no tiles are taken. The notification names the achievement, waits for a screen worth drawing on, holds
for three seconds, and costs the running game nothing observable.

The background path stays underneath it as the fallback for a game with 2D object mapping, with objects
switched off, or with no free OAM entry or VRAM slot -- and it is a *working* fallback, measured, at the
price of a background layer for three seconds.

### OAM entries are taken from the back

And that is a correction hardware made within one run.

They were taken from the front, on the reasoning that objects are ordered among themselves by OAM index —
lower is in front — so the earliest free entries are the ones least likely to be covered by one of the
game's own sprites. The result on Contra 4: the notification appeared, no scenery was lost, and the game
started **losing sprites** — bullets, and sometimes the player — erratically, for exactly as long as it was
up.

The mistake was believing "disabled right now" meant "not wanted". A game builds its object list from index
0 upward each frame, writes as many entries as it has sprites, leaves the rest disabled, and DMAs the whole
thing in its own VBlank handler. Ours is chained *after* that, so whatever the game had just put in the
eight entries we hold is overwritten before the screen is drawn, every frame. The entries were not free.
They were the next ones the game was going to need.

Nothing avoids that except choosing entries the game does not reach, and the back is where those are:
stealing from 127 downward only costs the game a sprite when it is already using more than 120, where
stealing from 0 upward cost it one almost immediately.

What that gives up is being in front of the game's *own* sprites — at index 120 the overlay is behind
nearly all of them, so a bullet crossing the text wins that pixel. A fair trade twice over: text with a
bullet through it is legible, and a deleted bullet is a bug. And it gives up nothing that matters, because
an object still beats **every background** at the same priority, which is the entire reason for this path.

### Three places this refuses to guess

- **2D mapping** (`DISPCNT` bit 4 clear) addresses object tiles as a 32-wide matrix rather than
  consecutively, so the arithmetic here is simply wrong for it. Falls back. Contra 4 uses 1D.
- **Objects disabled** (bit 12 clear) would mean enabling them, and a game that keeps them off may have an
  OAM full of entries it never intended to be seen. Falls back.
- **`attr3` is never written.** The fourth halfword of every OAM entry is part of the interleaved affine
  matrix table, so a disabled entry's may hold a live parameter for a sprite the game is rotating. Writing
  it would corrupt that sprite's matrix.

And "disabled" is exact rather than approximate: with the rotation flag clear, attribute 0 bit 9 is the
disable bit, so `(attr0 & 0x0300) == 0x0200` and nothing else means off. With the rotation flag *set*, bit
9 means double-size and the object is live — reading that pair as one field would treat every double-size
affine sprite in the game as free.

### The boundary, and insurance against reading it wrong

In 1D mapping the tile number steps by the boundary in `DISPCNT` bits 20-21 (32/64/128/256 bytes); the
classic reading, and the only one the GBA had, is a flat 32. Contra 4 sets 128.

If that interpretation is wrong in the direction of *too large*, the game's tiles appear further out in
VRAM than they are, the survey calls a slot free that is not, and the blit writes over somebody's sprite.
So **the survey marks the union of both readings**. One extra range per entry, and it makes that
impossible; it makes a free slot slightly rarer, which is a fallback rather than a fault.

The overlay's own placement is not exposed to the same risk, and by construction rather than by luck:
`spriteBlit()` writes to the byte address of the slot the survey approved, computed without reference to
the boundary at all. A wrong boundary would make our objects *read* their tiles from the wrong place — a
notification of visible nonsense — and could never make them write to it.

### Held every frame, for the same reason the layer was

The eight entries are rewritten on every frame the notification is up, because a game that keeps a shadow
copy of OAM and DMAs the whole thing each frame — the ordinary way to do it — would otherwise wipe them on
its next transfer. And the claimed slot is re-surveyed every frame and given back on eviction, because the
survey is a sample of registers the game rewrites: a range that was free when the notification started can
be the game's a frame later.

`overlaySpriteOam` and `overlaySpriteSlot` say which path ran and what it took, with **0xFF meaning the
background path**. A separate field rather than another bit of `overlayState`, because an object has no
layer and no character block — reporting through those bits would make "objects" indistinguishable from
"layer 0, block 0".

## Known graphical limitations of the overlay (deferred)

These are all in `ra_overlay.c`, all found by playing real games, and all deliberately
left alone — including for a first public release. They are listed together so the
decision is on the record rather than implicit in what nobody got around to fixing.

**What the object path changed about this list**, since it arrived after the list was written:

- **Item 5 is closed.** Skipped notifications were a "no free layer" condition; there is no layer to be
  free now, and the deferral holds a notification until the screen is worth drawing on rather than
  dropping it.
- **Item 3's exposure collapsed, and it has since been fixed.** `surveyBlocks()` was mode-blind — it
  read every `BGCNT` as a text background — and the object path already reduced that to the fallback
  path only, so a game taking the object path never reached it. It is no longer a bug at all: see "The
  survey learns to read the BG mode" below.
- **Item 2 moved rather than closed.** The object path borrows one entry of the *object* palette instead
  of one of the background palette. One entry either way, and for the same unavoidable reason: the text
  has to be some colour.
- **Items 1 and 4 are untouched.** The menu collision and having no say over which physical screen the
  sub engine feeds are both exactly as they were.

One item the object path adds: at OAM index 120 the overlay is **behind** the game's own sprites, so a
bullet crossing the text wins that pixel. Deliberate, and the alternative was measured — taking entries
from the front put us in front and deleted the game's sprites instead.

The reason they are acceptable is the same in every case: the overlay's design rule is
that **a notification that corrupts the game is worse than no notification**, and it
holds that rule. Every item below is either cosmetic, transient, or a design choice.
None of them can crash a game or corrupt a save.

| # | Limitation | Severity | Observed? |
| --- | --- | --- | --- |
| 1 | Collides with the in-game menu on the frame it opens | cosmetic, transient | yes, once |
| 2 | Borrows two palette entries the game may be using | cosmetic, transient | not since the fix |
| 3 | `surveyBlocks()` mis-reads non-text backgrounds | ~~**could corrupt graphics**~~ **fixed** | no |
| 4 | Cannot choose which physical screen it appears on | design decision | yes, by design |
| 5 | Silently skips notifications when no layer is free | design decision | yes, 5 of 18 attempts |

**1 — the menu collision.** Pressing X to open the in-game menu hands both screens to
the menu. If the overlay is holding a borrowed layer at that instant, both are writing
sub engine registers in the same frame. This produced the single remaining fault seen
after the palette fix. No achievement is going to unlock on the exact frame the menu
opens, so the exposure is close to nil. The clean fix is for the overlay to stand down
while the menu is up, which needs a way to know the menu is up.

**2 — the borrowed palette entries.** The text has to be *some* colour, so entries of the
sub background palette are taken and restored on hide. Which entries a game is using is
not discoverable from the registers, so there is no way to pick a provably free one. It
was fifteen until phase 1 found that the glyphs only ever used one, and it is two now:
the drop shadow needs a colour of its own, and white glyphs without one are legible only
where the game's artwork happens to be dark. Two is the floor this design has while the
text has an outline at all, and both are saved and put back the same way the one was.

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

> **The mode-blindness is fixed too, as of the survey rewrite.** The paragraph above stands as written
> — it was true when written — but the second half of it is no longer the state of the code. See "The
> survey learns to read the BG mode" below.

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

## The survey learns to read the BG mode

This is item 4 of "What is left", and item 3 of the deferred graphical limitations: the last of the
overlay's known ways to corrupt a game rather than merely fail to draw on it.

### What was wrong

`surveyBlocks()` decided which 16K blocks of sub BG VRAM the game was using by reading all four
`BGCNT` registers as though every one of them described a **text** background — character base in 16K
units, screen base in 2K units, map size from bits 14-15. It never consulted the BG mode in `DISPCNT`,
and it never looked at the colour-depth bit.

Three separate misreadings came out of that, and they fail in both directions:

- **An affine map is one byte per entry, not two.** At size 3 that is 128×128 entries — 16K, a whole
  block — where the text reading says 8K. The survey would call the second half of somebody's tilemap
  free and hand it to the overlay.
- **An extended-affine map is affine-shaped with two-byte entries**, so size 3 is 32K: two blocks, four
  times what the text reading allowed.
- **A bitmap's base is the screen-base field in 16K units, not 2K** — a factor of eight — and bit 2,
  which is the low bit of the character base everywhere else, is its colour depth instead. So the
  survey looked for a bitmap's pixels in the wrong block entirely, marked a character block that does
  not exist, and missed the four or eight blocks the bitmap actually covers.

### Why it was never seen

*Contra 4* runs in **BG mode 0**, where all four layers are text and the old reading is correct by
accident. That is measured, not assumed: `overlayDispcnt` came back `0x00211E10` from a pulse during
stage 1 gameplay, and its low three bits are 0. Every hardware run this project has ever done was in
the one mode where the bug cannot fire.

Which is also why this was fixed with tests rather than with a photograph. Reaching it on hardware
would mean finding a game that puts an affine or bitmap background on the **sub** engine and then
earning an achievement inside it. The registers are just numbers; the whole table costs nothing to
check on the host, and 35 assertions now do.

### What it reads now

`raOverlaySurvey()` takes `DISPCNT` and the four `BGCNT` values and returns the block map. It is pure —
it touches no hardware — which is the only reason it can be tested at all; `surveyBlocks()` is now a
four-line wrapper that samples the registers and calls it.

The layer-shape table it works from, which is the sub engine's and not the main engine's:

| mode | BG0 | BG1 | BG2 | BG3 |
|---|---|---|---|---|
| 0 | text | text | text | text |
| 1 | text | text | text | affine |
| 2 | text | text | affine | affine |
| 3 | text | text | text | extended |
| 4 | text | text | affine | extended |
| 5 | text | text | extended | extended |

An *extended* layer is a bitmap when `BGCNT` bit 7 is set and an affine background with 16-bit tile
indices when it is clear. Sizes, in bytes:

| size bits | text map | affine map | extended-affine map | bitmap, 8bpp | bitmap, 16bpp |
|---|---|---|---|---|---|
| 0 | 2,048 | 256 | 512 | 16,384 | 32,768 |
| 1 | 4,096 | 1,024 | 2,048 | 65,536 | 131,072 |
| 2 | 4,096 | 4,096 | 8,192 | 131,072 | 262,144 |
| 3 | 8,192 | 16,384 | 32,768 | 262,144 | 524,288 |

**The authority for all of this is libnds' own headers**, which are on disk next to the compiler rather
than remembered: `BgSize`/`BackgroundControl` in `nds/arm9/background.h` carry the size tables and the
bit patterns, `bgInit()` asserts *"Tile base is unused for bitmaps. Can be offset using mapBase *
16KB"* — which is the 16K-unit bitmap base in as many words — and `bgInitSub()` asserts *"Sub Display
has no large Bitmaps"*, which is what rules out mode 6 here.

Two deliberate choices in the new code:

- **A mode this engine does not have marks every block used**, so the notification is denied. Modes 6
  and 7 cannot be read, and this is the same trade the rest of the survey makes: a notification that
  does not appear is a missing feature, a corrupted game is a bug.
- **A base pointing outside the 128K window matches nothing** rather than being folded back into it.
  Both fields can express such an address — a character base reaches 240K and a bitmap base 496K — and
  there is no block of ours out there to protect. Guessing how the hardware wraps it would be exactly
  the kind of unmeasured assumption that has cost this project runs before.

### What is still a heuristic, and is not fixed

**Tile data is still marked as one 16K block, and it can be more than that.** How far a character base
actually reaches is not in any register: it depends on how many distinct tiles the map references. The
worst case is 1,024 tiles of 256 colours — 64K, four blocks — and marking that would deny nearly every
notification on a game that never uses it.

So this stays as it was, and it stays a known limitation rather than a fixed one. It is a much smaller
hole than the mode-blindness was: it is only ever an *undercount* of one specific kind, and it applies
equally to the text backgrounds the survey always handled, so nothing about it is new or worse.

## The unlock remembers when it happened

This is item 2 of "What is left". An achievement earned at nine in the evening and submitted at eight
the next morning was being recorded by the server as an eight-in-the-morning unlock, because the only
time in the request was the moment it arrived. The API's answer is `o=`, seconds since the unlock, and
this client is exactly the case it exists for.

### The two things the plan got wrong

**The RTC was not where the note said it was.** `sharedAddr[7]/[8]` hold hours and minutes and nothing
else, and only while the in-game menu is open, because `inGameMenu.c` is what writes them. No date, no
seconds, and not available on the frame an achievement fires. The cardengine reads the RTC itself
instead, with `rtcGetTimeAndDate()`.

**`o=` changes the signature.** This is the one that would have shipped a build where every submission
was refused. rcheevos' `rc_api_init_award_achievement_request_hosted()` normally hashes
id + username + hardcore; when `o=` is sent it appends **the id a second time and then the seconds**,
with rcheevos' own comment explaining that the server overloads its hash generator that way. The URL and
the digest have to change together, so `raQueueSign()` takes the seconds and switches both, and the host
suite pins the result against `printf '93119Bakke0931193600' | md5sum` rather than against itself.

### What the record looks like now

    <id>\t<YYYYMMDDhhmmss>\t<gamecode>\t<gametitle>\n

The tab delimits for the same reason it delimits the staged definitions block: no field can contain
one. The record grew from 16 bytes to 32 for the stamp and then to 48 for the game, and the file with
it — and it migrates itself, because the launcher reads whatever length it finds and always writes
`RA_QUEUE_BYTES` back. See "A queued unlock names its own game" for the last field.

**The stamp is optional, and that is what makes the upgrade a non-event.** A bare id — one typed by
hand, or queued by a build from before this existed — parses exactly as it always did and is sent
without `o=`. Hand-writability was the property that let the sending half be tested before the writing
half existed, and it survives intact.

### Where each half of the work happens, and why

The cardengine writes digits and does no arithmetic; the launcher does the calendar. That split is
deliberate: leap years, month lengths and the epoch all live on the side with a host test, and the side
running inside a game with no console stays a copy loop.

- **`raUnlockStamp()`** (ARM7 cardengine) reads the RTC and formats six fields. It is called *outside*
  the unlock's critical section — that section runs with IME off while an ARM9 card read may be waiting
  on it, and two SPI transactions do not belong in it.
- **`raQueueStampToUnix()` / `raQueueUnixToStamp()`** (launcher, pure) convert each way. Pinned against
  `date -u +%s`, not against each other, and then also checked as a round trip.
- **`raWifiSubmitOne()`** subtracts and sends.

Three details that are decisions rather than mechanics:

- **`rtcGetTimeAndDate()` returns integers, not BCD.** It masks the 12/24-hour bit and calls
  `BCDToInteger(t, 7)` before returning — read out of the disassembly of `libnds7.a`, because the header
  says nothing and the in-game menu's own use of it is ambiguous. What it does *not* normalise is the PM
  flag in 12-hour mode, where the hour arrives with 40 added (`RTCtime`'s comment: *"0 to 11 for AM, 52
  to 63 for PM"*). That subtraction is a no-op at 24 hours and the difference between 21:xx and 61:xx
  at 12.
- **Local time is treated as UTC at both ends.** The console has no zone, so the alternative is
  inventing one — and it cancels out of a difference. What it cannot defend against is the user changing
  the clock between earning and submitting, which no encoding could.
- **Anything implausible sends no `o=` at all.** No stamp, a clock that was never set (year 2000, which
  is why the floor is 2001), a clock that moved backwards, or a gap over a year: each one falls back to
  the old behaviour. A wrong time would be worse than the old behaviour; no time merely *is* the old
  behaviour. The launcher log says which happened, because `o=` is the one number in the request that
  nothing downstream can check — the reply does not echo it back.

### Confirmed on hardware, first try

*Contra 4*, achievement 302329, on a 3DS:

```
queue            1 to send
  302329  earned 154 s ago
  302329  awarded
award reply:
  {"Success":true,"AchievementID":302329,"AchievementsRemaining":44,"Score":1134,"SoftcoreScore":450}
awarded 1, refused 0, still owed 0
```

Four separate things in six lines, and each was a way this could have failed:

- **`time()` returns a real clock in the launcher.** The one thing the suite could not cover. The
  launcher's own ARM7 calls `initClockIRQ()` and that turns out to be enough.
- **The RTC stamp is right.** It passed `raQueueStampToUnix()`, which means the ARM7 read the clock,
  normalised the PM flag and formatted six fields without corrupting any of them.
- **Both ends read the *same* clock.** This is what 154 actually proves. Had the zone handling not
  cancelled, the delta would have been off by whole hours — 154 + 3600·N — rather than being a small
  plausible number. A tight value is the cancellation, observed.
- **The five-field signature is accepted.** `Success:true` with `AchievementID`, `Score` and
  `AchievementsRemaining` all present, which by this project's own earlier lesson is the part that says
  something was *recorded* rather than merely acknowledged. Stage 14 on the same boot agrees from the
  other end: `UserUnlocks:[302329,101000001]`, and the set came back 44 definitions instead of 45
  because 302329 was filtered out of it.

**The console's timezone never enters, and that is worth stating plainly.** `o=` is an offset, not an
instant, so the server dates the unlock from *its* clock minus the seconds. Whatever zone the console is
set to cancels out of the subtraction before it is ever sent. Treating local time as UTC at both ends is
not an approximation that mostly works — it is exact, because only the difference leaves the console.

**What this log still cannot show** is the time the server actually recorded. The award reply does not
echo `o=` back, and the `r=unlocks` reply carries ids without timestamps. The place that can confirm it
is the achievement's own unlock time on the RetroAchievements site, which should read 154 seconds before
the submission rather than at it.

## A queued unlock names its own game

Groundwork for showing pending unlocks in the in-game menu, and it started as a question rather than a
plan: *what happens if the queue holds more than one game?*

### What was actually broken, and what was not

**Submission was never broken.** `a=` is a globally unique achievement id and this client sends no
`m=`, so an unlock from game A submitted while booting game B goes to the right achievement. The
evidence is in the project's own files: `101000001` — the *Unknown Emulator* notice — appears in the
set for GameID 14856 and again in a boot of GameID 12917. One global id space.

**Two things were broken.**

The first is that the feature this is groundwork for could not exist. A pending list has to say which
game an unlock belongs to, and the record did not know.

The second is a silent-loss path. The launcher's rule is *"an answer clears the record; silence keeps
it"*, because a refusal is assumed permanent and legitimate. If the server ever refuses an award for
being outside the current game's session — not measured either way — that record is cleared and the
unlock is gone with nothing said. Naming the game does not fix that on its own, but it is what makes
the case visible instead of invisible.

### The game comes from the cartridge, not from the server

`gameCode` and `gameTitle` are the first sixteen bytes of every DS header, and the ARM7 cardengine
already holds a pointer to the running game's. So a record can name its game at the moment the
achievement fires — with the radio down, which is exactly the case that needs it: a queue full of game
A's unlocks while game B is running and there is no WiFi to drain it.

Two things fell out of that, and both were the question's doing rather than the plan's:

- **RetroAchievements' GameID is not in the record and does not need to be.** Submission never wanted
  it, and for naming the game the cartridge header is better — offline, and a name instead of a number.
- **The risky change disappeared.** Passing a GameID down to the cardengine would have meant a new
  field in the cardengine header and its two hand-mirrored copies — the `.align` trap that has already
  cost this project a run. The header pointer was already there, so none of that is needed.

The cost is that `gameTitle` is the ROM's internal title: twelve characters, upper case, `CONTRA4`
rather than `Contra 4`. Recognisable, free, and available offline.

### Where the tidying happens, and why it had to move

The cardengine copies both fields **raw** and the launcher sanitises them — stops at the first byte
that is not printable ASCII, drops trailing padding. That is not a preference, it is a budget:

```
cardenginei_arm7 (TWL-SDK)  33K region
  free before this change :  76 bytes
```

**Seventy-six bytes.** That binary is the tightest in the project — tighter than the ARM9 cardengine's
12K window, which has 1,224 spare — and it shares `cardengine.c` with the roomy one. Trimming and
range-checking two header fields there did not fit, and it belongs on the side with a host test anyway.
Same split as the stamp: this side writes bytes, the other interprets them.

Two measurements came out of making it fit, and both are the kind that are invisible until the linker
refuses:

- **A copy loop with a constant bound is not free.** `for (i = 0; i < 12; i++)` is one gcc unrolls into
  twelve load/store pairs — 264 bytes for two fields. `tonccpy()` is a call, and it fits.
- **The date was being validated twice.** `raQueueStampToUnix()` already refuses anything it will not
  vouch for and has the host tests behind it, so the cardengine's copy of those range checks was pure
  duplication on the side that could least afford it. Dropping it took the margin from **12 bytes to
  140**. A garbage clock now writes a well-formed stamp the launcher rejects, which is the same outcome
  by a shorter road.

### The parser's new sharp edge

`CONTRA4` ends in a digit. The queue parser's founding rule is that every non-digit is a separator, so
a title left unconsumed would have that `4` read as an achievement id and submitted as an unlock the
player never earned. Fields are therefore consumed to their delimiter whatever they contain — clipped
in the copy, never in the consumption — and the host suite pins that specific case.

Everything after the id stays optional and positional. A bare id, typed by hand or queued by an older
build, parses as it always did. A record that has a stamp but no game keeps its stamp.

## The notification moves to the corner and grows a shadow

Three changes to how the notification presents itself, asked for together and worth writing down
together because two of them are the same decision seen from different sides.

### `RA UNLOCKED` becomes `ACHIEVEMENT`

The heading was a debugging label that outlived its job. `RA UNLOCKED` said *the RetroAchievements
code in this fork fired*, which is a sentence about the implementation, and it was the right thing to
print back when "something fired" was the only reading available. The line underneath now carries the
achievement's own name, so the heading's only remaining job is to tell a player that the words below
are not part of the game they are playing. `ACHIEVEMENT` does that and says nothing about how.

One character of this is a judgement call rather than a transcription: the request was written
`ACHIVEMENT`, which is the ordinary typo for the word, and the string is user-visible. It went in
spelled correctly. Reverting is a one-character edit at `RA_TEXT_HEADING`.

### The message right-aligns, and the strip moves to row 1

`OVERLAY_ROW` was 10, which on a 24-row screen is the middle. That was never a decision — the overlay
had one row to be in and the middle was where it landed. It is 1 now: the top of the screen, with one
row of clearance rather than flush against the edge, because flush against the edge reads as a
rendering fault on a panel with any bezel at all.

The horizontal half is subtler, and it is the reason the two changes belong together. **The strip is
already the full width of the screen** — 32 tiles, 256 pixels, drawn as eight sprites side by side —
so there is no such thing as moving it right. Moving the strip up and leaving the text centred inside
it would have produced a message still floating in the middle of the screen, only higher: the same
complaint with a different Y. So the corner is made of two independent pieces, `OVERLAY_ROW` in
`ra_overlay.c` and the alignment in `ra_text.c`, and only both of them together are "the top right".

`RA_TEXT_MARGIN` is one column, kept clear at the right-hand end, and it earns its keep twice: text
flush against column 31 is text touching the bezel, and the shadow below hangs a pixel to the right of
every glyph — at column 31 that pixel falls off the strip and is clipped, on the one character where
its absence is most visible. `d` and `5` are the proof rather than the illustration: their bowls reach
pixel 7 of their cell, so their shadows reach pixel 8, which is the next tile.

### The shadow, and why it needed the renderer rewritten

A drop shadow was the biggest available readability win, and the cost is stated plainly: **a second
borrowed palette entry**, listed below as limitation 2. White glyphs on a game's own artwork are
legible exactly where the artwork happens to be dark, which is not a property any game guarantees;
white over black is legible over anything.

What made this more than a second loop was the geometry. A glyph used to be written as eight whole
words into the one tile it belonged to — the fastest thing possible, and structurally incapable of
drawing a shadow, because a shadow is the same glyph one pixel right and one pixel down and *a pixel
right of column 7 is the next tile*. Composing whole words can only draw shapes that respect the 8×8
grid, and a drop shadow is defined by not respecting it. So `ra_text.c` gained `raTextPixel()`,
addressing the strip as if it were a 256×16 bitmap, and everything above it works in pixels. Out of
range is dropped rather than wrapped: a shadow hanging off the bottom row is the ordinary case for a
descender, and wrapping it would put a smear at the far end of the row above.

The pass order is a correctness argument, not a preference. **Both shadows first, then both glyphs** —
not shadow-then-glyph per letter. A shadow is drawn a pixel into its neighbour's cell and the row-0
shadow of a descender lands inside row 1, so interleaving would let a later letter's shadow fall on an
earlier letter's ink: a grey notch bitten out of a white stroke, which on a photograph of a
three-second toaster reads as a broken font rather than as an ordering bug.

`RA_TEXT_INK` and `RA_TEXT_SHADOW` moved into `ra.h` next to the geometry, and that is the same lesson
this project keeps re-learning. `ra_text.c` wrote a literal `1` and `ra_overlay.c` defined
`OVERLAY_PAL_INDEX 1`, two constants in two files that had to agree and agreed only because nobody had
had a reason to change either. Adding a second colour is exactly that reason.

### What the host suite pins

The nibble-order test on `L` is now stated in two colours instead of one, and it is a stronger test for
it. `nibbleSpan()` became `nibbleSpanOf(word, colour)`, because "something is set here" can no longer
tell ink from the shadow of the letter to its left.

`L` is an upright at columns 1–2 over a foot at columns 1–6, so its shadow is an upright at 2–3 over a
foot at 2–7, one row lower. Ink is drawn second, so on rows 1–5 the shadow has a single visible column
at 3, and on row 6 the foot's ink covers it entirely. **That asymmetry is the assertion worth making**:
a shadow drawn after the ink would read as `span(2,3)` on those rows, which is the notched-font bug
above, caught by arithmetic instead of by a photograph.

Right alignment is pinned on `d` at the last usable column, with the shadow asserted to land in
column 31 — so the margin is checked to be doing the job it exists for, rather than merely existing.

## The self-test was queueing itself as an achievement

The bug: **in every game RetroAchievements does not know, this fork earned a fake achievement,
filed it on the card, showed it to the player as work waiting to sync, and sent it to the
server — once per boot, forever.**

### What the card said

Reported as "I ran a game with no RA support; the Contra 4 unlock went up to RA fine but never
cleared from the queue, and the menu still listed something pending". Two hypotheses were
plausible from the source — a lost award reply, or a stale WRAM block — and both were wrong. The
card settled it in three lines.

`sd:/ra_unlocks.txt`, pulled over FTP:

```
4026531840	20260815183256	C6PJ	PICROSS3D
```

`4026531840` is `0xF0000000`. That is `RA_SYNTHETIC_ID_BASE`, the id this fork gives a definition
that arrived without one — and the built-in self-test is exactly such a definition. And the
launcher log from the following boot:

```
-- stage 13: report what the last session earned --
queue            1 to send
  4026531840  earned 71 s ago
  4026531840  refused: Unknown achievement.
award reply:
  {"Success":false,"Status":404,"Code":"not_found","Error":"Unknown achievement."}
awarded 0, refused 1, still owed 0
```

Read together they describe a loop, not an incident. Picross 3D is a game the server answers for
with no definitions — `definitions none`, `2 server notice(s) dropped`, the set is literally called
*Unsupported Game Version (Picross 3D)*. With nothing staged, `cardenginei_arm9_ra` falls back to
its built-in self-test, the self-test triggers seconds into play, the trigger goes down the unlock
ring to the ARM7, and the ARM7 appends it to `ra_unlocks.txt` exactly as it would a real
achievement. Next boot: the launcher submits it, gets a 404, clears it — and the self-test refills
the file before the player reaches a menu.

So the queue was **never** clean on an unsupported game. It was cleared correctly every boot and
refilled every boot, which from the outside is indistinguishable from never being cleared at all.

### Three things it was costing

| | |
| --- | --- |
| The menu | Sync Pending counted a fake unlock. A fake is indistinguishable from a real one once it is in the file — the record even carries a game code and title, because the ARM7 stamps those from the running cartridge. |
| The card | One junk record per boot of every unsupported game. |
| The server | A `404 Unknown achievement` per boot, from a client whose User-Agent is still trying to get sanctioned by RA. Of everything here, this is the one that could have cost the project the feature. |

It also retires a symptom that was closed on the wrong evidence. "`RA UNLOCKED` appeared at the
start of the game where the probe should no longer be" was attributed to the `RA_LAUNCHER_WIFI=0`
delivery fault and fixed by `tools/ra_release.sh`. That fault was real — but the self-test *also*
fires on a correct `=2` build in any game the server does not know, so that symptom had a second
cause and it was never closed.

### The guard, and where it goes

In `ra_rc_queue_unlock()`, the one function every trigger passes through:

```c
if (id >= RA_SYNTHETIC_ID_BASE) {
    if (unlockSynthetic < 255) {
        unlockSynthetic++;
    }
    return;
}
```

**Here rather than downstream, because this is the only place that knows the id is synthetic.** By
the time it reaches the ARM7 or the launcher it is just a large number, and a large number is what
a real id looks like.

The notification is deliberately untouched. The overlay is driven by `triggeredCount`, which the
event handler bumps *before* calling this, so the self-test still proves on screen that the reader,
the runtime, the ring and the overlay all work. It just no longer claims a player earned something.

Counted rather than dropped in silence, and published as `raSnapshot.unlockSynthetic` in the two
padding bytes that were already there — the snapshot is still 0xCC. On a game with no definitions,
"the self-test fired and was correctly not queued" and "nothing fired at all" are the two states
worth telling apart, and last time it took a card's queue file to tell them apart.

### And the cards already carrying them

`raQueueScan()` drops synthetic ids on the way in and counts them separately from `dropped` —
these are well-formed records that meant something to the build that wrote them, so "cleaned one"
and "your file is corrupt" stay distinguishable. The launcher says so
(`N self-test id(s) discarded, not achievements`) and rewrites the file even when the queue reads
as empty, which it now does: without that the record would sit there being counted by the menu on
every future boot, because nothing else clears an empty queue.

The placement inside the scanner is the part that has to be right. The drop happens **after** the
record's stamp, code and title are consumed, not at the id — dropping early would leave
`20260815183256` and `PICROSS3D` to be read as two more ids, which is the same trap the tab
delimiter exists to avoid. The host suite pins that case with the exact record off the card.

One test changed rather than being added: the queue parser's upper-bound case used to assert that
`4294967295`, the largest `u32`, survives a scan. It no longer does — it is above
`RA_SYNTHETIC_ID_BASE` — so the boundary is now stated on `RA_SYNTHETIC_ID_BASE - 1`. Real RA ids
are six or seven digits, so nothing real is near either number.

### Confirmed on hardware, and it found a second one

*Ketsui Death Label* earned `301258` (*Sea Horse Hunter*), then the console booted **Arkanoid DS**,
which the server does not know:

```
the server does not know this hash
body: {"Success":true,"GameID":0}
...
queue            1 to send
  301258  earned 127 s ago
  301258  awarded
awarded 1, refused 0, still owed 0
```

`ra_unlocks.txt` afterwards: empty. The real unlock crossed a game boundary and cleared, and the
self-test wrote nothing behind it. That is both halves of the fix, in the hardest case available.

**Reading that log turned up a worse bug of the same shape**, and this one queues *real* ids.

`loadRaDefinitions()` stages `ra_achievements.txt` for whatever game is booting, unconditionally —
it is a hand-managed debugging file with no way to know which game it belongs to, and it says so.
That is harmless while something overwrites it, which for a ROM the server knows is exactly what
stage 15 does.

A ROM the server does **not** know never reaches stage 15 (`no GameID; the set cannot be asked
for`), `raWifiCacheLoad()` has no cache for it either, and the hand file survives into the game —
one game's triggers watching that game's addresses inside another game's RAM.

Worse than the self-test, in the one way that matters. A synthetic id gets a 404. **A real id gets
accepted**: a trigger firing on unrelated memory queues an unlock the server records, and the player
wakes up holding an achievement for a game they were not playing. Nothing undoes that from here.

**Reasoned from the source, not observed**, and the correction is worth keeping because it is the
kind of mistake this document exists to prevent. The first version of this section said the hazard
had been *seen*: the card's `sd:/ra_definitions.txt` held Ketsui's fifteen definitions while the
console booted Arkanoid DS. But `sd:/ra_definitions.txt` is `RA_DEFS_DUMP_PATH` — a file this
launcher **writes and never reads**. It was the record of the last successful fetch and evidence of
nothing else. The file that would have to be present is
`sd:/_nds/nds-bootstrap/ra_achievements.txt`, in a different directory, and whether that card has
one was never checked. Two files whose names differ by one word, and reading the wrong one as the
input turned a sound argument into a false sighting.

The fix is one line and it goes where the fact exists: `r=gameid` returning `GameID: 0` is the only
answer that can say *no set applies to this ROM*, so `raWifiIdentify()` clears the staged block
there. Not at the `done:` fallback, because a ladder that never got that far cannot tell "no set for
this ROM" from "never asked" — and the hand file is the only way to test definitions with no access
point at all. It stays staged on every path except the one that positively contradicts it.

### The same bug had a second spelling, and the fix only spelled it one way

Months later, a player asked why achievement **1** kept appearing on their account. It has no game
of its own as far as any DS set is concerned, it had been arriving since early on, and nobody had
looked because it looked like something RetroAchievements did rather than something we did.

It is `RA_TEST_ACHIEVEMENT_ID`, and it was `1`, under this comment:

> The test achievement's id. Any non-zero number does; it is only how the runtime identifies the
> trigger back to us, **and nothing here talks to the server yet**.

Every clause was true when it was written. The last one stopped being true when step 6b closed the
loop, and nothing brought the constant along.

**So the guard above was written for this exact failure and did not catch it.** It tests
`id >= RA_SYNTHETIC_ID_BASE`, because the card that exposed the bug was carrying `4026531840` —
`0xF0000000`, what a *staged definition with no id* gets. The **built-in** self-test is the other
kind of idless definition, and it had a constant of its own four hundred lines away. One bug, two
spellings, and the fix spelled it one way.

The consequence is the one this section already said was the worse kind: a synthetic id gets a 404,
and **a real id gets accepted**. Achievement 1 is published, on a Mega Drive game, so every boot of
a DS game the server does not know filed a Sonic unlock on the player's account. Confirmed by the
player from the site, and deleted there.

#### The fix is to stop having two numbers

`RA_TEST_ACHIEVEMENT_ID` is now `RA_SYNTHETIC_ID_BASE`. Not a second check — there is simply no
longer a value here that the guard has to be told about separately. Nothing collides: an idless
staged definition takes `RA_SYNTHETIC_ID_BASE + i`, and the self-test only activates when no
definition was activated at all, so index 0 is never both.

The host suite now pins it **by calling the guard**, not by comparing the two constants:
`ra_rc_queue_unlock(RA_TEST_ACHIEVEMENT_ID)` must leave the ring untouched and bump
`unlockSynthetic`. What has to hold is that nothing this binary invents can be queued, not that two
numbers happen to be ordered — a third source of made-up ids fails that test the same way. With the
old value it fails three times.

#### Cards already carrying one, and why nothing filters it

A queued `4026531840` is dropped on the way in, safely, because no real id is near it. **A queued
`1` cannot be filtered on the same terms**, because 1 *is* a real achievement — the player found
which one. Any rule that dropped it would be a guess about RetroAchievements' id space dressed up
as a check, and this document has a section about what those cost.

So the source is fixed and nothing cleans up after it. A card that has been running unsupported
games may still hold one; deleting `sd:/ra_unlocks.txt` clears it, and the launcher recreates the
file at full length on the next boot.

#### It was not the id in the run that found it

Worth separating, because the two are independent and both were real. In the log that exposed this,
id 1 was submitted **without `o=`** — no `earned N s ago` line beside it. A self-test record goes
through `raUnlockAppend()` like any other and carries a stamp, as the `4026531840` record above
does. So that particular `1` had no record of its own: it was the mode field's digit, left in front
of the parser by the padding bug. The recurring one, on games the server does not know, is this one.

Two sources, one wrong id, and the log alone could not have told them apart. The missing timestamp
could.

## The answer: the server takes `h=1` and files it as softcore

Two boots settle it, and they settle the parser fix at the same time.

**Boot 1** — empty queue, `hardcore=1`, `sync=1`, `submit=1`, then *Contra 4* was played and two
achievements were earned. **Boot 2** — the same ROM booted only to sync, nothing played.

### First, the queue file, byte for byte

```
b'302349\t20260816140959\tYCTE\tCONTRA 4\x00\x00\x00\x00\t1\n\x00\x00\x00\x00\x00\x00'
b'302329\t20260816140959\tYCTE\tCONTRA 4\x00\x00\x00\x00\t1\n\x00\x00\x00\x00\x00\x00'
```

There is the padding, in the bytes rather than in an argument: `CONTRA 4` in a twelve-byte field,
four NULs, *then* the tab and the mode. Forty-two bytes of record and six of NUL to the next one. The
old parser stopped on the first of those four NULs and left the `1` to be read as an id.

And boot 2's stage 13:

```
queue            2 to send
  302349  earned 197 s ago
  302349  awarded
  302329  earned 197 s ago
  302329  awarded
awarded 2, refused 0, still owed 0
2 of them earned in hardcore, per the record
```

Two records, two ids, both stamped, both hardcore, **no phantom**. `skipPadding()` and the
end-of-line guard are confirmed on hardware, on the exact byte pattern that broke them.

### And then the answer

`h=1` really was sent — that last log line is the proof, since it prints `q.hard`, which is what
`raWifiSubmitOne()` puts in the URL and in the signature. The server said:

```
{"Success":true,"AchievementID":302349,...,"Score":1154,"SoftcoreScore":462}
{"Success":true,"AchievementID":302329,...,"Score":1154,"SoftcoreScore":455}
```

**`Score` does not move. `SoftcoreScore` does.** And three other readings agree:

- `r=startsession` in the same boot: `"HardcoreUnlocks":[]`.
- `r=unlocks&h=0`, *after* the awards: `"UserUnlocks":[302329,302349,101000001]` — the softcore list
  now holds both.
- `"HardcoreMode":false` in that same reply.
- The next fetch dropped from 45 definitions to **43**, with `#!302329` marked earned. They are out
  of the set.

So: **the server accepts `h=1` from an unrecognised client, answers `Success:true`, and records a
softcore unlock.** Not a refusal. Not an error. The notice was telling the truth — "hardcore unlocks
cannot be earned using this emulator" — and the way it enforces it is silent downgrade.

That is the outcome this document guessed at as one of three, and named as the one with nothing in
the reply to match on. There is nothing to match on. `Success:true` is what a hardcore award and a
downgraded one both look like.

### Which corrects the warning again, in the other direction

The `ra.cfg` note called this "the bad case" and framed it as a cost of turning hardcore on. That
framing was wrong, and the measurement is what shows it: **an unlock is filed softcore whether this
client sends `h=0` or `h=1`.** Sending `h=1` buys nothing today. It also costs nothing extra.

The property that "an achievement is spent the moment it lands" is not new and is not about
hardcore. It is the standing behaviour of a softcore client, written down long before any of this,
and it is the reason `submit=0` exists. A player who wants a set kept fresh for a hardcore run *when
registration lands* should not be submitting at all — that is `submit=0`, and it always was.

So `hardcore=1` today is neither dangerous nor useful against the server. What it is good for is the
half that does work: the RAM viewer stays locked, the cheat engine takes it away and says so, and
every queued record carries the mode it was earned in. All of that is exercised and confirmed by
these two boots.

### One number not to read too hard

`SoftcoreScore` reads 462 on the first award and 455 on the second — backwards. 302329 is worth 3
points, from its own definition line, and 455 + 7 = 462 fits 302349 being worth 7. The likeliest
explanation is a stale read on the server's side between two requests seconds apart, and nothing in
this client depends on the field. Recorded rather than chased.

## The RAM viewer closes for a hardcore session

The hardcore gate used to refuse every session, and only one of its two reasons was about the
player's configuration. `raWifiHardcoreRefused()` checked for a cheat file and then refused anyway,
unconditionally, with the comment saying why: the in-game menu's RAM viewer *writes*, that is a
cheat device by any reading of RetroAchievements' rules, and it is a page two button presses away
rather than a setting anyone opts into. There was no state to consult, so the gate consulted none.

There is state now. The viewer refuses to edit in a hardcore session, and the unconditional half of
the gate is gone. What remains is the cheat-file check — and the User-Agent, which is not a fact
about this console and cannot be checked here at all.

### What the viewer actually does, which is narrower than "the RAM viewer"

Reading is not what the rules are about, and nothing about reading changed. Navigation, the jump-to
box, the ARM7 window, the cursor: all of it works in hardcore exactly as before. A hex dump of a
running game is a debugging tool and this fork has no reason to take it away from anybody.

What is refused is entering **edit mode**, which is the only thing in the viewer that writes. That
distinction is also what makes the refusal cheap: one branch, at the one place `mode` becomes 2.

Both write paths are behind it, and they are not the same path:

- **ARM9 memory is edited in place.** `ramPtr` is `(u8*)address` — the game's own memory, not a copy
  of it — so `KEY_UP` on a selected byte is a store to the running game. There is no commit step.
- **ARM7 memory is edited through a buffer**, and pushed across with the `RAMW` message on A or B.

Entering the mode is gated once, and the mode's body is gated again. The second gate cannot fire
today, because the branch above it is the only way in. It is there because a later change that adds
a second way in would reopen both paths at once, and the symptom would not be a crash anybody
notices — it would be an unlock claimed as hardcore.

Pressing A in a locked session says so, over the last two rows of the dump:

```
Hardcore: RAM editing is locked
Set hardcore=0 in ra.cfg to edit
```

Two lines because one of them has to be the way out. A refusal that does not say what to change is
a bug report waiting to be filed.

### The block, and why it is not a field in the pending tally

The menu has no configuration, no filesystem and no launcher. It learns things the way it learns
the sync tally: the launcher stages a block, the bootloader copies it into DSi WRAM, the menu reads
it where it lands. `CARDENGINEI_ARM9_RA_SESSION_LOCATION` is the fourth such block, after the
binary, the definitions and the tally, and it is wired exactly like them — its own magic, checked
separately by the bootloader, cleared by `loadFromSD()` on the way past so the guarantee does not
depend on the previous boot.

The obvious cheaper thing was a `u8` inside `raPendingBlock`, which already crosses this exact gap.
It is wrong, and the reason is *when* rather than *where*.

`raWifiStagePending()` runs at **stage 13**, most of the way up the network ladder. Every ordinary
early exit — `sync=0`, no access point, DHCP never answers, a refused login — leaves no pending
block at all. Those boots are not failures: they fall through to `done:`, load this ROM's cached
set, and play the game. rcheevos evaluates, achievements fire, and the queue fills up for a later
boot to submit. **A session that earns achievements with the radio off is the normal case for this
client, not an edge case.** A hardcore flag living in a block that is absent on exactly those boots
would read as "not hardcore" precisely when it matters, and the menu would unlock its editor for a
session whose unlocks are going out as `h=1`.

So the session block is written the moment `ra.cfg` has been parsed and the gate has ruled, at
stage 0c, before a single register of the radio is touched — above every `goto done` in the
function. It carries the mode the session will *actually* run in, not the one the file asked for,
because it is written after `raWifiHardcoreRefused()` has had its say.

### Absent means unlocked, and that is the safe direction

The menu treats a missing magic as "nobody told me anything", and answers softcore. That is not the
convenient default dressed up as the safe one — it is safe because of who reaches it:

| Boot | Session block | Can it submit a hardcore unlock? |
| --- | --- | --- |
| Plain nds-bootstrap, no RA | never staged | no — there is no RA client |
| `RA_LAUNCHER_WIFI=0` | never staged, `ra_wifi.c` is compiled out | no — nothing submits anything |
| DS/DSi, or a colour LUT selected | staged, never copied — the RA window does not load | no — no achievements are evaluated |
| 3DS, `hardcore=0` or no `ra.cfg` | staged, `hardcore = 0` | no — `h=0` on every request |
| 3DS, `hardcore=1` | staged, `hardcore = 1` | yes — and the editor is locked |

Every row that reads "unlocked" is a row that cannot claim hardcore. The failure this ordering
avoids is the other one, and it is the reason the fourth word joined the three that `loadFromSD()`
clears: an uncleared staging word is whatever RAM came up holding, and on the first boot after
power-on that word would otherwise decide whether a hardcore session may edit its own memory.

### A bug this found on the way past

`loadRaDefinitions()` capped the hand-written `ra_achievements.txt` at the definitions reservation
minus its header, ignoring the three structures that live in the top of that same reservation.
`raWifiFetchPatch()` has subtracted them for a while — a fetched set arrives from a scanner that
fills whatever it is given — but a hand-written file looked safe because it is bounded by whoever
wrote it.

It is the same bound. At 30 KB that file would have written straight through the tally, the
viewer's index and now the session block. Two of those being wrong is cosmetic. The third is a
hardcore session with its RAM editor silently handed back, from a file the player wrote. Both caps
now subtract all three, and the host suite pins the arithmetic against the reservation rather than
against the other copy of it.

### What to look for on the next run

The test rig is the standing one — TWiLight Menu at its defaults, on a 3DS, from the SD card. Two
boots answer everything, and neither needs the radio.

**`sync=0` is enough for everything below**, and it is the fastest way to get there: the ladder exits
at stage 0c instead of spending forty seconds failing to associate, and staging happens above that
exit, which is the case the whole design turns on. What it does not cover is the recorded mode — no
set is fetched, so on a ROM with no cache nothing fires — and the pending tally, which is staged ten
rungs later. For those, `sync=1` with `submit=0` gives the full ladder with the queue untouched. See
"What turning it on today actually risks" for why `submit` is the knob that matters here and `sync`
is not.

With `hardcore=1` in `ra.cfg`, `sd:/ra_wifi_launcher.log` should carry, in stage 0c:

```
hardcore         1
in-game menu     RAM editing locked -- hardcore
```

and **not** `hardcore refused: the in-game menu can write RAM`, which no longer exists. Then in the
game: open the menu, `RAM Viewer`, A to place the cursor, A again. The two red lines should appear
and the byte under the cursor should not change. B, then `SELECT` for the ARM7 window, and the same
again — that is the `RAMW` path rather than the in-place one, and it is worth pressing separately.

A third boot is worth one flash cycle on its own: `hardcore=1` **with cheats enabled in TWiLight
Menu**. The log should say `hardcore refused: the cheat engine runs for this ROM`, then
`RAM editing unchanged -- softcore`, and the viewer should edit. Then turn every cheat back off in
TWiLight and boot again — `cheatData.bin` is still on the card at that point, which is exactly the
case the old `!= 0` check got wrong — and hardcore should come back.

Both halves are **confirmed on hardware**: the viewer refuses the edit, and the menu reports the
mode.

**The menu answers all of this without the log**, which is the cheapest way to read the result: open
`Achievements...` and the line above `RetroAchievements` says `Hardcore -- RAM editing locked`,
`Softcore -- cheats are on` or `Softcore`. If it says `No session this boot` on a 3DS boot that
staged the RA window, something upstream of everything here is wrong and the log is the next stop.

With `hardcore=0`, or with `ra.cfg` absent, the log should say `RAM editing unchanged -- softcore`
and the viewer should edit exactly as it always has. That second boot is the one that matters most:
the failure this change can plausibly introduce is not a hardcore session that edits, it is every
*other* session losing its editor because the block was read wrong.

All of it with `sync=0`, per the note above: the ladder exits at stage 0c, ten rungs before the
pending tally is staged, and the lock must still be in force. The tally reading
`No queue was read this boot` on those runs is that exit, not a fault.

## A queued unlock remembers the mode it was earned in

Closing the RAM viewer exposed the other half of the same problem, and it was worse than the half
that had just been fixed.

**The mode was applied at submission time, from `ra.cfg`.** A record held `<id>`, a timestamp and
the game — nothing about how it was earned. So the sequence that defeats the whole thing was: play
softcore with the RAM editor open, earn unlocks, quit, set `hardcore=1`, boot. The launcher drains
the queue and sends every one of them with `h=1`, correctly signed. Nothing downstream could have
noticed; the requests are well-formed either way.

This was reachable only in theory while the gate refused every session outright. Lifting the gate
made it reachable in practice, which is what moved it from a note to a fix.

### The record is the only thing that lives long enough

The session that earned an unlock is over by the time it is sent — possibly days over, on a
different game, in a different mode. `ra.cfg` describes the session about to *start*. The queue
record is the only object that crosses from one to the other, so it is where the mode has to live:

```
<id>\t<YYYYMMDDhhmmss>\t<gamecode>\t<gametitle>\t<h>\n
```

Appended rather than inserted, which keeps every existing file readable, and it cost no migration:
the record was already 48 bytes and the longest one went from 44 to 46. Unlike the stamp (16 → 32)
and the game (32 → 48) before it, no card in the field needs resizing.

`h=` on `r=awardachievement` now comes from that field, and so does the signature. In both
directions: an unlock genuinely earned in hardcore is still sent as hardcore on a boot whose
`ra.cfg` has since been set to softcore. Dating it by the boot that reported it would be the same
mistake `o=` exists to avoid.

`raWifiSubmitOne()` **lost its `const raConfig*` parameter** to this change, which is the clearest
statement available that the file no longer has a say.

### How the ARM7 learns it, and why it is not a new slot

The ARM7 writes the queue record. It cannot read the session block — that lives in
`cardenginei_arm9_ra`'s DSi WRAM window, which the ARM7 does not map — and the launcher is long
gone. So the mode has to cross with the unlock.

`sharedAddr` has **four usable slots**; `RA_SHARED_UNLOCK_REQ` and `_ID` are two of them. Slots 11
and 12 are free and either would hold a flag, but spending a third on one bit would leave this one
feature holding three quarters of everything the ARM9 will ever be able to say to the ARM7.

So the mode rides in the **magic**: `RAUL` for softcore, `RAUH` for hardcore. One more compare on
the ARM7, no address space, and softcore keeps the value it always had.

It also makes the wrong thing impossible rather than merely unlikely. With a separate flag slot, a
request written without setting it reads as softcore *by omission* — a missed store in a path added
later, or a stale value from the previous unlock. Here the mode and the request are the same word:
there is no way to raise a request without saying which kind it is.

### Absent is softcore, and the digit has to be eaten

A record with no mode field reads as softcore. That covers a hand-typed id, a record from a build
before this field, and a record whose clock failed so it carries no fields at all — the last one
downgrades a genuine hardcore unlock, which is a loss for the player and the right direction to
lose in.

The parser **consumes** the digit whether or not it uses it, and that is not bookkeeping. Left
behind, the outer loop reads `1` as the next record's id and submits an unlock nobody earned. This
project already learned that lesson on `CONTRA4`, whose trailing `4` did exactly that before the
title field was consumed properly. The host suite pins it: two records must parse as two, not four.

`raQueuePack()` writes the field back out, because the records it keeps are the ones the server
never answered — the population most likely to be retried, and the last one that should be
downgraded on the way.

### What it cost, and the number is small enough to be uncomfortable

**`cardenginei_arm7_twlsdk` is down to 60 bytes of link margin, from 76.** That is the tightest
binary in the tree and the change spent 16 bytes of it. It links, and the linker is what enforces
this rather than a review — but the next thing to want a byte on the ARM7 side of the queue should
read this line first and expect to pay for it somewhere else.

The three other tight variants are unaffected in kind: `dsiware` 2,612 bytes, `cheat` 3,064, the
plain and `alt` builds 11,844.

## The menu says which mode the session is in, and why

Everything above changed what the loader *does* without changing what a player can *see*. The only
visible trace of hardcore was a refusal in the RAM viewer, which is two pages deep and only appears
if you go looking for the one thing that is now forbidden.

So the RetroAchievements folder carries a line, above its own name:

| What it says | When |
| --- | --- |
| `Hardcore -- RAM editing locked` | the session is hardcore |
| `Softcore -- cheats are on` | hardcore was asked for and the cheat engine took it |
| `Softcore` | `ra.cfg` says softcore, or there is no `ra.cfg` |
| `No session this boot` | no launcher staged a session — a plain build, or a console with no RA window |

A line rather than a page. It is one fact, and a page for one fact charges a button press for
nothing. The folder's own comment has said since it was written that it exists so that "forcing a
sync, reading the launcher's log or **showing the session** have somewhere to go that is not the
root menu" — this is that.

### The reason is the half that earns it

`Softcore` on its own answers a question nobody was asking. The player who needs this screen is the
one who set `hardcore=1`, is looking at a softcore session, and wants to know what took it — and
before this line, the only answer available was to power the console off, take the card out, and
read `ra_wifi_launcher.log` on a PC.

So the session block grew a `refusal` byte beside the flag. A reason code rather than a string,
because it crosses into a binary with no formatter and the set of reasons is small and closed. It is
open at the end deliberately: the User-Agent is the refusal this project expects to add next, and it
will cost a number and a line.

The bootloader sets the reason when it takes hardcore away for cheats, and **only when there was
something to take**: writing it on a session that never asked would have the menu explaining a
refusal that never happened.

### `hardcore` stays a flag

The obvious economy was to fold the reason into the existing byte and make it an enum — `0`
softcore, `1` hardcore, `2` refused-for-cheats. It is refused here, and the host suite pins the byte
at one byte to keep it refused.

Two things read that byte as a *capability*: the RAM viewer deciding whether to open its editor, and
the unlock path deciding which magic to send. Both test `!= 0`. Against an enum, a third value added
later would read as hardcore in exactly the two places where being wrong is expensive. The reason is
display-only, nothing gates on it, and it lives in its own byte where it cannot be mistaken for
permission.

`cardenginei_arm9_igm` grew 280 bytes for the line and the block's second field, to 21,428 of the
28,160 the B4DS layout allows.

## "A cheat file exists" was never the question

The gate's surviving half asked `conf->cheatSize != 0`, and that turns out to be the wrong question
asked of the wrong file. It was wrong in both directions, and only one of them is cosmetic.

### There is no cheats switch to read, and that is why the size is the switch

Worth stating before the predicate, because `> 4` looks like a threshold somebody tuned and it is
not. **nds-bootstrap has no cheats on/off setting anywhere.** Not in the configuration struct — the
only cheat-shaped fields are `cheatFileCluster`/`cheatSize`, `wideCheatFileCluster`/`wideCheatSize`
and the AP patch, all of them file locations and lengths. Not in the in-game menu: `MenuItem` has no
cheats entry, and the `"Cheats..."` string is commented out in `conf_sd.cpp` with `menu[6]` since
reclaimed by `RAM Viewer...`.

The cheat *engine binary* is staged unconditionally for every non-DSiWare boot, so its presence
means nothing either. What decides is `cheatSizeTotal`, and nothing else.

The switch lives in **TWiLight Menu**, which writes only the cheats the player ticked into
`cheatData.bin` before handing over. By the time nds-bootstrap sees it, an intention has already
become a length — so asking "how many bytes of cheats am I about to install" is not a proxy for the
real question. It *is* the real question, and the only form of it that exists on this side.

That also explains the number. `> 4` is not a tolerance: it is the length of a file that holds a
terminator and no cheats, which is what TWiLight leaves behind when the player opens the cheat list
and ticks nothing.

### The hole: `cheatData.bin` is one of three inputs

nds-bootstrap's own predicate, in `main.arm7.c`:

```c
cheatSizeTotal = wideCheatSize + cheatSize + (apPatchIsCheat ? apPatchSize : 0);
cheatsEnabled  = (cheatSizeTotal > 4 && cheatSizeTotal <= 0x8000);
```

Two of those three were not being looked at. A card with **wide cheats** and no `cheatData.bin`, or
with an **AP patch that is really a cheat file** — `pck_load.cpp` sets `apPatchIsCheat` for a `.bin`
AP fix and for a `cheatVer` entry in a pack — ran the cheat engine while the gate reported a clean
session and let it claim hardcore.

### The false positive: opening the cheat screen is not turning cheats on

The floor is `> 4`, not `> 0`, and that is the whole point of the number. TWiLight Menu writes
`cheatData.bin` when the player *opens* the cheat list for a game; switching every cheat back off
leaves the file behind, a few bytes long, holding its terminator and nothing else. Testing `!= 0`
refused hardcore for a player who had once looked at the cheat screen and turned everything off —
which is exactly the state RetroAchievements would call clean.

### Two gates, because neither side knows the whole answer

The launcher now sums the same three files against `RA_CHEATS_MIN_BYTES`. What it deliberately does
**not** reproduce is the ceiling: `cheatsEnabled` also refuses a total the cheat engine has no room
for, and that limit comes out of the ROM's own layout — `0x8000` in one place, a variable derived
from `0x4000` in `hook_arm7.c`, and `cheatSizeTotal` is zeroed outright for a console with nowhere
to put an engine. A launcher guess at it would be a fourth copy of a number that already exists
three times.

So the bootloader has the last word, one line after it copies the session block, at the point where
the answer is finally settled:

```c
if (cheatSizeTotal > CARDENGINEI_ARM9_RA_CHEATS_MIN_BYTES) {
    *(u8*)(CARDENGINEI_ARM9_RA_SESSION_LOCATION
           + CARDENGINEI_ARM9_RA_SESSION_HARDCORE_OFFSET) = 0;
}
```

**Downgrade only.** A boot can lose hardcore there and can never gain it: the launcher has already
signed `r=startsession` with what it believed, and a block granting more than the launcher claimed
would be the fork lying to itself before it lied to the server.

It reaches into the block by offset rather than through the structure, because this file does not
include `ra_wifi.h` — the same arrangement the pending block's magic already has, and for the same
reason. The host suite pins `offsetof(raSessionBlock, hardcore)` against the constant. That check
earns its place: a field inserted before `hardcore` would move the flag, the bootloader would go on
clearing a byte of the *magic* instead, the menu would read "nobody told me", and a cheating session
would get its RAM editor back with nothing failing to compile.

### What it did not cost

Nothing on the ARM7: the cheat total was already computed there for the engine's own sake, and the
extra work is a compare and a byte store. `cardenginei_arm7_twlsdk` stays at 60 bytes of margin.

### First hardware run: it submitted an achievement that does not exist, and the padding is why

The first boot with `hardcore=1`, `sync=1`, `submit=1` on *Contra 4* sent two unlocks. One of them
was id **1**.

```
queue            2 to send
  302329  earned 355 s ago
  302329  awarded
  1  awarded
award reply:
  {"Success":true,"AchievementID":1,"AchievementsRemaining":34,"Score":1154,"SoftcoreScore":460}
```

`AchievementsRemaining: 34` against 44 for the real one — id 1 belongs to a different game entirely,
and the account now holds it.

**It was the mode field's digit, read as the next record's id.** The exact failure the field's own
comment warned about, from the exact cause that comment did not think of.

`raUnlockAppend()` writes `gameTitle` as a **fixed twelve bytes** straight out of the ROM header,
deliberately: trimming it costs bytes the ARM7 does not have, and the comment there says so, in the
words "this side writes bytes, the side with a host test interprets them". *Contra 4*'s header pads
`CONTRA 4` with four NULs. `takeField()` stops at the first byte outside printable ASCII, so it
stopped on the padding — four bytes short of the tab that introduces the mode. The parser looked for
a tab, found a NUL, gave up, and left `1` in front of the outer loop.

That had been harmless for as long as the title was the **last** field. Nothing followed it, so
stopping early lost nothing. Appending one field turned a latent flaw into a submitted unlock.

#### The test existed and did not catch it, which is the part worth keeping

There was a test for exactly this — "two records must parse as two, not four" — and it passed. It was
written against `raQueuePack()`'s output, which trims its fields. **Two writers, one reader, and every
test in the file exercised the writer that agreed with it.** The cardengine's own record shape had
never been fed to the parser on a host.

The suite now builds the record byte for byte as the ARM7 does, NUL padding included. Neutered, the
fix fails it six times.

#### Fixed twice, and the second one is the one to keep

`skipPadding()` steps over the NUL padding inside a fixed-width field so the next delimiter can be
found. Only NULs, which is what makes it safe: a record with no mode field lands on its newline, and
one whose padding runs to the end of the record lands on the next record's first digit — which the
outer loop then reads as an id, exactly as it should. Skipping any non-printable, or scanning ahead
for a tab, would swallow the record after this one.

The second gate is structural: **a record that carried a stamp is machine-written, and nothing else
on its line is an id**, so the parser now runs to the newline whatever is left over. A tab after the
id is the thing no hand-typed list can contain, which is already what licenses reading the next
fourteen digits as a date. Bare ids on a line with no tab keep the forgiving behaviour that makes the
file hand-writable.

Measured rather than asserted: with `skipPadding()` disabled and only the newline guard in place, the
hardware-shaped records parse as the right *count* with nothing fabricated, and lose only the mode
they could not reach. Six failures become three, and none of the three is an invented unlock.

That is the gate worth having. Consuming each field correctly is a rule every future field has to
re-earn; stopping at the newline already holds for fields nobody has written yet.

#### What the run cost, and what it did not answer

The account holds achievement id 1, awarded softcore — `SoftcoreScore` went 455 to 460 while `Score`
stayed at 1154. Removing it is a request to RetroAchievements; nothing in this tree can.

And **the run did not test hardcore at all.** Because the mode was never parsed, both awards went out
with `h=0`. There is no `N of them earned in hardcore, per the record` line in that log, which is the
tell. What the server does with `h=1` from an unrecognised client is still exactly as unknown as it
was before the boot.

### What turning it on today actually risks, which took two corrections to get right

Setting `hardcore=1` before the User-Agent is recognised has a cost. Naming it correctly took two
passes, and both wrong versions are kept here because the wrong ones are the intuitive ones.

**The first version said it can lose unlocks.** The mechanism is real: `raWifiSubmitOne()` keeps a
record only when *nobody answered* — a dropped connection, a timeout. A record the server answers
and does not accept is counted as refused and cleared, because the refusals that rule was written
against are "unknown achievement" and "already held", where keeping it means retrying the same
rejection on every boot forever. So a refused hardcore award does vanish from the queue.

**But the achievement does not vanish with it.** A refusal means the server recorded nothing, so
`r=unlocks` will not list it, the scanner will not skip it, the definition is staged again and
rcheevos arms it again. The player replays that stretch of the game. That is a cost, not a loss, and
calling it a loss made the safe path look more necessary than it is.

**The version that is actually irreversible is the opposite one.** If the server *accepts* `h=1` and
quietly records it as a **softcore** award — a perfectly reasonable way to implement "this client may
not earn hardcore" — then the account now holds that achievement in softcore. `raWifiUnlocks()` asks
with `h=0` and always has, because the softcore list is the right skip list for a softcore fork. So
on the next boot that id is in the skip list, the definition is left out of the staged block, and it
**can never be earned in hardcore** without touching the account.

Which is to say: the danger is not the refusal. It is the silent acceptance.

### And the first boot with it on is safe, which is a later boot than it sounds

The queue on the boot where `hardcore=1` is first set holds records from earlier sessions. Those
carry no mode field, read as softcore, and go out with `h=0`. Nothing about them is affected.

The exposure needs a hardcore session to have queued something first, and then a *later* sync boot to
send it. There is a whole play session between switching it on and anything being at stake.

### Not guarded against, deliberately

A guard would have to tell "refused because this client may not claim hardcore" from "refused because
the id is wrong", and the only thing that could is the server's `Error` string, which nobody here has
read. A matcher written against a message never seen is the kind of guess this document has a section
about. The silent-acceptance case is worse still: there is nothing in the reply to match on, which is
the entire problem.

So it is said where a player reads it, in `tools/ra.example.cfg`, and the knobs are named for what
each one actually does:

| Setting | What it buys | What it costs |
| --- | --- | --- |
| `sync=0` | radio never comes up, nothing sent or cleared | no set is fetched — definitions come from the per-game cache, so a ROM with no cache fires nothing, and the pending tally is never staged |
| `submit=1`, `submit=0` | full ladder, set fetched, queue read and reported | `submit=0` neither sends nor clears, which is the knob that addresses this hazard — `sync=0` only does so as a side effect of turning everything off |
| everything at `1` | the answer | one achievement possibly held softcore for good |

**The last row is worth choosing on purpose.** What the server does with `h=1` from an unrecognised
client is the open question this whole area waits on, and running it is the only way anyone finds
out. Do it on a game that does not matter, with an achievement that is cheap to reach, and read the
award reply body in the log before concluding anything — it is already logged in full, for the
separate reason that `Success:true` and "the account holds it" turned out not to be the same
statement. Do not discover it by accident on a set you care about.

### What this still does *not* settle

The mode is now recorded honestly, but **whether a deferred unlock may be submitted at all** is
still RetroAchievements' call, not ours — see "What is left". If the answer is no, the queue's
design is what needs revisiting and this field goes with it. It was built ahead of that answer
because it is correct under either one: a record must not be upgraded after the fact regardless of
what the rules say about when it may be sent.

## The reader spends too much of the game's VBlank, and two games say so

### The outcome first, because the sections below are the archaeology

*Chrono Trigger* went from **unplayable to playable** on this fork. It froze on entering Leene
Square — always, at the same screen, with no menu involved — and it now passes it and continues into
the story. The world-map tearing that came with it is down to what the official release does, and
the character-text region that trembled with Crono low on screen is legible. That is a compatibility
gain on a game this fork previously could not run, and it was won by measurement rather than by
guessing: every step below is a number read off a 3DS with the RAM viewer.

**What it cost:** one setting, self-tuning, and a documented reduction in how often a trigger is
looked at. Nothing was disabled, no achievement is lost, and no other game changed.

**What was learned, in the order it was learned, and three of the five were wrong:**

| Step | Claim | Verdict |
| --- | --- | --- |
| 1 | The reader overruns the game's blanking period | **right** — and `raRearmVBlank()` returning early removed both symptoms with the binary still staged, so memory was exonerated and execution was the cause |
| 2 | Throttle it: skip frames to pay back an overrun | **half right** — cured the freeze, left the tearing. Skipping lowers the average and leaves the peak, and a tear is a one-frame event |
| 3 | Then the peak has to fit: `rcLinesMax` 101 against `rcRoomMax` 70 | **right, and it ended the whole approach** — 70 of a possible 71 means the game leaves nearly all its blanking and the work still needs 40% more than the console has |
| 4 | So divide the set: evaluate the trigger loop in slices | **right, and it is the fix** — `rcLines` 58 of 70, and the picture came back |
| 5 | The remaining cost is 845 cycles per memory read, so the window must be executing uncached | **wrong twice over** — `raMpuBits` 0xD9 said the window was already both instruction- and data-cached, and `rcMemrefMin` 45 of 47 said the bus was not stalling either |
| 6 | ...and the 845 was a bad division | **the real answer** — the memref pass walks 3.5× more entries than it reads, most of them arithmetic rather than memory, and per *entry* the cost is an ordinary 239 cycles |

Step 6 is why this stops here rather than continuing. There is no anomaly and therefore no large win
waiting: the reader is doing a great deal of work at a normal speed, and the only lever is to do less
of it per frame, which is exactly what step 4 does.

**Three things a future reader should take from this rather than from any one section:**

- **The cost of a set is its memrefs, not its achievements.** Contra 4's 45 definitions read 69
  addresses and have never torn on any build, in any test. Chrono Trigger's 98 read 237 and broke the
  game. A hundred simple definitions are cheaper than fifty full of `AddSource` arithmetic.
- **A snapshot field that is a running maximum can describe a configuration that no longer exists.**
  `rcLinesMax` said 100 for a whole session after the tuner had brought the real cost to 58, and it
  was read as evidence of failure once. `rcLines` was the field that was telling the truth.
- **A short negative on a probabilistic failure is not a negative** — the lesson the menu-freeze
  bisect below cost, and the reason every verdict in the table above is a number rather than an
  impression.

**Confirmed.** Two symptoms, one cause, and it is ours.

*Chrono Trigger* freezes on entering Leene Square — always, at the same screen, with no menu
involved, whether you talk to NPCs, play the strength minigame or walk straight there. And the same
build tears visibly on the world map.

Both disappear when `raRearmVBlank()` is made to return immediately. **In that build the binary is
still staged**, so the memory accounting is identical and only the execution is gone. Memory is
exonerated; the per-frame work is the cause.

### The three measurements that pin it

| Build | ROM cache | Reader running | Leene Square | World map |
| --- | --- | --- | --- | --- |
| Official release | 512 KB | no | passes | no tearing |
| Ours, colour LUT on | 205 KB | no | passes | no tearing |
| Ours | 256 KB | **yes** | freezes | **tears** |
| Ours, VBlank hook off | 256 KB | no | passes | no tearing |

The cache column is what kills the memory explanations. **A smaller cache passes and a larger one
freezes** — 205 KB is fine, 256 KB is not. That is not a size effect, and it took out both
`dsiWramCacheSize()` and the `isROMLoadableInRAM()` threshold in one reading. The window is not
oversized either: rcheevos links to 68 KB and its runtime arena measures 91.8% full on a real set,
128,352 bytes of 139,756, with 11 KB to spare. There is nothing to reclaim there.

What is left correlates perfectly with the reader executing.

### Why VBlank is the expensive place to be

`raVblankHandler` chains to the game's own handler first and runs our work after it, so the game's
VBlank work is not delayed by us — but the interrupt is still open. Everything the game does *after*
that interrupt returns is pushed later by however long we took, and VBlank is a few dozen scanlines.
Spill past it and the game's next display work lands inside the visible frame. That is tearing.

Where the game has slack it is ugly. Where it does not, it is fatal, and Leene Square is where
*Chrono Trigger* has none.

**The cost scales with the set.** Contra 4 has 45 definitions and has never torn, on any build, in
any test. That is the same axis the init path already respects: `RA_RC_INIT_BUDGET_LINES` is 120 of
263 scanlines, chosen because the most expensive single definition measured 27.

### And the steady-state frame has no budget at all

The init path is budgeted. The per-frame path measures and does not cap:

```c
startLine = RA_VCOUNT;
ra_rc_step(ra_rc_frame_step, snapshot);
lines = (RA_VCOUNT - startLine + RA_SCANLINES_PER_FRAME) % RA_SCANLINES_PER_FRAME;
```

`lines` and `linesMax` go into the snapshot and nothing acts on them. A set large enough to cost more
than the blanking period costs it every frame, forever.

### Running less often is already known to be acceptable

This is the part that makes a throttle cheap rather than a compromise. Before the hook moved to
VBlank, it chained onto VCOUNT with a Y-trigger, and *Contra 4* cleared `DISP_YTRIGGER_IRQ`
constantly: measured at the time, **the reader ran on about 8% of frames and achievements still
fired correctly**. Evaluation at a fraction of the frame rate is not a new risk here — it is what
this project shipped for a while without noticing a missed unlock.

What it does cost is latency on a notification, measured in tens of milliseconds, which nobody can
perceive.

### The fix: the frame budget is what is left of blanking, measured

The steady-state frame is capped the way init already is, and **the budget is not a constant**. It
is how much blanking is actually left when the evaluation starts:

```c
startLine = RA_VCOUNT;
room = (startLine >= RA_VBLANK_FIRST_LINE) ? RA_SCANLINES_PER_FRAME - startLine : 0;
```

That matters because the reader is chained *after* the game's own VBlank handler, so what remains
depends on what the game just did. Measuring the room rather than assuming it is what makes this
right on a game nobody has tested — which, given the two games that exposed this, is the case that
counts.

`ra_rc_frame_skip(lines, room)` then says how many frames to sit out so the average lands inside that
room. A set costing less than the room is never throttled at all, which is why Contra 4 is
unaffected: 28–31 against 71.

**Whole frames are skipped, never a partial set.** `rc_runtime_do_frame()` updates every memref and
evaluates every trigger in one call, so half a set on this frame and half on the next would hand the
delta operators two different notions of "previous". Skipping is only a lower sample rate, and the
8% figure above is this project's own evidence that it is survivable.

Three things the writing of it turned up:

- **A cost of zero must never throttle**, whatever the room says. Work that consumed no measurable
  scanline cannot have overrun anything. This is not a nicety: on a host `RA_VCOUNT` never advances,
  so every frame measures free, and the first version — which throttled to the floor whenever `room`
  was zero — silently stopped the suite's own frame ticks from evaluating. One test caught it.
- **The decision is a pure function**, for the reason `ra_queue.c` is pure: the arithmetic is the part
  with the logic and the part a host can check, while the thing it depends on is the one thing a host
  does not have. The suite drives `ra_rc_frame_skip()` directly.
- **`.bss` needs no ceremony after all.** An earlier note here warned that a skip counter would have
  to be initialised by hand. It does not: `ra_startup()` zeroes this binary's `.bss` on its first
  call, which is the same guarantee `stateMagic` already depends on in `cardengine.c`.

The floor is `RA_RC_FRAME_SKIP_MAX = 3` — evaluation at worst every fourth frame, about 15 Hz —
bounding the throttle rather than the cost so a pathological reading cannot stall detection for
seconds.

### Hardware: half of it worked, and the half that did not was predictable

| Symptom | Before the throttle | After |
| --- | --- | --- |
| Leene Square | freezes, every time | **passable** — play continues past it and further into the story |
| World map | tears | **still tears** |
| Character text region, Crono low on screen | trembles | still trembles |

The freeze is gone and the visual artefacts are not, and that split is not bad luck — it follows from
what a reactive throttle is. **Skipping frames lowers the average cost and leaves the peak exactly
where it was.** The frame that does run still spills past line 262 by however much it always did; the
only thing that changed is how often. Cumulative starvation is an average problem, so Leene Square
was cured. A tear is a single-frame problem, so it was not: it went from every frame to one frame in
four, and one frame in four is what a visible periodic wobble is made of.

The trembling text region deserves its own line because it was reported as **pre-existing on the RA
build** rather than introduced here, and it is the same shape as the tearing: *Chrono Trigger* draws
character text two different ways depending on where the main character stands, and the placement
that trembles is the one whose split lands late. A raster split arriving on the wrong scanline is
what a late interrupt looks like.

### The gate: decide before the work, not after it

`ra_rc_frame_skip()` answers "how much do I owe for what I just did". The question it cannot answer is
"should I have done it", and that is the one the peak needs. `ra_rc_frame_fits()` is asked first:

```c
static u8 ra_rc_frame_fits(u8 cost, u8 room, u8 declined) {
	if (cost == 0)                              return 1;
	if (declined >= RA_RC_FRAME_STARVE_MAX)     return 1;
	return (room >= cost) ? 1 : 0;
}
```

`room` is measured this frame, as before. `cost` is **the last evaluation's cost, not `linesMax`** —
and that choice is the one that keeps this from starving. `linesMax` is raised for good by a single
expensive frame, such as the one where a trigger fires and its events are delivered, so predicting
from a high-water mark would decline every ordinary frame for the rest of the session.

Both escapes are load-bearing:

- **A cost of zero always runs**, for the same reason it never throttles: nothing has been measured
  yet, or the work is free. This is also the case the host suite runs in, where `RA_VCOUNT` never
  advances and both inputs are zero forever.
- **Starvation always runs.** `RA_RC_FRAME_STARVE_MAX = 16`, so a game whose own handler never leaves
  room still gets an evaluation about every seventeenth frame — 3.7 Hz at worst, against the 8% of
  frames the old VCOUNT hook shipped on without missing an unlock. A reader that never evaluates is a
  worse bug than a visible one, and the floor is where that gets decided rather than left to a game.

The reactive throttle stays alongside it. The gate stops the overruns nobody had to take; the throttle
pays back the ones starvation forced.

### `rcRoomMax` is the number that says whether this can work at all

One byte, at `+0x87`, taking the reserved byte that has been sitting there since the struct was
written — so no offset moves and the hardware checklist stays valid. It is the most blanking the
reader has ever found still unspent when its turn came, and it is read **against `rcLinesMax`**:

- `rcRoomMax` comfortably above `rcLinesMax` — there are frames with room to spare, and declining
  the rest costs nothing but sample rate. The gate is the whole fix.
- `rcRoomMax` at or below `rcLinesMax` — no frame in the session could hold the work. Then only
  starvation-forced runs ever happen, the artefacts thin out by 16× rather than vanishing, and the
  thing left to attack is the cost itself rather than the schedule.
- `rcRoomMax` **zero** — every evaluation began outside the blanking period, meaning the game's own
  VBlank handler had already run past line 262 before ours started. That is the worst reading
  available here, and it is a reading rather than a missing measurement.

Which of the three *Chrono Trigger* gives is the next thing worth knowing, and the RAM viewer can
answer it directly: `rcLinesMax` at `+0x85`, `rcRoomMax` at `+0x87`, adjacent bytes in the snapshot.

### Hardware: `rcLinesMax` 101, `rcRoomMax` 70 — and that ends the scheduling approach

Read off the snapshot on *Chrono Trigger*'s world map, at `0x027FEA85` and `0x027FEA87`:

| Field | Offset | Value |
| --- | --- | --- |
| `rcLinesMax` | `+0x85` | **101** of 263 |
| `rcRoomMax` | `+0x87` | **70** of 71 |
| `rcActivated` | `+0x99` | 98 definitions |
| `rcInitLines` | `+0x6F` | 249 — the slowest single activation |
| `rcInitTotal` | `+0x9E` | 3,487 scanlines, 13 frames to parse the set |

**70 of a possible 71.** The blanking period is 71 scanlines and the reader found 70 of them
unspent, so the game's own VBlank handler returns almost immediately and leaves the reader
essentially all of it. There was no room being stolen and none to reclaim: the third bullet above
is the reading, and it is the bad one.

The work needs **101 lines against a hardware ceiling of 71** — 40% more than the console has, on
the reader's *best* frame. That is not a schedule that was chosen badly. No gate, no throttle and no
skip counter can place 101 lines inside 71, because the budget is not a policy: it is how many
scanlines a Nintendo DS frame spends not drawing. Everything above this section was solving the
wrong problem correctly.

And it is not a *Chrono Trigger* quirk. Contra 4's 45 definitions cost 67 lines and this set's 98
cost 101, so the cost tracks the set size, and any set past roughly seventy definitions is over the
ceiling on any game. Contra 4 has never torn on any build because it is the last size that fits.

### The fix: divide the set, not the calendar

`rc_runtime_do_frame()` is a memref pass followed by a loop over every trigger. The loop is now
divided:

```c
if (parts > 1 && ((u32)i % parts) != slice)
	continue;
```

**Reimplemented in `ra_rcheevos.c` as `ra_rc_do_frame_slice()`, not patched into rcheevos** — and the
first version of this was patched into rcheevos, which was wrong for a structural reason worth
recording. **rcheevos is a submodule**, pinned at RetroAchievements' own v12.4.0 (`2ad0b867`). The
parent repository records a gitlink, not the files, so an edit to the submodule's working tree
belongs to no commit this project can make: it builds on the machine that made it and is absent from
every fresh clone. The host suite would not have caught it and the release script would not have
caught it; the only thing that did was the working tree refusing to come clean.

What is lost by not calling upstream's function is the part this fork has no use for. Its loop raises
nine event types and `ra_rc_event_handler()` acts on exactly one, `ACHIEVEMENT_TRIGGERED`, counting
the rest; there are no leaderboards and no rich presence here, so those two loops iterate zero times.
What is kept is everything that changes behaviour — memrefs updated first, triggers skipped when null
or holding an invalid memref, `RESET` read back off the trigger rather than treated as a state, and
events raised only on a real transition.

One narrowing, written down because it changes a snapshot field's meaning: `rcEvents` now counts
trigger **state transitions** rather than upstream's nine event kinds.

Modulo rather than a contiguous range, so a change of `parts` mid-session cannot leave a band of
triggers unvisited for a whole cycle.

**Memrefs are updated on every call, and only the trigger loop is divided.** That is the half that
decides whether this is honest. Each trigger still sees a true one-frame delta whenever it is looked
at, rather than a stale one from its own last visit — which is more than whole-frame skipping ever
gave it, since skipping updates no memrefs at all on the frames it sits out. What the division costs
is sample rate: a trigger is evaluated every `parts` frames. That is the cost this project already
measured as survivable, when the old VCOUNT hook ran on 8% of frames without missing an unlock.

**An earlier note here rejected this, and the rejection was wrong.** It said splitting a set across
frames "would hand the delta operators two different notions of previous". Updating every memref
every frame is what makes that false — per trigger, the split is a lower sample rate and nothing
else, which is exactly what the throttle already was. The difference is that the split bounds the
peak and the throttle only bounded the average.

### `rcParts` tunes itself, and reports the one thing left to measure

```c
static u8 ra_rc_frame_parts(u8 parts, u8 cost, u8 room) {
	if (parts == 0)          parts = 1;
	if (cost == 0 || room == 0) return parts;
	if (cost > room && parts < RA_RC_PARTS_MAX) return (u8)(parts + 1);
	return parts;
}
```

**It only ever rises,** and not for want of ambition. A step down would have to predict what a
*larger* slice costs, and the cost is not proportional to the slice: `rc_update_memref_values()`
runs on every call whatever the slice, so a fixed share of every measurement belongs to work that
dividing cannot reduce. Guessing that share wrong in the optimistic direction is precisely the
oscillation this exists to end. Rising only is monotone, converges in at most seven frames, and a
spurious step costs sample rate rather than correctness.

`linesLastRun` is thrown away on a change of `parts` rather than scaled, for the same reason: what a
smaller slice will cost is the thing that cannot be predicted from a larger one. Zero already means
"not measured, so run" everywhere in this file, so the next frame is a measurement instead of a
refusal, and convergence is one frame per step.

**And it removes a hardware round-trip.** The open question was how much of the 101 lines is the
memref pass, since that share cannot be divided — and `rcParts` at `+0x69` answers it without
anyone measuring it separately:

- **1** — nothing was divided; the set fits whatever the game leaves. Every game before this one.
- **2 to 7** — the reader found a division that fits. The memref share is small and the fix holds.
- **8 (`RA_RC_PARTS_MAX`) with `rcLinesMax` still above `rcRoomMax`** — the memref pass alone does
  not fit, no division of the triggers ever will, and the next thing to attack is the peek path.

Eight is the floor on sample rate rather than a guess: a trigger visited every eighth frame is
7.5 Hz, against the one-in-twelve this project shipped on without missing an unlock.

### Hardware: it fits now, and `rcLinesMax` is the field that lies about it

Two readings, world map and Leene Square, same session:

| Field | Offset | World map | Leene Square |
| --- | --- | --- | --- |
| `rcParts` | `+0x69` | **8** | 8 |
| `rcLines` | `+0x84` | **58** | 60 |
| `rcLinesMax` | `+0x85` | 100 | 100 |
| `rcRoomMax` | `+0x87` | 70 | 70 |
| `rcPeeks` | `+0x7C` | 237 | 237 |

On screen: the world-map tearing down to roughly what the official release does, and the text region
trembling slightly but legible.

**`rcLines` 58 against `rcRoomMax` 70 is the reading that matters, and it fits.** `rcLinesMax` still
says 100 because it is a running maximum that is never reset — so it is reporting a frame from before
the tuner converged, when the set was still being evaluated whole. Against the interpretation written
one section above, "pinned at `RA_RC_PARTS_MAX` with `rcLinesMax` still above `rcRoomMax`" is
therefore *not* the reading it claimed to be: the ceiling is reached, and the work fits anyway.
`rcLinesMax` describes a configuration that is no longer in force.

### What the numbers say the cost is made of

`rcParts` 8 costing 58, and the undivided set costing 101, give the split:

```
fixed + variable/1 = 101      ->  variable ≈ 48   (the trigger loop, divisible)
fixed + variable/8 =  58      ->  fixed    ≈ 53   (the memref pass, not divisible)
```

**Three quarters of the budget is the fixed term.** `rc_update_memref_values()` runs on every call
whatever the slice, so 53 of the 70 available scanlines are spent before a single trigger is looked
at, and dividing the trigger list can only ever recover the other 48. That is why `rcParts` 8 wins so
little over `rcParts` 4, and it is the ceiling on this whole approach.

It also gives a figure that looked alarming and turned out to be a bad division: 237 peeks in ~53
scanlines reads as ~950 ARM9 cycles per memory read, and the section on the memref pass below shows
why that denominator is the wrong one — the pass walks about 3.5× more entries than it reads — a translate, a range check and a load. Nothing in `ra_rc_peek()`,
`ra_readable()` or `ra_read()` accounts for two orders of magnitude more than that costs. The same
ratio shows up in the trigger loop: 48 scanlines for 98 definitions is ~2,000 cycles each. Both
halves are uniformly slow by a similar factor, which points at how the code executes rather than at
what it does — this binary runs from DSi WRAM at `0x03740000`, a region a DS game's own MPU setup has
no reason to have marked cacheable. **That is the next lever, and it is a much bigger one than
slicing:** an instruction cache over a region we load once and never write is a safe change, and a
3–5× cut would take the whole per-frame cost to about 25 scanlines and end this section.

`rcMemrefLines` at `+0x25` measures the fixed term directly rather than inferring it from two
whole-frame readings on two different games. It takes one of the three bytes reserved there, because
the bytes next to `rcLinesMax` are spent; no existing offset moves.

**The parts tuner is left exactly as it is on purpose.** It is a ratchet — it converges during the
noisiest frames of the session, which is boot, where `rcInitTotal` says the parse alone spans 13
frames, and it can never come back down. Tuning against `rcRoomMax` instead of the frame's own room
would settle *Chrono Trigger* at 3 by the arithmetic above, which is 69 scanlines against 70: the
honest optimum and no margin at all. `rcParts` 8 costs 58 and has eleven lines to spare, and what it
buys with them — a picture at the level of the official release — is worth more than the difference
between visiting a trigger at 7.5 Hz and at 20 Hz. A confirmed-good result is not worth trading for
a computed one. The ratchet gets revisited when the fixed term comes down, because that is when the
numbers change.

### Hardware: `rcMemrefLines` = 47, and that closes the slicing question

Read on both screens of the same session, identical in each: `rcMemrefLines` **47**, `rcParts` 8,
`rcLines` 58, `rcRoomMax` 70.

The inference above said ~53 from two whole-frame readings on two different games; measured directly
it is 47. Close enough to leave the conclusion standing and precise enough to end the argument:

| Term | Scanlines | Divisible |
| --- | --- | --- |
| memref pass | **47** | no — runs on every call, whatever the slice |
| trigger loop, whole | ~50 | yes |
| in force at `rcParts` 8 | **58** of 70 | — |

**67% of the budget cannot be divided.** `rcParts` 8 already spends only 11 scanlines on triggers, so
doubling to 16 would save five and halve the detection rate for them. There is nothing left in
slicing. The approach is finished, it worked, and this is where it stops.

What that costs, stated plainly rather than left implicit: a trigger is visited every eighth frame,
so a hit-count condition counts eight times slower and a transient state a `ResetIf` or `PauseIf`
wanted to see can pass between visits. Nothing is lost from the queue and no unlock is dropped — the
precedent for a low sample rate is this project's own 8% of frames — but "hold this for 300 frames"
becomes 2,400, and that is a real behavioural change rather than a free win.

Which is the argument for the cache lead, and it is now the only lever left with a large number
behind it: 47 scanlines for 237 reads is ~840 ARM9 cycles each. If executing this binary cached takes
that down by 3–5×, the whole per-frame cost lands near 15 scanlines, `rcParts` goes back to 1, and
every caveat in the paragraph above disappears with it.

## The reader may be executing uncached, and one bit would say so

Two unrelated halves of the work being slow by the same factor is not a property of what the code
does. It is a property of how it runs. This binary executes from DSi WRAM at `0x03740000`, and a DS
game's own MPU setup has no reason to have marked that region cacheable — on a retail DS there is
nothing there at all.

### Instruction cache only, and the asymmetry is the whole safety argument

Cacheability on the ARM946 is per MPU region, and the region that happens to cover `0x03740000` may
cover a great deal more: the I/O registers at `0x04000000`, and the ROM cache immediately below us at
`0x03700000` which the card DMAs into. **A data cache over either of those is fatal** — `VCOUNT`
would stop advancing, and cached ROM would go stale under the DMA that filled it.

An *instruction* cache over exactly the same region is inert. Nothing is ever fetched for execution
from an I/O register or from a ROM cache line. Only code is fetched, and the only code in this region
is ours: written once by the bootloader before we ever run, and never modified afterwards. So the
instruction bit can be set on a region that the data bit must never touch, and that is why this
change is available at all.

### And it declines the one region it must not touch

If the winning region for our address also spans `0x02000000`, it governs how the **game's** code is
fetched. nds-bootstrap's own MPU patch can produce exactly that — it widens a region to `PAGE_128M`
at base 0 — and a loader that DMAs overlays into main RAM is the last program that should have an
instruction cache switched on underneath it. In practice such a region already has the bit set,
because no DS game gives up its instruction cache. Either way the decision belongs to a measurement,
so `ra_icache_claim()` reports that case and leaves it alone.

### `raMpuBits`, at `+0x26`

One byte, taking a reserved one so no offset moves, and it is a report rather than a boolean because
there are four ways this comes back and three of them mean something different has to be tried:

| Bit | Meaning |
| --- | --- |
| 0–2 | the winning MPU region for `0x03740000` |
| `0x08` | that region also covers main RAM, so it was left alone |
| `0x10` | the region was **already** instruction-cacheable |
| `0x20` | it was not, and this turned it on |
| `0x40` | the instruction cache is enabled globally in CP15 `c1` |
| `0x80` | the region is data-cacheable too — informational |
| `0xFF` | no enabled region covers the window at all |

**Read bit `0x40` first.** A per-region cacheability bit does nothing while the cache itself is off,
so a clear bit 6 makes every other bit here moot and sends the whole diagnosis somewhere else.

The two decisions are pure functions — `ra_mpu_region_pick()` over the eight raw region registers and
`ra_mpu_report()` over the three CP15 words — for the reason `ra_rc_frame_skip()` is one: the
arithmetic is the part with the logic and the coprocessor read is the part a host does not have. The
suite drives both directly, including the case that would have been a real bug: a size field of 31 is
the whole address space, and `1u << 32` is undefined.

### Hardware: `raMpuBits` = 0xD9. The cache was never off.

| Bit | Value | Meaning |
| --- | --- | --- |
| 0–2 | 1 | region 1 wins for `0x03740000` |
| `0x08` | set | it also covers main RAM, so it was left alone |
| `0x10` | **set** | it was **already** instruction-cacheable |
| `0x20` | clear | nothing was changed |
| `0x40` | set | the instruction cache is enabled globally |
| `0x80` | set | the region is data-cacheable too |

`rcMemrefLines` still 47, which is exactly right: nothing was changed, so nothing moved.

**The hypothesis is dead, and cleanly.** The window is not merely instruction-cached — it is
data-cached as well, because nds-bootstrap's own MPU patch widens region 1 to reach from main RAM
through `0x03740000`, and that region is fully cacheable. There was never an uncached window to fix.
The ~845 cycles per read is what this code costs *with* both caches on.

`ra_icache_claim()` stays. It reports, it declines the dangerous case, and on a game whose MPU puts
our window in a region of its own with the bit clear it would still do something. On the measured
configuration it does nothing at all, which is the useful thing to have written down.

### So the cost is the read itself, and two games now agree on the rate

The host measures what the arena measurement never asked: **69 reads** for Contra 4's 56-definition
set, all of them in the memref pass and *none* in trigger evaluation. Put beside the two hardware
timings, one rate explains both:

| | reads | rate | memref pass | remainder |
| --- | --- | --- | --- | --- |
| Chrono Trigger | 237 | 845 cyc/read | **47 lines** (measured) | — |
| Contra 4 | 69 | 845 cyc/read | 13.7 lines | 53 lines for 1,946 conditions = **117 cyc each** |

117 cycles per condition is a sane figure for evaluating a condition. 845 cycles for a translate, two
range checks and a load is not — and `ra_rc_translate()` is four instructions, `ra_readable()` two
comparisons, `ra_read()` a compare and a load. The code is not where it goes.

One candidate was the bus: scattered main RAM addresses are cache line fills on the external bus,
taken from inside VBlank where a DS game's DMA channels are moving VRAM and OAM and hold that bus.

### `rcMemrefMin` 45 against `rcMemrefLines` 47 kills that, and the denominator was wrong

Stable to 4% over a play session. Bus contention varies with what the game is transferring;
deterministic arithmetic does not. **It is fixed work.**

And then the host counted the thing the whole calculation had assumed. For Contra 4's set:

```
memref list    68 entries, 68 typed, 173 modified
memref pass    69 reads
```

**The pass walks 241 entries and reads 69 of them.** `rc_update_memref_values()` has two loops, and
only the first does reads. The second walks `modified_memrefs` and calls
`rc_get_modified_memref_value()`, which does no memory access at all in the common case — it
evaluates two operands, converts types and combines them. That is where `AddSource`, `SubSource`,
`AddAddress` and the arithmetic operators live.

On the ARM9 that arithmetic is not cheap. There is **no integer divide instruction**, so an operator
of `/` or `%` is a libgcc call of a few hundred cycles; `rc_typed_value_t` carries a float variant, and
there is no FPU either, so any definition using a float promotes the whole combine into soft-float.

Redone with the real denominator, scaling Contra 4's ratio onto Chrono Trigger's 237 reads:

| | entries walked | reads | cycles per entry |
| --- | --- | --- | --- |
| Contra 4 | 241 | 69 | — |
| Chrono Trigger | ~839 | 237 | 47 lines → **~239** |

**239 cycles for two operand evaluations, a type conversion and a combine is a normal number.** There
was never a 20× anomaly. The reader is doing a great deal of work at an ordinary rate, and the whole
"845 cycles per read" figure was an artefact of dividing the pass's cost by the wrong count — reads
instead of entries.

### Which closes the investigation, and makes slicing the fix rather than a workaround

Three hypotheses, all dead, and each died to a measurement rather than an argument: the window was
never uncached (`raMpuBits` 0xD9), the bus is not the bottleneck (`rcMemrefMin` 45 of 47), and the
per-entry cost is not anomalous (239 cycles for what that entry does). There is no large win sitting
here. The cost is the work.

So there is nothing left to make cheaper, only less of it to do per frame — which is what `rcParts`
already does. Note precisely what it does and does not reach: slicing divides the **trigger loop**,
which is why the measured total is 47 fixed plus 11 for triggers. The dominant term is untouched by
it, because every memref and every modified memref is updated on every call.

**And slicing the memref pass as well is not available, for a reason worth writing down.** Whole-frame
skipping was uniform: everything went stale by the same amount, so a trigger comparing two addresses
still compared two values sampled at the same instant. Per-memref slicing is not uniform. A trigger
in slice *k* would read some addresses updated this frame and others up to eight frames old, and a
condition comparing two of them would be comparing different moments. That is a correctness change,
not a sample-rate change, and it is the line this project has not crossed anywhere else.

The number to watch for a future set is therefore **not the achievement count** — it is the count of
memrefs and modified memrefs, which is what `rcPeeks` and the host's list walk report. A set of a
hundred simple definitions is cheaper than fifty full of `AddSource` arithmetic.

## The overlay was not the cause, and the log said so in one line

`overlay=0`, confirmed in the log — `on-screen popup off (overlay=0)` — and **Ketsui froze at the same
boss anyway**. Four rounds of work on the overlay were four rounds in the wrong place.

That is worth recording as a method failure rather than only a wrong guess. The overlay was suspected
because it is the part that draws, the symptom was visual, and it *did* have real bugs — the survey
counting a parked sprite bank as free is genuine and worth having fixed. But **the freeze was never
tested against it** until a switch existed to test it with, and building that switch took one commit
against four spent patching.

### What is left, and the queue file pointed at it

The queue came back **empty**. The unlock never reached the file. With the popup off, what remains on
the frame an achievement fires is:

```
rcheevos fires -> ra_rc_queue_unlock() -> ring -> ra_rc_offer_unlock() -> sharedAddr
              -> the ARM7's FIFO handler -> raUnlockAppend() -> fileRead x N -> fileWrite
```

That last step is **a blocking SD transaction on the ARM7, while the game is streaming from the same
card.** This project has already watched that contention corrupt this very file, twice, with the
game's own data. A transaction that stops the ARM7 from servicing the game is a plausible way to stop
the game dead, and the empty file is consistent with hanging *inside* it rather than before it.

### `queue=0` is the discriminator

It stops the handoff in `ra_rc_offer_unlock()` — one step before the FIFO request — so nothing
downstream runs at all: no request, no file read on the ARM7, no sector write. Detection, counting and
the session's own list all keep working, so the snapshot still says an achievement fired.

**Gated on the ARM9 side, and that is forced rather than chosen.** The session block lives in DSi
WRAM, which is the ARM9's, so the CPU that does the writing cannot read the switch.

What it costs is the thing that makes it a diagnostic rather than a setting: with `queue=0` an unlock
does not survive the session, because the file is the only thing that does.

## `queue=0` answered it: the freeze is the queue write, and IME off was never enough

`overlay=0` **and** `queue=0`, same game, same boss. **It did not hang.** One switch, one boot, one
answer — after four rounds of patching a part that turned out to be innocent.

So the freeze is the append, and the append is now diagnosed rather than suspected.

### What IME off protects, and what it does not

The critical section around `raUnlockAppend()` came with an argument, written down at the time:

> What does matter is that a competing read cannot *start* underneath this write, and on this CPU
> reads are driven from interrupt handlers, which IME off prevents.

That is true. It is also insufficient, and the gap is the whole bug: **a card read does not have to
start underneath the write to be ruined by it. It can already be half done.**

The ROM read path on the ARM7 is resumable, and deliberately so — a multi-megabyte read cannot block
a VBlank handler:

```
start_cardRead_arm9()  ->  fileReadNonBLocking()   issues an sdmmc command, returns false
                           readOngoing = true      the read now exists only as hardware state
        ... frames ...
resume_cardRead_arm9() ->  resumeFileRead()        CARD_CheckCommand(context.cmd): done yet?
```

Between those two moments the game's read is one outstanding command on the one SD controller, plus
512 bytes of `my_fat.c`'s single `globalBuffer`. IME off does not protect either. A VBlank landing in
that window ran the append straight through it: new commands on the same controller, the same buffer
rewritten.

### Every symptom, in both directions at once

This is the part that makes the diagnosis worth trusting — it does not explain the freeze and leave
the rest, it explains all of it, including the two things that never fitted the overlay:

| reported | what this predicts |
| --- | --- |
| Ketsui froze on the boss kill, twice, with `overlay=0` | the game's read never completed; the ARM9 spins in its card-read stub forever |
| `ra_unlocks.txt` held 464 bytes of Ketsui's credits text | our record's read-modify-write merged over the game's sector in the shared buffer |
| the queue came back **empty** on the last freeze | the hang is *inside* the append, before the write lands |
| it only ever happened at a scene transition | that is when the game streams, so that is when a read is outstanding |
| `queue=0` removed it | the append is the only thing that switch removes |

The `fileWrite()` sector-cache fix earlier in this document was the *same collision seen from the
other side*, and fixing it made the record come out clean while leaving the freeze — which at the time
looked like two bugs. It was one.

### The fix: wait for the card, on two sides, for two different reasons

**ARM7 — correctness.** `raUnlockService()` returns without touching the card while `readOngoing` is
set, leaving the request word standing so the next frame retries. It can only live here: the flag is
that CPU's own state and nothing else can see it.

**ARM9 — courtesy.** `ra_rc_offer_unlock()` does not raise the request at all while a card read of its
own is unserved (`raCardReadPending(shared[3])`). Not a corruption hazard — an unserved request cannot
be half done — but the ARM7 answers it from the VBlank handler, which cannot run while the append
holds IME off, so raising it now means the game waits for nothing. The unlock stays in the ring and
goes out on a later frame.

Waiting cannot become never: both are states this client clears itself, within a frame or two, and
every game idles between reads. Ketsui idles for the whole of the scene that used to hang.

### Why the split is where it is: four bytes

The obvious shape is one predicate on the ARM7 testing both words. It does not fit.
`cardenginei_arm7` for TWL-SDK games links into 33K with **60 bytes spare**, and `.bss` follows
`.text` in its linker script, so text growth overflows the region rather than merely tightening it.
Every arrangement was measured against it (text of the translation unit, baseline 7,737):

| arrangement | text | Δ | |
| --- | --- | --- | --- |
| one bool test added to the condition, in place | 7,829 | +92 | |
| the two-word predicate added in place | 7,905 | +168 | |
| lifted to a function, gcc inlined it back | 7,817 | +80 | |
| lifted, `noinline`, both halves of the predicate | 7,793 | +56 | **`.bss` 4 bytes past the region** |
| lifted, `noinline`, `readOngoing` only | 7,753 | **+16** | ships |

Two things came out of that table and neither was a guess. **Ninety-two bytes for one bool test** is
register pressure across an 1,860-byte straight-line function, not the cost of the test; lifting the
block into its own `noinline` function dropped `myIrqHandlerVBlank` to 1,260 and put 612 bytes in
`raUnlockService()`, and the test then cost what a test costs. And the second half of the wait runs on
the ARM9 because on the ARM7 it was four bytes too expensive — the ARM9 has room, already holds
`shared` in a register, and is the side that decides when to offer.

`raCardReadPending()` lives in `ra.h`, where both sides see it and where the host suite can pin its
truth table — the one part of this that can be checked without a card.

### `queue=1` again

The switch stays, as a diagnostic rather than a setting: with `queue=0` an unlock does not survive the
session, because the file is the only thing that does. It earned its keep in one boot and it is worth
keeping for the next time something on the frame an achievement fires needs to be ruled in or out.

### Superseded — this fix was correct and was not the cause

Kept as written because the reasoning about the resumable read is sound and the gate stays in the
tree. But it did **not** stop the freeze, and the section that follows explains why the argument was
looking in the wrong place: there is no IRQ nesting on this ARM7, so the append can never land inside
a blocking card read, and the record the next frozen session wrote came back complete. The window
this closes is real. It was not the one that matters.

### What is not claimed

That this was the *only* way the append could hurt a game. The stall is still real — an ARM9 read
request waits a few milliseconds on the frame an achievement unlocks — and `fileRead()`'s half of the
sector-cache exposure is still unfixed for lack of ARM7 space, both recorded elsewhere in this
document. What is claimed is narrower and testable: the window that hung Ketsui is closed, and the
next boot on the same boss is the check.

## The `readOngoing` gate was wrong, and the record it wrote is what says so

It froze again, same boss. And the queue file from that session came back like this:

```
offset 0    48 bytes of NUL                       <- slot 0, empty
offset 48   301278 \t 20260822210414 \t YKDJ \t KETSUI-DEATH \t 1 \n
```

**Complete and correct.** The id, a plausible stamp, the game code, the title, the hardcore flag. So
the append did not hang: it ran to the end, wrote its record and cleared the request. Whatever stops
the game is a *side effect* of that work, not a stall inside it — which retires the entire shape of
the previous section's argument, not just its conclusion.

Two things in that dump are worth keeping, because both were paid for:

**The record is in slot 1 with slot 0 empty.** Either the scan misread a NUL as a digit — the exact
failure the `prevSect` invalidation was added to prevent, which would mean it does not always work —
or `raUnlockStateMagic` survived in un-zeroed `.bss` from a previous boot and the scan never ran.
Unresolved, recorded, and not guessed at.

**The stamp is real**, so `rtcGetTimeAndDate()` completed too.

### What the code says, once it is read rather than assumed

Two corrections to the previous section, both found by reading and neither by another boot:

**There is no IRQ nesting on this ARM7.** `irqHandler` in `card_engine_header.s` dispatches one
handler and returns; nothing clears the CPSR I bit, which hardware set on entry. So the VBlank handler
*cannot* preempt the FIFO handler, and the append can never land inside a blocking card read or a
save. The whole "we clobbered a transfer in progress" family is dead for the blocking paths. The
resumable path is different — it spans IRQ returns by design — so the `readOngoing` gate is still
correct and stays; it simply was not what was wrong.

**The RTC is not on SPI.** `rtcTransaction()` bit-bangs `RTC_CR8` directly, a dedicated register, so
it cannot corrupt the touchscreen's bus. What it *does* cost is time: 8 command bits and 56 result
bits, two `swiDelay(48)` per edge, then a second transaction for the status register — order 4–5 ms,
inside the VBlank handler, with interrupts masked. That is a third of a frame in which the FIFO
handler is not answering the ARM9's card reads. Nothing else in nds-bootstrap reads the RTC while a
game runs; the in-game menu only does it with the game paused. This fork added that, and it has never
been tested in isolation.

### Three suspects, no way to read the answer, so the switch became a ladder

What is left on the frame an achievement fires: the shared-word handoff, the RTC read, the SD work.
Each guess costs a flash and a boss fight, and the last two guesses were both wrong. So `queue` stops
being a switch:

| `queue` | what crosses | unlock survives? |
| --- | --- | --- |
| `0` | nothing — the ARM9 does not offer | no |
| `3` | the request is acknowledged and cleared, nothing else | no |
| `2` | everything except the RTC read; the record is stamped `20000000000000` | yes, without `o=` |
| `1` | everything | yes |

Walked from `0` upward, **the step where the freeze reappears is the step that causes it.** Each one
is an `ra.cfg` edit rather than a rebuild, which is the whole point: four hypotheses, one flash.

Level `2` writes a fixed stamp rather than the NULL `raUnlockAppend()` also accepts, and that is a
size decision: month `00` makes `raQueueStampToUnix()` refuse it, so the launcher submits without
`o=` — the same outcome as a bare record, visible in the file as what it is. It is also the only rung
that is a usable fallback rather than purely diagnostic.

### The level rides in a slot, which is the opposite call to the one made for the mode

The mode had to be inseparable from the request magic, because a missing mode reads as softcore *by
omission* and silently mis-files a real unlock. A missing level reads as garbage, and the ARM7 tests
for the two values that skip work and does everything otherwise — so anything unrecognised lands on
shipping behaviour. The failure direction is toward doing the work, which inverts the argument
cleanly and is why a slot is safe here and was not there.

### Not compiled in on TWL-SDK, and that is sixty bytes against a hundred

Reading the slot and branching on it costs 100 bytes of `cardenginei_arm7`. The TWL-SDK variant links
into 33K with **60 spare**, and `.bss` follows `.text`, so it does not fit — measured, at
`toncset`-for-the-clear-loop and one-branch-instead-of-two and every other arrangement worth trying.
That variant ignores the level and always does the whole thing.

That is the right direction to fail in: a diagnostic that quietly did *less* work than asked would be
worse than one that is unavailable. And it is where it is least missed — the ladder exists to bisect
a freeze on Ketsui, an NTR title, which loads the variant with 11K spare. Recorded in
`tools/ra.example.cfg` so a level that appears to do nothing on a DSi-enhanced title is explained
rather than puzzling.

### Rung 3: the handoff is innocent

`queue=3` — the request crosses to the ARM7, is acknowledged and cleared, and the ARM7 does nothing
else. **A whole stage of Ketsui with no freeze**, including the boss that had killed every previous
attempt.

So the shared-word protocol is not it. Writing two words into the block, the ARM7 waking on the magic
and clearing it: none of that disturbs the game. That removes the third suspect and leaves the two
that are actual work on the ARM7 — the RTC read and the SD write — with `queue=2` between them.

Worth stating because it is the first thing on this trail that has been *ruled out* by evidence rather
than by argument: the previous two eliminations were both wrong.

### The overlay's garbage on Ketsui, read out of the snapshot

Independent of the freeze — it is still there at `queue=3`, where the ARM7 does nothing at all, so it
is entirely the ARM9's overlay. Three rows of the RAM viewer settled what four rounds of patching
could not:

```
EAB0  02 00 00 6A  80 54 75 03     rearm bits, overlayState 0x6A, overlayText 0x03755480
EAB8  00 07 01 00  00 00 00 00     overlayDispcnt 0x00010700, overlayWindow 0
EAC0  FF FF 00 00  52 44 41 31     spriteOam FF, spriteSlot FF, ..., defsMagic 'RDA1'
```

(The viewer prints bytes in address order, which `RDA1` at `+0xC4` — the constant `0x31414452` —
confirms, and which also confirms the whole layout against the pinned offsets.)

`overlayState` 0x6A unpacks as: extended palettes **off** (bit 0 clear — that long-standing suspicion
is dead), **layer 1**, **block 1**, a **real fade** in progress (bit 5), and the notification **held
back until that fade ended** (bit 6). `overlayDispcnt`, captured before a single register of the
game's is disturbed, says BG0, BG1 and BG2 are all enabled and BG3 and OBJ are not.

So the overlay borrowed BG1 — correctly, by `chooseLayer()`'s rules — and then chose **block 1**,
which is the block BG1 itself was using. It wrote its glyphs and a full 32×32 tilemap into a live
layer's character block while the game kept writing into it. Garbage in the place and for the
duration the notification would have had.

#### The survey exempted the one layer it should not have

```c
if (i == skipLayer) {
    continue;  /* the layer being borrowed */
}
```

The reasoning was that the overlay is taking that layer, so its VRAM is going spare. **The layer is
taken; its VRAM is not.** `chooseLayer()` promises precisely that, in as many words — "the overlay
points its character and screen bases at a block of its own, so the game's tiles and tilemap for this
layer sit there untouched throughout" — and this skip made that promise impossible to keep, because
the block the game's own layer was using was the one block the survey could not see.

It also contradicted the paragraph sitting directly beneath it, which already says a block referenced
by *any* layer counts as in use, enabled or not. That rule was learned the hard way on Contra 4. This
was the one layer exempted from it.

Fixed by deleting the exception; `raOverlaySurvey()` lost the parameter. The host case that used to
assert "the skipped layer contributes nothing" now asserts the opposite on the same input, which is
the shape a real fix leaves behind.

Whether this makes Ketsui *show* the notification or merely stay quiet depends on whether a block is
free once BG1's own is counted. Quiet would be the correct outcome; the file has said all along that
a missing notification is a missing feature and a corrupted game is a bug.

#### The deeper finding, not yet acted on

**Deferring a notification until a fade ends lands it at the least representative moment there is.**
Bit 6 says it waited for the fade; `overlayDispcnt` is what the sub engine looked like immediately
after one — BG3 off, OBJ off, mid-transition. The survey then treats that snapshot as the game's
steady state.

This is the same mistake the block survey already learned, one level up: sampling enable bits at a
moment when the game has things switched off, and concluding they are free. It was fixed for blocks
and it is still live for layers and for the object engine. Recorded rather than patched, because the
last two things patched on this trail were both patched before they were understood.

## The ladder answered it in three boots: reading the RTC is what froze the game

| rung | what the ARM7 does | Ketsui's boss |
| --- | --- | --- |
| `0` | nothing crosses at all | clear |
| `3` | acknowledges the request, nothing else | clear |
| `2` | everything **except reading the RTC** | clear |
| `1` | everything | **freezes** |

`2` and `1` differ in exactly one call. One variable, one answer, and it is not something any amount
of reading would have produced — the two guesses made by reading were both wrong, and this took three
`ra.cfg` edits and no rebuilds.

### Why a clock read can kill a game, and why it does not matter which reason

`rtcTransaction()` bit-bangs `RTC_CR8` with a `swiDelay(48)` on every clock edge, and
`rtcGetTimeAndDate()` does two of them: a command byte and seven result bytes, then a second
transaction for the status register. About 166 delays — call it a millisecond — spent **inside the
VBlank handler with the I bit set**. Two ways that hurts:

**Duration.** A millisecond in which the FIFO handler is not answering the ARM9's card reads, at the
one moment a bullet-hell shooter has least to spare.

**Collision.** The RTC is a serial bus with a chip select, and plenty of DS games read it from their
own ARM7. Our handler interrupts the game's main loop; if that loop was mid-transaction, driving CS
and SCK underneath it destroys the transaction and the reply the game is waiting for never comes.

The fix does not need to know which, because it removes both: **the clock is read once, in
`initialize()`**, which runs while the game is setting up its interrupts and has no main loop to
interrupt. Nothing in nds-bootstrap read the RTC during gameplay before this fork added it; the
in-game menu only ever did it with the game paused. That was the rule, and this broke it.

### What it costs, stated rather than hidden

**Every unlock in a session is now stamped with the moment the session started.** An hour in, that is
an hour of error, and ten achievements earned across an evening all carry one timestamp.

Measured against what it replaces it is still the right trade — without `o=` the server dates an
unlock by the boot that *reported* it, which with this client can be the next day — but it is a
regression against what shipped last week, and the detail page will show it.

The exact version is designed and does not fit. The ARM7 would count VBlanks and write the elapsed
seconds beside the stamp, and the launcher would do the addition and the day rollover, because it
owns the calendar and has host tests. **The record has no room for it**: fields come to 46 of
`RA_QUEUE_RECORD`'s 48 bytes with a six-digit id, and a frame count needs seven more. So it needs
`RA_QUEUE_RECORD` and `RA_QUEUE_MAX` to change together, which changes the on-card file's layout and
the launcher's packing. That is a real change, not a squeeze, and it is on the side of the tree that
can be tested — see "What is left".

A cheaper partial: refresh the cache whenever the in-game menu opens, which is a moment already known
to be safe because `inGameMenu.c` reads the clock there today. It costs a call and it helps only
players who open the menu, so it is worth doing and is not a substitute.

### Forty-four bytes, and two counters nobody could read

Fitting this into `cardenginei_arm7_twlsdk` took four arrangements. The one that shipped:

- The clock is captured into `raStampCache`, **initialised data rather than `.bss`** — it is part of
  the image the bootloader copies in, so it arrives holding `20000000000000`, a stamp
  `raQueueStampToUnix()` refuses. That is the safe answer already in place before anything runs,
  which is what let the read path drop its validity magic and its fallback branch entirely.
- `raUnlockAppend()` is handed `raStampCache` **by pointer**. The local `char stamp[14]`, its copy and
  the function that did the copying were what put `.bss` past the end of the region.
- `raStampCapture()` is `noinline`, so the RTC code exists once rather than once per inlined copy of
  `initialize()`. Worth 12 bytes.
- `raUnlockWritten` and `raUnlockDropped` are **deleted**. Both were incremented and never read:
  nothing on this CPU reports them, and the ARM9's own `unlockLost` covers the case that matters.
  Counting into a variable no one can see is not counting, and the binary needed the 24 bytes.

That leaves 44 bytes spare. `queue=2` is retired to `full` in the same change, because with the RTC
out of the hot path there is nothing left for it to skip; it keeps its number rather than being
renumbered, so the readings recorded above still mean what they say.

### Retracted: "no freeze" was a partial run

This was written up as confirmed on the strength of "no se cuelga con queue 1", and that was reported
before the run had finished. **The game froze at the end of the boss.** The kill itself is survived,
which is a change from the six sessions that died on it, but the session still ends there.

So the entry above is wrong and is corrected rather than quietly amended: the RTC fix is a real
removal of a real hazard — `queue=2` isolated it and the mechanism is written down — but it is **not
the whole cause**, and the ladder's readings need re-reading in that light:

| rung | what was reported | what it now means |
| --- | --- | --- |
| `3` | "no se colgó en todo el stage" | a whole stage, unambiguous |
| `2` | "no se colgó" | how far the run went is **not established** |
| `1` (before the fix) | froze at the boss kill | |
| `1` (after the fix) | survives the kill, freezes at the end of the boss | |

The `2` row is the load-bearing one and it is the one now in doubt. If that run also stopped short,
then `2` and `1` were never actually compared over the same stretch and the bisect proved less than
was claimed from it. What is solid is `3` versus everything else: a whole stage cleared with the ARM7
doing nothing but acknowledging the request.

Recorded as a method note, because it is the third time on this trail: **a negative result needs the
same stretch of play as the positive one it is compared against, and "it did not freeze" has to say
how far it got.** Two of the three wrong conclusions in this investigation came from comparing runs
that were not the same run.

### What the photograph could not settle

The overlay's garbage is still there, and a photograph of Ketsui's results screen was where the
reasoning ran out. It shows the sub screen holding the game's own score table with bars in a green
that looks wrong — but "looks wrong" is not a reading, and the two guesses available from it point
opposite ways:

**The palette theft.** `draw()` writes `OVERLAY_INK_COLOUR` (0x7FFF, white) and
`OVERLAY_SHADOW_COLOUR` (0x0000) into bank 15's ink and shadow entries, and puts them back on hide.
That is free if the game was not using them and destructive if it was, and nothing has ever checked
which. The white argues against this — the bars are green, not white — but a camera pointed at a
lit DS screen is not a colorimeter, and the entries the game lost are recoverable exactly.

**8bpp.** The whole palette argument assumes the borrowed layer is 4bpp, where bank 15 is a corner of
palette RAM that a game using banks 0-3 will not miss. At 256 colours there are no banks: the map's
palette field is ignored and a pixel value indexes all 256 entries, so `OVERLAY_PAL_BANK * 16 + ink`
is plain entry 241 — as likely to be in use as any other, and in the middle of the range a game
fills. `BGCNT` bit 7 says which, and the overlay has never looked at it.

So three more readings are published rather than argued about, all of them values the background path
already holds and throws away:

| field | offset | what it decides |
| --- | --- | --- |
| `overlaySavedInk` | `+0xCE` | the game's own colour at the ink entry, before it was overwritten |
| `overlaySavedShadow` | `+0xD0` | ...and at the shadow entry. Two blacks mean the theft is free |
| `overlayBgCnt` | `+0xD2` | the borrowed layer's whole `BGCNT`: char base, screen base, priority, size, **and bit 7** |

The snapshot goes to 0xD4. This is the same instrument that killed the extended-palette theory and
found the borrowed-block collision, pointed at the two things left.

## Confirmed: the freeze was the card, and moving the write off the game's frame ended it

`queue=1`, Ketsui, the same boss that had ended six sessions. **No freeze, and the unlocks were
written.** The last bug in this chain is closed.

The step that settled it was switching the overlay off *entirely* — no sprites, no borrowed layer,
not one write to the game's VRAM — and watching it freeze anyway. That exonerated the overlay, which
had absorbed six rounds of work, and left exactly one thing between "plays" and "hangs": the ARM7
opening the SD card from inside a VBlank handler, with IME off, while a bullet-hell streamed a scene
transition.

**The fix is a removal, not a mitigation.** Three separate diagnoses of *which part* of that
transaction was fatal were wrong — the overlay, a suspended card read, the RTC read — and each cost a
boss fight to test. So the transaction was moved out of the way instead of being bisected further. On
the frame an achievement fires the ARM7 now does arithmetic and nothing else. The card is opened
where the game is not using it: `inGameMenu()` (paused under `saveMutex`, and already using the card
for screenshots), `returnToLoader()` and `forceGameReboot()`.

What it costs is stated in the config file and worth repeating: an unlock that has not been drained
does not survive the console being switched off mid-session. Quitting normally drains it.

### And what it cost to fit

TWL-SDK keeps the old in-gameplay append. Deferring is 136 bytes of text and 28 of `.bss`, measured;
that binary links into 33K with **forty-four spare**, and a three-slot buffer with the service
inlined back into the handler still landed a hundred over. Every game this was built to rescue is
NTR — Ketsui, Contra 4, Chrono Trigger all load `cardenginei_arm7`, which has 11K free. Recorded
rather than hidden: those titles are not fixed, they are unchanged.

## `0 of 54 earned  2 sync` — the one field on the page that disagreed with the rest

Reported after two achievements unlocked cleanly. Zero is not what a player who just earned two
should be reading.

An achievement that has fired but not been sent **is earned**. The list under that line marks it with
a star, and the percentage at the right-hand end had counted it since the day it was added — so the
big number was the only field on the page saying otherwise. `sync` does not report a different kind
of achievement; it reports an acknowledgement still owed.

Fixed by printing `earned + queued`, which is the same sum the percentage already used.

### Checking the invariant instead of assuming it found a real bug

That sum is only safe if the two counts are disjoint, so the two places that raise
`RA_VIEWER_QUEUED` were read rather than trusted. The one that runs at build time guards correctly on
`RA_VIEWER_EARNED`. **The one that runs when an achievement fires did not:**

```c
if (!(v->entry[k].flags & RA_VIEWER_QUEUED)) {    /* EARNED never tested */
    v->entry[k].flags |= RA_VIEWER_QUEUED;
    v->queued++;
}
```

So an achievement the server had already reported as earned, firing again this session, came out
carrying **both** flags and bumping `queued` for something already inside `earned`. With the header
now adding the two, that entry would be counted twice and could read past the size of the set.

And it is not hypothetical. A cached set carries filtering as old as the cache, so `sync=0` — or an
unlock deleted on the website, which is exactly what was done to re-test this game — leaves an earned
achievement active in the staged set.

Pinned in the host suite by firing an already-earned id and asserting `queued` does not move, then
walking every entry for the both-flags state. Verified the way a pin has to be: with the fix reverted
the suite reports three failures.

### One observation worth more than the next commit

When it freezes, **does the in-game menu still open?** L+R+Down+B. If it does, the ARM7 is alive and
the ARM9 or the game is stuck, and every ARM7-side theory above is wrong together. If it does not,
the ARM7 is dead and the ladder will say which rung killed it. It costs nothing to look, on a boot
that is happening anyway.

## `overlay=0`, and why the overlay is the only part of this that needs a switch

Asked directly, after four rounds on one shooter: is this approach sustainable, and can the 3DS's own
ARM11 be used instead?

> **Corrected, later, and it was wrong twice.** "The ARM11 is not running" is not true, and this
> document asserted it in two places on the strength of nothing but repetition. A 3DS running a DS
> game boots **TWL_FIRM**, and TWL_FIRM includes **`TwlBg`, an ARM11 system module that runs for the
> whole session** -- it is what upscales and filters the DS output onto the 3DS's own screens. Luma3DS
> loads a replaceable `TwlBg.cxi` from `/luma/sysmodules`, which is how DS-mode screen filters and
> widescreen exist at all.
>
> There is also an established channel between the two sides: **RTCom**, which uses the 3DS's RTC to
> let the ARM7 and the ARM11 talk *while TWL_FIRM is running*. It is not theoretical -- it already
> powers circle-pad analog input in upstream nds-bootstrap, DS-mode widescreen, and gyro in
> GBARunner2. This fork does not speak it: `rtcom`, `circlepad` and `twlbg` appear nowhere in the
> tree.
>
> What that changes, and what it does not:
>
> **The overlay.** This is the real find. `TwlBg` already holds the composited DS picture on its way
> to the 3DS's screens, which makes a patched `TwlBg` the exact equivalent of the PSP framebuffer
> trick this fork went looking for and could not find on the DS side: draw the notification *after*
> the DS hardware is done, touching no layer, no sprite, no palette and no VRAM. It would work on
> every game unconditionally, including the ones where the nine VRAM banks leave nowhere safe to
> draw. It is the only approach discussed here that cannot corrupt a game, by construction.
>
> **The LED.** The 3DS's notification LED is MCU-controlled and reachable from the 3DS side, which is
> the half of the console the ARM11 lives on. Plausible; unverified here.
>
> **The radio.** Not helped. `TwlBg` has no network stack, and the WiFi chip is being driven by the
> DS side throughout. This one does not get easier.
>
> The cost is a change of shape rather than a change of code: it means shipping or contributing to a
> patched `TwlBg`, and users needing Luma3DS with external FIRM loading enabled. That is a dependency
> this fork does not have today. Recorded as the correction it is; the paragraph below is left
> standing as what was believed.

**The ARM11 is not available, and this project already wrote that down for the radio:** a 3DS running
a DS game is in DS/TWL mode, its operating system and its ARM11 are not running, and in that state
the console is a DSi for our purposes. There is no 3DS-side layer to draw on from here, no channel to
talk to one, and building it would be a Luma3DS project rather than an nds-bootstrap one.

**But the concern is right, and it is worth naming precisely.** The overlay is the only part of this
fork that has to *negotiate with a running game for its hardware*. Everything else reads memory, or
writes into a window nobody else owns. To draw a popup the overlay has to answer "which object VRAM
and which OAM entry is this game not using", and that question **has no reliable answer from
outside the game**. Every fix so far has been a better inference, not an answer:

| Round | The inference | How it failed |
| --- | --- | --- |
| pixels written once | the game will not overwrite our tiles | it did |
| probe two words | an overwrite will touch an end of the range | it landed in the middle |
| probe every object | the game will stop once we put them back | it writes there every frame |
| survey all OAM entries | a disabled entry's tiles are free | they are a parked bank |

The fourth is a genuine correction rather than another guess — a disabled entry's tiles *are* in use,
and that is a fact rather than an estimate. But the shape of the list is the point.

So the switch. `overlay=0` in `ra.cfg` stops the popup from drawing, and it is checked at the top of
`ra_overlay_tick()` rather than at each place that draws: a switch that turns this off should turn off
the surveys and the register reads too, not only the pixels.

**What is kept with it off:** the unlock is detected, queued, submitted, and listed in the in-game
menu with the date it was earned. What is lost is the popup — the only part of the feature that can
touch the game at all. It rides in a spare byte of the session block, which already crosses from the
launcher to the cardengine, so the bootloader's own two offsets into that block are undisturbed and
pinned to say so.

### The guard that should have caught the stale pin, and did not

Appending `overlayRedrawn` to the snapshot left `CHECK(sizeof(raSnapshot) == 0xCC)` naming the old
size. The suite failed. **`ra_release.sh` shipped the build anyway**, because its check was:

```sh
if ! bash tools/ra_reader_test.sh 2>&1 | grep -q "PASSED"; then
```

The suite runs several binaries and prints a verdict for each, so one `FAILED` among three `PASSED`
still matched the grep. That is not a check. It now uses the exit status — which `ra_reader_test.sh`
was already returning correctly — plus a second look for the word `FAILED`, and it was verified by
breaking a pin on purpose and watching it refuse.

This matters more than the pin did. "Always build with `tools/ra_release.sh`" is a rule this project
rests on, and for some number of commits the most important thing that script promised to check was
being read with a `grep -q`.

## The object VRAM survey called a parked sprite bank free

Three rounds of making the overlay put its pixels back did not fix Ketsui, and that is the signal that
the premise was wrong rather than the details. **Garbage where our notification belongs means somebody
else is writing there, which means the slot was never free.**

The hole is one line:

```c
if ((attr0 & 0x0300) == 0x0200) {
	continue;   /* not rotation/scaling, and disabled */
}
```

A disabled OAM entry's tiles counted as free, on the reasoning that an object which is not drawn is
not using its VRAM. **That is not true.** An entry off *this* frame still points at tile data the game
means to keep, and toggling entries while leaving their banks loaded is the ordinary way to run a lot
of sprites. A bullet-hell does it constantly.

Ketsui gave three symptoms of the one cause: the notification showed as garbage for its full three
seconds, the game's own graphics broke, and it froze at a stage transition — which is exactly when a
game reaches for the sprite banks it had parked. Re-blitting could never win that race, because the
game was writing there too. **The only winning move is not to take the slot.**

So every entry is measured now, enabled or not. Our own eight are excluded by index — they used to be
excluded *by* being disabled, which is what tied the two rules together, so dropping the one meant
naming ourselves in the other or the overlay would evict itself on its first frame.

**What it costs is visibility.** Fewer slots will look free, so more games fall back to the background
path or decline to show anything. That is the right direction: a notification nobody sees is a smaller
failure than a game that breaks, and this file already says so in three other places.

## `ra_unlocks.txt` came back with 464 bytes of somebody else's memory

Reported after a Ketsui session, and the file diagnosed itself. It is exactly 3,072 bytes — the right
size, no overrun — and:

```
rec 0  ................................................   all zero
rec 1  301278.20260822180054.YKDJ.KETSUI-DEATH.1.......   perfect
rec 2  ................p23""fffffffUU..................   not ours
```

Non-zero bytes run from offset 48 to offset **511** and stop. One 512-byte sector was rewritten: the
record correct inside it, and 464 bytes of unrelated data — audio-shaped, repeating on a 128-byte
stride — around it.

### `fileWrite()`'s read-modify-write trusted a cache over a buffer it does not own

A 48-byte record at offset 48 is a partial sector, so `fileWrite()` reads the sector, pastes the
record over part of it, and writes all 512 back. The read is `loadSectorBuf()`:

```c
if (prevFirstClust != file->firstCluster || prevSect != curSect || prevClust != file->currentCluster) {
	CARD_ReadSector(...globalBuffer...);
	...
}
```

**It skips the read on a cache hit, and `globalBuffer` is shared.** During gameplay something else
always might have used it since. So the merge pasted a correct record into a buffer holding the
game's data, and flushed the lot.

**Three independent confirmations, one of them from the file itself:**

| Evidence | What it proves |
| --- | --- |
| the record landed in slot 1 with slot 0 empty | the slot scan read a digit out of a buffer that no longer held the queue |
| exactly 512 bytes rewritten, record correct, surroundings not | the merge used stale buffer contents |
| `loadSectorBuf()` skips the read and nothing invalidates it | the mechanism, by reading |

### Fixed in `fileWrite()`, and the placement was decided by a linker

The invalidation goes at the top of `fileWrite()` rather than at the call site. That fixes **every**
caller — any partial write anywhere in nds-bootstrap had the same exposure — and costs one sector
read per write that begins mid-sector, which an aligned write pays once for a whole transfer.

Getting there took three attempts and the ARM7 refused two of them, which is worth recording because
it is the constraint that shaped the fix:

| Attempt | Result |
| --- | --- |
| export a helper from `my_fat.c`, call it from the cardengine | **link failed** — `.bss` past the end of `vram` on `cardenginei_arm7_twlsdk` |
| `resetPrevSect()` — which already existed for this, in `my_fat.c` — called from both `fileRead()` and `fileWrite()` | **link failed** again: the retained function body plus two calls grew `.text`, and `.bss` follows it |
| inline in `fileWrite()` only, plus one inline clear in the cardengine's slot scan | fits |

`.bss` overflowing when no variable was added is the tell: that section sits after `.text` in the
linker script, so growing code moves its *start* past the region end. The TWL-SDK ARM7 had 60 bytes
spare before this work and is the tightest binary in the tree.

**`fileRead()` is deliberately left alone**, and that is a limitation rather than a decision I like.
It means a partial *read* can still be served from a stale buffer. The one place that mattered — the
queue's slot scan — clears the cache itself before its first read, because the first read is the one
that decides which slot gets written. Any other partial read in the project keeps the old exposure.

### What this does *not* explain

**The crash is still unexplained, and confirmed so.** With the corruption fixed — the file comes back
clean, 42 non-zero bytes and nothing else — Ketsui still froze on the same boss kill. This bug writes
wrong bytes into a file on the SD card, which cannot hang a game, and now hardware says the two are
independent rather than that being an argument. The achievement that
fired is id 301278, `Bomb Quartet`, "Get 4 bombs in Single Play by beating a boss without dying" —
which confirms an unlock does happen at that moment, so the notification path is still the place to
look next.

**Later, and it was the wrong place to look.** The notification path was innocent; the freeze and this
corruption were the *same collision seen from two sides* — the append and the game's in-flight card
read sharing one SD controller and one 512-byte buffer. Fixing the merge made the record come clean
and left the hang, which is exactly what one bug with two victims looks like. See "`queue=0` answered
it".

## The overlay looks wrong on some games, and the file already knew why

Reported as inconsistent between games and not looking right. The cause is the overlay's own
reasoning, applied to two of the four things it needed to be applied to.

From the per-frame path, about the OAM entries:

> Rewritten every frame rather than left in place, because a game that keeps a shadow copy of OAM and
> DMAs the whole thing each frame — which is the ordinary way to do it — would otherwise wipe these on
> its next transfer.

That is exactly right, and it is equally true of the **pixels** and of the two **palette entries**.
Both were written once in `show()` and never again. A game that DMAs its object tiles or its object
palette every frame is doing something as ordinary as DMAing OAM — and what that produces is one of
our OAM entries pointing at tiles that are now the game's, in colours that are now the game's. A
notification that appears, in the right place, made of the wrong pixels.

**Inconsistent between games and consistent within one**, which is what was reported: it depends
entirely on whether that game's engine blanket-transfers those two regions.

**The eviction survey cannot catch it.** `surveyObjVram()` looks for an OAM entry *referencing* our
slot, and a game transferring tile data with no entry pointing here passes the survey while having
overwritten every byte of our glyphs.

So both are re-asserted every visible frame now, on the sprite path and on the background path. The
background path is if anything more exposed: the block it borrows is character VRAM a game may be
streaming tiles into.

### Probed before rewritten, because Ketsui hung at a boss kill

The first version blitted unconditionally: about two kilobytes a frame, which measures cheap in
scanlines. Hardware disagreed with "cheap". *Contra 4* came back correct — the fix works — and
**Ketsui hung on killing the first Novice boss**, which is both the moment an achievement fires and
the moment a bullet-hell shooter has least to spare. This runs inside the game's VBlank on top of a
memref pass already using 47 of the 70 available scanlines, and this fork has already killed one game
by adding per-frame work to that budget.

So the rewrite is conditional on a probe, which turns 512 writes a frame into a handful of reads on
every frame nothing trampled us.

**The first version probed two words — the first and the last — and said in its own comment that a
transfer overwriting the middle would be missed. Hardware then did exactly that.** Ketsui showed
garbage where the notification belongs, for the full three seconds: whatever it transfers lands
between those two words, so the strip was never put back. Contra 4 was correct either way because
nothing there touches the range at all.

Nine reads now, on the sprite path: the first word of each of the eight objects, plus the last word of
the range. A game overwriting a contiguous run cannot clear every object's first word without being
caught. The background path probes one word per tile for the same reason. That is the difference
between missing a stripe for one frame and missing the whole notification — the failure the two-word
version actually produced, rather than the mild one its comment predicted.

Both probes read back what the blit *wrote*, recorded as it wrote it, because the sprite blit is a
gather: it rearranges the strip's background layout into per-object order, so which source word lands
at the start of object k is not something a probe can work out without redoing the blit's arithmetic.

`overlayRedrawn` is appended to the snapshot for it, because "re-assert the pixels" was a *deduction*
— the file's own argument about OAM, applied to the two things it had not been applied to — and a
deduction that spends time inside a game's VBlank has to be able to say whether it was needed. Zero
on a game that leaves object VRAM alone; climbing on the game whose notification looked wrong.

### A real bug, introduced and caught inside the same edit

`draw()` **saves the palette entries it is about to overwrite**, so that `hide()` can put them back.
Called every frame, its second call would have saved *our own* colours as the game's originals, and
`hide()` would have restored those — leaving two entries of the game's palette permanently ours, on
every game, for the rest of the session.

`drawTiles()` is the split that makes the per-frame path unable to do it: the pixels and the map in
one function, the state reading and the borrowed palette in the other, and only the first is reachable
from the tick. The sprite path never had the problem — its palette save lives in `spriteShow()` rather
than in `spriteBlit()` — which is what made the asymmetry visible.

### What was checked and ruled out first

**OBJ extended palettes.** `SUB_DISPCNT` bit 31 is the object counterpart of the bit 30 that this
project already got wrong once on the background side, and nothing in the overlay reads it. It does
not matter: extended palettes apply only to 256-colour objects, and ours are 16-colour, which always
take standard object palette RAM. Ruled out by reading the sprite's own attribute setup rather than by
building anything.

## The boot takes fifty seconds, and nobody could say where they went

Reported as a standing complaint rather than a regression: the ladder has always been slow. And the
log could not answer it — the rungs are a chip bring-up, a DHCP lease, five HTTP round trips and a
hundred-kilobyte download, and which of them is the fifty seconds is not guessable from outside.

So the first change is the measurement. `raWifiStep()` is already called once per stage in both
modes, so one line there gives a timeline with no new call sites:

```
[  0s] Reading the game
[  2s] Reading your settings
[  2s] Connecting to Wi-Fi
[ 21s] Getting a network address
[ 24s] Signing in to your account
...
```

**And `time(NULL)` was the wrong clock**, which hardware answered on the first run: every stage in
the log read `[  0s]`. It reads a value the ARM7 refreshes, and in this launcher the ARM7 is running
dsiwifi rather than libnds' own VBlank work — so the clock is set once at boot and then sits still.
Correct for stamping an unlock, which is what it was measured doing; useless for measuring a boot.

A VBlank counter was the other candidate and it is worse. `raWifiIdle()` runs in the wait loops and
*not* inside a blocking `recv()`, which is precisely where the seconds being hunted are going. A
timer counts through a blocked CPU; a frame counter cannot. TIMER0 cascaded into TIMER1 gives 32 bits
at 32,728 Hz, read low-high-low so a wrap between the two registers cannot report a two-second jump.
Both are stopped in `raWifiShutdown()` beside the one dsiwifi leaves behind.

### The same log answered the other open question

`console 32x24, map no`. **`consoleGetDefault()->fontBgMap` is null in this launcher**, so the tile
map the fixed-layout screen drew into was never there — which is exactly why that build showed a
black bottom screen and nothing else: `raMap` stayed null, the paint returned early every frame, and
every other writer was gated off behind quiet mode.

That is the reading the fifth attempt did not have and the reason the geometry line was kept. The
fix it points at is small — `consoleSelect()` returns the *previously current* console, so calling it
twice hands back the live one without changing anything — but the screen is not what this section is
about, and a fixed layout is not what the boot time needs.

### Two costs found by reading, both paid on every boot

**The tail drain was eight seconds, flat.** It exists because dsiwifi narrates asynchronously and
keeps talking after the last rung is decided — the probe's first hardware run printed the line naming
the access point *after* its own summary. What it did not need was to spend the full eight seconds
every time, and it did: there was no early exit. Eight seconds came off every boot this fork has ever
done, whether there was anything left to say or not. It now ends after half a second of silence, with
the same ceiling as before for a chip that keeps talking.

**DNS was resolved once per request.** Every rung opens its own connection — each request carries
`Connection: close`, because a DS with 191K of heap after lwip is up is not a place to keep sockets
alive across stages — and each of those asked DNS again. lwip caches, but a miss is a round trip to
the resolver on a link this fork has measured at its slowest, and there is no version of "the address
of retroachievements.org" that changes between two rungs of the same boot. Resolved once per boot now.

### The timeline, and it was not what anyone would have guessed

| Stage | starts | costs |
| --- | --- | --- |
| hash the ROM | 0.2s | 0.4s |
| read `ra.cfg` | 0.6s | — |
| chip up and associate | 0.6s | 1.3s |
| DHCP | 1.9s | 3.2s |
| `r=login` | 5.1s | **10.3s** |
| `r=gameid` | 15.4s | **10.2s** |
| `r=startsession` | 25.6s | **10.2s** |
| send the queue (empty) | 35.8s | — |
| `r=unlocks` | 35.8s | **10.2s** |
| `r=patch`, 64,424 bytes | 46.0s | **13.8s** |
| **total** | | **59.8s** |

**The radio is 5 seconds of it.** The chip bring-up everyone would have blamed — SDIO reset, BMI, a
full firmware upload, WMI, a scan, WPA2 — is 1.3 seconds, and the DHCP lease is 3.2.

**Four requests cost 10.2 seconds each for about a kilobyte.** A cost that is identical across four
replies of different sizes is not a transfer, it is a wait. And the fifth is the same 10.2 plus 3.6
for the 64 KB that actually moved — which says the transfer rate was never the problem either.

### Reading until the peer closes means waiting for the peer to close

```c
while (total < outSize - 1) {
	const int got = recv(sock, out + total, outSize - 1 - total, 0);
	if (got <= 0) break;      /* ...after SO_RCVTIMEO, every time */
	total += got;
}
```

The body arrives in the first recv or two. Then the loop asks for more, and there is no more, so it
sits on the socket timeout before the peer's close is noticed.

A note in `raNetStreamHeaderLine()` had already decided against the fix, and reads as a warning now:

> Content-Length is not needed because `Connection: close` and the chunk terminator both say where
> the body ends, and a length we believed but did not enforce would be worse than none.

Both statements are true. What they miss is the price: one of those two ways of knowing costs a
socket timeout every time it is used. So the length is read now — and *enforced*, which answers the
second half honestly: `done` is set only when the counted bytes reach the declared length, so a
reply that promises more than it sends still ends where it always did, at the close.

Both readers get it. The streaming one stops on `stream.done`; the plain one keeps returning the raw
reply its callers were written against, so it scans the headers itself — `raNetHeaderLength()` and
`raNetHeaderChunked()`, pure and host-driven, because every part of it is a string problem: the
header is case-insensitive by the standard, Cloudflare does not send it the way anyone expects, and a
`Content-Length` appearing inside a body of achievement descriptions must never be mistaken for the
header.

### Hardware: 59.8s to 10.5s, and the block is byte-identical

| Stage | before | after |
| --- | --- | --- |
| hash the ROM | 0.4s | 0.4s |
| chip up and associate | 1.3s | 1.3s |
| DHCP | 3.2s | 3.3s |
| `r=login` | **10.3s** | **0.5s** |
| `r=gameid` | **10.2s** | **0.3s** |
| `r=startsession` | **10.2s** | **0.4s** |
| `r=unlocks` | **10.2s** | **0.5s** |
| `r=patch` | **13.8s** | **3.5s** |
| **total** | **59.8s** | **10.5s** |

**What makes this proof rather than a claim** is that `body was 64424 bytes` and `block 26691 of
29431 used` are identical to the slow run's. Had the early exit truncated anything, the patch body
would have arrived short and the block would not land on the same byte. With them: `98 kept`, `98
with ids`, `4 clipped`, `98/98 desc/points`, `24 clipped`, `already earned 4` with all four ids,
`staged 98 definitions`, `reached stage 15 of 15`.

The 3.5s left in `r=patch` is the 64 KB actually moving, which is what the breakdown predicted:
10.2 fixed plus 3.6 real, with only the real part remaining.

**The one hazard the change introduces**, written down rather than left implicit: a server declaring
a `Content-Length` smaller than what it sends would be truncated here. HTTP servers do not do that,
and one that did would already have defeated the JSON parsing. A reply with neither header behaves
exactly as before and waits for the close — so the worst case of this change is the old behaviour,
not a broken one.

### What is left, and why the obvious one is now not worth doing

- **`r=patch` re-downloading an unchanged set is no longer the lever it was.** At 60 seconds it was
  the largest transfer in the boot; at 10.5 it is 3.5 seconds of a ten-second boot. Using the
  per-game cache when the ladder *succeeds* would recover about three of them and would cost a set
  that can go stale between server-side revisions. **Not worth it** — recorded as a decision rather
  than as an open item.
- **The radio is 5 of the 10 seconds** and is dsiwifi's, not ours.
- **`sync=0`** remains the answer for a session where nobody wants to wait at all, and is now a much
  smaller saving than it was.


## An achievement's detail page says when it was earned

`r=startsession` is the only rung in this API that answers the question. `r=unlocks` replies in bare
ids and `r=patch` describes the *set* rather than the account, but a session reply carries objects:

```json
{"Success":true,"ServerNow":1786244358,
 "HardcoreUnlocks":[{"ID":91467,"When":1786166850}],
 "Unlocks":[{"ID":93119,"When":1786243172}, ...]}
```

That reply was already being made, and already being parsed for its ids. The dates were sitting
beside them, unread.

### The path, and why each hop is where it is

| Where | What happens |
| --- | --- |
| `r=startsession`, stage 12 | ids and `When`s extracted into a table; `ServerNow` against the RTC gives this console's offset from UTC |
| `r=patch`, stage 15 | each earned achievement's `#!` record gains a fifth field: the date, packed |
| the bootloader | copies the block into DSi WRAM as it always did |
| `cardenginei_arm9_ra` | reads that field into `raViewerEntry.when` while indexing |
| the in-game menu | unpacks five fields with shifts and prints them |

**The conversion happens in the launcher**, and that is the load-bearing choice. The launcher has a
real clock and a real libc; the in-game menu has neither and has no business owning a calendar. So a
date crosses as 27 bits and the menu does five shifts.

**And it is packed rather than left as an epoch,** which costs the same four bytes. An epoch would
have pushed the calendar arithmetic across the boundary into the binary that cannot afford it.

### Two clocks, and `ServerNow` is what reconciles them

The server answers in UTC. The queue file's stamps are the console's local time. The page shows both
kinds of date side by side — a server-confirmed unlock and one still waiting to sync — so one of them
has to be converted or the page tells the truth twice in two different timezones.

There is no timezone setting anywhere in this fork to consult, and `ServerNow` makes one unnecessary:
it is the server's clock at the moment this console's clock read `time(NULL)`, so the difference
between them *is* this console's offset from UTC, whether or not anyone ever configured it. A console
with a wrong RTC gets wrong dates here — and it already writes wrong stamps into the queue, so
trusting it breaks nothing that was not already broken.

### When there is no date, nothing is printed

Three ways for that to happen, and none of them prints "unknown":

- **The account does not hold it.** The status line above has already said so.
- **It was earned during this session.** The flag flips while the game runs, and nothing in that
  context has a date — `sharedAddr` carries hours and minutes and no calendar at all.
- **The block filled.** The date is the first field `raPatchWriteEarned()` trims, ahead of the
  description, and the only one whose loss is not counted: the other three are text a person came
  here to read.

An empty row says less than a wrong one, and "unknown" beside an achievement a player is looking at
invites the question of what else about it is unknown.

### What it cost

`raViewerEntry` went from 12 bytes to 16, so 128 of them plus the header is 2,060 and the viewer
reservation grew from `0x800` to `0xA00`. That comes out of the definitions block's own 32K, which
the largest measured set uses 26,663 of. `sizeof(raViewerEntry) == 16` is now pinned on the host
beside the reservation check — without it, the growth would have overrun the pending tally directly
above with nothing failing to compile.

`raWhenPack()` refuses anything out of range rather than wrapping it, because a wrong date shown
confidently is worse than no date, and both of its sources are external: a stamp from a file a person
can edit, and a timestamp from a server. Zero cannot collide with a real value — month and day are
1-based, so a packed date always has a non-zero month field.

## `01 of 101` — a set of a hundred and one read as a set of one

Reported on *Chrono Trigger*: the achievements page's header counted the total wrong, and only on
that game. It was never a counting fault. `printDec()` writes exactly the digits it is asked for,
taken from the low end:

```c
void printDec(int x, int y, u32 val, int digits, FontPalette palette, bool main) {
	u16 *dst = ... + y * 0x20 + x;
	for (int i = digits - 1; i >= 0; i--) {
		*(dst + i) = ('0' + (val % 10)) | palette << 12;
		val /= 10;
	}
}
```

The header asked for two cells. Every other set tested has been under a hundred, so two cells were
enough and the field looked correct for a year. *Chrono Trigger*'s is over it, and 101 in two cells
is `01`.

**Widening the field is not the whole fix**, because `printDec()` pads with zeros: three cells turn a
forty-five-achievement set into `045`. `raPrintNum()` counts the digits first and places the number
right-aligned in the field. The screen is cleared at the top of every draw, so the leading cells are
already blank and the number only has to be *placed* — there is nothing to pad with.

And the same defect was one page away. Sync Pending printed its total in **one** cell, so twelve
records waiting read as two. The queue holds up to `RA_QUEUE_MAX` of them.

Both fields are now three and two cells respectively, and the sizes are checked at compile time
rather than believed:

```c
typedef char raCountFitsThreeCells[(RA_VIEWER_MAX_ENTRIES <= 999 && RA_QUEUE_MAX <= 99) ? 1 : -1];
```

That pin is the actual lesson. A field too narrow for its source does not fail — it reports a smaller
number, plausibly, forever, and only a set that crosses the boundary ever exposes it.

### ...and the percentage, on the right of the same line

```
  12 of 101 earned   3 sync  14%
 101 of 101 earned          100%
```

**Queued counts toward it.** An achievement that has fired but not been sent is earned — it is the
same thing the list below marks with a star — and a percentage that ignored it would fall behind the
stars on the page under it. The two counts are disjoint in practice: `QUEUED` is this console's word
for an unlock and `EARNED` is the server's answer, and an unlock crosses from one to the other on the
boot that submits it. Clamped at 100 rather than trusted, because the counts come from a block
another binary wrote.

## The boot screen has two audiences, and `verbose_log` is which one it is for

The launcher's RA ladder narrated everything it did to the screen: stage headings, the SCFG
registers, every line dsiwifi prints on its way up, a heap report between rungs, and a twenty-line
summary. Several hundred lines. That is the right screen for finding out why a boot failed and the
wrong one for a person waiting to play a game.

`verbose_log` in `ra.cfg` picks. **It defaults to 0**, which is the one default in the parser that is
not "behave exactly as before" — a card with no opinion gets the quiet screen, and the verbose one is
a diagnostic mode you ask for.

### The log file is not affected, and that is the point of the key

`verbose_log` governs **the screen only.** `ra_wifi_launcher.log` gets everything in both modes.

That asymmetry is deliberate rather than a shortcut. Every hardware finding in this project arrived
through that file, and a setting that silenced it would mean a reflash before any problem could be
looked at — which is exactly the cost the quiet mode is supposed to remove. As it stands, quiet mode
is safe to leave on forever: when something goes wrong the full story is already on the card.

### What the quiet screen shows

Ten steps, each named for what the player is waiting for rather than for the API call underneath, a
bar, and the essentials underneath once they are known:

```
Downloading achievements
 [######################] 100%
Signed in as <username>
Game found on the server
98 achievements ready
2 sent, 0 still waiting
34 earned before now
```

Eleven steps and five result lines, so the top two lines scroll off by the end. Several hundred lines
became twenty-six, which was the actual complaint.

The two live lines are redrawn in place; the essentials are written once at the end, from `verdict` —
the same structure the verbose summary is written from, so the two cannot disagree about what
happened. They are ordered by what a *failing* boot needs first: "Could not sign in" at the top of
that block is the whole diagnosis for the most common problem this loader will ever have.

A single cell on a row of its own pulses while a rung blocks — `.` `o` `O` `o`, about one
breath a second. It earns its place: association can take forty seconds and DHCP ten more, during
which the bar and the caption are both correct and both still, and a run that has stopped looks
exactly like a run that is waiting.

**It was a rotating `- \ | /` first, and that was the wrong shape.** Four glyphs of different widths
flickering next to a word reads as corruption at this resolution rather than as progress. Pulsing one
dot changes the *size* of a mark that never moves, which is what a progress animation looks like; a
spinning stick is what a terminal looks like. The sequence returns through `o` rather than snapping
from `O` back to `.`, so it breathes instead of ticking, and it runs every sixteenth frame because
faster reads as flicker again.

### Four things about drawing on a DS console that cost more thought than the feature

- **The config is read before anything is printed.** `verboseLog` decides what the screen is *for*, so
  it cannot be learned at stage 0c with three stages already on the screen. The read moved to the top
  of `raWifiProbe()`; stage 0c still reports every field, and the property that made the old ordering
  right is kept — the file is still parsed with no radio up, so a bad config is still a line in the
  log before the network can be blamed for it.

### The fixed layout was attempted five times and abandoned. Here is the record.

The screen prints, one short line per step, through the same `iprintf()` the verbose path has always
used. It scrolls. A fixed layout with the bar pinned mid-screen and the stage message in a reserved
area is what was asked for and is **not delivered** — five attempts, each fixing a real fault without
fixing the screen, and the fifth broke the boot. The faults are all real and are written down here so
the next attempt starts from them rather than from an assumption:

1. **Positioning `PrintConsole`'s cursor and calling `iprintf()` leaves the timing of the write to
   stdio.** A buffer flushed later prints in sequence from wherever the cursor is by then. Reported
   as "the messages are scrolled instead of holding their position".
2. **Writing the last usable cell of a row advances the cursor past the end**, so the console wraps
   and every absolute row addressed afterwards is one out. Reported as "it comes apart from 40% on" —
   40% being where the first rung long enough to animate begins, with the animated character sitting
   at column 31.
3. **Drawing into the tile map directly fixes both of those and still only holds while nothing else
   prints a newline.** Any other `printf` on this console scrolls the map and takes those cells with
   it. A launcher is not a program that can promise nothing else prints.
4. **Repainting every row every frame answers that in theory.** On hardware it produced a black
   bottom screen with nothing on it and a boot that did not finish. Candidate causes, none confirmed:
   a palette read back from `fontCurPal` that resolves to nothing, a map base that is not where
   `consoleGetDefault()` reports, or seven hundred VRAM writes per frame inside a wait loop. **It
   broke the loader, and a progress display is not worth a boot.**

What is kept from all of it: `raWifiScreenInit()` writes the console's geometry and whether the tile
map was found to the log — `console 32x24, map yes`. That is the reading a sixth attempt should start
from, and it is the thing the first five did not have.

What the printed version costs, stated rather than glossed: **there is no animation while a rung
blocks.** Association can take forty seconds, and without a moving character a run that has stopped
looks exactly like a run that is waiting. That is a real gap, left as a gap rather than filled with a
third animation nobody asked for.

And two lines per step rather than one, which is forced rather than chosen: the bar is 29 columns and
the longest caption is 26, so one line carrying both wraps mid-word on a 32-column console.

- **Colour stays an escape in the string**, since printing is what happens now, and colour is the one
  escape sequence in this tree with a hardware record of working. `raWifiVisible()` was retired
  during the tile-map attempt and is not needed here either: nothing pads or centres any more.

`raWifiBar()` and `raWifiCentre()` live in `ra_screen.c` rather than `ra_wifi.c`, for the reason
`ra_wifi_verdict.c` exists: everything else in that file needs a console, a FIFO or a socket, and these need `sniprintf`.
That is not ceremony over four lines of division — **a progress bar is a thing whose bugs are
invisible.** A full bar beside "95%" gets reported as a fault in the loader, a bar that reaches 90%
and stops looks like a hang, and neither would ever fail a build. Both ends of the range are pinned on
the host, along with the rounding that makes the last step read 100%.

## The in-game menu can kill the game when it closes — upstream, and this fork accelerates it

Reported on **Chrono Trigger** and **Super Mario 64 DS**: open the in-game menu, navigate it, choose
Return to Game, the bottom screen comes back to the game, and one to three seconds later the last
sound stretches out and the picture stops for good.

**It is nds-bootstrap's, not this fork's.** An unmodified official release does it too. This fork
raises the rate, which is why it looked like ours for a dozen flash cycles.

### What it is

The ARM9 dies and the ARM7 lives — the stretched, endlessly repeating last sample is the ARM7 still
running while nothing feeds it. No red exception screen, so it is a hang rather than a fault.

The trigger is exclusively the menu. Playing for a quarter of an hour without opening it does
nothing. The menu itself is fine: it opens, it can be navigated, it exits, and the game's picture
comes back before anything goes wrong.

`INGAME_MENU_LOCATION` is not free memory. `loadInGameMenu()` on the ARM7 writes 0xA000 bytes of
what is there to the page file, reads the menu in over it, and `unloadInGameMenu()` reverses that on
the way out. The two to three seconds of stretched audio are that SD traffic, during which the ARM9
is spinning with `IME` at zero — that part is normal and the official release does it too. What is
not normal is failing to recover afterwards.

That region is inside what nds-bootstrap uses to cache the ROM, which fits the delay: the game
resumes, runs on restored memory, and dies when it reaches whatever did not come back the same.

### It is a rate, not a switch, and that is what cost the investigation

Every game fails. **Chrono Trigger and SM64DS take two or three menu cycles; Contra 4 takes many
more**, and when it goes it goes differently — the menu stays on screen and the game never returns.
One failure, two presentations, and a per-cycle probability that varies by how hard the game leans
on the ROM cache.

A bisect was run over 225 commits and converged, confidently, on `67b28dd` — a commit whose only
additions are `clampAddress()` and a range table that **cannot execute unless the RAM viewer is
opened**, which it was not. Rebuilding the previous commit with 176 bytes of dead weight, matching
that commit's size exactly, did not reproduce it.

That contradiction was the tell and it was misread twice: first as a false negative somewhere in the
bisect, then as layout sensitivity. Both were wrong in the same way. **A short negative on a
probabilistic failure is not a negative.** Every "clean" verdict was six to eight menu cycles on a
build whose real rate needed twenty, and the bisect was measuring a gradient — each commit that
added per-frame ARM9 work nudged the probability up. `67b28dd` did not introduce anything; it was
where the rate crossed what six cycles could see.

The same mistake made "it is not upstream" look settled. The official release was tried early, with
the few cycles that were enough for SM64DS on this fork, and passed. It fails at twenty.

### What was ruled out, and none of it was wasted

| Ruled out | How |
| --- | --- |
| This session's work | `065e944`, before any of it, fails identically |
| The network, the fetch, the queue | Reproduces with `sync=0`, radio never up |
| The achievements pages | Reproduces having opened only the root menu |
| `cardenginei_arm9_ra` | Reproduces with a colour LUT on, which stops it being staged |
| The reader's VBlank hook | Reproduces with `raRearmVBlank()` returning immediately, so no RA code runs in-game at all |
| `IgmText`, `IGM_ENTRY`, `IGM_PALS` | All three verified against the linked ELF; both IGM variants share one `.space` |
| `menuItems[9]` | Nine entries maximum, and it holds nine |
| `printDec` on a 3-digit count | Truncates rather than overrunning |
| The IGM binary's size | +176 bytes of dead weight on a passing build changes nothing |
| The IGM's `.bss` reaching its stack | 10.8 KB of margin either side of the culprit commit |
| Symbol alignment | No symbol changes its 4-byte alignment across that commit |
| The ARM9/ARM7 menu handshake | Bounding all four unbounded waits does not stop it |

### Where it points, unverified

Two things the exit path does not put back, both noted without being called the cause:

- **`BG_GFX_SUB` is backed up for `sizeof(igmText.font) * 4` bytes only.** A game using more
  sub-screen tile graphics than that loses the rest permanently.
- `REG_MOSAIC_SUB`, `REG_BLDCNT_SUB`, `REG_BLDALPHA_SUB` and `REG_BLDY_SUB` are zeroed and never
  restored; the source says they are write-only and cannot be read back.
- `bgBak` links at an address that is **not word-aligned** — `0x02F8CF67`, three past a boundary —
  and is the source of a `tonccpy` into VRAM. DS VRAM ignores byte writes, a lesson this project
  already paid for once in the overlay.

None of those hangs a CPU on its own, which is why none is being claimed.

### What this fork should do about it

Nothing on suspicion, and nothing that pretends to fix somebody else's bug.

**Deliberately not reported upstream yet.** Recorded here so that a later reader does not mistake
that for having been forgotten: it is a decision, and it can be revisited whenever the
characterisation above is worth somebody else's time. What would go in such a report is already
written: that it is rate-based rather than deterministic, that the rate tracks how hard the game
leans on the ROM cache, and that the failure is in the resume rather than in the menu.

What is worth carrying regardless is that **this fork raises the rate**, because it adds per-frame
ARM9 work and takes DSi WRAM that would otherwise cache the ROM. That is a real cost of the feature,
and a player who uses the in-game menu heavily will meet it sooner here than on a stock build.

## Status

- [x] Baseline: unmodified nds-bootstrap builds
- [x] Phase 0: per-frame game RAM snapshot — **confirmed on hardware**
- [x] Phase 0.5: text notification over a running game — **confirmed on hardware**
- [x] Phase 1: parameterised watchlist + pointer chains — **confirmed on hardware**,
      on *Space Invaders Extreme* and *Final Fantasy III*
- [x] `cardenginei_arm9_ra`, a separate ARM9 binary in DSi WRAM — **confirmed on
      hardware**. Staged, copied, recognised, called every frame, executing, and
      reporting back. 256K of window with code execution, which retired the cardengine's
      12K as the project's binding constraint.
- [x] Phase 2: `rcheevos` evaluating a **real achievement set** — **confirmed on
      hardware**. Submodule pinned at v12.4.0, runtime only, on our own allocator in DSi
      WRAM, `peek()` routed through the watchlist's own validation. 45 definitions from
      the server, all 45 activated, `rcBadLine` 0. `rc_client` remains ruled out, see
      open question #4.
- [x] Phase 3: **real network, softcore unlocks, closed end to end on hardware.**
      `login`, `gameid`, `startsession`, `awardachievement`, `unlocks` and `patch` all
      answer over plain HTTP; the set is fetched, cached per ROM and staged; already-earned
      achievements are filtered out; the radio comes down and the game boots. rcheevos
      fires inside the running game, the id crosses to the ARM7 through `sharedAddr`, the
      cardengine appends it to `sd:/ra_unlocks.txt` with no launcher and no network, and
      the next boot submits it. **The server registered it** — `rcActivated` fell from 45
      to 44 on the following fetch, which is the submission confirmed from the other end.
- [x] **The notification says which achievement.** Titles ride the staged block as
      `<id>:<memaddr>\t<title>`; the font and the renderer live in `cardenginei_arm9_ra`;
      the overlay draws on **its own sprites**, so no background layer, no game sprite and
      no tile of the game's is taken. Read back on hardware as *"Welcome to the jungle"*,
      stable, with nothing missing from the screen.
- [x] **The VRAM survey reads the BG mode.** The last of the overlay's known ways to
      corrupt a game rather than merely fail to draw on it. Affine and extended-affine map
      sizes and a bitmap's 16K-unit base are all read correctly now, where every `BGCNT`
      used to be taken for a text background. Host-tested rather than hardware-confirmed,
      and deliberately: *Contra 4* runs in BG mode 0, where the old reading is correct by
      accident and the bug cannot fire.
- [x] **An unlock carries when it was earned** — **confirmed on hardware**. The cardengine stamps
      the queue record from the RTC and the launcher sends `o=`, so the server dates an achievement
      by the moment it fired rather than by the boot that reported it. Includes the signature
      change `o=` requires — the id twice and then the seconds. Read back on *Contra 4* as
      `302329  earned 154 s ago` followed by `302329  awarded`, with the account holding it on the
      same boot's `r=unlocks`.
- [x] **The in-game menu shows what is waiting to sync** — **confirmed on hardware**. An
      `Achievements...` entry in the root menu with `Sync Pending` inside it, one line per game:
      the game, how many of its unlocks are still owed, and how long the oldest has waited. Counts
      rather than a list, because achievement titles only exist for the game that is running; days
      rather than a date, because five characters cannot tell the ninth of August from the eighth of
      September and the launcher has the clock anyway. The queue record carries the game it came
      from, read from the ROM's own header, so it works with the radio down.
- [x] **The RAM viewer closes for a hardcore session.** The launcher stages the mode it
      settled on before it touches the radio, on every path out of the ladder rather than
      only the ones that reach stage 13, and the in-game menu refuses to enter edit mode
      when it reads hardcore. Reading memory is untouched. This retired the *unconditional*
      half of `raWifiHardcoreRefused()`: the gate now refuses for a cheat file and for
      nothing else. **Confirmed on hardware**: with `hardcore=1` the viewer refuses to
      edit, and the menu's own line reports the mode.
- [x] **The menu says which mode the session is in, and why.** A line in the
      RetroAchievements folder: `Hardcore -- RAM editing locked`,
      `Softcore -- cheats are on`, `Softcore`, or `No session this boot`. The reason is the
      half that earns it — before this, a player who asked for hardcore and got softcore
      could only find out why by reading the launcher's log on a PC. **Confirmed on
      hardware.**
- [x] **The cheat check asks whether cheats are *on*, not whether a file exists.** It was
      `conf->cheatSize != 0`, which missed wide cheats and AP patches that are really cheat
      files — both of which run the engine — and which refused hardcore to a player who had
      opened the cheat screen once and turned everything off. It is now the same sum
      nds-bootstrap builds `cheatSizeTotal` from, against the same floor, and the bootloader
      clears the staged session's hardcore flag if it installs the engine after all.
- [x] **The queue parser survives the record the cardengine actually writes** —
      **confirmed on hardware**. `gameTitle` is a fixed twelve bytes with NUL padding, and
      the parser used to stop on the padding, lose every field after it and hand the
      leftovers to the outer loop as an achievement id. It submitted one. Two gates now:
      step over the padding, and never read past a stamped record's newline.
- [x] **A queued unlock carries the mode it was earned in** — **confirmed on hardware**:
      two records earned in a hardcore session, both read back as hardcore, both submitted
      as such. `h=` and the signature come
      from the record, not from `ra.cfg` at submission time, so earning in softcore with
      the RAM editor open and then setting `hardcore=1` no longer upgrades anything. The
      mode crosses to the ARM7 in the unlock request's own magic rather than in a slot of
      its own, and `raWifiSubmitOne()` lost its config parameter to the change. Cost: the
      TWL-SDK ARM7 is down to 60 bytes of link margin. **Built and host-tested; not yet
      confirmed on hardware.**
- [x] **Chrono Trigger runs** — **confirmed on hardware**, and it is a compatibility gain rather
      than an RA feature. It froze on entering Leene Square, every time, with no menu involved; it
      now passes it and continues into the story, the world-map tearing is down to what the official
      release does, and the character-text region is legible. The reader's per-frame evaluation was
      overrunning the game's blanking period — `rcLinesMax` 101 against a hardware ceiling of 71 —
      and the fix is to evaluate the trigger loop in self-tuning slices (`rcParts`) rather than to
      schedule work that fits no schedule. Three hypotheses died to measurements on the way: the
      window was never uncached (`raMpuBits` 0xD9), the bus was not stalling (`rcMemrefMin` 45 of
      47), and the alarming "845 cycles per read" was a division by the wrong count. Cost: a trigger
      is visited every eighth frame, which is inside the 8% of frames this project already shipped
      on without missing an unlock. No other game changed, and nothing was disabled.
- [x] **The queue write no longer hangs the game it is queueing for.** *Ketsui* froze on the boss
      kill that fires an achievement, four times, and the overlay was suspected for four rounds and
      was innocent — `overlay=0` froze identically. `queue=0` did not freeze, which named the append,
      and the mechanism is that the ARM7's critical section protects against a card read *starting*
      underneath the write but not against one already suspended mid-command: the ROM read path is
      resumable, so between `fileReadNonBLocking()` and `resumeFileRead()` the game's read is one
      outstanding sdmmc command plus 512 bytes of a shared buffer, and IME off protects neither. That
      one collision accounts for the freeze, for `ra_unlocks.txt` coming back holding the game's own
      credits text, and for the empty queue on the last hang. The append now waits for `readOngoing`
      to clear on the ARM7, and the ARM9 does not raise the request while a read of its own is
      unserved. Cost: 16 bytes on the tightest binary in the tree, and the split across two CPUs is
      because a single-sided version was four bytes too big. **Hardware says this was not the cause**
      — it froze again on the same boss, and the record that session wrote came back complete, so the
      append is not hanging. The gate is correct and stays; the freeze is a side effect of the work
      rather than a stall inside it, and `queue` is now a ladder that bisects which part.
- [x] **The queue write no longer freezes the game.** Bisected with the `queue` ladder rather than
      guessed at: `3` and `2` clear Ketsui's boss, `1` freezes, and `2` differs from `1` in exactly
      one call — reading the RTC. `rtcGetTimeAndDate()` bit-bangs about a millisecond of serial bus
      inside the VBlank handler with interrupts masked, either starving the ARM9's card reads or
      landing on top of a transaction the game's own ARM7 had in flight. The clock is now read once in
      `initialize()`, before the game has a main loop to interrupt. Cost: every unlock in a session is
      stamped with the session's start, which is a real regression against exact times and still far
      better than the reporting boot's date. **NOT confirmed** — see the retraction below. The boss
      kill itself is survived; the game still freezes at the end of the boss.
- [x] **The queue write no longer freezes the game** — **confirmed on hardware**. The overlay was
      exonerated by switching it off entirely and watching Ketsui freeze anyway; what was left was
      the ARM7 opening the SD card from a VBlank handler mid-transition. Unlocks are now held in
      memory and written when the in-game menu opens or the game is quit. `queue=1` on the boss that
      had ended six sessions: no freeze, unlocks written. NTR titles only — deferring did not fit
      the TWL-SDK ARM7's forty-four spare bytes.
- [ ] **Hardcore.** Blocked on nothing in this tree any more, and now measured rather
      than inferred: `h=1` from this client returns `Success:true` and is filed as a
      **softcore** unlock — hardcore score unchanged, `HardcoreUnlocks` empty,
      `r=unlocks&h=0` returning it. The *"Warning: Unknown Emulator"* notice is telling
      the truth and enforces it by silent downgrade. What it needs is
      `nds-bootstrap-ra/0.1` recognised by RetroAchievements, which is a conversation
      rather than a commit.
- [ ] Phase 4: rich presence, achievement list, login status

## What is left

Ordered by value rather than by effort, and the first item is not code.

### 1. Get the User-Agent recognised — now genuinely the only thing blocking hardcore

The server's own notice says it blocks **hardcore only** for an unrecognised client, so softcore works
today and hardcore is a registration away. Until recently that was half true: the client was also
disqualifying itself, because the in-game menu could edit memory. It no longer can in a hardcore
session — see "The RAM viewer closes for a hardcore session" — so the sentence in this heading is
now literal.

Three questions belong in that conversation rather than being decided here, and the second one
gates the third:

- **What the registration process is** for a client that is not an emulator.
- **Whether deferred submission is sanctioned.** This client cannot reach the network while a game is
  running — measured and documented under "In-game networking, reopened and then closed by
  measurement" — so an unlock is queued to the SD card and sent on a later boot, sometimes a day
  later. `o=` already carries when it was earned rather than when it was sent, so the server dates it
  correctly, but the submission itself is still out of band compared with every emulator
  integration. It is better raised than discovered.
- **Whether a queued unlock may be submitted in a mode chosen after it was earned.** ~~Ours
  currently can be~~ — fixed, see "A queued unlock remembers the mode it was earned in". The record
  now carries the mode and `h=` is taken from it, so flipping `hardcore=1` between the boot that
  earned an unlock and the boot that sends it no longer upgrades it. Worth mentioning to them
  anyway: it is the sort of thing a client should be able to say it handles, rather than be asked.

A draft of the message is in `docs/ra-registration-request.md`, kept in the tree so the questions
asked and the answers given end up in the same place as everything else this project has had to
learn the hard way.

### 2. ~~`o=` on `r=awardachievement`, so the timestamp is when it was earned~~ — done

An unlock used to carry the time it was *submitted*, which with this client can be a day later. It now
carries when it was earned. See "The unlock remembers when it happened".

**Two things in that item were wrong, and both cost more than the rest of the work.** The RTC is *not*
usably in `sharedAddr[7]/[8]` — those hold hours and minutes only, and only while the in-game menu is
open, because the menu is what fills them. The cardengine reads the RTC directly instead. And the change
is not confined to adding a parameter: sending `o=` **changes the signature**, because rcheevos appends
the id a second time and then the seconds. `o=` with the old digest is refused, and refused with a
message that says nothing about which half was wrong.

### 3. ~~The in-game log reader~~ — superseded, and three smaller things in its place

The launcher's log on screen was the original item. What got built instead was **Sync Pending**, which
answers the question that actually comes up while playing — *did the achievement I just earned get
queued* — and needed the same plumbing. The log reader is still possible by the clean route (hand the
menu the file's cluster) and is no longer the interesting half.

What the work left behind, all confirmed and none of it fixed:

- **`ndsHeader` is not stable during play.** The queue record reads `gameCode`/`gameTitle` from
  `0x027FFE00` at the moment an achievement fires, and the game reuses that memory once it is running.
  Proven by the pairing: an unlock seconds into the game carries `YCTE`/`CONTRA 4`, one at a stage end
  minutes later carries neither. The fix is to capture both fields once at cardengine init.
- **The queue file never migrates when it is empty.** `raWifiSubmitQueue()` returns early on an empty
  queue without rewriting, so a file created at an older `RA_QUEUE_RECORD` keeps its old length
  forever — `queue empty (2048 bytes read)` on a build whose records are 48 bytes.
- **`raPendingBlock` sits where it does by argument, not measurement.** It is in the top of the
  definitions' reservation, out of the heap, which is right. The hardware evidence that once pointed
  elsewhere was the build-mode fault and is void.

### 3b. ~~The in-game log reader~~ — dropped

Showing `ra_wifi_launcher.log` in the in-game menu, by handing the menu the log file's cluster the way
`ra_unlocks.txt` is handed over. **Not going to be built**, and the reason is worth keeping because it
is not "we ran out of time".

The item only ever existed to avoid a physical cost: reading the log meant powering down, taking the
card out, putting it in a PC, and putting it back. That is what made a two-line reading expensive, and
it is what the feature was buying back.

Both halves of that have since gone away. **Sync Pending answers the question that actually comes up
while playing** — is what I just earned safe, and how much is waiting — which is the only thing anyone
was really opening the log for mid-session. And the card no longer has to move: reading it over FTP
after quitting costs nothing, which removes the rest of the reason.

What is left over is genuine debugging, and that is not a mid-game activity. It wants the whole file
on a screen with a scrollbar, not thirty-two columns of a DS panel.

### 4. ~~`surveyBlocks()` is still mode-blind~~ — done

It read every `BGCNT` as though it were a text background, ignoring the BG mode and the colour-depth
bit, so for an affine or bitmap background the character and screen bases meant different things. It
could both miss a block the game was using and mark one it was not.

**Fixed.** `raOverlaySurvey()` reads the BG mode, distinguishes text from affine from extended, and
takes a bitmap's base in 16K units with its depth from bit 2. Tested on the host rather than on
hardware, because *Contra 4* runs in BG mode 0 where the old reading is correct by accident and the bug
cannot fire. See "The survey learns to read the BG mode".

What remains is smaller and is written down there: **tile data is still marked as a single 16K block**,
which is a heuristic and not a reading, because how far a character base reaches is not in any register.

### 6. The in-game achievement viewer — the launcher half is **confirmed on hardware**, the menu half is not

Contra 4, GameID 12917, on a 3DS, with two achievements earned and submitted in the boots before:

```
definitions      43 kept, 0 unofficial      <- 45 in the set, less the 2 earned
titles           43 with, 3 clipped
desc / points    43 / 43 with
earned shown     2
already earned   2 of 3 matched this set
block            9662 of 32247 used, 9662 wanted
def 1    46      #!302329	Welcome to the Jungle	3	Clear Stage 1
```

Every claim this format was designed on, measured rather than argued:

- **The 64-byte description cap is right.** 43 of 43 descriptions survived with none clipped; the
  longest in the set is 57 characters. The graded degradation never fired, which is the correct
  outcome for a set at 30% of the block rather than evidence that it works -- the 56-definition set
  measured at 87% is still the case that needs it.
- **Moving an achievement to display-only is what made "earned as well as pending" affordable.**
  `302329` was 120 bytes armed and is 46 bytes earned, and the block fell from 9,924 to 9,662 for
  two of them. The memaddr is four fifths of a record and the earned half does not carry one.
- **`wanted` equals `used`**, so nothing was traded away.
- **The title cut works**: the notification read `ACHIEVEMENT / Welcome to the Jungle`, with no
  points or description trailing it.

Two things were confirmed on the way that belong to the branch before this one. The orphaned-set
discard fired for the first time on real hardware -- `staged definitions discarded: they cannot be
this ROM's`, on Arkanoid DS -- and two queued unlocks crossed from a supported game to an
unsupported one and came back `AchievementsRemaining` 44 then 43 with `SoftcoreScore` 457 then 460,
which is the server saying it recorded them rather than merely `Success:true`.

What is missing is the page itself, and it is blocked on one decision that is worth stating rather
than discovering.

**The menu cannot parse the block, and the reason is that the block is mutated in place.**
`ra_split_definitions()` turns a record's newline into a NUL so rcheevos gets C strings, and turns
the tab after the memaddr and the tab after the title into NULs for the same reason. So by the time
a player can open the menu, the block looks like

```
<memaddr>\0<title>\0<points>\t<description>\0
```

and the NUL that ends the record is indistinguishable from the two that came from tabs. Every scheme
for recovering the boundaries from the bytes alone is a heuristic -- "a record starts with `#!`, or
with digits-then-colon, or with something memaddr-shaped" -- and this project has a section about
what heuristics cost.

**So `cardenginei_arm9_ra` should publish a viewer index**, built at init while the block is still
pristine, in the definitions' own reservation beside `raPendingBlock`: one entry per achievement
with the id, the offsets of the three text fields, and a flag for earned. 128 entries at 12 bytes is
1.5 KB of the 32 KB reservation, it needs no heuristic, and it is the pattern
`CARDENGINEI_ARM9_RA_PENDING_LOCATION` already established.

Three things then have to move together, and the third is the one that has bitten this project
twice: the index needs its own constant in `locations.h`, the menu needs its label in `IgmText`, and
**`sizeof(struct IgmText)` has a hand-written mirror in `arm9_igm/source/card_engine_header.s`** as a
`.space` directive that no compiler checks. Growing the structure without growing that number is
what produced the black-screen-then-TWiLight crash the first time the RetroAchievements folder was
added.

### 7. Rich presence — measured, and the answer is "most games, not all"

Two halves, and only one is blocked. **Rendering** a script is reachable: it is memory reads against
a script, which this fork already does sixty times a second, with rcheevos here and an arena that
has margin. **Sending** it is not, for the reason written down under "In-game networking, reopened
and then closed by measurement" -- the game's boot kills the link and dsiwifi's ARM7 half wants
104,148 bytes against 12,636 free.

So the question for the reachable half was never whether a script can be evaluated. It was whether
it can be **stored**. One counter and three boots answered it:

| game | set | RP script | total | of the 30,199-byte block |
| --- | --- | --- | --- | --- |
| Ketsui Death Label | ~4,000 | 1,786 | 5,786 | 19% |
| Contra 4 | 9,662 | 1,768 | 11,430 | 38% |
| **Super Mario 64 DS** | 28,585 | **14,115** | 42,700 | **141%** |

An eight-fold spread, and the two extremes are the same game. Worse: Mario 64 DS's 28,585 is the set
*without* titles, points or descriptions -- with them it passes 33,600 and overflows the block on its
own. Its arena is at 91% too (128,352 of 140,852), so there is no room to move it to either.

**So the policy is fit-or-drop, counted and logged.** A script that fits is stored and rendered; one
that does not is refused whole, because a truncated Rich Presence script is not a shorter script --
its lookup tables are what make up most of its length, and rcheevos would refuse to parse the
remainder. Two of the three games measured get rich presence; the largest does not, and the log says
which.

The path that would give it to Mario 64 DS as well is worth naming rather than pretending it does not
exist: **put the script on the SD instead of in the block**, and hand the cardengine its cluster the
way `ra_unlocks.txt` is handed over today. Then it never competes with the definitions. The cost
moves to the arena at parse time, which for the one game that needs it is already at 91% -- so that
route needs its own measurement before it is a plan.

### 5. The deferred graphical limitations

Catalogued above with what the sprite path changed about each. Items 1 (the in-game menu collision) and
4 (no say over which physical screen the sub engine feeds) are untouched and both need work outside the
overlay.

### Not on this list, and deliberately

- **`rc_client`.** Ruled out with a measurement, open question #4. Nothing has changed that.
- **In-game networking.** Closed by measurement, not by preference: the link dies seconds after the game
  boots, and `SCFG_EXT7 BIT(18)` survives, so the SDIO enable is not the cause.
- **Token caching.** The password stays in the plaintext config file in odelot's format, by explicit
  decision, which means `r=login` sends it in the clear over plain HTTP on every boot. Neither the
  password nor the token is ever written to `ra_wifi_launcher.log`, because that log is shared:
  `raConfigRedact()` emits only `(set, N chars)` and `raWifiLogBody()` strips the token from any logged
  reply body.
