<p align="center">
   <img src="logo.png">
</p>

# nds-bootstrap-ra

**A fork of [nds-bootstrap](https://github.com/DS-Homebrew/nds-bootstrap) that adds
RetroAchievements support on real DS hardware.**

This is *not* the upstream project. It tracks `DS-Homebrew/nds-bootstrap` and adds
one thing on top: achievements that unlock against the RetroAchievements servers
while you play on the console, with no emulator involved. Development and testing
happen on a Nintendo 3DS running DS games natively in DS mode.

**The RetroAchievements support targets the 3DS family only.** On a DSi, or on a DS
through a flashcard, this fork behaves exactly like upstream: no reader, no overlay, and
no VCOUNT interrupt forced on. That is a deliberate scope decision, not a limitation
waiting to be lifted — the two things this fork does to a running game are the kind that
surface as intermittent oddities rather than crashes, and no other console has been
tested. See [docs/retroachievements.md](docs/retroachievements.md) for the reasoning.

If you want plain nds-bootstrap, use
[the upstream project](https://github.com/DS-Homebrew/nds-bootstrap) instead —
it is maintained, this fork is experimental.

## What the fork adds

nds-bootstrap is not an emulator: it loads a DS ROM and runs it natively, with a
**cardengine** injected into the game's own address space. That injected code is
what makes RetroAchievements possible here — reading the game's RAM is just a
pointer dereference.

The work is split into four deliberately separate modules:

| Module | Responsibility | Status |
| --- | --- | --- |
| `ra_reader` | Read the game's RAM every frame: a watchlist with pointer chains, re-resolved each frame. Knows nothing about RetroAchievements. | working on hardware |
| `ra_overlay` | Draw a notification over the running game. Knows nothing about RetroAchievements either. | working, needs a real font |
| `ra_client` | Wrap `rcheevos`' `rc_client`; evaluate conditions, fire unlocks. | not started |
| `ra_net` | HTTP(S) transport to the RA servers. | not started |

Current state, what to do next, how to observe the reader on hardware, and the open
design questions are all in **[docs/retroachievements.md](docs/retroachievements.md)** —
start with *Where this stands, and what to do next* at the top of it.

`RA_READER_ENABLED` in `retail/common/include/ra.h` is a kill switch: set it to
`0` and the cardengine behaves exactly like upstream.

## Base functionality

Everything upstream does still works, since this fork only adds to it. In short:
DS/DSi ROMs and homebrew run natively from an SD card on DSi/3DS through CFW, or
through flashcards on a DS. Saving is supported (`.sav`, or `.pub`/`.prv` for
DSiWare), as are cheats and faster load times than a real cartridge on games that
support them. Anti-piracy patches can be supplied as IPS files but are not
included here.

B4DS mode — nds-bootstrap running on DS-mode flashcards with locked SCFG, or on a
DS Phat/Lite — supports most DS ROMs that work on DSi/3DS. The RetroAchievements
work currently targets the DSi-capable `cardenginei` path only.

For upstream ROM compatibility questions, consult the upstream project; this fork
does not maintain a separate compatibility list, and any ROM issue you hit should
be reproduced against upstream before reporting it here.

## Compiling

You need devkitARM with the Nintendo DS libraries. **The toolchain version
matters:** this builds against **libnds 1.8.0** (devkitARM r65, as pinned by
`devkitpro/devkitarm:20241104`). libnds 2.x removed `nds/fifocommon.h`,
`nds/fifomessages.h`, `nds/arm7/clock.h` and `sec_t`, all of which are used here,
so a current toolchain will not build this.

1. Install devkitPro's `pacman` as described on the
   [devkitPro wiki](https://devkitpro.org/wiki/Getting_Started), then install the
   DS libraries:
   ```
   sudo dkp-pacman -S nds-dev
   ```
   (the command varies by OS; `sudo` may not be needed, and it may be plain
   `pacman`)
2. Clone this repository and enter it:
   ```
   git clone https://github.com/Bakkerrrs/nds_bootstrap_ra.git
   cd nds_bootstrap_ra
   ```
3. Build the `lzss` **host** tool into your PATH — without it every `.lz77` target
   fails with `Error 127`:
   ```
   gcc lzss.c -o /usr/local/bin/lzss
   ```
   On Windows it must instead be `lzss.exe` in the repository root.
4. Build:
   ```
   make
   ```
   Output lands in `retail/bin/` and `hb/bin/`. Use `make package-nightly` to
   collect both into `bin/`.

**Build serially.** `make -j` races: sub-makes link before their dependencies
exist, giving errors like `cannot find arm9mpu_reset.o` or `cannot find my_fat.o`.

## Frontends

A frontend is not required — nds-bootstrap reads its parameters from an ini file —
but it is strongly recommended.

[TWiLight Menu++](https://github.com/DS-Homebrew/TWiLightMenu) configures
nds-bootstrap automatically, with per-game settings.
[Forwarders](https://wiki.ds-homebrew.com/ds-index/forwarders) let you launch
games straight from the DSi or 3DS HOME Menu; hold <kbd>Y</kbd> while loading one
to edit its per-game settings.

Both were built for upstream nds-bootstrap. They work with this fork because the
ini format is unchanged, but nothing here is coordinated with them.

## Licence and attribution

GPL-3.0, inherited from nds-bootstrap — see [LICENSE](LICENSE). All upstream
copyright notices in the source files are kept intact, as the licence requires.

This fork is built on [nds-bootstrap](https://github.com/DS-Homebrew/nds-bootstrap)
by the DS-Homebrew project and its contributors; see the upstream repository for
its authorship and credits. It also uses devkitARM and libnds from
[devkitPro](https://devkitpro.org), and intends to use
[rcheevos](https://github.com/RetroAchievements/rcheevos) (also GPL) for
achievement evaluation.

Not affiliated with or endorsed by RetroAchievements, DS-Homebrew, or Nintendo.
Intended for use with games you own.
