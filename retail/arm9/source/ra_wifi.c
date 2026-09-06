/*
    Step two of the ladder, on the launcher's ARM9: ask nds-bootstrap's own ARM7 to bring
    the Atheros chip up, write down verbatim what it says, and stop on a summary.

    What this is *not* is a second copy of tools/wifiprobe/. The probe answered "does WiFi
    work on this console" and answered it completely, up to RetroAchievements replying over
    plain HTTP. This answers the one thing that run could not: whether dsiwifi comes up as a
    guest of nds-bootstrap's ARM7 rather than of the libnds template. Those differ in ways
    that all bear on the WiFi SDIO block --

      the launcher's ARM7 never writes SCFG; it inherits whatever launched it, where the
      probe sets REG_SCFG_EXT itself. If the extended TWL I/O is not open, the chip is not
      on the bus and nothing else matters, so that word is the first thing logged;

      it is already driving SD/eMMC through my_sdmmc at 0x04004800, one instance of the
      same IP block below the WiFi SDIO at 0x04004A00, with NDMA slot 0 taken;

      it owns the FIFO, and its idle loop is swiIntrWait rather than a main loop.

    So the ARM9 side here deliberately links nothing. dsiwifi's ARM9 half is lwip, and lwip
    is DHCP and sockets -- step three. Everything this step asks about happens on the ARM7,
    and all the ARM9 has to do is send one IPC message, read the narration, and count rungs.
    Two facts fall out of that, both useful: no library, and no 800K of lwip pools trying to
    fit in the launcher's 736K link region.

    The log is written to the card and it is meant to be diffed against /wifiprobe.log.
    That is the test design rather than a convenience -- "did the chip arrive in the same
    state under our boot path" is a text comparison between two runs of the same driver,
    which is a reading that cannot come out two ways.

    This file is part of nds-bootstrap and is licensed under the GPL-3.0,
    the same terms as the rest of the project.
*/

#include "ra_wifi.h"

#if RA_LAUNCHER_WIFI

#include <nds.h>
#include <stdio.h>
#include <sys/stat.h>   /* mkdir(), for the per-game cache directory */
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>
/*
    time(), for `o=` on r=awardachievement. It works here because the launcher's own ARM7 starts
    libnds' RTC tracking IRQ -- initClockIRQ() in retail/arm7/source/main.c -- which is what keeps the
    value the ARM9 reads. The cardengine half cannot rely on that, because in there the ARM7 is the
    game's; it reads the RTC directly instead. See raUnlockAppend().
*/
#include <time.h>

/*
    Step 3 links dsiwifi's ARM9 half after all -- see retail/dsiwifi9/, which rebuilds it with
    lwip's pools cut from 833K of .bss to 76K so it fits the launcher's link region.

    That changes who owns FIFO_DSWIFI. wifi_host_init() installs its own datamsg handler on
    that channel and drives the whole sequence itself, and two owners of one channel is not a
    thing -- so the probe stops speaking IPC and calls the same API tools/wifiprobe/ calls.
    Which is an improvement in its own right: the control and the measurement now go through
    the same entry points, so a difference between them cannot be ours.

    What is lost is that association and end-of-handshake used to arrive as the driver's own
    signals; now they are read out of its prose like everything else. See
    RA_WIFI_SAY_ASSOC in ra_wifi_verdict.c.
*/
#include "locations.h"

#include "dsiwifi9.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

/* The host and port live in ra_wifi.h now, with the rest of ra_net's contract. */
/*
    A login for a user that does not exist, exactly as the probe does it. The reply is a 401
    with a JSON body, which proves DNS, TCP, HTTP and the API parsing our query without this
    program ever handling a real password. Over cleartext that distinction is worth keeping,
    and it is the reason step 3's first rung needs no credentials at all.
*/

/* What the launcher's ARM7 stashed there at boot, before anything could change it. */
#define REG_SCFG_EXT7 *(u32*)0x02FFFDF0

/*
    The ARM7 says when installWifiFIFO() has returned. Without this the ARM9 could send
    INIT_IOP into a channel with no handler -- unlikely, since loadFromSD() has already
    spent hundreds of milliseconds on the card by the time we get here, but a race that
    would present as "the chip never answered" is exactly the kind this project has paid
    for before. It also diagnoses the honest mistake of rebuilding one CPU and not the
    other, which a bare timeout would blame on the hardware.
*/
#define RA_WIFI_ARM7_READY_CHANNEL FIFO_USER_04
/* Mode 2's teardown handshake. Must match retail/arm7/source/ra_wifi7.c. */
#define RA_WIFI_ARM7_STOP_CHANNEL  FIFO_USER_07
#define RA_WIFI_WAIT_STOP          3   /* seconds */

/* Seconds. Generous, because the cost of being wrong is a run that reports the wrong rung. */
#define RA_WIFI_WAIT_ARM7   3
#define RA_WIFI_WAIT_CHIP   20
/*
    Was 40, and 40 was measured as a cost rather than chosen as a limit: a boot with the access point off
    spent forty seconds failing to associate, and dsiwifi printed "netif is not up, old style port?" on
    every poll for all of it -- forty-one identical lines of log before the first useful one.

    A DSi that has not associated in twelve seconds is not about to. Every successful run in this project
    associated in well under that, with the WPA2 handshake landing a second or two after the scan, so this
    cuts the worst case by two thirds and the noise with it. `sync=0` is the real answer for a card that
    is never going to have an access point; this is for the one whose router happens to be down.
*/
#define RA_WIFI_WAIT_LINK   12
#define RA_WIFI_WAIT_DHCP   30
#define RA_WIFI_WAIT_TAIL   8   /* after the summary: dsiwifi keeps narrating */
/*
    Per-recv(), not for the whole reply. Long enough that a slow server is not mistaken for a
    lost FIN, short enough that the run still ends: the reply itself arrives in one packet.
*/
#define RA_WIFI_RECV_TIMEOUT 5

/*
    dsiwifi's narration is captured into RAM by the interrupt and written out by the main
    loop. Both halves of that split were learned from the first hardware run:

    The interrupt must not touch the card. On the DSi the SD is driven by the *ARM7*, over
    the FIFO -- so an fputs() inside a FIFO datamsg handler means FIFO traffic from inside a
    FIFO interrupt, against an ARM7 that is at that moment running a WiFi stack. The first
    run got away with it twice. That is not the same as it being safe, and step 4 puts the
    same code next to a game.

    16K because a full run of the probe narrates two or three. An overflow is counted and
    reported rather than silently wrapping: a log with a hole in it that does not say so is
    worse than a short one.
*/
#define RA_WIFI_TEXT_MAX 0x4000

static FILE*         logFile;
static raWifiVerdict verdict;

/* Drains happen once a frame; sync every eighth that moved bytes, so roughly 8 per second. */
#define RA_WIFI_SYNC_EVERY 8

static u32           syncPending;
/* Stage 0b computes it; stage 11 asks the server about it. */
static char          romHash[33];
/*
    Stage 10 gets it; stage 12 spends it. Hoisted out of raWifiLogin() for that reason and for
    no other -- and it is the one static in this file that must never be logged. See
    raConfigRedact(): a token is the account.
*/
static char          raToken[64];
/*
    The username the server itself used in the login reply. Not a secret -- it is on the account's
    public page -- so unlike raToken it is logged, and it is a separate static because the signature
    r=awardachievement wants must be computed over the same spelling that goes in u=.
*/
static char          raUser[sizeof(((raConfig*)0)->username)];
/*
    Which card the launcher booted from, kept at file scope for one reason: the failure paths in the
    fetch have to reach the per-game cache, and they are called from places that were never given the
    flag. Set once by raWifiProbe() before anything can use it.
*/
static bool          raWifiSdFound;
/*
    Achievements the account already holds. 128 because RA_DEFS_MAX_LINES is 128 and a skip list
    longer than the set it filters would be pointless; a bigger set truncates and says so.
*/
#define RA_WIFI_UNLOCKS_MAX 128
static u32           unlockedIds[RA_WIFI_UNLOCKS_MAX];
/*
    What the account already holds and *when* it earned each one, from r=startsession -- the only rung
    in this API that answers the second question. Kept for the patch fetch two rungs later, which is
    where an earned achievement is written into the staged block and is therefore the one moment its
    date can ride along.
*/
static u32           earnedId[RA_WIFI_UNLOCKS_MAX];
static u32           earnedWhen[RA_WIFI_UNLOCKS_MAX];   /* UTC, as the server sent it */
static int           earnedCount;
/*
    ...and this console's offset from the server's clock, in seconds, from ServerNow against the RTC.
    Zero until r=startsession answers, which means an unconverted UTC date rather than a wrong one.
*/
static s32           clockOffset;

/*
    The packed local date an achievement was earned, or 0 if this account does not hold it or the
    server did not say.

    The conversion happens here, in the launcher, and that is deliberate: this side has a real libc
    and a real clock, and the in-game menu has neither. It gets five fields and shifts.

    gmtime() rather than localtime() because the offset has already been applied -- what is handed to
    it is not UTC any more, it is this console's own wall clock expressed as an epoch, and asking a
    timezone library to shift it again would move it twice.
*/
static u32 raWifiEarnedWhen(u32 id) {
	int k;

	for (k = 0; k < earnedCount; k++) {
		if (earnedId[k] == id) {
			const time_t     local = (time_t)((s32)earnedWhen[k] + clockOffset);
			const struct tm* t     = gmtime(&local);

			if (!t) {
				return 0;
			}
			return raWhenPack(t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
			                  t->tm_hour, t->tm_min);
		}
	}
	return 0;
}
static u16           unlockCount;
/*
    How many of those are the server's own pseudo-achievements rather than the set's, by the same
    RA_ODD_ID_FROM boundary the scanner drops them on. Counted because it is the difference between
    a skip list that matched nothing for a reason and one that matched nothing unexplained -- and on
    this account it is the whole of the mismatch. See the reading in docs/retroachievements.md.
*/
static u16           unlockNotices;
static char          textBuf[RA_WIFI_TEXT_MAX];
static volatile u32  textHead;      /* written only by the interrupt */
static u32           textTail;      /* read only by the main loop */
static volatile u32  textDropped;

/*
    Make what has been written survive a power-off, without closing the file.

    fflush() is not enough and the first hardware run proved it the expensive way: the run
    reached stage 5 of 5 on screen and left a **zero-byte** log. fflush() only pushes
    newlib's stdio buffer down into libfat's write(), which does write the data clusters --
    but a FAT file's length lives in its *directory entry*, and libfat only writes that from
    _FAT_syncToDisc(), reached from close() and fsync() and nothing else. This probe halts
    deliberately and never closes anything, so the bytes were on the card and the metadata
    said the file was empty.

    fsync() is therefore not a tidiness call, it is the fix. It also has to be per-write
    rather than at the end, because the thing most worth logging is a run that hangs:
    dsiwifi has two untimed loops in its bring-up, and a log that only becomes real at the
    end is exactly the log you do not get.
*/
static void raWifiSync(void) {
	if (logFile) {
		fflush(logFile);
		fsync(fileno(logFile));
	}
}

/*
    Move whatever the interrupt captured to the screen and the card. Main loop only.

    Copied out in bounded pieces because textBuf holds raw bytes with no terminator: the
    interrupt stores the string contents and not the NUL, so there is nothing to print until
    a piece is terminated here.
*/
/*
    The screen, and only the screen.

    Zero until ra.cfg is read, which is why the config is read before anything is printed: a run that
    starts verbose and goes quiet three lines in would be worse than either mode. The log file is
    written in both modes and is not affected by any of this -- see raConfig.verboseLog.
*/
static u8  raVerbose;

/*
    The quiet screen's layout, as rows below wherever this block starts. Everything is centred and
    nothing is ever written into the console's final column.

    **Both of those are fixes for the same fault.** The first version pinned rows at absolute columns
    and padded each one to a hard-coded width of 32. Writing the last cell of a row advances the
    cursor past the end, the console wraps to the next row, and from then on every absolute row this
    code addresses is one out -- which is what "it comes apart from 40% on" looked like: the point
    where the pulsing character, alone at column 31, started running every frame.

    So the geometry is read from PrintConsole rather than assumed, the drawing stays inside
    `width - 1`, and the message keeps a reserved area that is cleared whole before the next stage
    writes into it -- rather than being padded over and hoping the padding was counted right.
*/

static u8  raStep;
static u8  raWidth = 32;
#define RA_LINE_MAX 32

/*
    **The quiet screen prints, and does not try to hold a position.**

    Five versions of this went the other way and it is worth recording why the direction was
    abandoned, because each attempt fixed a real fault and none of them fixed the screen:

      1. Positioning PrintConsole's cursor and calling iprintf() leaves the *timing* of the write to
         stdio. A buffer flushed later prints in sequence from wherever the cursor is by then.
      2. Writing the last usable cell of a row advances the cursor past the end, so the console wraps
         and every absolute row addressed afterwards is one out.
      3. Drawing into the tile map directly fixes both of those -- and still only holds while nothing
         else prints a newline, because any other printf on this console scrolls the map and takes
         those cells with it.
      4. Repainting every row every frame answers that, in theory. On hardware it produced a black
         bottom screen with nothing on it and a boot that did not finish. Whatever the reason -- a
         palette that resolves to nothing, a map base that is not where consoleGetDefault() says, or
         the cost of seven hundred VRAM writes inside a wait loop -- **it broke the loader**, and a
         progress display is not worth a boot.

    So this prints. One short line per step through the same iprintf() the verbose path has always
    used, which is the one mechanism in here with a hardware record of working. It scrolls, and a
    fixed layout is a nicer thing that this does not have; it is also eleven lines instead of several
    hundred, which was the actual complaint.

    The geometry line raWifiScreenInit() writes to the log is kept deliberately. Any future attempt
    at a fixed layout should start from that reading rather than from another assumption -- see the
    note in docs/retroachievements.md.
*/
#define RA_MAP_STRIDE   32
#define RA_ROWS_VISIBLE 24

static PrintConsole* raCon;
static u8            raHasMap;

static void raWifiScreenInit(void) {
	raCon = consoleGetDefault();
	if (!raCon) {
		return;
	}
	if (raCon->consoleWidth > 8) {
		raWidth = (u8)raCon->consoleWidth;
	}
	raHasMap = (u8)(raCon->fontBgMap && raWidth == RA_MAP_STRIDE);
}

/*
    One step, as a line. `\x1b[K` is not used and neither is any other escape beyond colour: the
    verbose path has only ever used colour, so colour is the only escape with a hardware record here.
*/
/*
    Elapsed time, from a hardware timer, and **not from time(NULL)** -- which was the first attempt
    and which hardware answered immediately: every stage in the log read `[  0s]`.

    time(NULL) on this console reads a value the ARM7 refreshes, and in this launcher the ARM7 is
    running dsiwifi rather than libnds' own VBlank work, so the clock is set once at boot and then
    sits still. It is correct for stamping an unlock -- which is what it was measured doing -- and
    useless for measuring a fifty-second boot.

    A VBlank counter was the other candidate and it is worse: raWifiIdle() runs in the wait loops and
    *not* inside a blocking recv(), which is precisely where the seconds this is trying to find are
    going. A timer counts through a blocked CPU; a frame counter cannot.

    TIMER0 cascaded into TIMER1 gives 32 bits at 32,728 Hz -- about 36 hours before it wraps, against
    16 bits on its own which wraps in two seconds. Both are stopped in raWifiShutdown() beside the
    one dsiwifi leaves behind: a timer still running when the game starts is the game's problem.
*/
#define RA_TICKS_PER_SEC 32728   /* 33.514 MHz / 1024 */

static void raWifiClockStart(void) {
	TIMER0_CR = 0;
	TIMER1_CR = 0;
	TIMER0_DATA = 0;
	TIMER1_DATA = 0;
	TIMER0_CR = TIMER_DIV_1024 | TIMER_ENABLE;
	TIMER1_CR = TIMER_CASCADE | TIMER_ENABLE;
}

