# Drawing an achievement notification from TwlBg

**A proposal to the TWPatcher / RTCom authors, with the DS half already built.**

## What this asks for

A patched `TwlBg` that, once per unlock, draws a short line of text over the DS picture on its way to
the 3DS's screens — and nothing else. No input interception, no scaling change, no game-specific
patch. The data is already published by the DS side; what is missing is the drawing.

## Why it has to be there and not here

This project puts RetroAchievements on retail DS games running under nds-bootstrap. Detecting an
unlock works. Telling the player about it, on the DS side, does not — and after six rounds on
hardware the reason turned out to be structural rather than a bug.

The overlay has to borrow the running game's own hardware: a background layer, a 16 KB character
block, a sprite slot, two palette entries. Which of those is free is *inferred* from registers the
game rewrites every frame, and the inference is sometimes wrong. When it is wrong, the game's
graphics are corrupted.

On *Ketsui Death Label* it is not merely wrong, it is impossible. The nine VRAM banks read back:

```
A  0x83   3D texture          F  0x80   LCDC
B  0x8B   3D texture          G  0x80   LCDC
C  0x84   sub BG              H  0x00   disabled
D  0x84   sub OBJ             I  0x00   disabled
E  0x83   texture palette
```

The two free banks are the two whose only sub-engine homes are `0x06208000` and `0x06600000` —
both already covered by C and D. And with all of A–D taken, the main engine's display-capture route
is gone too. **There is nowhere on that console's DS hardware to draw that can be shown to be safe.**
The popup is now opt-in per game and off by default, because a missing notification is a missing
feature while a corrupted game is a bug.

`TwlBg` has none of that problem. It holds the composited picture *after* the DS hardware is
finished. Drawing there costs the game nothing, cannot corrupt anything, and works on every title
rather than on the ones where a survey happens to find a spare corner.

## What already exists

- **The hardware path.** `LgyFB` puts the DS picture into ordinary memory by DMA, driven by the
  ARM11, before the PICA200 displays it. Documented in GBATEK; see below.
- **The channel.** RTCom — two free legacy RTC registers, readable and writable by both the ARM7 and
  the ARM11, refreshed every frame. It already gives DS games circle-pad input on a 3DS.
- **The tooling.** TWPatcher builds a patched `TwlBg.cxi`, and TWiLight Menu already loads a
  different `TwlBg` per game.
- **The precedent.** `sm64ds_cpad_via_rtcom` and `Analog-Controls-for-NDS-Games-on-3DS` show ARM7 and
  ARM11 cooperating under TWL_FIRM in shipped software.

What none of them do is draw. The circle-pad project says so explicitly: it does not touch the
framebuffer. That is the single missing piece.

## What this side publishes

An unlock is written into a structure in DS main RAM on the frame it fires, by
`cardenginei_arm9_ra`. Offsets are from the start of `raSnapshot`, which begins with the ASCII magic
`RA2S` (`52 41 32 53`):

| offset | type | field | meaning |
| --- | --- | --- | --- |
| `+0x00` | `char[4]` | magic | `RA2S` — how to find the structure |
| `+0xE0` | `u32` | `notifySeq` | incremented once per unlock; `0` = none this session |
| `+0xE4` | `u32` | `notifyId` | the achievement's RetroAchievements id |
| `+0xE8` | `u8` | `notifyHardcore` | 1 if the session is hardcore |
| `+0xE9` | `u8` | `notifyLen` | bytes of the title in use; 0 if the name was not resolved |
| `+0xEC` | `char[64]` | `notifyTitle` | the achievement's own name, NUL-padded |

`sizeof(raSnapshot)` is `0x12C`, and every offset above is pinned by a compile-time assertion in this
project's host test suite, so they cannot drift without the build failing.

### The protocol is one counter and no handshake

Keep the last `notifySeq` you saw. When it changes, read the payload and show a notification. That
is all of it.

- **Nothing is ever cleared and nothing is acknowledged.** A reader that starts late, restarts, or
  misses a frame resynchronises on the next unlock instead of hanging on a flag nobody will clear.
- **The payload is written before the counter, always**, and the DS side cleans its data cache and
  drains its write buffer in between. A reader that sees a new sequence has a complete record behind
  it. That ordering is the entire synchronisation.

### It is not a promise — here is a console doing it

Read out of a 3DS with the RAM viewer, at `snapshot + 0xE0`, immediately after unlocking
*Bomb Quartet* in *Ketsui Death Label*. Bytes in address order:

