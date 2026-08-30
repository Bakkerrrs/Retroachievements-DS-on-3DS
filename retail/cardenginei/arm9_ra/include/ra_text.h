/*
    RetroAchievements support for nds-bootstrap -- the notification's typography (DSi WRAM).

    Turns two strings into a ready-made strip of 4bpp background tiles that the ARM9 cardengine's
    overlay can copy straight into borrowed VRAM.

    It lives here rather than in the overlay for one reason, and the reason is a measurement: the
    ARM9 cardengine's 12K window has **24 bytes free**, and it has already rejected a debug field of
    four bytes and a diagnostic mode of fifty-six. A font for printable ASCII is 760 bytes. There was
    never a version of this that fitted next to the code that draws it.

    So the work is split where the space is. This side owns the font, the character lookup, the
    alignment, the drop shadow and the bit expansion -- all of it in a 256K window with room to
    spare -- and hands over pixels. The overlay keeps every line of its borrow-and-return
    negotiation with the game, which was expensive to get right, and loses only the part that knew
    what letters look like.

    This file is part of nds-bootstrap and is licensed under the GPL-3.0,
    the same terms as the rest of the project.
*/

#ifndef RA_TEXT_H
#define RA_TEXT_H

#include <nds/ndstypes.h>
/* RA_TEXT_* live there because both binaries compile from them. See the note beside raSnapshot. */
#include "ra.h"

/*
    Render two lines and return the strip.

    Each line is right-aligned against RA_TEXT_MARGIN and drawn twice: once as a drop shadow in
    RA_TEXT_SHADOW, a pixel right and a pixel down, then as ink in RA_TEXT_INK on top. The overlay
    borrows a palette entry for each. Without the shadow the glyphs are white on whatever the game
    happens to be drawing, which is legible only where that happens to be dark.

    Either line may be NULL or empty, which renders as blanks -- a notification with no title is
    still a notification. Text wider than RA_TEXT_COLS - RA_TEXT_MARGIN is clipped rather than
    wrapped: the launcher already limits a title to what fits (RA_PATCH_TITLE_MAX), so this is the
    backstop for a hand-written file rather than the normal path.

    The returned pointer is to static storage that stays valid until the next call, which is what the
    overlay's deferral needs: a notification may wait up to 90 frames for the screen to be worth
    drawing on, and the text has to still be there when it is.
*/
const void* ra_text_render(const char* line1, const char* line2);

#endif
