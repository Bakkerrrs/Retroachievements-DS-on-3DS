/*
    RetroAchievements support for nds-bootstrap -- game RAM reader.

    Runs inside the ARM9 cardengine, which is injected into the game's own
    address space, so reading the game's RAM is just a pointer dereference --
    the same trick the in-game menu's RAM viewer uses.

    This file is part of nds-bootstrap and is licensed under the GPL-3.0,
    the same terms as the rest of the project.
*/

#ifndef RA_READER_H
#define RA_READER_H

#include "ra.h"

#if RA_READER_ENABLED

/*
    Everything this fork does per frame, from the VCOUNT interrupt handler: the
    notification overlay, then cardenginei_arm9_ra, which evaluates the watchlist.

    Pass ce9->consoleModel; the work is skipped on anything but the 3DS family, which is
    the only target -- see the scope note in docs/retroachievements.md. Pass wramLoaded
    from the bootloader's flag; the window is verified again here before anything in it is
    called.

    Runs in an interrupt handler, so it must stay short and must not block.
*/
void ra_tick(u8 consoleModel, bool wramLoaded);

/*
    The watchlist API moved to cardenginei_arm9_ra along with the watchlist. ra_watch_add()
    and ra_watch_clear() live in retail/cardenginei/arm9_ra/source/cardengine.c, which is
    also where phase 2's client will call them from -- there is no reason for a request to
    cross into the cardengine and back out again.
*/

/*
    The watch results and the reader's own diagnostics, in the cardengine's .bss.

    Exposed directly rather than behind an accessor because there is nothing an
    accessor could add: the buffer is at a fixed link-time address, it is what
    tools/ra_snapshot_addr.sh reports and what the in-game menu's RAM viewer is
    pointed at, and in a module with a few hundred bytes of headroom a function that
    only returns &buffer is not worth its own code.

    Do not read it without checking magic[] first -- .bss here is never zeroed.
*/
extern raSnapshot raSnapshotBuffer;

#endif /* RA_READER_ENABLED */

#endif /* RA_READER_H */