```
+0xE0   01 00 00 00                           notifySeq       = 1
+0xE4   DE 98 04 00                           notifyId        = 0x000498DE = 301278
+0xE8   01                                    notifyHardcore  = 1
+0xE9   0C                                    notifyLen       = 12
+0xEA   00 00                                 (padding)
+0xEC   42 6F 6D 62 20 51 75 61 72 74 65 74   "Bomb Quartet"
+0xF8   00 00 ...                             NUL-padded to 64
```

`301278` is that achievement's real id on RetroAchievements, and `0x0C` is the exact length of its
name. Use this as a test vector: a reader that renders `Bomb Quartet` from those bytes has the
interface right.

### Finding it

The snapshot lives in the cardengine's own `.bss`, so its address depends on which cardengine variant
the running title loaded — `0x027FEA00` for a retail DS game, elsewhere for DSi-enhanced ones. A
reader scans DS main RAM once at startup for the `RA2S` magic.

**This is negotiable and we would rather negotiate it.** A fixed address would be easier for you and
we can publish one; it rides in the snapshot today only because allocating new memory in a running
game's main RAM is the exact risk this project has spent a week removing. Say where you want it.

## The pixels are in memory, and the ARM11 is what puts them there

The obvious objection to all of the above is that TwlBg might never touch pixels — that the DS
picture could go from the video controller to the LCD through hardware, with the ARM11 only setting
filter coefficients. If that were so this proposal would end here.

It is not so, and GBATEK documents why. The legacy video path is:

```
NDS video controller
   -> LgyFB            hardware block, optional upscale (256x192 -> 320x240 at 1.25x)
   -> LGYFB_FIFO       0x10310000 (bottom) / 0x10311000 (top)
   -> CDMA             channels 0Dh/0Eh, one request per 8 output lines
   -> memory           RGBA8888, RGB8880, RGB5551 or RGB5650, selected by LGYFB_CNT bits 8-9
   -> PICA200          which displays it
```

GBATEK, verbatim: *"The input comes directly from the GBA/NDS video controller, **the output must be
DMAed to memory**."* And on who does that: *"It's the job of the **ARM11** and its DMA to take care of
all memory transfers during this process."*

So there is a real framebuffer, in ordinary memory, in a format of the ARM11's own choosing, and the
ARM11 is the party that fills it and then hands it to the GPU. **Drawing a line of text into it is a
memory write between two steps the ARM11 already performs.** There is even `LGYFB_ALPHA` at `+0x20`
if blending is wanted.

Two more things fall out of the register map that are useful here:

- **The two screens are independent units.** `LGYFB_0` at `0x10110000` is the NDS *bottom* screen and
  `LGYFB_1` at `0x10111000` is the *top*. A notification can go on whichever suits, with no
  negotiation of any kind with the game.
- **Nothing about the game changes.** The DS side keeps every layer, sprite, palette entry and VRAM
  bank it had. This is the property the DS-side overlay could never have.

### What is left, honestly

Knowing the hardware path is not the same as knowing TwlBg's code. What remains is finding where
TwlBg keeps that buffer and where to hook between the DMA completing and the frame being handed to
the PICA200 — and that needs the binary, which is Nintendo firmware living on the console. We are not
in a position to obtain or analyse it. That is the work we are asking for, and it is now a bounded
piece of reverse engineering against a documented hardware path rather than an open question about
whether the idea is possible at all.

### The one remaining unknown on our side

**Can the ARM11 read DS main RAM directly?** If it can, the table above is the whole interface.

There is published reason to think it can: 3dbrew's Legacy FIRM PXI page says the legacy ARM9 *"loads
the TWL launcher located at physical address 0x27C00000, **which was written there by the TwlBg ARM11
process**"* — so the two already share physical memory in that direction.

If it turns out not to reach the region we use, RTCom is the fallback and costs nothing to switch to:
two bytes per frame is 120 bytes/second, so a 64-character title streams in about half a second,
which is well inside the delay before a notification would be shown anyway. Say which you prefer and
we will restructure.

## What we will do

Whatever shape suits you: fixed address, RTCom streaming, a different structure, a different trigger.
The DS half is small, it is written, it is confirmed on hardware, and we own it entirely.

## Contact

This lives at <https://github.com/Bakkerrrs/Retroachievements-DS-on-3DS>. The reasoning behind every
decision above, including the six rounds that failed, is in `docs/devlogs/`.
