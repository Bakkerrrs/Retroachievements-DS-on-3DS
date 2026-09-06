# v0.1.0 — RetroAchievements for DS games on a 3DS

First release. A fork of nds-bootstrap that evaluates RetroAchievements sets inside a running
retail DS game, unlocks them, and reports them to the server — from the console itself, over its
own WiFi, with no PC in the loop.

**Asset:** `nds-bootstrap-release.nds` · `md5 756504ba14330509122e22db87dd7f03` · 2,007,552 bytes

## Install

1. Copy `nds-bootstrap-release.nds` over your existing `nds-bootstrap.nds`
   (TWiLight Menu++ keeps it at `sd:/_nds/nds-bootstrap.nds`; keep a backup of the original).
2. Copy `tools/ra.example.cfg` to `sd:/_nds/nds-bootstrap/ra.cfg` and fill in `username=` and
   `password=`.
3. Launch a DS game the normal way. The boot screen shows the login, the set download and the
   sync; it takes about ten seconds with an access point in range.

Your RetroAchievements password is stored in that file in plain text and sent in the clear on
every boot. `dorequest.php` answers over plain HTTP, and that is what makes any of this possible
on a console with no TLS. It is a real cost, chosen deliberately — do not use this on a shared
card.

## What works

- The achievement set for the running game is fetched at boot, already-earned ones filtered out,
  and the rest evaluated **every frame inside the game**, by rcheevos v12.4.0 running in a
  separate ARM9 binary in the DSi WRAM window.
- Unlocks are submitted to the server, and confirmed from the other end: the count of active
  definitions drops on the next boot.
- An unlock earned with no network is written to `sd:/ra_unlocks.txt` and submitted by a later
  boot, so a session away from WiFi still counts.
- The achievement's own name is drawn over the running game when it unlocks.
- An `Achievements...` menu inside the in-game menu: the full set with what you have earned, the
  overall percentage, when each one was earned, and what is still waiting to sync.
- `hardcore=1` locks the in-game RAM editor for the whole session and records the mode in every
  unlock, so a later boot cannot upgrade a softcore unlock.

## What does not work yet, stated plainly

**Hardcore is not accepted by the server.** Everything on this side is done, but
RetroAchievements does not recognise this client's User-Agent and files `h=1` unlocks as
**softcore** — measured on a real account, not assumed: the server answers `Success`, the hardcore
score does not move, and the achievement comes back in the softcore list. Getting the client
recognised is a conversation, not a commit. See `docs/ra-registration-request.md`.

**The on-screen notification is unreliable on some games.** It draws on sprites where it can find
free ones and falls back to borrowing a background layer where it cannot. On *Contra 4* and
*Chrono Trigger* it is correct and stable. On *Ketsui* it currently shows corrupted graphics where
the text should be. Under investigation; the reasoning is in `docs/devlogs/`.

**Unlock timestamps are the session's start, not the moment earned.** Reading the console's clock
on the frame an achievement fires was found to freeze at least one game, so the clock is now read
once at boot. An hour into a session that is an hour of error. It is still far better than the
alternative it replaced — an unlock reported by a later boot used to be dated by *that* boot,
which can be the next day.

**One known compatibility regression is open.** *Ketsui* can still freeze at a stage transition.
The investigation is in `docs/devlogs/2026-08-23.md`; `queue=3` in `ra.cfg` is a diagnostic that
avoids it at the cost of not saving unlocks.

## Requirements

A 3DS with TWiLight Menu++ and a retail DS ROM on the SD card, and an 802.11b access point the
DSi WiFi stack can associate with (WPA2 works; the radio is brought up only for the boot and then
shut down before the game starts).

## Reproducing this build

`bash tools/ra_release.sh` from a clean checkout with devkitARM and `lzss` on `PATH`. The script
cleans first, refuses to ship a build that is not a network build, checks the in-game menu's one
unchecked size mirror, and runs the host test suite — 987 checks — before it will produce a ROM.

The md5 above will only match if you build at the same absolute path: dsiwifi's lwip compiles
`__FILE__` into 26 assertion strings, so the source path ends up in the ROM, and because the
payloads are LZ77-compressed a few changed bytes cascade into most of the file. Same path, same
bytes.

## Credits

Upstream [nds-bootstrap](https://github.com/DS-Homebrew/nds-bootstrap) is by RocketRobz and
contributors, GPL-3.0, and this is a fork of it. [rcheevos](https://github.com/RetroAchievements/rcheevos)
is RetroAchievements' own, vendored as a submodule at `2ad0b86` and never modified. The
configuration file's shape follows odelot's RetroAchievements work on the MiSTer cores, so a file
written for that loads here unchanged.
