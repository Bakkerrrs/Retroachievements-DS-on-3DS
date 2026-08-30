# WiFi probe

Does WiFi work on this console at all, before any of it is nds-bootstrap's problem?

This is step one of the ladder in `docs/retroachievements.md`, and it is **not** part of
nds-bootstrap. It is an ordinary DSi-mode homebrew `.nds` — no cardengine, no injected
code, no game running. That is the whole point: it is a control. If WiFi does not work
here, in the easy case, it certainly will not work from inside a game's interrupt handler,
and the answer is in without touching the loader.

## Building

```sh
git submodule update --init --recursive
make -C tools/wifiprobe
```

Produces `tools/wifiprobe/wifiprobe.nds`. It builds `dsiwifi` from the submodule in
`libs/dsiwifi` into that tree, so nothing has to be installed into `$DEVKITPRO` first.

The top-level `make package-nightly` does not touch this, and this does not touch
nds-bootstrap. A control that shares code with the thing it is controlling is not one.

The one thing they now share is `libs/dsiwifi`, because step two of the ladder links its
ARM7 half into nds-bootstrap's launcher. That costs the control nothing: what must not be
shared is *nds-bootstrap's* code, so that a failure here cannot be caused by it. The driver
being the same driver is the point — if the two runs disagree, the difference is
nds-bootstrap, which is the whole question step two asks.

## Running it

Copy `wifiprobe.nds` to the SD card and **launch it in DSi mode** — from TWiLight Menu++
with DSi mode on, or from Unlaunch. This matters: launched as an NTR title the console
never opens SCFG, `isDSiMode()` is false, the ARM7 skips the extended I/O setup, and the
Atheros chip is not on the bus at all. The probe would then report "no WiFi" for a reason
that has nothing to do with the question. The `.nds` is stamped with unitcode `0x03` to
make that the default rather than something to remember.

The console's WiFi needs to be configured beforehand, in the system settings, the same way
any online DSi title expects.

It writes `/wifiprobe.log` on the SD card as well as printing to the screen. **Send the
file, not a photo.** The interesting line in a stack log is never the last one, and this
project has spent enough sessions reading hex off a camera.

## Reading the result

It ends with `reached stage N of 6`:

| Stage | Reached | What it means |
|---|---|---|
| 0 | nothing | `DSiWifi_InitDefault()` did not return — the ARM7 side hung |
| 1 | init | the library came up; whether the *chip* did is in the log above it |
| 2 | associated | an IP was assigned, so the Atheros path works and WPA2 is available |
| 3 | resolved | DNS answered for `retroachievements.org` |
| 4 | connected | TCP to port 80 |
| 5 | requested | the request went out |
| 6 | **answered** | RetroAchievements replied over plain HTTP |

**Stage 6 means live unlocks are reachable from DSi mode**, and the remaining question is
only whether the same thing survives inside nds-bootstrap — which is step two and three of
the ladder, and a much harder question.

**Stage 1 with no association** is the outcome to read carefully rather than quickly. The
`dsiwifi` log above it is the only place that distinguishes the two cases that matter: the
chip arriving warm and needing only WMI init, versus arriving cold and needing the full BMI
bootloader plus a firmware upload. The AR6002/AR6013/AR6014 keeps no firmware in flash —
the Xtensa core's code is uploaded to RAM on every boot, normally by the system menu, and
booting through ntrboot may not pass through it. That is the highest risk in the whole
plan, which is why it is being tested first and cheaply.

Anything below stage 2 makes the deferred design the answer: unlocks queued on the console
during play and synced afterwards by a 3DS-mode companion app, which has an ARM11, a mature
network stack and no contest for the game's ARM7. That is also what every one of odelot's
real-hardware adapters does — none of them networks from the constrained side.

## What it deliberately does not do

**It asks for no credentials.** The request logs in as a user that does not exist, because
a well-formed `invalid_credentials` reply proves the entire path — DNS, TCP, HTTP, and the
API parsing our query — without this program ever handling a real password. Over cleartext
that distinction is worth keeping.

**It checks the reply for content, not just for bytes.** A captive portal, a proxy or a
Cloudflare interstitial would all succeed at the socket level and mean nothing. The API's
own error code coming back is what proves we reached RetroAchievements rather than
something that answered on its behalf.