/*
    Tenths of a second since the start. Read low-then-high-then-low, because the two halves are two
    registers and the low one can wrap between the reads -- which would report a jump of two seconds
    once every two seconds, and this is a measurement whose whole point is being believed.
*/
static u32 raWifiTenths(void) {
	u32 lo, hi, lo2;

	lo  = TIMER0_DATA;
	hi  = TIMER1_DATA;
	lo2 = TIMER0_DATA;
	if (lo2 < lo) {
		hi = TIMER1_DATA;
		lo = lo2;
	}
	return (((hi << 16) | lo) * 10u) / RA_TICKS_PER_SEC;
}

/*
    Declared here because the timing line below is the first thing in the file that logs, and the
    logger itself needs the drain that needs the screen state above it. One forward declaration is
    cheaper than moving either.
*/
static void raWifiLog(const char* fmt, ...);

static void raWifiStep(u8 step, const char* caption) {
	char bar[RA_BAR_MIN];

	raStep = step;
	/*
	    Logged in both modes and before the screen work, because the log is where this gets read.
	    Without it "the boot takes fifty seconds" is a complaint with nowhere to go: the rungs are
	    a chip bring-up, a DHCP lease, five HTTP round trips and a hundred-kilobyte download, and
	    which of them is the fifty seconds is not guessable from the outside.
	*/
	{
		const u32 tenths = raWifiTenths();

		raWifiLog("[%3lu.%lus] %s\n",
		          (unsigned long)(tenths / 10), (unsigned long)(tenths % 10), caption);
	}
	if (raVerbose) {
		return;
	}
	raWifiBar(bar, sizeof(bar), step, RA_STEP_MAX);
	/*
	    Two lines, and that is forced rather than chosen: the bar is 29 columns and the longest
	    caption is 26, so one line carrying both wraps mid-word on a 32-column console -- which looks
	    worse than anything else considered here.
	*/
	iprintf("%s\n %s\n", caption, bar);
}

/*
    Nothing to animate any more, and that is a real loss rather than a simplification: association
    can take forty seconds, and without a moving character a run that has stopped looks exactly like
    a run that is waiting.

    The cheapest honest substitute would be a dot per second on the current line, and it is not here
    because a line that grows is what the patch download's dots already are and they are gated off in
    this mode. Left as a deliberate gap rather than a third animation nobody asked for.
*/
static void raWifiSpin(void) {
}

/*
    An essential line, printed once, when it becomes true. Quiet mode only -- in verbose mode the
    summary below says all of this and more.
*/
static void raWifiEssentialPal(const char* colour, const char* fmt, ...) {
	char    line[RA_LINE_MAX];
	va_list args;

	if (raVerbose) {
		return;
	}
	va_start(args, fmt);
	vsniprintf(line, sizeof(line), fmt, args);
	va_end(args);

	iprintf("%s%s\x1b[37m\n", colour, line);
}

#define raWifiEssential(...)  raWifiEssentialPal("", __VA_ARGS__)
#define raWifiWarn(...)       raWifiEssentialPal("\x1b[33m", __VA_ARGS__)
#define raWifiError(...)      raWifiEssentialPal("\x1b[31m", __VA_ARGS__)

static void raWifiDrain(void) {
	const u32 head = textHead;        /* snapshot: the interrupt may advance it as we go */
	char      piece[129];
	bool      moved = false;

	while (textTail < head) {
		u32 n = head - textTail;

		if (n > sizeof(piece) - 1) {
			n = sizeof(piece) - 1;
		}
		memcpy(piece, textBuf + textTail, n);
		piece[n] = 0;
		textTail += n;

		if (raVerbose) {
			iprintf("%s", piece);
		}
		if (logFile) {
			fputs(piece, logFile);
		}
		/*
		    Classified here rather than in the interrupt, so the handler stays a memcpy.
		    Reassembly across pieces is raWifiVerdictChunk()'s job either way, and the
		    boundaries it has to survive are the FIFO's, not this buffer's.
		*/
		raWifiVerdictChunk(&verdict, piece);
		moved = true;
	}

	/*
	    Synced when a line lands, but not more than a few times a second.

	    Every fsync() is an SD transaction, and on the DSi the SD is the *ARM7's* -- served
	    over the FIFO by the same CPU that is at that moment running dsiwifi's timer, its SDIO
	    interrupt and a WPA2 link. One v6 run froze immediately after the summary's heap line,
	    which is SD I/O with the network live, and the run before it did not. That is the
	    contention #1d worried about, seen for the first time, and it is the one hazard step 4
	    inherits directly.

	    Fewer transactions is not a fix and is not claimed as one -- a blocking libfat call
	    cannot be bounded by a frame counter. It is less exposure while the reading is being
	    taken, which is worth having until the contention itself is understood.
	*/
	if (moved && ++syncPending >= RA_WIFI_SYNC_EVERY) {
		syncPending = 0;
		raWifiSync();
	}
}

/*
    Our own lines. Drains first so the file reads in the order things happened rather than
    with the summary interleaved into whatever dsiwifi was mid-sentence about.
*/
static void raWifiLog(const char* fmt, ...) {
	char    line[192];
	va_list args;

	raWifiDrain();

	va_start(args, fmt);
	vsniprintf(line, sizeof(line), fmt, args);
	va_end(args);

	if (raVerbose) {
		iprintf("%s", line);
	}
	if (logFile) {
		fputs(line, logFile);
	}
	raWifiSync();
}

/*
    A whole reply body in the log, in pieces that fit, with the token removed on the way.

    Two reasons this is not just raWifiLog("%s", body). The line buffer above is 192 bytes, so a
    900-byte reply logged that way is silently cut at the first 192 -- which is how three runs went by
    with `r=unlocks` reporting a parsed count and never showing what it was parsing. And this log is
    written to be sent to someone else, so a body must never carry the session token out with it.

    The redaction is a safety net rather than a fix for a known leak: no reply this client reads is
    supposed to echo the token back. A net is worth having anyway, because the cost of being wrong
    once is an account, and because the next endpoint added here does not have to remember the rule.
    See raConfigRedact() for the same rule applied to the password.

    The `>= 8` guard is load-bearing: strncmp against a zero-length token matches at every position,
    which would spin here forever.
*/
static void raWifiLogBody(const char* label, const char* body) {
	const size_t tokenLength = strlen(raToken);
	size_t       i = 0;
	char         chunk[144];

	raWifiLog("%s:\n", label);

	while (body[i]) {
		size_t n = 0;

		while (body[i] && n < sizeof(chunk) - 1) {
			if (tokenLength >= 8 && strncmp(body + i, raToken, tokenLength) == 0) {
				if (n + 7 >= sizeof(chunk) - 1) {
					break;   /* no room; flush and let the next chunk carry it */
				}
				memcpy(chunk + n, "(token)", 7);
				n += 7;
				i += tokenLength;
				continue;
			}
			chunk[n++] = body[i++];
		}
		if (n == 0) {
			break;           /* cannot happen with a 144-byte chunk; not worth risking a spin */
		}
		chunk[n] = 0;
		raWifiLog("  %s\n", chunk);
	}
}

/*
    dsiwifi's narration, unedited, captured from the interrupt. This is the channel that
    answers the question, so it is passed through rather than summarised -- the summary is
    derived from it afterwards and can be checked against it.

    No I/O here. See RA_WIFI_TEXT_MAX for why that matters more than it looks.
*/
static void raWifiCapture(const char* text) {
	u32 i;

	for (i = 0; text[i]; i++) {
		if (textHead >= sizeof(textBuf)) {
			textDropped++;
			return;
		}
		textBuf[textHead++] = text[i];
	}
}

/*
    The SCFG words, first, because they decide whether the rest of the run means anything.
    Reported and not corrected: opening SCFG here would make this a measurement of a boot
    path nds-bootstrap does not have. If the bit is closed, that is the finding.
*/
static void raWifiReportScfg(void) {
	raWifiLog("isDSiMode        %s\n", isDSiMode() ? "yes" : "no");
	raWifiLog("SCFG_EXT  ARM9   %08lX\n", (unsigned long)REG_SCFG_EXT);
	raWifiLog("SCFG_EXT  ARM7   %08lX\n", (unsigned long)REG_SCFG_EXT7);
	raWifiLog("SCFG_CLK  ARM9   %08lX\n", (unsigned long)REG_SCFG_CLK);
	raWifiLog("SCFG_ROM  ARM9   %08lX\n", (unsigned long)REG_SCFG_ROM);

	/*
	    BIT(18) is the bit nds-bootstrap's own my_installSystemFIFO() tests, in a
	    commented-out guard, before installing the SD/MMC handlers -- so on this codebase's
	    reading it is the SD/MMC enable. The WiFi SDIO at 0x04004A00 is a second instance of
	    that controller 0x200 higher, which makes it *likely* to be the same gate and not
	    certain. Printed as the fact it is rather than as the conclusion it suggests: if
	    this reads set and the chip still never answers, the two are not the same gate, and
	    that is worth knowing rather than assuming either way.
	*/
	raWifiLog("SCFG_EXT7 BIT(18) %s\n", (REG_SCFG_EXT7 & BIT(18)) ? "set" : "clear");

	if (REG_SCFG_EXT7 == 0) {
		raWifiLog("\x1b[31mARM7 has no SCFG access at all.\x1b[37m\n"
		          "the extended TWL I/O is closed, so the\n"
		          "WiFi SDIO block is not on the bus.\n");
	}
}

static bool raWifiWaitArm7(void) {
	int frames = RA_WIFI_WAIT_ARM7 * 60;

	while (frames-- > 0) {
		if (fifoCheckValue32(RA_WIFI_ARM7_READY_CHANNEL)) {
			return fifoGetValue32(RA_WIFI_ARM7_READY_CHANNEL) != 0;
		}
		swiWaitForVBlank();
	}
	return false;
}

/*
    How much heap is left, at the moments it changes.

    Reported because it is the number the ladder keeps spending: lwip's send path allocates from
    the same malloc as libfat and the launcher's own strings, and the static side of the region
    has grown from 569,136 to 598,220 of 753,664 across steps 3b, 3c and 3d -- every static
    response buffer and the scanner's carry buffer raises the floor the heap starts from. Better
    to have the figure from a run that worked than to discover it from one that did not.

    Step 3d turned out to need none of it: the set is streamed into the staging block and the
    recv() window is static, so the largest reply in this project allocates nothing. That was
    the design intent and this line is how it gets checked rather than assumed.
*/
static void raWifiReportHeap(const char* when) {
	/*
	    Third attempt at this line, and the two wrong versions are worth keeping in view
	    because each was wrong in a way that mattered.

	    `mallinfo().fordblks of .arena` reads as almost-out-of-memory: `arena` is what newlib
	    has claimed by sbrk() so far, not what is available. Then `fake_heap_end - sbrk(0)`
	    reported **13.4 MB**, which is true and useless -- libnds sets `fake_heap_end` from
	    main RAM and knows nothing about nds-bootstrap's link region. It was measured at
	    0x02FF3794.

	    What actually bounds the heap here is neither: it is `IMAGES_LOCATION`. The launcher
	    decompresses the boot images to 0x02338000 for the bootloader to display later, and
	    `ds_arm9_ndsbs.mem` ends the link region at exactly that address -- but nothing
	    *enforces* it, because the enforcement libnds would do lives in `fake_heap_end` and
	    that has been set 12.8 MB higher. So malloc will grow straight through the images, and
	    then through the RA staging block at 0x02600000, the cardengine staging at 0x026F0000
	    and the ARM7's at 0x027B2000, silently, and the failure would appear as a corrupt boot
	    screen or a cardengine that starts and dies.

	    So the number reported is the distance to `IMAGES_LOCATION`, and `fake_heap_end` is
	    printed beside it precisely so the gap between the two is visible rather than
	    reassuring.
	*/
	extern char*          fake_heap_end;
	const struct mallinfo mi   = mallinfo();
	char* const           top  = (char*)sbrk(0);
	char* const           safe = (char*)IMAGES_LOCATION;

	raWifiLog("heap %-11s %ld safe + %lu free, top %08lX, limit %08lX\n", when,
	          (long)(safe - top), (unsigned long)mi.fordblks,
	          (unsigned long)top, (unsigned long)fake_heap_end);

	if (top >= safe) {
		raWifiLog("\x1b[31mthe heap has passed IMAGES_LOCATION\x1b[37m\n");
	}
}

/* Drain once per frame, so a hang leaves everything up to it already on the card. */
static void raWifiIdle(void) {
	swiWaitForVBlank();
	raWifiDrain();
	raWifiSpin();
}

/*
    Wait for a rung, and give up rather than hang -- every step below can fail by never
    completing, and a probe that hangs teaches nothing. The point of the run is to come back
    with a number.

    Deliberately does *not* flush the classifier's partial line while it waits. The log
    arrives from an interrupt, so a flush here would land at an arbitrary point in whatever
    line is mid-delivery and cut it in two -- and every string that decides a rung is longer
    than the fragment either half would leave. That would present as a chip that never came
    up, which is the worst way to be wrong. The only flush is at the end, where nothing more
    is coming.
*/
static bool raWifiWaitStage(int wanted, int seconds, const char* what) {
	int frames = seconds * 60;

	while (frames-- > 0) {
		if (raWifiVerdictStage(&verdict) >= wanted) {
			return true;
		}
		raWifiIdle();
	}
	raWifiLog("\x1b[31mno %s after %ds\x1b[37m\n", what, seconds);
	return false;
}

static void raWifiReportIp(const char* what, u32 addr) {
	const u8* b = (const u8*)&addr;

	raWifiLog("%-16s %u.%u.%u.%u\n", what, b[0], b[1], b[2], b[3]);
}

/*
    DHCP, and it is lwip's first real test rather than a formality: everything below this
    point ran without an IP stack in step 2, so a failure here is the new code and not the
    driver.
*/
static bool raWifiWaitIp(int seconds) {
	int frames = seconds * 60;

	while (frames-- > 0) {
		const u32 addr = DSiWifi_GetIP();

		if (addr != 0 && addr != 0xFFFFFFFF) {
			verdict.ip    = addr;
			verdict.gotIp = 1;
			raWifiReportIp("IP", addr);
			return true;
		}
		raWifiIdle();
	}
	raWifiLog("\x1b[31mno IP after %ds\x1b[37m\n", seconds);
	return false;
}

/*
    Say when a request needed more than one connect.

    Printed only when it happened, because the interesting reading is the frequency: a run of the
    ladder once had its third socket lose a race inside lwip -- see RA_NET_CONNECT_TRIES -- and a
    retry that succeeds silently would turn that into an impression instead of a number.
*/
static void raWifiReportAttempts(const char* what, const raNetProgress* p) {
	if (p->attempts > 1) {
		raWifiLog("\x1b[33m%s needed %u connect attempts\x1b[37m\n", what, p->attempts);
	}
}

/*
    Stage 7-9: reach the API, with no credentials at all.

    The request logs in as a user that does not exist, which is the same thing
    tools/wifiprobe/ does and for the same reason: a well-formed `invalid_credentials` proves
    DNS, TCP, HTTP and the API parsing our query, without the reading depending on the config
    file being right. So when the login below fails, the failure is the login.

    The reply is checked for *content*, not for bytes. A captive portal, a proxy or a
    Cloudflare interstitial all succeed at the socket level and mean nothing.
*/
/*
    Stages 7 to 9 -- DNS, TCP and "is this really the API" -- used to be a request of their own: a
    deliberate `r=login` with no credentials, sent only to see `invalid_credentials` come back.

    It was removed because r=login proves all three and runs immediately after. The throwaway cost a
    DNS lookup, a TCP connection, a round trip and eight lines of log to establish something the next
    request establishes as a side effect -- and every one of those rungs is now set from the login's own
    raNetProgress, which already records resolved, connected and sent.

    Worth keeping the reasoning rather than just the deletion: the probe existed when *nothing* had ever
    reached the API from here and the question was whether plain HTTP worked at all. That was answered on
    hardware a long time ago (see #1a), and a test that has passed every run since is a cost, not a check.
*/

