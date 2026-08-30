# Devlogs

The working record of how this fork was built: **255 entries across 17 days**, in order.

This project was developed against hardware it could not debug — a DS game running on a 3DS, with no
console, no debugger and no breakpoints. The only instrument was a snapshot of the reader's own state
in main RAM, read by eye through the in-game menu's RAM viewer, one photograph at a time. A wrong
guess cost a card flash and a boss fight.

So the reasoning was written down as it happened, at the moment each thing was measured, and that
record is what this folder is. It is not a changelog. Each entry says what was tried, what the console
answered, what the numbers were, and — often — which idea the evidence killed. **The retractions are
kept.** Several conclusions in here are corrected by later ones, and the corrections are left in place
rather than tidied away, because how a wrong answer was found is worth as much as the right one.

If you are looking for the current state of the design rather than how it got there, read
[`../retroachievements.md`](../retroachievements.md) instead. That document is the destination; this
folder is the road.

## Index

| Day | Entries | Opens with | Ends with |
|---|---|---|---|
| [`2026-08-04`](2026-08-04.md) | 24 | Add RetroAchievements game RAM reader (phase 0) | Document the on-screen notification, confirmed working on hardware |
| [`2026-08-05`](2026-08-05.md) | 3 | Phase 1: read a watchlist with pointer chains, not one fixed window | Document the exact snapshot offsets to read on hardware |
| [`2026-08-06`](2026-08-06.md) | 13 | Phase 1 confirmed on hardware, and open question #2 answered | Write the loader for cardenginei_arm9_ra: stage, copy into WRAM, set the flag |
| [`2026-08-07`](2026-08-07.md) | 9 | The WRAM binary runs on hardware, first attempt | Our own allocator, because newlib's does not work in this window |
| [`2026-08-08`](2026-08-08.md) | 43 | The snapshot lives in a RAM mirror, so its console address needs one | Step 4, offline: run the server's 56 definitions, and fix the clock first |
| [`2026-08-09`](2026-08-09.md) | 56 | Step 4 crashed. Three hypotheses measured and killed, one real bug found | Say which text was on screen when, not just that the title appeared |
| [`2026-08-10`](2026-08-10.md) | 18 | It is only ever visible on BG0, and now it says so instead of drawing into nothing | Fix the in-game menu crash: an unchecked mirror of sizeof(IgmText) |
| [`2026-08-11`](2026-08-11.md) | 2 | Shorten the menu labels and stop the folder repeating its own item | Share the queue scratch: a second copy cost 8 KB of the launcher's heap |
| [`2026-08-12`](2026-08-12.md) | 5 | Diagnostic: put arm9_ra back to what cb14541 had | Publish what the definitions block actually contains |
| [`2026-08-13`](2026-08-13.md) | 6 | Take the pending tally out of the heap again, now that the evidence is void | Take the game's identity at init, and keep the queue file at its full length |
| [`2026-08-15`](2026-08-15.md) | 4 | Document sync and submit in the example config | Clear the pending block's staging word in the launcher |
| [`2026-08-16`](2026-08-16.md) | 29 | Write down the test rig, and what it let us rule out | Write down the menu freeze, and the five things it is not |
| [`2026-08-17`](2026-08-17.md) | 3 | The menu freeze is upstream's, and it is a rate rather than a switch | The RAM viewer lock and the menu's mode line are confirmed on hardware |
| [`2026-08-20`](2026-08-20.md) | 2 | The reader spends too much of the game's VBlank, and two games say so | Budget the reader's frame against what is left of the blanking period |
| [`2026-08-21`](2026-08-21.md) | 12 | Decide before the work whether the frame has room for it | Pulse one dot instead of spinning a stick |
| [`2026-08-22`](2026-08-22.md) | 17 | Centre the quiet screen, and stop writing the console's last column | The object VRAM survey counted a parked sprite bank as free |
| [`2026-08-23`](2026-08-23.md) | 9 | overlay=0, and fix the release guard that let a failing suite ship | Retract "confirmed": the queue=1 run had not finished, and the game freezes at the end of the boss |