/*
    Stage 10: r=login, with the credentials from ra.cfg.

    The password is percent-encoded rather than pasted in, and that is the one detail in this
    function that can fail convincingly: an `&` in a password ends the parameter early, a `+`
    decodes as a space, a `%` opens an escape that is not there. Each produces a well-formed
    request that comes back `invalid_credentials` -- so the user would be told their password
    is wrong when it is not. tools/ra_launcher_test.c pins the encoder against exactly those.

    Nothing secret is logged. Not the password, and not the token either: a token grants the
    same power over the account, and this log is a file whose purpose is to be sent to someone.
    A length answers the only question a log needs to answer.
*/
static void raWifiLogin(const raConfig* cfg) {
	static char   response[2048];
	char          user[3 * sizeof(cfg->username)];
	char          pass[3 * sizeof(cfg->password)];
	char          path[320];
	raNetProgress p;
	int           got;

	if (!cfg->usable) {
		raWifiLog(cfg->found
		          ? "\x1b[33mra.cfg has no username/password; login skipped\x1b[37m\n"
		          : "\x1b[33mno ra.cfg; login skipped\x1b[37m\n");
		return;
	}

	if (!raNetUrlEncode(cfg->username, user, sizeof(user))
	 || !raNetUrlEncode(cfg->password, pass, sizeof(pass))) {
		raWifiLog("\x1b[31mcredentials too long to encode\x1b[37m\n");
		return;
	}
	if (sniprintf(path, sizeof(path), "/dorequest.php?r=login&u=%s&p=%s", user, pass)
	    >= (int)sizeof(path)) {
		raWifiLog("\x1b[31mlogin request too long\x1b[37m\n");
		return;
	}

	raWifiLog("logging in as    %s\n", cfg->username);

	memset(&p, 0, sizeof(p));
	got = raNetHttpGet(RA_NET_HOST, path, response, sizeof(response), &p);

	/*
	    Rungs 7 to 9, from this request rather than from a throwaway one before it. DNS, TCP and "the API
	    answered" are all side effects of any successful call, and this is the first call there is.
	*/
	verdict.dnsOk = p.resolved;
	verdict.tcpOk = p.connected;
	if (p.resolved) {
		raWifiLog("resolved         %s\n", RA_NET_HOST);
		raWifiReportIp("its address", p.address);
	}
	if (p.connected) {
		raWifiLog("connected        port %d\n", RA_NET_PORT);
	}
	raWifiReportAttempts("login", &p);
	if (got < 0) {
		raWifiLog("\x1b[31mlogin HTTP failed at step %d\x1b[37m\n", -got);
		return;
	}
	raWifiLog("%d bytes back\n", got);
	/*
	    Recognisably the API: a JSON object with the fields it uses. Enough for rung 9, whether or not
	    the credentials in it were right.
	*/
	if (strstr(response, "\"Success\"")) {
		verdict.apiOk = 1;
	}

	if (raNetJsonString(response, "Token", raToken, sizeof(raToken))) {
		verdict.loggedIn = 1;
		raWifiLog("\x1b[32mlogged in, token %s\x1b[37m\n", raConfigRedact(raToken));
		/*
		    The account's own spelling of its name, which is not always what ra.cfg says -- RA
		    matches the login case-insensitively and answers with the canonical form. It matters
		    here and nowhere else: r=awardachievement's signature is an md5 over the username, so
		    the launcher has to hash whatever it also sends in u=. Using the canonical one for both
		    is what rcheevos does. If the reply has no User field, cfg's spelling stands, which is
		    the behaviour we had before asking.
		*/
		/*
		    Only on a clean read. raNetJsonString() leaves a *partial* copy behind when the value
		    does not fit and returns false, so testing the buffer instead of the return value would
		    adopt a truncated username -- and a truncated username signs every award wrong while
		    looking entirely reasonable in the log. Same buffer size as ra.cfg's field, so the
		    encoded form always fits the caller's buffer too.
		*/
		if (raNetJsonString(response, "User", raUser, sizeof(raUser))) {
			if (strcmp(raUser, cfg->username) != 0) {
				raWifiLog("the account is   %s\n", raUser);
			}
		} else {
			sniprintf(raUser, sizeof(raUser), "%s", cfg->username);
		}
		return;
	}

	/*
	    The API's own error, verbatim -- it distinguishes a wrong password from a banned
	    account from a malformed request, and guessing between those from a console is exactly
	    what this project does not do.
	*/
	{
		char code[64];

		if (raNetJsonString(response, "Code", code, sizeof(code))) {
			raWifiLog("\x1b[31mlogin refused: %s\x1b[37m\n", code);
		} else {
			raWifiLog("\x1b[31mno token in the reply\x1b[37m\n");
		}
		raWifiLog("body: %s\n", raNetBody(response));
	}
}

/*
    Stage 11: r=gameid, which turns the ROM's hash into the server's own verdict on it.

    `r=patch` needs a GameID and this is where it comes from, but the reason to do it as its own
    rung is that it answers a question nothing local can. Step 3b proved the hash matches what
    rcheevos computes, and the user checked it by eye against the set's page. Neither of those is
    the server saying "I know this dump", and the difference matters: a hash that RetroAchievements
    does not recognise is what a trimmed, translated or differently-patched ROM produces, and it
    looks exactly like a game with no achievement set.

    No credentials needed -- this request is unauthenticated -- so it is deliberately independent
    of whether the login above worked.

    **A GameID of zero is an answer, not an error.** It is what the API returns for a hash it does
    not know, and saying so plainly is the whole value of this rung.
*/
static void raWifiIdentify(void) {
	static char   response[1024];
	char          path[128];
	raNetProgress p;
	int           got;
	u32           gameId = 0;

	if (!romHash[0]) {
		raWifiLog("\x1b[33mno ROM hash; identification skipped\x1b[37m\n");
		return;
	}
	if (sniprintf(path, sizeof(path), "/dorequest.php?r=gameid&m=%s", romHash)
	    >= (int)sizeof(path)) {
		raWifiLog("\x1b[31mgameid request too long\x1b[37m\n");
		return;
	}

	raWifiLog("asking about     %s\n", romHash);

	memset(&p, 0, sizeof(p));
	got = raNetHttpGet(RA_NET_HOST, path, response, sizeof(response), &p);
	raWifiReportAttempts("gameid", &p);
	if (got < 0) {
		raWifiLog("\x1b[31mgameid HTTP failed at step %d\x1b[37m\n", -got);
		return;
	}
	raWifiLog("%d bytes back\n", got);

	if (!raNetJsonNumber(response, "GameID", &gameId)) {
		raWifiLog("\x1b[31mno GameID in the reply\x1b[37m\n");
		raWifiLog("body: %s\n", raNetBody(response));
		return;
	}

	verdict.gameId = gameId;
	if (gameId == 0) {
		/*
		    Not a failure of ours. The request worked, the API answered, and the answer is that
		    it has never seen this dump -- so there is no set to fetch and 3d would have nothing
		    to do. Said in full, because the next move is to find the supported ROM rather than
		    to debug any of this.
		*/
		raWifiLog("\x1b[33mthe server does not know this hash\x1b[37m\n"
		          "the dump is not one the set covers.\n");
		raWifiLog("body: %s\n", raNetBody(response));
		/*
		    And throw away anything already staged for it, because this reply is the one thing that
		    can say no set applies -- and until now it was the only answer that left the *wrong* set
		    armed.

		    loadRaDefinitions() stages sd:/_nds/nds-bootstrap/ra_achievements.txt for whatever game is
		    booting, unconditionally: it is a hand-managed debugging file with no way to know which
		    game it belongs to, and it says so. That is harmless while it is overwritten -- for a ROM
		    the server knows, stage 15 replaces the block with that ROM's own set, empty or not.

		    A ROM the server does *not* know never reaches stage 15 ("no GameID; the set cannot be
		    asked for"), raWifiCacheLoad() has no cache for it either, and the hand file therefore
		    survives into the game -- one game's triggers watching that game's addresses inside
		    another game's RAM.

		    Worse than the self-test bug this branch started with, and in the one way that matters:
		    these are **real RetroAchievements ids**. A trigger firing on unrelated memory queues an
		    unlock the server will happily accept, and the player wakes up holding an achievement for
		    a game they were not playing. Nothing undoes that from here.

		    Reasoned from the source, **not** observed: it needs a card that has ra_achievements.txt,
		    and no run has been made on one. An earlier version of this comment cited a card whose
		    sd:/ra_definitions.txt held another game's set -- that file is RA_DEFS_DUMP_PATH, which
		    this launcher *writes* and never reads, so it was evidence of the last fetch and of
		    nothing else. The defect is in the path either way; the sighting was not.

		    Cleared here rather than at the `done:` fallback because here is where the fact exists. A
		    ladder that never got this far cannot tell "no set for this ROM" from "never asked", and
		    the hand file is the only way to test definitions with no access point at all -- so it
		    stays staged on every path except the one that positively contradicts it.
		*/
		*(u32*)CARDENGINEI_ARM9_RA_DEFS_BUFFERED_LOCATION = 0;
		raWifiLog("staged definitions discarded: they cannot be this ROM's\n");
		return;
	}

	verdict.identified = 1;
	raWifiLog("\x1b[32mGameID           %lu\x1b[37m\n", (unsigned long)gameId);
}

/*
    Stage 12: r=startsession -- tell the server a play session exists.

    Added because an award came back `Success:true` and the achievement did not appear on the account.
    rcheevos sends this when a game loads, before anything else game-specific, and this client never
    sent it at all -- so "the server will not record an unlock without a session" was a hypothesis with
    no evidence either way, which is the worst kind to leave standing.

    It is not built on spec. A correct RA client sends it regardless of how the current question turns
    out, and its reply is a second source for something already in doubt: `Unlocks` and
    `HardcoreUnlocks`, as arrays of `{"ID":n,"When":t}` -- a different shape from `r=unlocks`'s flat
    array, of the same facts. Two sources that can be compared is what this needs.

    Parameters read from rc_api_init_start_session_request_hosted(): g, then h and m together, then l.
    `l` is the client library version; rcheevos sends its own, so this sends this project's, because
    claiming to be a version of rcheevos that is not running would be a lie to a server that uses the
    field to tell clients apart.

    Fails open like every other rung: no session means the award below still tries, because an award
    that might work is better than one that certainly does not.
*/
static void raWifiStartSession(const raConfig* cfg) {
	static char   response[2048];
	static u32    unlocks[RA_WIFI_UNLOCKS_MAX];
	char          user[3 * sizeof(raUser)];
	char          path[384];
	raNetProgress p;
	int           got;
	int           soft;
	int           hard;

	if (!verdict.gameId || !raToken[0] || !raNetUrlEncode(raUser, user, sizeof(user))) {
		raWifiLog("\x1b[33mno GameID or token; no session\x1b[37m\n");
		return;
	}
	if (sniprintf(path, sizeof(path),
	              "/dorequest.php?r=startsession&u=%s&t=%s&g=%lu&h=%d&m=%s&l=%s",
	              user, raToken, (unsigned long)verdict.gameId, cfg->hardcore ? 1 : 0,
	              romHash, RA_NET_CLIENT_VERSION) >= (int)sizeof(path)) {
		raWifiLog("\x1b[31msession request too long\x1b[37m\n");
		return;
	}

	memset(&p, 0, sizeof(p));
	got = raNetHttpGet(RA_NET_HOST, path, response, sizeof(response), &p);
	raWifiReportAttempts("session", &p);
	if (got < 0) {
		raWifiLog("\x1b[33msession HTTP failed at step %d; awarding anyway\x1b[37m\n", -got);
		return;
	}
	raWifiLog("%d bytes back\n", got);
	raWifiLogBody("session reply", raNetBody(response));

	if (!raNetJsonTrue(response, "Success")) {
		raWifiLog("\x1b[33mthe server did not start a session\x1b[37m\n");
		return;
	}

	verdict.sessionOk = 1;
	raWifiLog("\x1b[32msession started\x1b[37m\n");

	/*
	    Counted, not merged into the skip list -- and hardware turned that from a cautious choice into a
	    correct one. A reply for this game returned 91467 in HardcoreUnlocks and *not* in Unlocks, so the
	    two lists are tracked independently: a hardcore unlock does not imply the softcore one. This fork
	    plays softcore, so an achievement held only in hardcore has not been earned yet and belongs in the
	    block where the player can earn it. Merging the hardcore list would silently remove exactly those.

	    The skip list therefore stays with r=unlocks, whose h=0 form is the softcore list. Asking twice
	    also keeps the two able to disagree in the log rather than one quietly overwriting the other.
	*/
	soft = raNetJsonObjectField(response, "Unlocks", "ID", unlocks, RA_WIFI_UNLOCKS_MAX);
	/*
	    ...and *when*, from the same objects, which is the only place in this whole API that says so.
	    `r=unlocks` answers in bare ids and `r=patch` describes the set rather than the account, so a
	    reply this rung was already making is the one source for the date the achievements page shows.

	    Two calls over one parse because the extractor takes one field, and the array is walked in
	    order both times -- so whenWasEarned[k] belongs to unlocks[k]. Read back rather than assumed:
	    a count that disagreed would mean the reply had objects missing one of the two fields, and
	    that is checked below rather than trusted.
	*/
	{
		const int whens = raNetJsonObjectField(response, "Unlocks", "When",
		                                       earnedWhen, RA_WIFI_UNLOCKS_MAX);

		if (soft > 0 && whens == soft) {
			int k;

			for (k = 0; k < soft; k++) {
				earnedId[k] = unlocks[k];
			}
			earnedCount = soft;
		} else if (soft > 0) {
			/*
			    Two counts that disagree mean objects missing one of the fields, and there is no way
			    to tell which id lost its date. Staging none beats staging them shifted by one, which
			    would date every achievement with its neighbour's time.
			*/
			raWifiLog("\x1b[33m%d unlocks but %d dates -- no times staged\x1b[37m\n", soft, whens);
		}
	}
	/*
	    The clock difference, taken once, from two numbers in the same reply and the console's own RTC.

	    The server answers in UTC and the queue file's stamps are the console's local time, and the
	    achievements page shows both kinds side by side -- so one of them has to be converted or the
	    page tells the truth twice in two different timezones. There is no timezone setting anywhere in
	    this fork to consult, and `ServerNow` is what makes one unnecessary: it is the server's clock at
	    the moment this console's clock read time(NULL), and the difference between them is this
	    console's offset from UTC whether or not anyone ever configured it.

	    A console with a wrong RTC gets wrong dates here, and it already gets wrong stamps in the queue,
	    so nothing new is broken by trusting it.
	*/
	{
		u32 serverNow = 0;

		if (raNetJsonNumber(response, "ServerNow", &serverNow) && serverNow) {
			clockOffset = (s32)((u32)time(NULL) - serverNow);
			raWifiLog("clock offset     %ld s from the server\n", (long)clockOffset);
		}
	}
	hard = raNetJsonObjectField(response, "HardcoreUnlocks", "ID", unlocks, RA_WIFI_UNLOCKS_MAX);
	verdict.sessionUnlocks = (u16)(soft > 0 ? soft : 0);
	verdict.sessionHardcore = (u16)(hard > 0 ? hard : 0);
	/*
	    -1 is "the key was not there at all", which is a different statement from an empty list and is
	    said differently. See raNetJsonObjectField().
	*/
	raWifiLog("session unlocks  %d soft, %d hard\n", soft, hard);
}

/*
    Stage 13: r=awardachievement -- report what the last play session earned.

    This closes the loop, and it is the first rung that sends something rather than asking something.
    The cardengine cannot reach the network: it runs inside the game with the radio already torn down,
    so an unlock is written to sd:/ra_unlocks.txt and sent here, one boot late. See RA_QUEUE_PATH.

    Three things about the order and the rules, because each one is a decision rather than an
    accident.

    **It runs before r=unlocks.** If it ran after the fetch, the achievement just awarded would still
    be in the staging block, would trigger again next session, and would be queued again -- a loop
    that never drains. Sending first means the very next rung sees it in the account's unlocks and the
    scanner leaves it out.

    **An answer clears the record; silence keeps it.** The rule is about who has seen the id, not about
    whether they liked it. A server that answered has the unlock (RA returns Success for an
    already-unlocked achievement, and an error for one it will always refuse), so retrying forever
    would only spam it. A request that never got an answer proved nothing, so that id is still owed and
    survives into the next boot. Every refusal is logged with the server's own body -- the whole point
    of a queue is that nothing disappears quietly.

    **The file is rewritten in place, same length.** Truncating it could hand its clusters back, and
    the cardengine half of this can only write into clusters that already exist.
*/
/*
    `hardcore` is the record's own, not the configuration's, and that distinction is the whole point
    of the field existing.

    ra.cfg describes the session about to start. This request is about a session that is already
    over -- possibly days over, on a different game, in a different mode. Taking `h=` from the file
    meant a player could earn unlocks in softcore with the in-game menu's RAM editor open, set
    hardcore=1, boot, and have them submitted as hardcore. Nothing downstream could have noticed:
    the request is well-formed and correctly signed either way.

    So the record decides, and it decides in both directions. A hardcore unlock is still sent as
    hardcore on a boot whose ra.cfg has since been set to softcore -- it *was* earned in hardcore, and
    dating it by the boot that reported it is the same mistake `o=` exists to avoid.

    The one thing that is still the session's to decide is what raWifiHardcoreRefused() rules on, and
    that already happened before any of these records were written -- which is why this function no
    longer takes the config at all. It used to, for this one field, and losing the parameter is the
    clearest statement available that the file no longer has a say here.
*/
static void raWifiSubmitOne(u32 id, u32 when, int hardcore, raQueue* q) {
	static char   response[1024];
	char          user[3 * sizeof(raUser)];
	char          path[384];
	char          signature[33];
	char          offset[32];
	u32           seconds;
	raNetProgress p;
	int           got;

	/*
	    How long ago this was earned, which is what the server wants rather than when. Best effort by
	    design: anything that does not make sense sends no `o=` at all and the unlock is dated by its
	    submission, which is exactly what every unlock did before this existed. A wrong time would be
	    worse than the old behaviour; no time is merely the old behaviour.

	    Three ways it can make no sense, and each one is real. The record may carry no stamp -- a
	    hand-typed id, or one queued by a build from before the stamp. The console's clock may be
	    unset, in which case raQueueStampToUnix() has already refused the date and `now` is nonsense
	    too. And the clock may have moved backwards between earning and submitting, which makes the
	    subtraction wrap; that is caught by comparing before subtracting rather than after.
	*/
	seconds = 0;
	offset[0] = 0;
	if (when) {
		const u32 now = (u32)time(NULL);

		if (now >= when) {
			seconds = now - when;
		}
		/*
		    A year, as a sanity bound rather than a rule. Past it the likelier explanation is a clock
		    that is wrong at one end than an achievement genuinely waiting that long to be sent, and
		    the server is better served by no claim than by an implausible one.
		*/
		if (seconds > 365u * 24u * 60u * 60u) {
			seconds = 0;
		}
		if (seconds) {
			sniprintf(offset, sizeof(offset), "&o=%lu", (unsigned long)seconds);
		}
	}

	/*
	    Signed with the raw username, encoded with the escaped one. Both come from the same string,
	    and the reason they must is in raQueueSign() -- which also takes `seconds`, because `o=` in the
	    URL and the two extra fields in the digest have to appear together or the server refuses it.
	*/
	raQueueSign(id, raUser, hardcore, seconds, signature);

	if (!raNetUrlEncode(raUser, user, sizeof(user))) {
		raWifiLog("\x1b[31musername too long to encode\x1b[37m\n");
		q->kept++;
		return;
	}
	if (sniprintf(path, sizeof(path),
	              "/dorequest.php?r=awardachievement&u=%s&t=%s&a=%lu&h=%d%s&v=%s",
	              user, raToken, (unsigned long)id, hardcore, offset, signature)
	    >= (int)sizeof(path)) {
		raWifiLog("\x1b[31maward request too long\x1b[37m\n");
		q->kept++;
		return;
	}
	/*
	    Said out loud, because this is the only number in the request that cannot be checked against
	    anything else afterwards: the reply does not echo it back. If the console's clock is wrong the
	    log is where that shows.
	*/
	if (seconds) {
		raWifiLog("  %lu  earned %lu s ago\n", (unsigned long)id, (unsigned long)seconds);
	} else if (when) {
		raWifiLog("\x1b[33m  %lu  stamp unusable, sent without o=\x1b[37m\n", (unsigned long)id);
	}

	memset(&p, 0, sizeof(p));
	got = raNetHttpGet(RA_NET_HOST, path, response, sizeof(response), &p);
	raWifiReportAttempts("award", &p);
	if (got < 0) {
		/*
		    Nobody saw it. Kept, and said out loud rather than folded into a total, because "the
		    network dropped one" and "the server refused one" are different problems.
		*/
		raWifiLog("\x1b[33m  %lu  not sent (step %d), still owed\x1b[37m\n",
		          (unsigned long)id, -got);
		q->kept++;
		return;
	}

	q->sent++;
	if (raNetJsonTrue(response, "Success")) {
		q->accepted++;
		raWifiLog("\x1b[32m  %lu  awarded\x1b[37m\n", (unsigned long)id);
		/*
		    Logged on *success* too, and that is not verbosity. The first accepted award read
		    `93119 awarded` and the very next rung still reported one unlock, the server's own
		    notice -- so Success:true and the account holding the achievement are not the same
		    statement, and a one-word summary cannot tell them apart. The reply carries
		    AchievementID, Score and AchievementsRemaining; those are what say whether anything
		    was recorded. See docs/retroachievements.md.
		*/
		raWifiLogBody("award reply", raNetBody(response));
		return;
	}

	/*
	    Answered and not accepted. Cleared anyway -- see the rule above -- and the server's own words
	    go in the log, because this is the one place where its reply is the only thing that can say
	    whether the id was wrong, the signature was wrong, or it was already held.
	*/
	q->refused++;
	{
		char error[160];

		if (raNetJsonString(response, "Error", error, sizeof(error))) {
			raWifiLog("\x1b[33m  %lu  refused: %s\x1b[37m\n", (unsigned long)id, error);
		} else {
			raWifiLog("\x1b[33m  %lu  refused\x1b[37m\n", (unsigned long)id);
		}
		raWifiLogBody("award reply", raNetBody(response));
	}
}

/*
    The queue's scratch, at file scope so the two functions that touch it share one copy.

    That is a measurement, not tidiness. When raWifiStagePending() below had statics of its own -- a
    second RA_QUEUE_BYTES buffer and a second raQueue, which carries a game code and title per entry
    now -- it cost this launcher **8,192 bytes of its safe heap**, read straight off two hardware logs
    from the same console, same game, same boot:

        with two copies    heap after hash 20480 safe + 9984 free
        with one           heap after hash 28672 safe + 10912 free

    Nothing in the ladder failed for it, which is exactly why it went unnoticed for two builds: stage
    15 still staged all 45 definitions and the run still reached 15 of 15. The number was sitting
    beside the success the whole time.

    Sharing is safe because raWifiStagePending() runs only after raWifiSubmitQueue() has returned.
*/
static char    file[RA_QUEUE_BYTES];
static raQueue q;

static void raWifiSubmitQueue(const raConfig* cfg, bool sdFound) {
	const char* const path = sdFound ? RA_QUEUE_PATH : RA_QUEUE_PATH_FAT;
	int               keep[RA_QUEUE_MAX];
	int               keepCount = 0;
	int               i;
	FILE*             f;
	size_t            got;

	memset(&q, 0, sizeof(q));

	f = fopen(path, "rb");
	if (!f) {
		/*
		    No queue file is the normal state of a fresh install, and it is also what the cardengine
		    needs created for it: it can write into clusters that exist and cannot allocate any. So
		    the file is made here, full length, zero filled -- and this boot has nothing to send.
		*/
		f = fopen(path, "wb");
		if (!f) {
			raWifiLog("\x1b[33mno %s and it could not be created\x1b[37m\n", path);
			return;
		}
		memset(file, 0, sizeof(file));
		got = fwrite(file, 1, sizeof(file), f);
		fclose(f);
		if (got != sizeof(file)) {
			raWifiLog("\x1b[33m%s is short -- the card refused a write\x1b[37m\n", path);
			return;
		}
		verdict.submitDone = 1;
		raWifiLog("queue            created, %d bytes, nothing owed\n", (int)sizeof(file));
		return;
	}

	got = fread(file, 1, sizeof(file), f);
	fclose(f);

	raQueueScan(&q, file, (int)got);
	if (q.count == 0) {
		/*
		    Read, and empty. A successful pass over the queue, which is why it counts as reaching the
		    rung: most boots earn nothing and a ladder that stopped at 11 on those would be reporting
		    a failure that did not happen.
		*/
		verdict.submitDone = 1;
		raWifiLog("queue            empty (%d bytes read)\n", (int)got);
		if (q.dropped) {
			raWifiLog("\x1b[33m%d unusable value(s) in the file\x1b[37m\n", q.dropped);
		}
		/*
		    Said out loud rather than cleaned quietly, because "empty" and "empty once the self-test's
		    fakes were thrown out" are different states, and the second one dates the card to a build
		    from before the ARM9 guard.
		*/
		if (q.synthetic) {
			raWifiLog("\x1b[33m%d self-test id(s) discarded, not achievements\x1b[37m\n",
			          q.synthetic);
		}
		/*
		    A file that is not RA_QUEUE_BYTES long is brought up to length before returning, and an
		    empty queue is the only path that could ever leave it short: every other exit rewrites the
		    whole thing.

		    This is not tidiness. The cardengine computes a record's offset as slot * RA_QUEUE_RECORD
		    and can reach slot RA_QUEUE_MAX - 1, but it cannot allocate -- it writes into clusters that
		    already exist. Against a file left at an older, smaller RA_QUEUE_RECORD, a high enough slot
		    writes past the end of it and into whatever cluster follows, which belongs to another file.
		    Raising RA_QUEUE_RECORD from 16 to 32 and then to 48 each left every existing card in that
		    state, and nothing else would ever have fixed it: the file only grows on a boot that had
		    something to send, and a card that cannot sync is exactly the one accumulating unlocks.

		    A file holding only synthetic ids is rewritten by the same code for a different reason:
		    the queue reads as empty, so nothing below would ever clear it, and the record would sit
		    there being counted by the in-game menu on every future boot.
		*/
		if (got != sizeof(file) || q.synthetic) {
			FILE* resize = fopen(path, "wb");

			if (resize) {
				const int was = (int)got;

				memset(file, 0, sizeof(file));
				got = fwrite(file, 1, sizeof(file), resize);
				fclose(resize);
				raWifiLog("queue            rewritten, %d bytes (was %d)\n", (int)got, was);
			} else {
				raWifiLog("\x1b[33mqueue is %d bytes, not %d, and could not be rewritten\x1b[37m\n",
				          (int)got, (int)sizeof(file));
			}
		}
		return;
	}

	raWifiLog("queue            %d to send\n", q.count);
	if (q.dropped) {
		raWifiLog("\x1b[33m%d unusable value(s) ignored\x1b[37m\n", q.dropped);
	}
	if (q.truncated) {
		raWifiLog("\x1b[33m%d beyond the %d the queue holds\x1b[37m\n", q.truncated, RA_QUEUE_MAX);
	}

	/*
	    `submit=0` in ra.cfg: report the queue and leave it alone. For testing rather than for players
	    -- an unlock is spent the moment it lands, because the server returns it in r=unlocks from then
	    on and the scanner leaves it out of the block, so the cheapest repeatable test case on the card
	    disappears. Checked after the queue has been read and counted, so the log still says what would
	    have been sent.
	*/
	if (!cfg->submit) {
		raWifiLog("\x1b[33msubmit=0 in ra.cfg; %d unlock(s) kept, nothing sent\x1b[37m\n", q.count);
		verdict.submitDone = 1;
		verdict.submitKept = (u16)q.count;
		return;
	}

	if (!raToken[0]) {
		/*
		    Nothing was sent, so nothing is cleared. The ids stay in the file and the next boot with a
		    working login sends them -- which is the behaviour a queue is for.
		*/
		raWifiLog("\x1b[33mno token; %d unlock(s) stay queued\x1b[37m\n", q.count);
		verdict.submitKept = (u16)q.count;
		return;
	}

	for (i = 0; i < q.count; i++) {
		const int before = q.kept;

		raWifiSubmitOne(q.ids[i], q.times[i], q.hardcore[i], &q);
		if (q.kept > before) {
			keep[keepCount++] = i;
		}
	}

	verdict.submitDone     = 1;
	verdict.submitAccepted = (u16)q.accepted;
	verdict.submitRefused  = (u16)q.refused;
	verdict.submitKept     = (u16)q.kept;

	raWifiLog("awarded %d, refused %d, still owed %d\n", q.accepted, q.refused, q.kept);
	/*
	    Said only when there is something to say, and worth saying because it is the one number in
	    this pass that ra.cfg does not explain. A boot configured softcore can legitimately submit
	    hardcore unlocks earned before the setting changed, and a log that did not mention it would
	    make that look like a bug in the mode handling rather than the point of it.
	*/
	if (q.hard) {
		raWifiLog("%d of them earned in hardcore, per the record\n", q.hard);
	}

	/*
	    Rewrite whatever is still owed, over the same length. Done even when keepCount is 0 -- that is
	    the clearing case and it is the common one.
	*/
	if (raQueuePack(&q, keep, keepCount, file, sizeof(file)) < 0) {
		raWifiLog("\x1b[31mthe queue could not be packed; %s left alone\x1b[37m\n", path);
		return;
	}
	f = fopen(path, "rb+");
	if (!f) {
		raWifiLog("\x1b[33mcould not rewrite %s; ids may be sent twice\x1b[37m\n", path);
		return;
	}
	got = fwrite(file, 1, sizeof(file), f);
	fclose(f);
	if (got != sizeof(file)) {
		raWifiLog("\x1b[33m%s is short -- ids may be sent twice\x1b[37m\n", path);
	}
}

/*
    Stage the pending tally for the in-game menu, and then hand the radio back.

    Written by re-reading the queue file rather than from the raQueue that was just sent, and that is
    the point: this has to describe what the file *ends up* holding, and raWifiSubmitQueue() leaves it
    by half a dozen different routes -- nothing owed, submit=0, no token, everything sent, some kept.
    Reading the result once is correct on all of them, where staging at each exit would be six chances
    to describe a state that did not happen. One file read at boot buys that.

    Staged even when there is nothing pending, because an empty block and a missing one mean different
    things to the menu: "nothing is waiting" against "this boot never looked".
*/
static void raWifiStagePending(bool sdFound, const char* ndsPath) {
	const char* const path = sdFound ? RA_QUEUE_PATH : RA_QUEUE_PATH_FAT;
	raPendingBlock*   block = (raPendingBlock*)CARDENGINEI_ARM9_RA_PENDING_BUFFERED_LOCATION;
	size_t            got = 0;
	FILE*             f;

	memset(&q, 0, sizeof(q));
	memset(file, 0, sizeof(file));

	f = fopen(path, "rb");
	if (f) {
		got = fread(file, 1, sizeof(file), f);
		fclose(f);
	}
	raQueueScan(&q, file, (int)got);
	raQueueTally(&q, (u32)time(NULL), block);

	/*
	    Which game is about to start, read from the first sixteen bytes of its own header -- the same
	    two fields the cardengine stamps into a record. Read here rather than in the menu because the
	    launcher has the file open path and the menu has neither a filesystem nor a reliable way to
	    know which of the two header addresses this game uses.
	*/
	if (ndsPath) {
		FILE* rom = fopen(ndsPath, "rb");

		if (rom) {
			char head[RA_QUEUE_TITLE + RA_QUEUE_CODE];

			if (fread(head, 1, sizeof(head), rom) == sizeof(head)) {
				int n;

				for (n = 0; n < RA_QUEUE_TITLE; n++) {
					const unsigned char c = (unsigned char)head[n];

					if (c < ' ' || c > '~') {
						break;
					}
					block->thisTitle[n] = head[n];
				}
				while (n > 0 && block->thisTitle[n - 1] == ' ') {
					n--;
				}
				block->thisTitle[n] = 0;

				for (n = 0; n < RA_QUEUE_CODE; n++) {
					const unsigned char c = (unsigned char)head[RA_QUEUE_TITLE + n];

					if (c < ' ' || c > '~') {
						break;
					}
					block->thisCode[n] = head[RA_QUEUE_TITLE + n];
				}
				block->thisCode[n] = 0;
			}
			fclose(rom);
		}
	}

	if (block->total) {
		raWifiLog("pending          %d unlock(s) across %d game(s)\n",
		          block->total, block->games);
	}
}

/*
    Stage 13: r=unlocks -- which of these has the account already earned.

    Runs before the fetch, because that is the only order in which it helps: the block is 88% full
    with a set of this size, and every definition already earned is one that does not need to be in
    it. The arena and the per-frame budget follow the block down.

    **Fails open.** A request that does not answer leaves the skip list empty and every definition is
    staged, which is exactly what happened before this stage existed. The distinction the code cares
    about is between "the account has earned nothing" and "we could not ask" -- both stage everything,
    and only the second is worth a warning.

    h=0 because this fork is softcore, which ra.cfg says and the server independently agrees with:
    see the `Warning: Unknown Emulator` notice.
*/
static void raWifiUnlocks(const raConfig* cfg) {
	static char   response[4096];
	char          user[3 * sizeof(cfg->username)];
	char          path[384];
	raNetProgress p;
	int           got;
	int           count;

	unlockCount   = 0;
	unlockNotices = 0;

	if (!verdict.gameId || !raToken[0] || !raNetUrlEncode(cfg->username, user, sizeof(user))) {
		raWifiLog("\x1b[33mno GameID or token; staging the whole set\x1b[37m\n");
		return;
	}
	if (sniprintf(path, sizeof(path), "/dorequest.php?r=unlocks&u=%s&t=%s&g=%lu&h=0",
	              user, raToken, (unsigned long)verdict.gameId) >= (int)sizeof(path)) {
		raWifiLog("\x1b[31munlocks request too long\x1b[37m\n");
		return;
	}

	memset(&p, 0, sizeof(p));
	got = raNetHttpGet(RA_NET_HOST, path, response, sizeof(response), &p);
	raWifiReportAttempts("unlocks", &p);
	if (got < 0) {
		raWifiLog("\x1b[33munlocks HTTP failed at step %d; staging the whole set\x1b[37m\n", -got);
		return;
	}
	raWifiLog("%d bytes back\n", got);

	/*
	    The whole reply, before anything is parsed out of it. Three runs reported a count from this
	    endpoint without ever showing what produced the count, and then a run awarded 93119 and this
	    endpoint still named only the notice -- at which point the parsed number is the one thing that
	    cannot settle the question. Under 1 KB, and the log has room.
	*/
	raWifiLogBody("unlocks reply", raNetBody(response));

	count = raNetJsonIdList(response, "UserUnlocks", unlockedIds, RA_WIFI_UNLOCKS_MAX);
	if (count < 0) {
		/*
		    No UserUnlocks key at all, which is a failure rather than an empty account -- the two
		    look identical from the skip list and mean different things about whether to trust it.
		*/
		raWifiLog("\x1b[33mno UserUnlocks in the reply; staging the whole set\x1b[37m\n");
		raWifiLog("body: %s\n", raNetBody(response));
		return;
	}

	unlockCount           = (u16)count;
	verdict.unlocksKnown  = 1;
	verdict.unlockCount   = unlockCount;
	raWifiLog("\x1b[32malready earned    %d\x1b[37m\n", count);

	/*
	    The ids themselves, and this line exists because a run reported `already earned 1` while the
	    fetch kept all 55 -- so the server had named an achievement that was not in the set we staged.
	    A count could not say which, and *which* was the only thing that distinguished the
	    possibilities: a pseudo-achievement we filter, one belonging to a different subset of this
	    game, or a numbering that does not match `r=patch`'s at all.

	    It was the first. The id was 101000001, the `Warning: Unknown Emulator` notice, which this
	    client drops from the block by the same boundary -- so it is labelled here rather than left
	    for the reader to cross-reference against the stage 13 warning. Nothing about the set's own
	    numbering is settled by that; see the note in docs/retroachievements.md.

	    Eight at most, because the point is to identify a mismatch rather than to dump a full account.
	*/
	{
		int shown = (count < 8) ? count : 8;
		int i;

		for (i = 0; i < count; i++) {
			if (unlockedIds[i] >= RA_ODD_ID_FROM) {
				unlockNotices++;
			}
		}
		for (i = 0; i < shown; i++) {
			raWifiLog("  earned id      %lu%s\n", (unsigned long)unlockedIds[i],
			          (unlockedIds[i] >= RA_ODD_ID_FROM) ? "  (server notice)" : "");
		}
		if (count > shown) {
			raWifiLog("  ...and %d more\n", count - shown);
		}
	}
	if (count >= RA_WIFI_UNLOCKS_MAX) {
		/*
		    Truncation is safe and is still said out loud: a short skip list only means a few
		    already-earned achievements get staged again, which costs block space rather than
		    correctness.
		*/
		raWifiLog("\x1b[33mskip list full at %d; some will be staged again\x1b[37m\n",
		          RA_WIFI_UNLOCKS_MAX);
	}
}

/*
    Stage 13: r=patch -- fetch the real achievement set, and put it where the game will find it.

    This is the rung the whole ladder was for. Everything below it was a measurement; this
    produces the artifact, and it produces it into the same staging block a hand-written
    ra_achievements.txt already goes to -- CARDENGINEI_ARM9_RA_DEFS_BUFFERED_LOCATION, with the
    same header, written by the same rules. The bootloader copies that into DSi WRAM and the
    cardengine's reader splits it into lines. None of that path is new, which is the point of
    having built it for a file first.

    The reply is never held. See ra_patch.c: it is over 100 K for this game, the heap has about
    85 K left, and the destination is 32 K -- so the definitions are pulled out of the byte
    stream as it arrives and the rest is discarded going past.

    Two things this deliberately does not do. It does not log the reply, because logging what
    does not fit in memory to a card over a live WiFi link is the one thing #1d says not to do,
    and because the definitions themselves are the evidence. And it does not log the token: the
    request carries it, the log records that a request was made.
*/
/*
    The fetch failed, so put the user's own file back.

    Stage 12 streams the reply straight into the definitions block, which is where
    loadRaDefinitions() had already put sd:/_nds/nds-bootstrap/ra_achievements.txt -- so by the time
    a fetch fails, the file's text is already gone. In mode 1 that did not matter, because that build
    never boots a game. In mode 2 it means a console with no network would lose the definitions it
    would otherwise have run, which is strictly worse than not having tried.

    Re-staging rather than preserving, because the reply is three times the size of the block and
    there is nowhere else to scan it. Cheap: one file read, and only on the path that already failed.
*/
/* Defined further down, beside the cache writer it mirrors. */
static bool raWifiCacheLoad(bool sdFound);

/*
    Put something in the block after a fetch that did not produce one.

    The cache first, because it is the set *for this ROM* -- a previous boot's own answer from the
    server. ra_achievements.txt is the fallback under it: one hand-managed file that has no way to know
    which game is running, which is exactly the limitation the cache was added to remove.

    Called from every failure path in the fetch, because the fetch streams into this block and destroys
    whatever was there on its way even when it then fails.
*/
static void raWifiRestoreDefinitionsFile(void) {
	if (raWifiCacheLoad(raWifiSdFound)) {
		return;
	}
	loadRaDefinitions();
	if (*(u32*)CARDENGINEI_ARM9_RA_DEFS_BUFFERED_LOCATION == CARDENGINEI_ARM9_RA_DEFS_MAGIC) {
		raWifiLog("restored         ra_achievements.txt (%lu bytes)\n",
		          (unsigned long)*(u32*)(CARDENGINEI_ARM9_RA_DEFS_BUFFERED_LOCATION + 4));
	}
}

/*
    A heartbeat on the screen while the set streams in, and screen *only*.

    The recv() loop cannot yield -- it blocks on the socket -- so nothing else can report
    progress while a hundred kilobytes come down, and a run that stalls halfway would look
    identical to one that never started. A dot every 8 K makes the difference visible without a
    single SD transaction: iprintf() here is writes to the console's own memory, where
    raWifiLog() would be libfat and an fsync() over the FIFO to the ARM7 that is at that moment
    running the WiFi stack. That combination is what froze a v6 run.
*/
static u32 patchProgress;

static void raWifiPatchSink(void* ctx, const char* data, int length) {
	raPatchFeed(ctx, data, length);

	patchProgress += (u32)length;
	if (patchProgress >= 8192) {
		patchProgress = 0;
		/*
		    Quiet mode has the caption and the bar for this, and a row of dots would run off the
		    end of the line the caption is on. The recv() loop cannot yield, so the spinner cannot
		    turn here either -- which is exactly why the dots exist in verbose mode.
		*/
		if (raVerbose) {
			iprintf(".");
		}
	}
}

static void raWifiFetchPatch(const raConfig* cfg) {
	static raPatch patch;
	char           user[3 * sizeof(cfg->username)];
	char           path[384];
	char* const    block = (char*)(CARDENGINEI_ARM9_RA_DEFS_BUFFERED_LOCATION
	                               + CARDENGINEI_ARM9_RA_DEFS_HEADER);
	/*
	    Minus the three things that live in the top of this same reservation: the pending tally the
	    menu's Sync Pending page reads, the viewer's index, and the session block that tells the menu
	    whether it may edit RAM. Subtracted here rather than trusted to stay clear -- the block is
	    written by a scanner that fills whatever it is given, so the only thing standing between a
	    large set and those three structures is this number.
	*/
	const u32      blockMax = CARDENGINEI_ARM9_RA_DEFS_MAX
	                          - CARDENGINEI_ARM9_RA_PENDING_MAX
	                          - CARDENGINEI_ARM9_RA_VIEWER_MAX
	                          - CARDENGINEI_ARM9_RA_SESSION_MAX
	                          - CARDENGINEI_ARM9_RA_DEFS_HEADER - 1;
	raNetProgress  p;
	int            got;

	if (!verdict.gameId) {
		raWifiLog("\x1b[33mno GameID; the set cannot be asked for\x1b[37m\n");
		return;
	}
	if (!raToken[0]) {
		/*
		    r=patch is the first request in this ladder that needs credentials. Said plainly
		    because the previous rung succeeds without them, so "the server knew the ROM but the
		    set did not arrive" would otherwise look like a server problem.
		*/
		raWifiLog("\x1b[33mno token; r=patch needs a login\x1b[37m\n");
		return;
	}
	if (!raNetUrlEncode(cfg->username, user, sizeof(user))) {
		raWifiLog("\x1b[31musername too long to encode\x1b[37m\n");
		return;
	}
	/*
	    The token goes in unencoded. RA's tokens are alphanumeric, and encoding one would be
	    fine -- but it would mean copying a secret into a second buffer for no reason, and this
	    file is careful about how many places hold one.
	*/
	if (sniprintf(path, sizeof(path), "/dorequest.php?r=patch&u=%s&t=%s&g=%lu",
	              user, raToken, (unsigned long)verdict.gameId) >= (int)sizeof(path)) {
		raWifiLog("\x1b[31mpatch request too long\x1b[37m\n");
		return;
	}

	raWifiLog("asking for       game %lu\n", (unsigned long)verdict.gameId);
	/*
	    Zeroed before the request rather than after it, so a reply that never comes leaves the
	    block invalid instead of leaving whatever was there looking valid.
	*/
	*(u32*)CARDENGINEI_ARM9_RA_DEFS_BUFFERED_LOCATION = 0;
	raPatchReset(&patch, block, blockMax);
	/*
	    Set after the reset, which zeroes the struct. Empty when r=unlocks did not answer, and then
	    the scanner behaves exactly as it did before that stage existed.
	*/
	patch.skipIds   = unlockedIds;
	patch.skipCount = unlockCount;
	patch.whenOf    = raWifiEarnedWhen;
	patchProgress   = 0;

	memset(&p, 0, sizeof(p));
	got = raNetHttpGetStream(RA_NET_HOST, path, raWifiPatchSink, &patch, &p);
	raPatchFinish(&patch);
	if (raVerbose) {
		iprintf("\n");
	}
	raWifiReportAttempts("patch", &p);

	if (got < -1000) {
		raWifiLog("\x1b[31mthe server answered HTTP %d\x1b[37m\n", -got - 1000);
		raWifiRestoreDefinitionsFile();
		return;
	}
	if (got < 0) {
		raWifiLog("\x1b[31mpatch HTTP failed at step %d\x1b[37m\n", -got);
		raWifiRestoreDefinitionsFile();
		return;
	}

	raWifiLog("body was         %d bytes\n", got);
	raWifiLog("definitions      %u kept, %u unofficial\n",
	          patch.kept, patch.unofficial);
	/*
	    The reading step 5 exists for. A set where every definition carried its own achievement id is
	    a set that can be reported on; one id short is one achievement that can never be awarded, and
	    the block alone cannot say which of those happened.
	*/
	raWifiLog("ids              %u with, %u without\n", patch.withId, patch.withoutId);
	/*
	    And the labels, so the on-screen notification naming the wrong thing -- or nothing -- has an
	    account here rather than being diagnosed from a photograph. `clipped` is cosmetic by design;
	    `no room` is the block being full enough that labels were traded away to keep the achievements
	    themselves, which is the point at which the block size is worth revisiting.
	*/
	raWifiLog("titles           %u with", patch.withTitle);
	if (patch.titleCut) {
		raWifiLog(", %u clipped", patch.titleCut);
	}
	if (patch.titleNoRoom) {
		raWifiLog("\x1b[33m, %u dropped for room\x1b[37m", patch.titleNoRoom);
	}
	raWifiLog("\n");
	/*
	    The two viewer fields on their own line, and the degradation order read left to right: the
	    description is the first thing traded away when the block fills, points second, the title
	    last. A run where `desc` reports fewer than `titles` is a run where the block is the binding
	    constraint on what the in-game viewer can show, which is the number worth having.
	*/
	raWifiLog("desc / points    %u / %u with", patch.withDesc, patch.withPoints);
	if (patch.descCut) {
		raWifiLog(", %u clipped", patch.descCut);
	}
	if (patch.descNoRoom || patch.pointsNoRoom) {
		raWifiLog("\x1b[33m, %u / %u dropped for room\x1b[37m",
		          patch.descNoRoom, patch.pointsNoRoom);
	}
	raWifiLog("\n");
	/*
	    And what the viewer gets for the half of the set the player has already finished. Separate
	    from `definitions` on purpose: those are armed and these are not, and a set whose armed half
	    is empty because it has been completed is a very different reading from one whose armed half
	    is empty because the block overflowed.
	*/
	/*
	    The measurement rich presence is waiting on, and the only thing this build does about it.
	    Rendering a script on the console is reachable -- it is memory reads, which is what this fork
	    already does sixty times a second -- and sending one is not, because the game's boot kills the
	    link. So the open question is whether a script would *fit* beside the set, and one number off a
	    real game answers it before anything is designed.
	*/
	if (patch.richSeen) {
		raWifiLog("rich presence    %lu bytes (measured, not kept)\n",
		          (unsigned long)patch.richBytes);
	} else {
		raWifiLog("rich presence    none in the reply\n");
	}
	if (patch.earned || patch.earnedNoRoom) {
		raWifiLog("earned shown     %u", patch.earned);
		if (patch.earnedNoRoom) {
			raWifiLog("\x1b[33m, %u with no room\x1b[37m", patch.earnedNoRoom);
		}
		raWifiLog("\n");
	}
	if (patch.skipCount) {
		/*
		    Printed whenever there was a skip list at all, matched or not. A missing line used to mean
		    "nothing matched", which is a reading by absence -- and absence is indistinguishable from
		    a line that was never written. `0 of 1` says the thing that a silence only implies.
		*/
		raWifiLog("already earned   %u of %u matched this set\n",
		          patch.alreadyDone, patch.skipCount);
		/*
		    Two different readings, and only one of them is a warning.

		    Every unmatched id being a server notice is fully accounted for: the scanner drops those
		    from the block by the same boundary, so of course the skip list finds nothing to skip.
		    That is this account's whole mismatch and it needs a fact, not a colour. An id left over
		    after the notices are subtracted is the interesting case -- the set does not contain it
		    and we cannot say why -- and that one stays yellow.
		*/
		if (patch.alreadyDone + unlockNotices < patch.skipCount) {
			raWifiLog("\x1b[33m%u named id(s) this set does not contain\x1b[37m\n",
			          patch.skipCount - patch.alreadyDone - unlockNotices);
		} else if (unlockNotices) {
			raWifiLog("of those, %u is the server's own notice\n", unlockNotices);
		}
	}
	/*
	    The one this project cannot explain, with the reply's own bytes around it. Two lookups
	    established that the set publishes 55 achievements while 56 core definitions arrive, and that
	    the extra id returns NOT FOUND -- so what is left is to see what object it is in, verbatim,
	    rather than to infer a filter from a threshold. See RA_ODD_ID_FROM.
	*/
	if (patch.oddIds) {
		/*
		    The server's own message to the player, passed through rather than swallowed. The first
		    one read "Warning: Unknown Emulator -- Hardcore unlocks cannot be earned using this
		    emulator", which is RetroAchievements telling us it does not recognise the User-Agent
		    this client sends. Worth seeing on every run, because it is the server's answer to a
		    question this project has not asked it yet.
		*/
		raWifiLog("\x1b[33m%u server notice(s) dropped, first id %lu\x1b[37m\n",
		          patch.oddIds, (unsigned long)patch.oddId);
		raWifiLog("context: %s\n", patch.oddContext);
	}
	if (patch.dropped || patch.tooLong || patch.cutShort || patch.empty
	 || patch.oddFlags || patch.noFlags) {
		/*
		    Printed only when there is something to print, and printed in full when there is:
		    a set that lost thirty definitions to a full block looks exactly like a set with
		    thirty fewer achievements, from the block alone.
		*/
		raWifiLog("lost             %u full, %u too long, %u cut, %u empty\n",
		          patch.dropped, patch.tooLong, patch.cutShort, patch.empty);
		if (patch.oddFlags || patch.noFlags) {
			raWifiLog("odd              %u other flags, %u no flags\n",
			          patch.oddFlags, patch.noFlags);
		}
	}
	raWifiLog("block            %lu of %lu used, %lu wanted\n",
	          (unsigned long)patch.used, (unsigned long)blockMax,
	          (unsigned long)patch.wanted);
	raWifiLog("memaddr length   %lu shortest, %lu longest, of %d\n",
	          (unsigned long)patch.shortest, (unsigned long)patch.longest,
	          RA_PATCH_MEMADDR_MAX - 1);

	if (!patch.kept) {
		raWifiLog("\x1b[31mno definitions in the reply\x1b[37m\n");
		raWifiRestoreDefinitionsFile();
		return;
	}

	/*
	    The first three definitions, clipped, as the cheapest possible check that these are
	    memaddr strings and not 30 K of something else that happened to sit between quotes.

	    Three rather than one because of what the first run showed: a single `1=1.300.` is
	    ambiguous -- it is valid syntax for "always true, 300 hits" and it is also exactly what a
	    fragment would look like. Three consecutive entries are not ambiguous in the same way, and
	    ra_definitions.txt below settles it completely.
	*/
	{
		const char* at = block;
		int         n;

		for (n = 0; n < 3 && *at; n++) {
			char line[57];
			u32  full = 0;
			u32  i;

			/* The real length first, then the clip -- a clipped length would say nothing. */
			while (at[full] && at[full] != '\n') {
				full++;
			}
			for (i = 0; i < sizeof(line) - 1 && i < full; i++) {
				line[i] = at[i];
			}
			line[i] = 0;
			raWifiLog("def %d %5lu      %s\n", n + 1, (unsigned long)full, line);

			at += full;
			if (*at) {
				at++;
			}
		}
	}

	/*
	    Header last, and the magic last of all -- the same order loadRaDefinitions() uses, for
	    the same reason: the bootloader copies this block unconditionally and decides whether to
	    believe it by the magic alone, so a magic written before the length would make a
	    half-finished block indistinguishable from a finished one.
	*/
	*(u32*)(CARDENGINEI_ARM9_RA_DEFS_BUFFERED_LOCATION + 4) = patch.used;
	*(u32*)CARDENGINEI_ARM9_RA_DEFS_BUFFERED_LOCATION       = CARDENGINEI_ARM9_RA_DEFS_MAGIC;

	verdict.patched   = 1;
	verdict.defsKept  = patch.kept;
	verdict.defsBytes = patch.used;
	raWifiLog("\x1b[32mstaged %u definitions for the cardengine\x1b[37m\n", patch.kept);
}

/*
    Write the staged definitions out as text, so the set itself can be checked rather than its
    summary.

    Called last -- after the summary has been written and fsync()'d -- and that ordering is the
    whole reason it is a separate function. This is tens of kilobytes of SD I/O with the WiFi link
    still up, which is exactly the ARM7 contention #1d is about and which froze a run once
    already. Putting it after the summary means a freeze here costs the file and keeps the
    reading; putting it before would have risked the reverse.

    The file is the same format the launcher already *reads* from ra_achievements.txt, so it is
    not only evidence: copy it to sd:/_nds/nds-bootstrap/ and a normal build boots the game with
    the server's own achievement set, with no network involved. That is a useful thing to have
    while step 4 is still being built.
*/
/*
    Add this ROM to sd:/ra_hashes.txt, once.

    Called from stage 0b, right after the hash exists and before anything can fail. The point is
    picking the next game to test: the launcher already prints the hash on screen and into the log two
    seconds into the boot, but the log is truncated every boot, so a shelf of ROMs has to be walked one
    copy-off-the-card at a time. This file is append-only, so booting each candidate once leaves a list
    to check against the site in one sitting.

    Deduplicated by reading what is already there and looking for the hash. Bounded at
    RA_HASHES_READ_MAX and *fails toward appending* if the file is longer than that -- a duplicate line
    costs nothing and a missing one costs a boot.

    The ROM's own name goes on the line, because a bare hash is not something anyone can act on.
*/
static void raWifiRecordHash(bool sdFound, const char* romPath) {
	const char* const path = sdFound ? RA_HASHES_PATH : RA_HASHES_PATH_FAT;
	static char       seen[RA_HASHES_READ_MAX];
	FILE*             f;
	const char*       name;

	if (!romHash[0]) {
		return;
	}

	f = fopen(path, "rb");
	if (f) {
		const size_t got = fread(seen, 1, sizeof(seen) - 1, f);

		fclose(f);
		seen[got] = 0;
		if (strstr(seen, romHash)) {
			raWifiLog("hashes           already in %s\n", path);
			return;
		}
	}

	/* Just the file name: the directory is the same for every entry and the line has to stay short. */
	name = romPath ? strrchr(romPath, '/') : NULL;
	name = name ? name + 1 : (romPath ? romPath : "(unknown)");

	f = fopen(path, "ab");
	if (!f) {
		raWifiLog("\x1b[33mcould not append to %s\x1b[37m\n", path);
		return;
	}
	fprintf(f, "%s  %s\n", romHash, name);
	fclose(f);
	raWifiLog("hashes           added to %s\n", path);
}

/*
    Write whatever is staged in the definitions block to a file.

    Factored out of raWifiDumpDefinitions() so the per-game cache writes the same bytes through the same
    path: two writers of one format would be two chances for them to disagree, and the cache is read
    back by the same reader that reads ra_achievements.txt.

    Returns false and says nothing -- the caller knows which file it was and what that means.
*/
static bool raWifiWriteBlockTo(const char* path) {
	const char* const text = (const char*)(CARDENGINEI_ARM9_RA_DEFS_BUFFERED_LOCATION
	                                       + CARDENGINEI_ARM9_RA_DEFS_HEADER);
	const u32         length = *(u32*)(CARDENGINEI_ARM9_RA_DEFS_BUFFERED_LOCATION + 4);
	FILE*             out;
	bool              ok;

	if (*(u32*)CARDENGINEI_ARM9_RA_DEFS_BUFFERED_LOCATION != CARDENGINEI_ARM9_RA_DEFS_MAGIC
	 || length == 0) {
		return false;
	}
	out = fopen(path, "w");
	if (!out) {
		return false;
	}
	ok = (fwrite(text, 1, length, out) == length);
	fclose(out);
	return ok;
}

/*
    The fetched set, kept for this exact ROM.

    Named by the hash rather than the GameID, because the GameID needs the network and the hash does not
    -- a cache a later boot cannot find without doing the request is not a cache. See RA_CACHE_DIR.

    mkdir every time and ignore the result: there is no portable "does this directory exist" here that is
    cheaper than trying, and an existing directory failing is the normal case.
*/
static void raWifiCachePath(char* out, size_t outSize, bool sdFound) {
	sniprintf(out, outSize, "%s/%s.txt",
	          sdFound ? RA_CACHE_DIR : RA_CACHE_DIR_FAT, romHash);
}

static void raWifiCacheWrite(bool sdFound) {
	char path[96];

	if (!romHash[0]) {
		return;
	}
	mkdir(sdFound ? RA_CACHE_DIR : RA_CACHE_DIR_FAT, 0777);
	raWifiCachePath(path, sizeof(path), sdFound);

	if (raWifiWriteBlockTo(path)) {
		raWifiLog("cached           %s\n", path);
	} else {
		raWifiLog("\x1b[33mcould not cache to %s\x1b[37m\n", path);
	}
}

/*
    ...and load it back, into the same block the fetch would have filled.

    Byte-for-byte the staging conf_sd.cpp does for ra_achievements.txt -- magic written *last*, so a
    half-written block is never mistaken for a whole one. Used when the fetch did not happen or did not
    work, which is the case the cache exists for.
*/
static bool raWifiCacheLoad(bool sdFound) {
	char   path[96];
	FILE*  file;
	long   size;
	bool   ok = false;

	if (!romHash[0]) {
		return false;
	}
	raWifiCachePath(path, sizeof(path), sdFound);
	file = fopen(path, "rb");
	if (!file) {
		return false;
	}
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, 0, SEEK_SET);

	if (size > 0
	 && size < (long)(CARDENGINEI_ARM9_RA_DEFS_MAX - CARDENGINEI_ARM9_RA_DEFS_HEADER - 1)) {
		u8* text = (u8*)(CARDENGINEI_ARM9_RA_DEFS_BUFFERED_LOCATION
		                 + CARDENGINEI_ARM9_RA_DEFS_HEADER);

		if (fread(text, 1, size, file) == (size_t)size) {
			text[size] = 0;
			*(u32*)(CARDENGINEI_ARM9_RA_DEFS_BUFFERED_LOCATION + 4) = (u32)size;
			*(u32*)CARDENGINEI_ARM9_RA_DEFS_BUFFERED_LOCATION = CARDENGINEI_ARM9_RA_DEFS_MAGIC;
			ok = true;
		}
	}
	fclose(file);
	if (ok) {
		raWifiLog("\x1b[32mcached set       %ld bytes for this ROM\x1b[37m\n", size);
	}
	return ok;
}

static void raWifiDumpDefinitions(bool sdFound) {
	const char* const path = sdFound ? RA_DEFS_DUMP_PATH : RA_DEFS_DUMP_PATH_FAT;
	const char* const text = (const char*)(CARDENGINEI_ARM9_RA_DEFS_BUFFERED_LOCATION
	                                       + CARDENGINEI_ARM9_RA_DEFS_HEADER);
	const u32         length = *(u32*)(CARDENGINEI_ARM9_RA_DEFS_BUFFERED_LOCATION + 4);
	FILE*             out;

	if (*(u32*)CARDENGINEI_ARM9_RA_DEFS_BUFFERED_LOCATION != CARDENGINEI_ARM9_RA_DEFS_MAGIC
	 || length == 0) {
		return;
	}

	out = fopen(path, "w");
	if (!out) {
		raWifiLog("\x1b[33mcould not write %s\x1b[37m\n", path);
		return;
	}
	/*
	    Written and closed, not fsync()'d and left open like the log. The log is deliberately
	    never closed because a hang must not lose it; this one is finished when it is finished, and
	    close() is what makes libfat write the directory entry -- the same lesson the zero-byte log
	    taught.
	*/
	if (fwrite(text, 1, length, out) == length) {
		fclose(out);
		raWifiLog("definitions to   %s\n", path);
	} else {
		fclose(out);
		raWifiLog("\x1b[33m%s is short -- the card refused a write\x1b[37m\n", path);
	}
}

/*
    Give the radio back, so the bootloader can have both CPUs.

    This is the whole reason mode 2 can exist. Four things are live once the ladder has run, and
    every one of them is a thing that fires after the bootloader has replaced the code it belongs
    to:

      the ARM7's SDIO card interrupt and its TIMER3, which dsiwifi's own wifi_card_deinit() masks;
      the ARM9's TIMER3, which drives ath_lwip_tick() every 100 ms;
      and dsiwifi's datamsg handler on FIFO_DSWIFI.

    Order is deliberate. The ARM7 goes first, because it is the one holding the chip: until its
    card interrupt is masked the radio can still call it, and a call into overwritten code is the
    failure being avoided. Only then are the ARM9's own two stopped -- if it were the other way
    round, the ARM7's log messages would arrive at a channel with no handler while it was still
    working.

    Returns false when the ARM7 never acknowledges. That is the one outcome where booting is worse
    than not booting, and the caller is the one that decides.
*/
bool raWifiShutdown(void) {
	int frames = RA_WIFI_WAIT_STOP * 60;

	raWifiLog("\n-- giving the radio back --\n");

	fifoSendValue32(RA_WIFI_ARM7_STOP_CHANNEL, 1);
	while (frames-- > 0) {
		if (fifoCheckValue32(RA_WIFI_ARM7_STOP_CHANNEL)) {
			break;
		}
		raWifiIdle();
	}
	if (frames <= 0) {
		raWifiLog("\x1b[31mthe ARM7 never confirmed wifi_card_deinit()\x1b[37m\n"
		          "not booting: its card IRQ may still be live.\n");
		return false;
	}
	fifoGetValue32(RA_WIFI_ARM7_STOP_CHANNEL);
	raWifiLog("ARM7            deinitted\n");

	/*
	    The ARM9's two. timerStop(3) because dsiwifi's ARM9 half starts a 100 ms TIMER3 in
	    wifi_host_init() and nothing else in this launcher uses that timer; clearing the datamsg
	    handler because FIFO_DSWIFI is the channel dsiwifi drives its whole sequence over, and a
	    handler pointing into replaced code is the same hazard as a live interrupt.

	    The log handler goes too. raWifiCapture() only writes to a buffer, so it is harmless -- but
	    "harmless" is a property of this build rather than of the arrangement, and the drain below
	    is the last one there will be.
	*/
	timerStop(3);
	/*
	    ...and the two this file started for its own timing. A timer left running when the game boots
	    is the game's problem, and this one is ours.
	*/
	TIMER0_CR = 0;
	TIMER1_CR = 0;
	fifoSetDatamsgHandler(FIFO_DSWIFI, 0, 0);
	DSiWifi_SetLogHandler(0);

	raWifiDrain();
	raWifiLog("ARM9            timer and FIFO handler stopped\n");
	return true;
}

/*
    Everything that disqualifies a hardcore session, in one place, decided before a single request is
    signed.

    RetroAchievements' hardcore rules forbid cheats, savestates, rewind and slowdown. What this fork
    can be held to is what it can *observe*, and the honest reading of nds-bootstrap is:

      cheats        the cheat engine actually running, which is three files summed against
                    RA_CHEATS_MIN_BYTES and not `conf->cheatSize != 0`. Observable, gated here, and
                    gated again by the bootloader once it knows whether the engine fits. See the
                    raWifiProbe() call in main.cpp for what asking the easy question cost.
      savestates    **nds-bootstrap has none.** There is no savestate feature anywhere in the tree,
                    so there is nothing to switch off and nothing to check. Stated rather than left
                    as an assumed gap, because "we did not find it" and "it does not exist" are
                    different claims and only one of them is true here.
      RAM editing   the in-game menu's RAM viewer writes -- and **now refuses to when the session is
                    hardcore**. This was an unconditional refusal here for as long as it could not
                    be anything else: the editor is a page two button presses away rather than a
                    setting a player turns on, so there was no state to consult. There is now.
                    raWifiStageSession() below hands the menu the mode this boot settled on and the
                    menu holds its editor shut for it, which is what lets this function stop
                    refusing every session out of hand. See CARDENGINEI_ARM9_RA_SESSION_LOCATION,
                    and the `mode == 1` branch of ramViewer() where the refusal now lives.

    The rule for what to do about it: **fall back to softcore and say so**, never claim hardcore and
    hope. An unlock claimed as hardcore under a cheat file is exactly what gets a client's
    User-Agent refused, and this fork's User-Agent is still trying to be recognised at all.

    One disqualification is left and this function cannot make it, because it is not a fact about
    this console: the server does not recognise our User-Agent, and its own notice says it declines
    hardcore for a client it does not know. Whatever ra.cfg asks for, hardcore is not actually
    available until that is answered -- see "What is left" in docs/retroachievements.md. What this
    change buys is that the answer is now the only thing in the way.
*/
static u8 raWifiHardcoreRefused(const raConfig* cfg, bool cheatsOn) {
	if (!cfg->hardcore) {
		return RA_REFUSED_NONE;
	}
	if (cheatsOn) {
		raWifiLog("\x1b[33mhardcore refused: the cheat engine runs for this ROM\x1b[37m\n");
		return RA_REFUSED_CHEATS;
	}
	return RA_REFUSED_NONE;
}

/*
    Tell the in-game menu what kind of session it is about to be opened in.

    Called from the one place that knows the answer, and -- this is the whole point of it being its
    own block rather than a field in the pending tally -- called *before* every path that can leave
    raWifiProbe() early. The tally is staged at stage 13, so it does not exist on a boot with no
    access point. This does, because such a boot still falls through to `done:`, still loads this
    ROM's cached set, still plays the game and still queues unlocks for a later boot to submit as
    hardcore. The menu has to be told on those boots most of all.

    Written after raWifiHardcoreRefused() has had its say, so what the menu is handed is the mode the
    session will actually run in rather than the one ra.cfg asked for.

    `refusal` rides along so the in-game menu can say *why* rather than only *what*. A player who set
    hardcore=1 and is playing softcore has exactly one question, and the alternative answer to it is
    "take the card out and read a log on a PC".

    The magic goes in last, for the reason the other three blocks give: the staging region is
    uninitialised, conf_sd.cpp cleared this word on its way past, and the bootloader trusts it.
*/
static void raWifiStageSession(int hardcore, u8 refusal, int overlay, u8 queue, u8 level) {
	raSessionBlock* const block = (raSessionBlock*)CARDENGINEI_ARM9_RA_SESSION_BUFFERED_LOCATION;

	block->hardcore = hardcore ? 1 : 0;
	block->refusal  = refusal;
	block->overlay  = overlay ? 1 : 0;
	block->queue    = queue;
	block->magic = RA_SESSION_MAGIC;

	raWifiLog("in-game menu     RAM editing %s\n",
	          hardcore ? "\x1b[32mlocked -- hardcore\x1b[37m" : "unchanged -- softcore");
	/*
	    Says *why* it is off, because "off" alone sends a player to read source. Since the popup
	    became opt-in per game, not-listed is the ordinary case rather than an error -- see
	    RA_OVERLAY_LISTED.
	*/
	if (overlay) {
		raWifiLog("on-screen popup  on%s\n",
		          level >= RA_OVERLAY_ALWAYS ? " (overlay=2, every game)" : " (this game is listed)");
	} else if (level == RA_OVERLAY_OFF) {
		raWifiLog("on-screen popup  \x1b[33moff (overlay=0)\x1b[37m\n");
	} else {
		raWifiLog("on-screen popup  \x1b[33moff -- add this GameID to overlay_games to enable\x1b[37m\n");
	}
	/*
	    A ladder rather than a switch now -- see RA_SHARED_UNLOCK_LEVEL. Every step below `full` is a
	    diagnostic and says so on the line, because a player who left one set would otherwise lose
	    unlocks with nothing on screen or in the log to explain it.
	*/
	switch (queue) {
	case RA_QUEUE_LEVEL_OFF:
		raWifiLog("write to queue   \x1b[33moff (queue=0) -- unlocks will not survive\x1b[37m\n");
		break;
	case RA_QUEUE_LEVEL_NOSTAMP:
		raWifiLog("write to queue   on (queue=2 is retired and behaves as 1)\n");
		break;
	case RA_QUEUE_LEVEL_HANDOFF:
		raWifiLog("write to queue   \x1b[33mhandoff only (queue=3) -- unlocks will not survive\x1b[37m\n");
		break;
	default:
		raWifiLog("write to queue   on\n");
		break;
	}
}

void raWifiProbe(bool sdFound, const char* ndsPath, bool cheatsOn) {
	raWifiSdFound = sdFound;
	static raConfig config;
	int             stage;
	/*
	    Why hardcore was refused, if it was, carried from the gate down to the staging call so the
	    in-game menu can say it. Declared out here rather than beside the gate because the gate runs
	    inside `if (config.found)` and the staging call has to happen either way.
	*/
	u8              refusal = RA_REFUSED_NONE;

	raWifiClockStart();
	logFile = fopen(sdFound ? RA_WIFI_LOG_PATH : RA_WIFI_LOG_PATH_FAT, "w");

	/*
	    **Read before a single line is printed**, and that is the whole reason it moved up here from
	    stage 0c: verboseLog decides what the screen is for, and a run that starts verbose and turns
	    quiet three lines in would be worse than either mode. Nothing else about the ordering changes
	    -- the file is still parsed with no network up, so a bad config is still a line in the log
	    before the radio can be blamed for anything, and stage 0c below still reports every field.
	*/
	raConfigRead(sdFound ? RA_CFG_PATH : RA_CFG_PATH_FAT, &config);
	raVerbose = config.verboseLog;

	if (raVerbose) {
		iprintf("\x1b[33mnds-bootstrap RA WiFi, step 3\x1b[37m\n");
		iprintf("launcher context, no game running\n\n");
		if (logFile) {
			iprintf("logging to %s\n", sdFound ? RA_WIFI_LOG_PATH : RA_WIFI_LOG_PATH_FAT);
		} else {
			iprintf("\x1b[33mno log file; screen only\x1b[37m\n");
		}
	} else {
		raWifiScreenInit();
		/*
		    Logged rather than acted on, and kept after the fixed-layout attempt was abandoned: it is
		    the reading any future attempt should start from instead of an assumption.
		*/
		raWifiLog("console          %ux%u, map %s\n",
		          raWidth, RA_ROWS_VISIBLE, raHasMap ? "yes" : "no");
		iprintf("\x1b[33mRetroAchievements\x1b[37m\n");
	}

	raWifiVerdictReset(&verdict);

	raWifiLog("\n-- stage 0: the bus --\n");
	raWifiReportScfg();

	/*
	    Step 3b, and it runs here for two reasons. It needs no network, so putting it first
	    means a failure cannot be blamed on one. And it reads the ROM off the card while the
	    heap is still whole -- the launcher has roughly 352 K free at this point and 191 K once
	    lwip is up, and although ra_hash.c streams rather than allocating, libfat still wants
	    buffers and there is no reason to make it compete.

	    The hash is what 3c and 3d will ask the server about, so it is logged in full: it can
	    be checked against the game's page on retroachievements.org by eye, which is a cheaper
	    verification than any amount of code.
	*/
	raWifiStep(RA_STEP_GAME, "Reading the game");
	raWifiLog("\n-- stage 0b: the ROM's RetroAchievements hash --\n");
	{
		raHashInfo hashInfo;

		raWifiLog("ROM              %s\n", ndsPath ? ndsPath : "(none given)");
		if (ndsPath && raHashRom(ndsPath, romHash, &hashInfo)) {
			raWifiLog("\x1b[32mhash             %s\x1b[37m\n", romHash);
			/*
			    Recorded here rather than at the end: this is the one line of the whole run that is
			    useful for picking the *next* game, and a run that stops later must still leave it.
			*/
			raWifiRecordHash(sdFound, ndsPath);
		} else {
			raWifiLog("\x1b[31mhash failed: %s\x1b[37m\n", raHashLastError());
		}
		/*
		    Printed even on failure, because these three numbers are what say *why* the
		    streaming implementation exists: bufferBytes is what rcheevos' own function would
		    have had to allocate in one block.
		*/
		raWifiLog("arm9 / arm7      %lu / %lu bytes\n",
		          (unsigned long)hashInfo.arm9Size, (unsigned long)hashInfo.arm7Size);
		raWifiLog("would malloc     %lu bytes\n", (unsigned long)hashInfo.bufferBytes);
	}
	raWifiReportHeap("after hash");

	/*
	    Step 3c's half that needs no network. Read here, beside the hash, for the same reason:
	    a missing or malformed config file should be a line in the log before anything can be
	    blamed on the radio.

	    Reported in detail because every field is a way for a login to fail silently -- an empty
	    username, a `notYet` count that is really a typo, a `password=` line the user thought
	    they filled in. The secret itself is never printed: see raConfigRedact().
	*/
	raWifiStep(RA_STEP_SETTINGS, "Reading your settings");
	raWifiLog("\n-- stage 0c: the RetroAchievements config --\n");
	raWifiLog("ra.cfg           %s\n", config.found ? "found" : "absent");
	raWifiLog("verbose_log      %s\n", config.verboseLog ? "1" : "0");
	raWifiLog("overlay          %s\n", config.overlay ? "1" : "0");
	raWifiLog("queue            %s\n", config.queue ? "1" : "0");
	if (config.found) {
		raWifiLog("username         %s\n", config.username[0] ? config.username : "(empty)");
		raWifiLog("password         %s\n", raConfigRedact(config.password));
		raWifiLog("hardcore         %s\n", config.hardcore ? "1" : "0");
		/*
		    Decided here, before stage 10, so every request in the ladder is signed with the mode the
		    session will actually run in. Deciding it later would sign the login one way and the
		    awards another.
		*/
		refusal = raWifiHardcoreRefused(&config, cheatsOn);
		if (refusal) {
			config.hardcore = 0;
			raWifiLog("hardcore         falling back to softcore\n");
		}
		if (config.notYet) {
			raWifiLog("%u keys parsed but not acted on yet\n", config.notYet);
		}
		if (config.unknown) {
			raWifiLog("\x1b[33m%u unknown keys -- check for typos\x1b[37m\n", config.unknown);
		}
		if (config.badLines) {
			raWifiLog("\x1b[33m%u lines with no '='\x1b[37m\n", config.badLines);
		}
	} else {
		raWifiLog("put username= and password= in\n%s\n",
		          sdFound ? RA_CFG_PATH : RA_CFG_PATH_FAT);
		raWifiWarn("No ra.cfg: playing offline");
	}

	/*
	    And the menu is told, here, outside the `config.found` branch and above every `goto done`
	    below it. Outside the branch because a missing ra.cfg is a softcore session rather than an
	    unknown one, and the menu should be told that rather than left to infer it from silence.
	    Above the exits because the exits are the common case: see raWifiStageSession().
	*/
	/*
	    The popup's decision is resolved **here**, into the single bit the session block already
	    carries, so no game-side binary changes and the tight ones spend nothing. This side is the one
	    that knows the GameID and has room to walk a list. See RA_OVERLAY_LISTED.

	    verdict.gameId is zero when the ladder did not reach stage 11, or when the server did not
	    recognise the ROM -- and zero is on no list, so an unidentified game does not draw.
	*/
	raWifiStageSession(config.hardcore, refusal,
	                   raOverlayAllowed(config.overlay, config.overlayGames, verdict.gameId),
	                   config.queue, config.overlay);

	/*
	    `sync=0`: stop here, before a single register of the radio is touched.

	    This is the point to do it because of the order the stages happen to be in, which turns out to be
	    exactly right: the ROM's hash is stage 0b and the config is stage 0c, so both are already in hand
	    before the ARM7 is asked for anything. The cache is keyed by that hash, so `done:` can load this
	    ROM's set with nothing powered up -- no association attempt, no forty-second wait, no fifteen
	    seconds re-fetching a set that has not changed.

	    Everything the game does still happens: rcheevos evaluates the cached set, the notification draws,
	    and an unlock is queued to the card for whichever later boot has sync on.
	*/
	if (!config.sync) {
		raWifiLog("\n\x1b[33msync=0 in ra.cfg -- the radio stays off\x1b[37m\n");
		raWifiEssential("Wi-Fi off (sync=0)");
		goto done;
	}

	raWifiStep(RA_STEP_WIFI, "Connecting to Wi-Fi");
	raWifiLog("\n-- the ARM7 half --\n");

	if (!raWifiWaitArm7()) {
		raWifiLog("\x1b[31mthe ARM7 never reported installWifiFIFO().\x1b[37m\n"
		          "if only the ARM9 was rebuilt, rebuild both:\n"
		          "make RA_LAUNCHER_WIFI=1\n");
		goto done;
	}
	raWifiLog("installWifiFIFO() is in\n");

	/*
	    One call starts everything, and it is the same call tools/wifiprobe/ makes.
	    DSiWifi_InitDefault() installs dsiwifi's own handler on FIFO_DSWIFI, sends INIT_IOP,
	    and from there the ARM7 does the SDIO reset, BMI, the firmware launch, WMI and the
	    scan while the ARM9 half brings lwip up and starts DHCP. All of it narrates through
	    the log handler.
	*/
	raWifiLog("\n-- stage 1-5: bring the chip up and associate --\n");
	DSiWifi_SetLogHandler(raWifiCapture);
	DSiWifi_InitDefault(WFC_CONNECT);

	if (!raWifiWaitStage(RA_WIFI_STAGE_WMI, RA_WIFI_WAIT_CHIP, "WMI")) {
		goto done;
	}
	if (!raWifiWaitStage(RA_WIFI_STAGE_READY, RA_WIFI_WAIT_LINK, "link")) {
		goto done;
	}

	/*
	    And here step 3 begins: everything above this line was proven in step 2 without an IP
	    stack, so anything that fails from now on is lwip in the launcher -- which is the one
	    thing this round exists to find out.
	*/
	raWifiStep(RA_STEP_ADDRESS, "Getting a network address");
	raWifiLog("\n-- stage 6: DHCP --\n");
	if (!raWifiWaitIp(RA_WIFI_WAIT_DHCP)) {
		goto done;
	}

	raWifiReportHeap("with lwip up");

	raWifiStep(RA_STEP_SIGNIN, "Signing in to your account");
	raWifiLog("\n-- stage 10: log in --\n");
	raWifiLogin(&config);
	raWifiReportHeap("after login");

	raWifiStep(RA_STEP_LOOKUP, "Looking up this game");
	raWifiLog("\n-- stage 11: does the server know this ROM --\n");
	raWifiIdentify();
	raWifiReportHeap("after gameid");

	/*
	    And the last rung, which is the first one that leaves something behind: the set is
	    written to the staging block the bootloader copies into DSi WRAM. This build still does
	    not boot a game -- see RA_LAUNCHER_WIFI -- so what it proves is that the definitions
	    arrive and fit, which is the question. Running them is step 4.
	*/
	/*
	    Before r=unlocks on purpose, and the reason is the loop rather than tidiness: an achievement
	    reported here is one the next rung sees the account holding, so the scanner leaves it out of
	    the block and it does not trigger again next session. See RA_WIFI_STAGE_SUBMIT.
	*/
	/*
	    First of the game-specific rungs, because it is what the official client does first and because
	    the award below may depend on it existing. See RA_WIFI_STAGE_SESSION.
	*/
	raWifiStep(RA_STEP_SESSION, "Starting your session");
	raWifiLog("\n-- stage 12: start a play session --\n");
	raWifiStartSession(&config);
	raWifiReportHeap("after session");

	raWifiStep(RA_STEP_SENDING, "Sending what you earned");
	raWifiLog("\n-- stage 13: report what the last session earned --\n");
	raWifiSubmitQueue(&config, sdFound);
	raWifiStagePending(sdFound, ndsPath);
	raWifiReportHeap("after award");

	raWifiStep(RA_STEP_EARNED, "Checking what you have");
	raWifiLog("\n-- stage 14: what has this account already earned --\n");
	raWifiUnlocks(&config);
	raWifiReportHeap("after unlocks");

	raWifiStep(RA_STEP_DOWNLOAD, "Downloading achievements");
	raWifiLog("\n-- stage 15: fetch the set --\n");
	raWifiFetchPatch(&config);
	raWifiReportHeap("after patch");

done:
	/*
	    The set for this ROM, if the ladder did not produce one.

	    Hardware found the gap this closes: with the access point off the ladder stopped at stage 3, so
	    raWifiFetchPatch() -- where the cache was wired in -- was never reached at all, and the summary
	    read `definitions none`. Every early exit has the same shape, whether it is no link, no config,
	    a refused login or a hash the server does not know, and this is the one place all of them pass
	    through.

	    Safe under an absent cache: raWifiCacheLoad() leaves the block untouched and returns false, so
	    whatever loadRaDefinitions() staged before the ladder ran still stands. The cache is preferred
	    over that file because it is this ROM's own set rather than one hand-managed file that cannot
	    know which game is running.

	    With one exception, and it is the case that made the exception necessary: if the server was
	    asked about this ROM and answered that it does not know it, raWifiIdentify() has already
	    thrown the staged definitions away. "Still stands" above means "on every path that did not
	    positively contradict it" -- a hand file surviving into a game it was never written for is
	    how a real achievement id gets queued from a game nobody was playing.
	*/
	if (!verdict.patched && raWifiCacheLoad(sdFound)) {
		verdict.defsBytes = *(u32*)(CARDENGINEI_ARM9_RA_DEFS_BUFFERED_LOCATION + 4);
	}

	/*
	    Give the tail a chance before the summary rather than after it. dsiwifi narrates
	    asynchronously and keeps talking once the last rung is decided -- the probe's first
	    hardware run printed the line naming the access point and its security mode *after*
	    its own summary. Draining here means those lines land above the summary they inform,
	    and the summary is computed from a log that is actually finished.
	*/
	/*
	    Marked at both ends on purpose. One v6 run froze somewhere in here -- after the HTTP
	    heap line and before the summary -- and with the window unmarked there was no way to
	    tell the tail drain from the summary's own writes. A repeat now says which.
	*/
	raWifiLog("\n-- draining the tail --\n");
	{
		/*
		    **Until it goes quiet, not for a fixed eight seconds.**

		    This loop existed because dsiwifi narrates asynchronously and keeps talking after the last
		    rung is decided -- the probe's first hardware run printed the line naming the access point
		    *after* its own summary. What it did not need was to spend the full eight seconds every
		    time, and it did: there was no early exit, so eight seconds came off every boot this fork
		    has ever done, whether there was anything left to say or not.

		    Half a second of silence is the end of it. The ceiling stays exactly where it was, so a
		    chip that keeps talking is still bounded by the same number as before.
		*/
		int       frames = RA_WIFI_WAIT_TAIL * 60;
		int       quiet  = 0;
		u32       last   = textHead;

		while (frames-- > 0 && quiet < 30) {
			raWifiIdle();
			if (textHead != last) {
				last  = textHead;
				quiet = 0;
			} else {
				quiet++;
			}
		}
	}
	raWifiSync();
	raWifiVerdictFlush(&verdict);
	stage = raWifiVerdictStage(&verdict);

	raWifiLog("\n-- summary --\n");
	raWifiLog("chip             %s\n", verdict.chip[0] ? verdict.chip : "unidentified");
	raWifiLog("arrived          %s\n", raWifiVerdictArrival(&verdict));
	raWifiLog("BMI / launch     %s / %s\n",
	          verdict.bmiSeen ? "yes" : "no",
	          verdict.firmwareLaunched ? "yes" : "no");
	raWifiLog("firmware / WMI   %s / %s\n",
	          verdict.firmwareReady ? "ready" : "no",
	          verdict.wmiReady ? "ready" : "no");
	raWifiLog("associated       %s\n", verdict.associated ? "yes" : "no");
	raWifiLog("link ready       %s\n", verdict.linkReady ? "yes" : "no");
	if (verdict.gotIp) {
		raWifiReportIp("IP", verdict.ip);
	} else {
		raWifiLog("IP               none\n");
	}
	raWifiLog("DNS / TCP / API  %s / %s / %s\n",
	          verdict.dnsOk ? "ok" : "no",
	          verdict.tcpOk ? "ok" : "no",
	          verdict.apiOk ? "ok" : "no");
	raWifiLog("logged in        %s\n", verdict.loggedIn ? "yes" : "no");
	if (verdict.identified) {
		raWifiLog("GameID           %lu\n", (unsigned long)verdict.gameId);
	} else {
		raWifiLog("GameID           %s\n", verdict.gameId == 0 ? "unknown to the server" : "not asked");
	}
	/*
	    Reported unconditionally, including as three zeros. `submitted` at 0/0/0 says the queue was
	    read and was empty, which is the normal boot; the line missing would leave no way to tell that
	    from a rung that never ran.
	*/
	/*
	    Both sources for the same fact, side by side, because they disagreed once and that is the whole
	    reason the session rung exists. `session unlocks` comes from r=startsession's objects, `already
	    earned` from r=unlocks' flat array.
	*/
	raWifiLog("session          %s, %u soft / %u hard\n",
	          verdict.sessionOk ? "started" : "none",
	          verdict.sessionUnlocks, verdict.sessionHardcore);
	raWifiLog("submitted        %u ok, %u refused, %u owed\n",
	          verdict.submitAccepted, verdict.submitRefused, verdict.submitKept);
	if (verdict.unlocksKnown) {
		raWifiLog("already earned   %u\n", verdict.unlockCount);
	} else {
		raWifiLog("already earned   unknown\n");
	}
	if (verdict.patched) {
		raWifiLog("definitions      %u in %lu bytes\n",
		          verdict.defsKept, (unsigned long)verdict.defsBytes);
	} else if (verdict.defsBytes) {
		/*
		    From the cache rather than from the server, and said differently on purpose: the set is
		    real and the game will evaluate it, but it is as old as the last successful fetch and the
		    already-earned filtering in it is that old too. "none" would be a lie and "N in M bytes"
		    would hide which of the two happened.
		*/
		raWifiLog("definitions      %lu bytes, cached\n", (unsigned long)verdict.defsBytes);
	} else {
		raWifiLog("definitions      none\n");
	}
	if (verdict.mboxAllocFailed) {
		raWifiLog("\x1b[31mthe ARM7 could not allocate its mboxes\x1b[37m\n");
	}
	raWifiLog("log lines        %u\n", verdict.lines);
	if (textDropped) {
		raWifiLog("\x1b[31m%lu chars dropped: the log has a hole\x1b[37m\n",
		          (unsigned long)textDropped);
	}

	/*
	    And the quiet screen's own ending: the bar finished, and the four or five facts a person
	    actually wanted from the wait. Written from `verdict`, the same structure the summary above is
	    written from, so the two cannot disagree about what happened.

	    Ordered by what a failing boot needs first. Signing in is the step that fails when a password
	    is wrong, and "Could not sign in" at the top of the block is the whole diagnosis for the most
	    common problem this loader will ever have.
	*/
	if (!raVerbose) {
		raWifiStep(RA_STEP_MAX, (stage >= RA_WIFI_STAGE_PATCHED) ? "Ready" : "Finished with problems");

		if (config.sync) {
			if (verdict.loggedIn) {
				raWifiEssential("Signed in as %s", raUser[0] ? raUser : config.username);
			} else if (!config.usable) {
				raWifiWarn("No username or password set");
			} else if (verdict.apiOk) {
				raWifiError("Could not sign in");
			} else if (verdict.gotIp) {
				raWifiError("Could not reach the server");
			} else if (verdict.linkReady) {
				raWifiError("No network address");
			} else {
				raWifiError("No Wi-Fi connection");
			}
		}

		if (verdict.identified) {
			raWifiEssential("Game found on the server");
		} else if (verdict.loggedIn) {
			raWifiWarn("Game not in the database");
		}

		/*
		    Said differently for the cache, because the difference matters to a player rather than
		    only to a log: a cached set is real and will fire, and its "already earned" filtering is
		    as old as the last successful download.
		*/
		if (verdict.patched) {
			raWifiEssential("%u achievements ready", verdict.defsKept);
		} else if (verdict.defsBytes) {
			raWifiEssential("Achievements from last time");
		} else {
			raWifiWarn("No achievements loaded");
		}

		if (verdict.submitAccepted || verdict.submitRefused || verdict.submitKept) {
			raWifiEssential("%u sent, %u still waiting",
			                verdict.submitAccepted, verdict.submitKept);
		}
		if (verdict.unlocksKnown) {
			raWifiEssential("%u earned before now", verdict.unlockCount);
		}

		/*
		    Leave the cursor below the block. Whatever the launcher prints next is not ours to place,
		    and it must not land on top of the only lines this screen exists to show.
		*/
	}

	raWifiLog("\n\x1b[33mreached stage %d of %d\x1b[37m\n", stage, RA_WIFI_STAGE_MAX);
	if (stage >= RA_WIFI_STAGE_PATCHED) {
		raWifiLog("the set is staged for the cardengine.\n");
	} else if (stage >= RA_WIFI_STAGE_SUBMIT) {
		raWifiLog("the queue is reported; the set is what is missing.\n");
	} else if (stage >= RA_WIFI_STAGE_SESSION) {
		raWifiLog("a play session is open on the server.\n");
	} else if (stage >= RA_WIFI_STAGE_IDENTIFIED) {
		raWifiLog("logged in, and the server knows the ROM.\n");
	} else if (stage >= RA_WIFI_STAGE_LOGGED_IN) {
		raWifiLog("logged in from the launcher.\n");
	} else if (stage >= RA_WIFI_STAGE_ANSWERED) {
		raWifiLog("the launcher can reach RetroAchievements.\n");
	} else if (stage >= RA_WIFI_STAGE_READY) {
		raWifiLog("the link is up; the IP stack is where it stopped.\n");
	} else {
		raWifiLog("stopped here -- see the log above.\n");
	}

	/*
	    Said explicitly because the first run could not say it, and the run was wasted for
	    exactly that reason: the log is fsync()'d, so it is complete on the card right now,
	    with the file still open. Powering off here does not lose it.
	*/
	raWifiSync();

	/*
	    The definitions go out here, deliberately after the summary is already on the card. See
	    raWifiDumpDefinitions(): this is the largest SD write of the run and the link is still up.
	*/
	if (verdict.submitKept) {
		raWifiLog("still owed       %u, next boot\n", verdict.submitKept);
	}
	if (verdict.unlocksKnown) {
		raWifiLog("already earned   %u\n", verdict.unlockCount);
	} else {
		raWifiLog("already earned   unknown\n");
	}
	if (verdict.patched) {
		raWifiDumpDefinitions(sdFound);
		/*
		    And keep it for this ROM, so a boot that skips the ladder still has a set. Written after
		    the dump rather than instead of it: the dump is the artifact a human reads against the
		    set's page, the cache is what the next boot loads.
		*/
		raWifiCacheWrite(sdFound);
		raWifiSync();
	}

#if RA_WIFI_BOOTS_GAME
	/*
	    Mode 2: give the radio back and return, and the launcher goes on to boot the game with the
	    set already staged. Nothing above this line is different from the diagnostic -- same ladder,
	    same log, same summary -- which is deliberate: the reading and the thing that plays are the
	    same code, so a difference between them cannot be ours.
	*/
	if (!raWifiShutdown()) {
		/*
		    The one refusal. An ARM7 that never confirmed the teardown may still be taking SDIO
		    interrupts, and the bootloader is about to overwrite the code those interrupts vector
		    into -- so this stops on the summary exactly as the diagnostic does, rather than handing
		    a live radio to a CPU that is about to forget how to answer it.

		    A halt is a bad outcome and it is the *better* bad outcome: the alternative is a crash
		    somewhere in the game, minutes later, with nothing on the card explaining it.
		*/
		raWifiSync();
		raWifiLog("\nstopped rather than boot with a live radio.\n");
		while (1) {
			raWifiIdle();
		}
	}

	raWifiSync();
	raWifiLog("\nradio down -- booting the game.\n");
	/*
	    Closed here, unlike the diagnostic's log, and for the opposite reason. There it is left open
	    because a hang must not lose the late lines; here the launcher is about to hand both CPUs to
	    the bootloader, so there are no late lines and close() is what writes the directory entry.
	*/
	if (logFile) {
		fclose(logFile);
		logFile = 0;
	}
	return;
#else
	raWifiLog("\nlog written and synced -- safe to power off.\n"
	          "this build does not boot games.\n");

	/*
	    And it stops here. The ARM7 may be sitting in one of dsiwifi's untimed `while`
	    loops -- there are two in wifi_card_wlan_init(), waiting for the firmware-ready flag
	    and for WMI -- with a timer IRQ and an AUX IRQ live besides. Handing that to the
	    bootloader, which is about to overwrite the ARM7's code, is not something to do for
	    a measurement. See RA_LAUNCHER_WIFI in ra_wifi.h.

	    Kept draining rather than spinning idle, so a line dsiwifi emits a minute from now
	    still reaches the card. The file is deliberately never closed -- with fsync() per
	    write there is nothing a close would add, and closing would silently drop exactly
	    those late lines.
	*/
	while (1) {
		raWifiIdle();
	}
#endif
}

#endif /* RA_LAUNCHER_WIFI */
