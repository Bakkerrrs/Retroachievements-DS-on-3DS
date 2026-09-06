/*
    Host-side test for the RA watchlist and its pointer-chain walker.

    Everything else in this fork has to be checked by flashing a card and reading hex out
    of the in-game menu's RAM viewer, which is slow enough that three separate bugs in the
    overlay each cost a flash cycle to find. The walker is pure logic over addresses, so it
    does not need the hardware -- and getting it wrong is a Data Abort inside the game's
    interrupt handler, i.e. a crash rather than a glitch. So it is tested here first.

    It compiles cardenginei_arm9_ra's source verbatim -- statics and all, nothing stubbed
    or conditionally compiled. The trick that makes that a real test rather than a mock is
    the link address: the runner links this at 0x02100000 and maps a page at 0x04000000, so
    the snapshot it hands over really does sit inside the main RAM range the walker
    validates against, and the I/O reads really do land on mapped memory. The chain
    self-tests resolve for the same reason they do on hardware, not because a check was
    relaxed.

    It followed the code when the watchlist moved out of the cardengine and into DSi WRAM;
    that is the point of pointing it at a source file rather than at an interface.

    Run it with tools/ra_reader_test.sh.

    This file is part of nds-bootstrap and is licensed under the GPL-3.0,
    the same terms as the rest of the project.
*/

#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>

/*
    __bss_start, __bss_end and __vram_top are NOT defined here. The runner supplies them with
    --defsym, as absolute addresses inside the real DSi WRAM window this test mmaps below, and
    that is a fix rather than a flourish.

    They used to be 1-byte dummies in this file, and the cardengine takes their *addresses*:
    `ra_startup(__bss_start, __bss_end, ...)` zeroes the range between the first two and puts
    its arena at the second. Which meant the arena was whatever the linker's ordering of three
    dummy symbols happened to make it -- and the linker is under no obligation to keep them in
    declaration order. It bit twice. Adding rcheevos' hash.c to the link put __vram_top *below*
    __bss_end, making the span -1 and the arena the whole process; and raising
    RA_DEFS_MAX_LINES from 8 to 128 added 512 bytes of statics to this translation unit and did
    it again. Both times the suite passed at -O0 and segfaulted at -O1, in tests unrelated to
    the change.

    With --defsym the addresses are chosen rather than inherited, and they mirror hardware: the
    window at CARDENGINEI_ARM9_RA_LOCATION, a 16K .bss at the bottom, the definitions block at
    the top, and the arena between them. Nothing added to this file can move it again.

    The scratch window below is the same lesson at smaller scale. The tests that call
    ra_startup() directly hand it a .bss range and an arena top, and the arena has to be real
    memory *contiguous with* the .bss -- which two separate arrays, `fakeBss` and `fakeArena`,
    were only ever by the linker's good manners. One buffer with offsets into it is correct by
    construction.
*/
#define FAKE_BSS_BYTES   4096
#define FAKE_ARENA_BYTES 0x3B000

static char fakeWindow[FAKE_BSS_BYTES + FAKE_ARENA_BYTES];

#define FAKE_BSS      (fakeWindow)
#define FAKE_BSS_END  (fakeWindow + FAKE_BSS_BYTES)
#define FAKE_TOP      (fakeWindow + sizeof(fakeWindow))

/*
    RA_ALLOC_NO_LIBC_NAMES keeps our malloc from replacing glibc's underneath printf and the
    test harness itself -- see the note above ra_alloc_malloc(). The allocator is exercised
    through its ra_ names instead, which is the same code.
*/
#define RA_ALLOC_NO_LIBC_NAMES
#include "../retail/cardenginei/arm9_ra/source/ra_alloc.c"
#include "../retail/cardenginei/arm9_ra/source/startup.c"
#include "../retail/cardenginei/arm9_ra/source/cardengine.c"
/*
    Included rather than linked, for the same reason as the two above: the translation and
    the peek path are static, and they are exactly the parts worth testing. rcheevos itself
    is a normal library and is linked -- see tools/ra_reader_test.sh for the file list.
*/
#include "../retail/cardenginei/arm9_ra/source/ra_rcheevos.c"

/*
    The launcher's side, declarations only. Both implementations are separate translation
    units linked by the runner -- see the note above the step-two section for why they must
    not be #included here.
*/
#include "ra_wifi.h"

#define MIRROR_LOW   0x02000000u          /* console 0x000000 */
#define MIRROR_HIGH  0x02800000u          /* the same page, two 4M mirrors up */
#define APART        0x02C00000u          /* masks to the same console address, own memory */

#define IO_BASE  0x04000000
#define IO_SIZE  0x2000
#define DISPCNT  ((vu16*)RA_DEFAULT_WATCH_ADDRESS)

/*
    Stands in for the cardengine's raSnapshotBuffer. Aligned and, thanks to the link
    address, inside main RAM as far as the walker is concerned.
*/
static raSnapshot snapshot __attribute__((aligned(16)));

static int failures;

#define CHECK(cond) do { \
	if (cond) { \
		printf("  ok    %s\n", #cond); \
	} else { \
		printf("  FAIL  %s\n", #cond); \
		failures++; \
	} \
} while (0)

static void expect_status(const char* what, int index, u8 want) {
	const u8 got = snapshot.results[index].status;
	if (got == want) {
		printf("  ok    %s: status %u\n", what, want);
	} else {
		printf("  FAIL  %s: status %u, wanted %u\n", what, got, want);
		failures++;
	}
}

/*
    True when a DS address has no console address in the RetroAchievements DS map. Stated
    here rather than in the reader because nothing in the shipped code needs to ask any
    more -- the self-test no longer points at our own memory -- but the property is what
    the anchor choice rests on, so it is worth pinning.
*/
static int ra_rc_console_unmapped(u32 dsAddress) {
	return !(dsAddress >= RA_DS_SYSTEM_RAM_BASE
	      && dsAddress <  RA_DS_SYSTEM_RAM_BASE + RA_DS_SYSTEM_RAM_SIZE);
}

/* Words inside main RAM the test can point chains at. */
static u32 target = 0xDEADBEEF;
static u32 targetPtr;
static u32 targetPtrPtr;

/*
    ------------------------------------------------------------------------------------
    Step two's log classifier.

    Different subject from everything above -- the launcher rather than the cardengine --
    and here rather than in a second runner for a plain reason: this file is the thing
    docs/retroachievements.md tells you to run before anything else, and a check nobody
    runs is not a check.

    It earns its place the same way the example-file test does. raWifiVerdict reads how the
    Atheros chip arrived out of dsiwifi's printf text, because the driver exposes none of
    those facts any other way, and its failure mode is silent: a renamed string does not
    break a build, it reports the wrong world after a play session. So the strings are
    pinned here against the log a real console produced, and the runner greps the submodule
    for the format strings besides.

    Unlike the cardengine sources above, the launcher's two files are *linked* rather than
    #included -- see the runner. That is not a style preference. `__bss_end` and `__vram_top`
    are 1-byte dummies in this translation unit whose *addresses* define the arena
    `ra_alloc.c` hands out, so any static added to this TU moves what lies between them and
    the allocator starts writing over live variables. Including ra_hash.c here did exactly
    that: the suite passed at -O0 and segfaulted at -O1, several tests before the new code
    was even reached. Anything with file-scope state stays in its own TU.
    ------------------------------------------------------------------------------------
*/


/*
    The log a 3DS produced, read off disk rather than transcribed.

    docs/logs/ra_wifi_launcher-3ds.log is the real artifact from the run that reached stage 5
    of 5 -- dsiwifi's narration verbatim, plus the summary the console itself printed at the
    bottom. Reading the file is the point: a fixture typed into this file could drift from the
    evidence it claims to be, and the same argument already applies to
    tools/ra_achievements.example.txt further down.

    It also means the test can check something a synthetic fixture cannot: that this
    classifier, run on the host, reproduces the verdict the console printed from the same
    bytes. The summary block in the file says `arrived cold` and `chip AR6014`, and the
    assertions below derive exactly that from the narration above it.
*/
#define WIFI_LOG_PATH      "docs/logs/ra_wifi_launcher-3ds.log"
#define WIFI_LOG_HTTP_PATH "docs/logs/ra_wifi_launcher_http-3ds.log"

static char  wifiLogText[8192];
static size_t wifiLogLength;

static int wifi_load_log(const char* path) {
	FILE* f = fopen(path, "rb");

	if (!f) {
		printf("  cannot open %s -- run from the repository root\n", path);
		failures++;
		return 0;
	}
	wifiLogLength = fread(wifiLogText, 1, sizeof(wifiLogText) - 1, f);
	fclose(f);
	wifiLogText[wifiLogLength] = 0;
	return wifiLogLength > 0;
}

/*
    Feed the log the way the hardware delivers it: cut into 59-character pieces, which is
    what wifi_ipcSendStringAlt() puts in a single FIFO message.

    This is the case the reassembly in raWifiVerdictChunk() exists for, and the one a
    per-chunk matcher would fail: "Firmware 609c0202 ready, handshaking..." is 38 characters
    and the strings being matched are up to 21, so a cut in the wrong place hides them
    completely. A matcher without reassembly passes every other test in this file and then
    reports that the chip never came up.
*/
static void wifi_feed_chunked(raWifiVerdict* v, int chunk) {
	size_t i;

	raWifiVerdictReset(v);
	for (i = 0; i < wifiLogLength; i += chunk) {
		char   piece[128];
		size_t n = wifiLogLength - i;

		if (n > (size_t)chunk) {
			n = (size_t)chunk;
		}
		memcpy(piece, wifiLogText + i, n);
		piece[n] = 0;
		raWifiVerdictChunk(v, piece);
	}
	raWifiVerdictFlush(v);
}

static void wifi_feed_lines(raWifiVerdict* v, const char* const* lines) {
	int i;

	raWifiVerdictReset(v);
	for (i = 0; lines[i]; i++) {
		raWifiVerdictLine(v, lines[i]);
	}
}

/*
    Which nibbles of a 4bpp tile word carry a given colour index, as a bitmask of pixel columns.

    A mask rather than a position, so a test can say "the ink spans columns 1 to 6" without caring
    how the font is bearing-shifted. See span().

    It takes the colour rather than testing for non-zero, and that is not generality for its own
    sake: the glyphs are drawn in two colours now, ink over a drop shadow, so "something is set
    here" can no longer tell an ink pixel from the shadow of the letter to its left -- and the whole
    question the shadow raises, whether the ink still wins where the two overlap, is invisible to a
    mask that ignores the value.
*/
static u32 nibbleSpanOf(u32 word, u32 colour) {
	u32 mask = 0;
	int n;

	for (n = 0; n < 8; n++) {
		if (((word >> (n * 4)) & 0xF) == colour) {
			mask |= 1u << n;
		}
	}
	return mask;
}

/* The mask of pixel columns from `from` to `to` inclusive. */
static u32 span(int from, int to) {
	u32 mask = 0;
	int n;

	for (n = from; n <= to; n++) {
		mask |= 1u << n;
	}
	return mask;
}

static void test_wifi_verdict(void) {
	raWifiVerdict v;

	if (!wifi_load_log(WIFI_LOG_PATH)) {
		return;
	}

	/*
	    The 3DS log, classified on the host, must come out the way the console said it did.

	    It stops at 5 and not higher because the log is a *step 2* run: there was no IP stack
	    in that build, so rungs 6 to 9 have nothing to be read from. Which makes this fixture
	    do double duty -- it is also the check that adding those rungs did not quietly move
	    the ones below them.
	*/
	printf("\nthe 3DS log classifies as the console reported it\n");
	wifi_feed_chunked(&v, 59);
	CHECK(v.chipSeen == 1);
	CHECK(strcmp(v.chip, "AR6014") == 0);
	CHECK(v.coldStart == 1);
	CHECK(v.bmiSeen == 1);
	CHECK(v.firmwareLaunched == 1);
	CHECK(v.firmwareReady == 1);
	CHECK(v.wmiReady == 1);
	CHECK(v.mboxAllocFailed == 0);
	CHECK(strcmp(raWifiVerdictArrival(&v), "cold") == 0);
	/*
	    Association and the end of the handshake now come out of the text, because step 3
	    hands FIFO_DSWIFI to dsiwifi's own ARM9 half and those IPC messages stop reaching us.
	    This log was captured before that change and still carries both lines -- so it proves
	    the text-based reading agrees with what the IPC-based one reported at the time.
	*/
	CHECK(v.associated == 1);
	CHECK(v.linkReady == 1);
	CHECK(raWifiVerdictStage(&v) == RA_WIFI_STAGE_READY);
	CHECK(v.gotIp == 0);   /* a step-2 build: there was no lwip to get one */

	/* And the summary the console printed is in the file, saying the same two things. */
	CHECK(strstr(wifiLogText, "arrived          cold") != NULL);
	CHECK(strstr(wifiLogText, "chip             AR6014") != NULL);
	CHECK(strstr(wifiLogText, "reached stage 5 of 5") != NULL);

	/*
	    Several chunk sizes, because an off-by-one in the reassembly would survive one of
	    them. 59 is what dsiwifi actually sends; the small ones cut every matched string at
	    least once, and 1 is the degenerate case where no chunk can ever contain a match.
	*/
	printf("\nthe same log split into FIFO chunks reads identically\n");
	{
		raWifiVerdict whole, split;
		const int     sizes[] = { 59, 23, 7, 1 };
		unsigned      i;

		raWifiVerdictReset(&whole);
		raWifiVerdictChunk(&whole, wifiLogText);
		raWifiVerdictFlush(&whole);

		for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
			wifi_feed_chunked(&split, sizes[i]);
			printf("  chunk %d\n", sizes[i]);
			CHECK(split.chipSeen         == whole.chipSeen);
			CHECK(split.coldStart        == whole.coldStart);
			CHECK(split.bmiSeen          == whole.bmiSeen);
			CHECK(split.firmwareLaunched == whole.firmwareLaunched);
			CHECK(split.firmwareReady    == whole.firmwareReady);
			CHECK(split.wmiReady         == whole.wmiReady);
			CHECK(split.mboxAllocFailed  == whole.mboxAllocFailed);
			CHECK(split.lines            == whole.lines);
			CHECK(strcmp(split.chip, whole.chip) == 0);
			CHECK(raWifiVerdictStage(&split) == raWifiVerdictStage(&whole));
		}
	}

	/*
	    A warm arrival is the same log without one line, and it has to come out differently
	    or the classifier is not distinguishing anything. Constructed rather than recorded --
	    this console has never produced it -- and it is the reading step two exists to look
	    for, since a warm chip under our boot path would be the one real difference from the
	    control.
	*/
	printf("\nwarm and cold are told apart by the one line that says so\n");
	{
		const char* const warm[] = {
			"Mfg 02010271 Cid 0d000001 (AR6014)",
			"Reset cause: 00000002",
			"BMI version: 2300006f",
			"Launching!",
			"Firmware 609c0202 ready, handshaking...",
			"AR6014 fully initialized!",
			NULL,
		};

		wifi_feed_lines(&v, warm);
		CHECK(v.coldStart == 0);
		CHECK(strcmp(raWifiVerdictArrival(&v), "warm") == 0);
		/*
		    And this is the line that lets the ladder reach WMI. Taken from
		    libs/dsiwifi/arm_iop/source/wifi_card.twl.c, where it is printed as
		    "%s fully initialized!" once wmi_is_ready() returns.
		*/
		CHECK(v.wmiReady == 1);
		CHECK(raWifiVerdictStage(&v) == RA_WIFI_STAGE_WMI);
	}

	/*
	    Silence has to be distinguishable from failure, which is the whole reason the ladder
	    starts at zero rather than at one. An ARM7 that never answered and an ARM7 whose chip
	    refused look identical from the ARM9 unless stage 0 means something.
	*/
	printf("\nsilence is stage 0, not a failed chip\n");
	raWifiVerdictReset(&v);
	raWifiVerdictFlush(&v);
	CHECK(raWifiVerdictStage(&v) == RA_WIFI_STAGE_START);
	CHECK(strcmp(raWifiVerdictArrival(&v), "never answered") == 0);
	CHECK(v.lines == 0);

	/*
	    The two rungs the ARM7 reports over IPC rather than in text, so the ladder's top is
	    not decided by string matching at all.
	*/
	/*
	    The four rungs above the link are lwip's, and lwip answers in return values rather
	    than in narration -- so the probe sets them and no string can. Checked in order,
	    because the ladder only means anything if each rung is the thing that proves it.
	*/
	printf("\nthe rungs above the link come from lwip and the API, not from the log\n");
	wifi_feed_chunked(&v, 59);
	CHECK(raWifiVerdictStage(&v) == RA_WIFI_STAGE_READY);
	v.gotIp = 1;
	CHECK(raWifiVerdictStage(&v) == RA_WIFI_STAGE_IP);
	v.dnsOk = 1;
	CHECK(raWifiVerdictStage(&v) == RA_WIFI_STAGE_RESOLVED);
	v.tcpOk = 1;
	CHECK(raWifiVerdictStage(&v) == RA_WIFI_STAGE_CONNECTED);
	v.apiOk = 1;
	CHECK(raWifiVerdictStage(&v) == RA_WIFI_STAGE_ANSWERED);
	/* Step 3c's rung: r=login returning a token. */
	v.loggedIn = 1;
	CHECK(raWifiVerdictStage(&v) == RA_WIFI_STAGE_LOGGED_IN);
	/* And r=gameid, which is unauthenticated and so does not depend on the rung below it. */
	v.identified = 1;
	CHECK(raWifiVerdictStage(&v) == RA_WIFI_STAGE_IDENTIFIED);
	/*
	    Step 3d's rung, and the top of the ladder: r=patch put real definitions in the staging
	    block. The last rung is pinned against RA_WIFI_STAGE_MAX so adding one without extending
	    the summary's wording fails here rather than reporting "stage 12 of 12" for a run that
	    stopped at 12 of 13.
	*/
	/*
	    Then the three game-specific rungs, set one at a time and checked in order, so a rung
	    inserted without thinking about where it belongs fails here rather than on a console.

	    The order is the loop: report what the last session earned, *then* ask what the account
	    holds, *then* fetch. Reversing the first two would leave a just-awarded achievement in the
	    block to trigger and re-queue forever -- see RA_WIFI_STAGE_SUBMIT.
	*/
	v.sessionOk = 1;
	CHECK(raWifiVerdictStage(&v) == RA_WIFI_STAGE_SESSION);
	v.submitDone = 1;
	CHECK(raWifiVerdictStage(&v) == RA_WIFI_STAGE_SUBMIT);
	v.unlocksKnown = 1;
	CHECK(raWifiVerdictStage(&v) == RA_WIFI_STAGE_UNLOCKS);
	v.patched = 1;
	CHECK(raWifiVerdictStage(&v) == RA_WIFI_STAGE_PATCHED);
	CHECK(RA_WIFI_STAGE_PATCHED == RA_WIFI_STAGE_MAX);
	/*
	    And the rungs are consecutive. Renumbering three constants by hand is exactly the edit that
	    leaves a gap, and a gap makes "reached stage N of 14" mean nothing.
	*/
	CHECK(RA_WIFI_STAGE_SESSION == RA_WIFI_STAGE_IDENTIFIED + 1);
	CHECK(RA_WIFI_STAGE_SUBMIT == RA_WIFI_STAGE_SESSION + 1);
	CHECK(RA_WIFI_STAGE_UNLOCKS == RA_WIFI_STAGE_SUBMIT + 1);
	CHECK(RA_WIFI_STAGE_PATCHED == RA_WIFI_STAGE_UNLOCKS + 1);
	/*
	    A queue pass that found nothing still reaches the rung. Most boots earn nothing, and if the
	    empty case did not count, the ladder would report a failure at 11 on almost every run.
	*/
	{
		raWifiVerdict empty;

		raWifiVerdictReset(&empty);
		empty.loggedIn   = 1;
		empty.identified = 1;
		empty.submitDone = 1;
		CHECK(empty.submitAccepted == 0 && empty.submitKept == 0);
		CHECK(raWifiVerdictStage(&empty) == RA_WIFI_STAGE_SUBMIT);
	}

	/*
	    The step-3 run, which reached stage 9. Its narration carries the same five rungs and
	    nothing more -- the four above them are lwip's, and lwip does not narrate -- so this
	    is where the split between text and return values gets pinned against a real run
	    rather than against an argument about the design.

	    It also re-checks the bring-up: the same chip, the same cold arrival, from a build
	    that had linked an entire IP stack into the same ARM9 since the last log was taken.
	*/
	printf("\nthe step-3 log: the same five rungs in the text, nine in the summary\n");
	if (wifi_load_log(WIFI_LOG_HTTP_PATH)) {
		wifi_feed_chunked(&v, 59);
		CHECK(strcmp(v.chip, "AR6014") == 0);
		CHECK(v.coldStart == 1);
		CHECK(v.wmiReady == 1);
		CHECK(v.associated == 1);
		CHECK(v.linkReady == 1);
		CHECK(v.mboxAllocFailed == 0);
		/* Five from the text, and the text cannot know about the rest. */
		CHECK(raWifiVerdictStage(&v) == RA_WIFI_STAGE_READY);
		CHECK(v.gotIp == 0);
		CHECK(v.dnsOk == 0);
		/* What the console itself concluded once lwip had answered. */
		CHECK(strstr(wifiLogText, "reached stage 9 of 9") != NULL);
		CHECK(strstr(wifiLogText, "DNS / TCP / API  ok / ok / ok") != NULL);
		CHECK(strstr(wifiLogText, "invalid_credentials") != NULL);
		/*
		    dsiwifi's wifi_host_tick() reads
		        if (host_bLwipInitted && addr == 0xFFFFFFFF || addr == 0x0)
		    which groups as (a && b) || c -- so before lwip is initialised at all, the
		    zeroed netif's addr == 0 fires it on its own and dhcp_start() complains. Pinned
		    here because the line looks alarming in an otherwise clean log and is not:
		    dhcp_start() validates and returns, and the condition goes false once there is
		    an address. If a submodule bump fixes the precedence, this fails and the note in
		    docs/retroachievements.md can go.
		*/
		CHECK(strstr(wifiLogText, "netif is not up, old style port?") != NULL);
	}

	/*
	    A chip name it cannot parse must leave the field empty rather than half-filled: the
	    summary line on screen is the only place this value is used, and one that says
	    nothing is better than one that says something wrong.
	*/
	printf("\nan unparseable chip line reports no chip, not a wrong one\n");
	{
		const char* const odd[] = {
			"Mfg 02010271 Cid 0d000001 AR6014",     /* no parentheses */
			"Mfg 02010271 Cid 0d000001 (",          /* no closing one */
			"Mfg ()",                               /* empty */
			NULL,
		};

		wifi_feed_lines(&v, odd);
		CHECK(v.chipSeen == 1);
		CHECK(v.chip[0] == 0);
		CHECK(strcmp(raWifiVerdictArrival(&v), "warm") == 0);
	}
}

/*
    raOverlaySurvey() -- which 16K blocks of sub BG VRAM a BG configuration is using.

    Tested here rather than on hardware because the bug it fixes is unreachable there with the game the
    project has. Contra 4 runs in BG mode 0, where every layer is text and the old text-only reading is
    correct by accident; seeing the mode-blindness fire would mean finding a game that puts an affine
    or bitmap background on the *sub* engine and then earning an achievement inside it. The registers
    are just numbers, so the whole table can be checked here for nothing.

    Each case names the blocks it expects and every other block is asserted clear, because both
    directions are failures with different consequences: a block wrongly left clear is one the overlay
    may take and corrupt, and a block wrongly marked is a notification denied for no reason.
*/
#define SURVEY_TEXT_BG(charBase, screenBase, size) \
	((u16)(((charBase) << 2) | ((screenBase) << 8) | ((u32)(size) << 14)))

static void expect_blocks(const char* what, u32 dispcnt, const u16* bgcnt, u32 wantMask) {
	bool used[8];
	u32 got = 0;
	int b;

	raOverlaySurvey(dispcnt, bgcnt, used);
	for (b = 0; b < 8; b++) {
		if (used[b]) {
			got |= 1u << b;
		}
	}
	if (got == wantMask) {
		printf("  ok    %s -> blocks 0x%02X\n", what, got);
	} else {
		printf("  FAIL  %s -> blocks 0x%02X, wanted 0x%02X\n", what, got, wantMask);
		failures++;
	}
}

static void test_overlay_survey(void) {
	u16 bg[4];
	int i;

	/*
	    A layer that references nothing but block 0 in every slot, so a case can set one layer and
	    leave the other three saying the same harmless thing. Size 0 text: tiles in block 0, a 2K map
	    at screen base 0, which is also block 0.
	*/
	for (i = 0; i < 4; i++) {
		bg[i] = SURVEY_TEXT_BG(0, 0, 0);
	}

	printf("\ntext backgrounds read exactly as they did before\n");
	{
		/* Mode 0: all four layers text, which is the case hardware has confirmed. */
		expect_blocks("mode 0, everything in block 0", 0, bg, 1u << 0);

		bg[1] = SURVEY_TEXT_BG(2, 8, 0);   /* tiles block 2, map at 8*2K = block 1 */
		expect_blocks("tiles and map in different blocks", 0, bg,
		              (1u << 0) | (1u << 1) | (1u << 2));

		/*
		    **Every layer counts, including the one about to be borrowed.**

		    This case used to pass skipLayer=1 and expect block 0 alone -- BG1's tiles in block 2 and
		    its map in block 1 going unreported because BG1 was the layer being taken. Hardware showed
		    what that costs: Ketsui borrowed BG1, the survey could not see the block BG1 was using,
		    the overlay chose it, and wrote glyphs into a live layer's character block. Taking the
		    layer's registers does not take the layer's VRAM, and the same bg[] must now answer with
		    all three blocks whoever is borrowing what.
		*/
		expect_blocks("the borrowed layer counts like any other", 0, bg,
		              (1u << 0) | (1u << 1) | (1u << 2));

		/*
		    64x64 tiles is 4096 entries of two bytes -- 8K, four 2K units. Placed at screen base 6 it
		    starts at 12K, inside block 0, and runs to 20K, which is inside block 1.
		*/
		bg[1] = SURVEY_TEXT_BG(0, 6, 3);
		expect_blocks("a 64x64 text map spans the block boundary", 0, bg,
		              (1u << 0) | (1u << 1));

		bg[1] = SURVEY_TEXT_BG(0, 0, 0);
	}

	printf("\nan affine map is one byte per entry, not two\n");
	{
		/*
		    Mode 2 makes BG2 and BG3 affine. Size 3 is 128x128 tiles = 16,384 one-byte entries = 16K,
		    a whole block. Read as text it would have been taken for 8K, and the second half of the
		    map -- eight kilobytes of it -- would have looked free.
		*/
		bg[3] = SURVEY_TEXT_BG(0, 8, 3);   /* screen base 8 = 16K = block 1 */
		expect_blocks("128x128 affine fills a whole block", 2, bg,
		              (1u << 0) | (1u << 1));

		/* The same register in mode 0, where BG3 is text: 8K, so only half as far. */
		expect_blocks("the same bits as text reach half as far", 0, bg,
		              (1u << 0) | (1u << 1));

		/*
		    Placed so the difference shows. Screen base 12 = 24K, mid-block-1. Affine 16K runs to
		    40K, into block 2. Text 8K stops at 32K, exactly on the boundary and never entering it.
		*/
		bg[3] = SURVEY_TEXT_BG(0, 12, 3);
		expect_blocks("affine 128x128 at 24K reaches block 2", 2, bg,
		              (1u << 0) | (1u << 1) | (1u << 2));
		expect_blocks("text 64x64 at 24K stops at the boundary", 0, bg,
		              (1u << 0) | (1u << 1));

		/* A small affine map is smaller than a text one, so the survey must not over-mark either. */
		bg[3] = SURVEY_TEXT_BG(0, 8, 0);   /* 16x16 tiles = 256 bytes */
		expect_blocks("16x16 affine is 256 bytes", 2, bg, (1u << 0) | (1u << 1));

		bg[3] = SURVEY_TEXT_BG(0, 0, 0);
	}

	printf("\nthe BG mode decides which layers are affine at all\n");
	{
		/*
		    The same 128x128 register on BG2, which is text in modes 0, 1 and 3 and affine in 2 and
		    4. Screen base 12 = 24K: text stops at 32K, affine runs to 40K and into block 2.
		*/
		bg[2] = SURVEY_TEXT_BG(0, 12, 3);
		expect_blocks("mode 1 leaves BG2 text", 1, bg, (1u << 0) | (1u << 1));
		expect_blocks("mode 3 leaves BG2 text", 3, bg, (1u << 0) | (1u << 1));
		expect_blocks("mode 2 makes BG2 affine", 2, bg,
		              (1u << 0) | (1u << 1) | (1u << 2));
		expect_blocks("mode 4 makes BG2 affine", 4, bg,
		              (1u << 0) | (1u << 1) | (1u << 2));

		/* BG1 is text in every mode, so the mode must change nothing about it. */
		bg[2] = SURVEY_TEXT_BG(0, 0, 0);
		bg[1] = SURVEY_TEXT_BG(0, 12, 3);
		for (i = 0; i <= 5; i++) {
			expect_blocks("BG1 is text whatever the mode", (u32)i, bg,
			              (1u << 0) | (1u << 1));
		}
		bg[1] = SURVEY_TEXT_BG(0, 0, 0);
	}

	printf("\nan extended-affine map is affine-shaped with two-byte entries\n");
	{
		/*
		    Mode 5 makes BG2 and BG3 extended. With bit 7 clear that is an affine background with
		    16-bit tile indices, so a 128x128 map is 32K -- two whole blocks, twice the affine one
		    and four times what the old reading allowed.
		*/
		bg[3] = SURVEY_TEXT_BG(0, 8, 3);
		expect_blocks("128x128 extended affine is 32K", 5, bg,
		              (1u << 0) | (1u << 1) | (1u << 2));
		expect_blocks("the same bits as plain affine are 16K", 2, bg,
		              (1u << 0) | (1u << 1));

		bg[3] = SURVEY_TEXT_BG(0, 0, 0);
	}

	printf("\na bitmap's base is in 16K units, and it has no tiles\n");
	{
		/*
		    Bit 7 set in an extended slot is a bitmap. The base is the screen-base field in 16K units
		    rather than 2K, which is the field the old survey got wrong by a factor of eight, and bit
		    2 is the colour depth rather than the low bit of a character base.

		    Screen base 2, 8-bit, 256x256 = 64K: blocks 2 through 5. Read the old way that base meant
		    4K -- block 0 -- so every one of those four blocks looked free.
		*/
		bg[3] = (u16)(SURVEY_TEXT_BG(0, 2, 1) | 0x0080);
		expect_blocks("8-bit 256x256 at base 2 covers four blocks", 5, bg,
		              (1u << 0) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5));

		/* 16-bit doubles it, and bit 2 is what says so. */
		bg[3] = (u16)(SURVEY_TEXT_BG(0, 2, 1) | 0x0080 | 0x0004);
		expect_blocks("16-bit 256x256 covers eight", 5, bg,
		              (1u << 0) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5)
		              | (1u << 6) | (1u << 7));

		/* The smallest: 128x128 8-bit is 16K, exactly one block, and no character block with it. */
		bg[3] = (u16)(SURVEY_TEXT_BG(0, 3, 0) | 0x0080);
		expect_blocks("8-bit 128x128 is one block and no tiles", 5, bg,
		              (1u << 0) | (1u << 3));

		/*
		    Bit 2 set with bit 7 set is depth, not a character base of 1 -- so block 1 must stay
		    clear here. That is the misreading in its purest form.
		*/
		bg[3] = (u16)(SURVEY_TEXT_BG(0, 3, 0) | 0x0080 | 0x0004);
		expect_blocks("bit 2 is depth, not a character base", 5, bg,
		              (1u << 0) | (1u << 3) | (1u << 4));

		/* But with bit 7 clear it *is* a character base, in the very same slot. */
		bg[3] = (u16)(SURVEY_TEXT_BG(1, 3, 0) & ~0x0080);
		expect_blocks("with bit 7 clear the same bit 2 is a character base", 5, bg,
		              (1u << 0) | (1u << 1));

		/*
		    Bit 7 in a *text* slot is the 256-colour flag and must not be read as a bitmap. Mode 0
		    makes BG3 text, so this is an ordinary 8-bit-tile background: tiles in block 0, a 2K map
		    at screen base 3.
		*/
		bg[3] = (u16)(SURVEY_TEXT_BG(0, 3, 0) | 0x0080);
		expect_blocks("bit 7 on a text layer is colour depth, not a bitmap", 0, bg,
		              1u << 0);

		bg[3] = SURVEY_TEXT_BG(0, 0, 0);
	}

	printf("\na base outside the 128K window matches nothing rather than folding\n");
	{
		/* Character base 15 is 240K, past the end of sub BG VRAM. */
		bg[3] = SURVEY_TEXT_BG(15, 0, 0);
		expect_blocks("a character base past the window marks nothing", 0, bg, 1u << 0);

		/* A bitmap at base 31 is 496K. Its 64K would land at blocks 31-34 if they existed. */
		bg[3] = (u16)(SURVEY_TEXT_BG(0, 31, 1) | 0x0080);
		expect_blocks("a bitmap base past the window marks nothing", 5, bg, 1u << 0);

		/* One that starts inside and runs past the end marks what it reaches and no more. */
		bg[3] = (u16)(SURVEY_TEXT_BG(0, 7, 1) | 0x0080 | 0x0004);  /* 112K, 128K of pixels */
		expect_blocks("a bitmap running off the end marks only what it reaches", 5, bg,
		              (1u << 0) | (1u << 7));

		bg[3] = SURVEY_TEXT_BG(0, 0, 0);
	}

	printf("\na mode this engine does not have denies rather than guesses\n");
	{
		/*
		    6 is the main engine's large bitmap -- libnds refuses it on the sub display outright --
		    and 7 is not a mode. Neither can be read, so every block is called used and the overlay
		    stays quiet. A missing notification is a missing feature; a corrupted game is a bug.
		*/
		expect_blocks("mode 6 marks everything used", 6, bg, 0xFFu);
		expect_blocks("mode 7 marks everything used", 7, bg, 0xFFu);

		/* And an unreadable mode stays unreadable however the layers are arranged. */
		expect_blocks("an unreadable mode is unreadable for every layer", 6, bg, 0xFFu);
	}

	printf("\nonly the low three bits of DISPCNT choose the mode\n");
	{
		/*
		    The rest of the register is the game's -- enable bits, display mode, extended palettes --
		    and the survey must not be perturbed by any of it. 0x00211E10 is the real value read off
		    Contra 4 during stage 1, whose low bits are mode 0.
		*/
		bg[3] = SURVEY_TEXT_BG(0, 12, 3);
		expect_blocks("Contra 4's own DISPCNT reads as mode 0", 0x00211E10, bg,
		              (1u << 0) | (1u << 1));
		expect_blocks("the same mode with every other bit set", 0xFFFFFFF8u | 2u, bg,
		              (1u << 0) | (1u << 1) | (1u << 2));
		bg[3] = SURVEY_TEXT_BG(0, 0, 0);
	}
}

int main(void) {
	u32 offsets[RA_CHAIN_MAX];
	int i;

	/*
	    Map the I/O page: the diagnostic watch reads the sub engine's DISPCNT, so that
	    has to be real memory here.
	*/
	if (mmap((void*)IO_BASE, IO_SIZE, PROT_READ | PROT_WRITE,
	         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED) {
		perror("mmap 0x04000000");
		return 2;
	}

	/*
	    A real main RAM mirror, set up before anything ticks because ra_rc_console_address()
	    reads through it. MIRROR_LOW and MIRROR_HIGH are one page of memory mapped twice, 8M
	    apart -- the same relationship 0x027FC000 has to 0x023FC000 on the DS. APART is a
	    separate page that masks to the same console address and must therefore be refused.

	    Both are far above where this binary links, so MAP_FIXED cannot land on it.
	*/
	{
		int fd = memfd_create("ra_mirror", 0);

		if (fd < 0 || ftruncate(fd, 0x1000) != 0
		 || mmap((void*)MIRROR_LOW,  0x1000, PROT_READ | PROT_WRITE,
		         MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED
		 || mmap((void*)MIRROR_HIGH, 0x1000, PROT_READ | PROT_WRITE,
		         MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED
		 || mmap((void*)APART, 0x1000, PROT_READ | PROT_WRITE,
		         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED) {
			perror("mmap main RAM mirror");
			return 2;
		}
	}

	/*
	    The definitions block the bootloader would have written at the top of the WRAM
	    window. Mapped so ra_definition() has something real to read; left with no magic, so
	    every test below runs against the built-in self-test definition unless it says
	    otherwise.

	    Mapped at the block's real size rather than a page, because the last test in this
	    file stages the shipped example file here and that file is mostly comments.
	*/
	/*
	    The whole DSi WRAM window, at the address it really has, rather than just the
	    definitions block at the top of it. The allocator's arena lives in here now -- see the
	    note about --defsym above -- so it has to be real memory, and mapping the true window
	    means heapSize and the arena arithmetic are the ones hardware computes.
	*/
	if (mmap((void*)CARDENGINEI_ARM9_RA_LOCATION, CARDENGINEI_ARM9_RA_SIZE,
	         PROT_READ | PROT_WRITE,
	         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED) {
		perror("mmap the WRAM window");
		return 2;
	}
	*(u32*)CARDENGINEI_ARM9_RA_DEFS_LOCATION = 0;

	printf("link addresses\n");
	CHECK(ra_in_main_ram((u32)&snapshot, sizeof(snapshot)));
	CHECK(ra_in_main_ram((u32)&target, 4));

	/*
	    docs/retroachievements.md tells you to read specific offsets in the in-game menu's
	    RAM viewer. Those offsets are the interface, so they are pinned here: reorder the
	    struct and this fails instead of the instructions going stale.
	*/
	printf("\nthe documented snapshot offsets hold\n");
	CHECK(sizeof(raResult) == 0x0C);
	CHECK(__builtin_offsetof(raSnapshot, ticks) == 0x04);
	CHECK(__builtin_offsetof(raSnapshot, deniedNoLayer) == 0x14);
	CHECK(__builtin_offsetof(raSnapshot, watchCount) == 0x18);
	/* Took one of the three bytes reserved at 0x25; the bytes beside rcLinesMax are spent. */
	CHECK(__builtin_offsetof(raSnapshot, rcMemrefLines) == 0x25);
	CHECK(__builtin_offsetof(raSnapshot, raMpuBits) == 0x26);
	CHECK(__builtin_offsetof(raSnapshot, rcMemrefMin) == 0x27);
	CHECK(__builtin_offsetof(raSnapshot, linesMax) == 0x1B);
	CHECK(__builtin_offsetof(raSnapshot, wramMagic) == 0x1C);
	CHECK(__builtin_offsetof(raSnapshot, wramTicks) == 0x20);
	CHECK(__builtin_offsetof(raSnapshot, wramState) == 0x24);
	CHECK(__builtin_offsetof(raSnapshot, selfCell) == 0x28);
	CHECK(__builtin_offsetof(raSnapshot, selfCellPtr) == 0x2C);
	CHECK(__builtin_offsetof(raSnapshot, results) == 0x30);
	CHECK(__builtin_offsetof(raSnapshot, heapSize) == 0x60);
	CHECK(__builtin_offsetof(raSnapshot, heapUsed) == 0x64);
	CHECK(__builtin_offsetof(raSnapshot, wramStage) == 0x68);
	/*
	    rcStackUsed went into reserved2, at the only aligned u16 slot in it, so the offsets below
	    are untouched. It is the field that says how deep rcheevos went on the stack this binary
	    now gives it -- see the note in ra.h about the IRQ stack it was borrowing before.
	*/
	CHECK(__builtin_offsetof(raSnapshot, rcParts) == 0x69);
	CHECK(__builtin_offsetof(raSnapshot, rcStackUsed) == 0x6A);
	CHECK(__builtin_offsetof(raSnapshot, rcStage) == 0x6C);
	CHECK(__builtin_offsetof(raSnapshot, rcTriggered) == 0x70);
	CHECK(__builtin_offsetof(raSnapshot, rcMeasured) == 0x74);
	CHECK(__builtin_offsetof(raSnapshot, rcTarget) == 0x78);
	CHECK(__builtin_offsetof(raSnapshot, rcPeeks) == 0x7C);
	CHECK(__builtin_offsetof(raSnapshot, rcPeeksRejected) == 0x80);
	CHECK(__builtin_offsetof(raSnapshot, rcLines) == 0x84);
	CHECK(__builtin_offsetof(raSnapshot, rcLinesMax) == 0x85);
	/* Took the byte that had been reserved at 0x87 since the struct was written. */
	CHECK(__builtin_offsetof(raSnapshot, rcRoomMax) == 0x87);
	CHECK(__builtin_offsetof(raSnapshot, heapBreak) == 0x88);
	CHECK(__builtin_offsetof(raSnapshot, heapTop) == 0x8C);
	CHECK(__builtin_offsetof(raSnapshot, mallocProbe) == 0x90);
	CHECK(__builtin_offsetof(raSnapshot, sbrkProbe) == 0x94);
	CHECK(__builtin_offsetof(raSnapshot, rcFromFile) == 0x98);
	CHECK(__builtin_offsetof(raSnapshot, rcActivated) == 0x99);
	CHECK(__builtin_offsetof(raSnapshot, rcDefLength) == 0x9A);
	CHECK(__builtin_offsetof(raSnapshot, rcBadLine) == 0x9C);
	/*
	    Step 4's two fields, and they went into reserved4[3] rather than onto the end for the
	    reason every field above did: the hardware checklist in docs/retroachievements.md reads
	    these by offset out of a hex viewer, so a shifted offset silently invalidates every
	    reading anyone has ever photographed. Three spare bytes at an odd address take exactly
	    one u8 and one aligned u16, and the struct stays 0xA0.
	*/
	CHECK(__builtin_offsetof(raSnapshot, rcFirstTriggered) == 0x9D);
	CHECK(__builtin_offsetof(raSnapshot, rcInitTotal) == 0x9E);
	CHECK(__builtin_offsetof(raSnapshot, rcFirstId) == 0xA0);
	CHECK(__builtin_offsetof(raSnapshot, rcDefsWithId) == 0xA4);
	CHECK(__builtin_offsetof(raSnapshot, rcDefsNoId) == 0xA6);
	/* Step 3b's four, also appended. */
	CHECK(__builtin_offsetof(raSnapshot, shared) == 0xA8);
	CHECK(__builtin_offsetof(raSnapshot, unlockSent) == 0xAC);
	CHECK(__builtin_offsetof(raSnapshot, unlockQueued) == 0xAE);
	CHECK(__builtin_offsetof(raSnapshot, unlockLost) == 0xAF);
	/* The hook re-arm's three counters, appended after step 3b's four. */
	CHECK(__builtin_offsetof(raSnapshot, rearmTable) == 0xB0);
	CHECK(__builtin_offsetof(raSnapshot, rearmIe) == 0xB1);
	CHECK(__builtin_offsetof(raSnapshot, rearmDispstat) == 0xB2);
	/*
	    0xB8, grown 0xA0 -> 0xA8 by step 5, 0xA8 -> 0xB0 by step 3b, 0xB0 -> 0xB4 by the hook re-arm and
	    0xB4 -> 0xB8 by the notification's text -- appended every time, so every offset above them keeps
	    the address the hardware checklist reads it at.
	*/
	/*
	    The overlay's packed state, and the size it has to stay inside. Four separate fields here took
	    the ARM9 cardengine's window to -56 bytes free, so this is one byte on purpose.
	*/
	CHECK(__builtin_offsetof(raSnapshot, overlayState) == 0xB3);
	/* Where the rendered strip is; see RA_TEXT_BYTES and ra_overlay_tick(). */
	CHECK(__builtin_offsetof(raSnapshot, overlayText) == 0xB4);
	/* The sub engine as the game had it; see the note beside these in ra.h. */
	CHECK(__builtin_offsetof(raSnapshot, overlayDispcnt) == 0xB8);
	CHECK(__builtin_offsetof(raSnapshot, overlayWindow) == 0xBC);
	/* Which path drew, and what it took; see the note beside these in ra.h. */
	CHECK(__builtin_offsetof(raSnapshot, overlaySpriteOam) == 0xC0);
	CHECK(__builtin_offsetof(raSnapshot, overlaySpriteSlot) == 0xC1);
	/*
	    0xD0 since overlayRedrawn was appended. Pinned rather than derived because the whole point of
	    appending is that **no existing offset moves** -- a field inserted rather than appended would
	    shift every address in the hardware checklist in docs/retroachievements.md and nothing else
	    would notice. It caught exactly that omission: the field went in and this line still said 0xCC.
	*/
	CHECK(sizeof(raSnapshot) == 0xE0);
	CHECK(__builtin_offsetof(raSnapshot, overlayRedrawn) == 0xCC);
	CHECK(__builtin_offsetof(raSnapshot, overlaySavedInk) == 0xCE);
	CHECK(__builtin_offsetof(raSnapshot, overlaySavedShadow) == 0xD0);
	CHECK(__builtin_offsetof(raSnapshot, overlayBgCnt) == 0xD2);
	/*
	    The nine bank registers, and the accumulated mask beside them. Pinned because the whole point
	    is reading them off a photograph at a known address: a shifted offset makes the reading a lie
	    rather than an error.
	*/
	CHECK(__builtin_offsetof(raSnapshot, vramCnt) == 0xD4);
	CHECK(__builtin_offsetof(raSnapshot, vramEverOn) == 0xDE);
	CHECK(sizeof(((raSnapshot*)0)->vramCnt) == 9);
	/* Nine banks needs nine bits, and the field has to hold them. */
	CHECK((1u << 8) <= 0xFFFFu);
		CHECK(__builtin_offsetof(raSnapshot, defsMagic) == 0xC4);
		CHECK(__builtin_offsetof(raSnapshot, defsLength) == 0xC8);

	/*
	    The shared block's real size, pinned because overrunning it is silent: slot 13 is
	    UNPATCHED_FUNCTION_LOCATION, a table the loader uses to put the game's own functions back. A
	    request slot past the end would corrupt it and present as the game misbehaving with nothing
	    pointing here.
	*/
	CHECK(CARDENGINE_SHARED_ADDRESS_SDK1 + CARDENGINE_SHARED_SLOTS * 4 == UNPATCHED_FUNCTION_LOCATION);
	CHECK(CARDENGINE_SHARED_ADDRESS_SDK5 + CARDENGINE_SHARED_SLOTS * 4 == UNPATCHED_FUNCTION_LOCATION_SDK5);
	CHECK(RA_SHARED_UNLOCK_REQ < CARDENGINE_SHARED_SLOTS);
	CHECK(RA_SHARED_UNLOCK_ID < CARDENGINE_SHARED_SLOTS);
	/* And past the eight the rest of the cardengine already uses. */
	CHECK(RA_SHARED_UNLOCK_REQ > 8 && RA_SHARED_UNLOCK_ID > 8);
	CHECK(RA_SHARED_UNLOCK_REQ != RA_SHARED_UNLOCK_ID);

	/*
	    And when a side is allowed to act on the request. This is the Ketsui freeze, reduced to the only
	    part of it that can be checked without a card: the append ran while a game card read was
	    suspended mid-command, and IME off does not protect hardware state. raCardReadPending() carries
	    the reasoning and the split -- correctness on the ARM7 off its own `readOngoing`, courtesy here
	    off the request word, because the ARM7 was four bytes short of testing it. Truth table:
	*/
	printf("\nthe unlock waits for the card, and only for the card\n");
	/* Nothing asked for: offer it now. */
	CHECK(raCardReadPending(0) == 0);
	/* All three ARM9 card-read magics, unserved. */
	CHECK(raCardReadPending(0x025FFB08u) == 1);
	CHECK(raCardReadPending(0x025FFB09u) == 1);
	CHECK(raCardReadPending(0x025FFB0Au) == 1);
	/* Either side of the range, and the other magics that live in the same word: not our business. */
	CHECK(raCardReadPending(0x025FFB07u) == 0);
	CHECK(raCardReadPending(0x025FFB0Bu) == 0);
	CHECK(raCardReadPending(0x52534554u) == 0);   /* 'TESR' -- reset */
	CHECK(raCardReadPending(0x444D4152u) == 0);   /* 'RAMD' -- the RAM dump */
	/* The bound the caller relies on: slot 3 is inside the block, and is not one of the two this
	   feature owns. */
	CHECK(3 < CARDENGINE_SHARED_SLOTS);
	CHECK(3 != RA_SHARED_UNLOCK_REQ && 3 != RA_SHARED_UNLOCK_ID);

	/*
	    The level slot and its ladder -- see RA_SHARED_UNLOCK_LEVEL. Pinned because the ARM7 tests for
	    two exact values and does everything otherwise, so a renumbering that collided with FULL would
	    silently turn a diagnostic into shipping behaviour.
	*/
	CHECK(RA_SHARED_UNLOCK_LEVEL < CARDENGINE_SHARED_SLOTS);
	CHECK(RA_SHARED_UNLOCK_LEVEL > 8);
	CHECK(RA_SHARED_UNLOCK_LEVEL != RA_SHARED_UNLOCK_REQ);
	CHECK(RA_SHARED_UNLOCK_LEVEL != RA_SHARED_UNLOCK_ID);
	/* Four distinct values, and FULL is the one an unrecognised level falls back to. */
	CHECK(RA_QUEUE_LEVEL_OFF == 0);
	CHECK(RA_QUEUE_LEVEL_FULL != RA_QUEUE_LEVEL_NOSTAMP);
	CHECK(RA_QUEUE_LEVEL_FULL != RA_QUEUE_LEVEL_HANDOFF);
	CHECK(RA_QUEUE_LEVEL_NOSTAMP != RA_QUEUE_LEVEL_HANDOFF);
	CHECK(RA_QUEUE_LEVEL_MAX >= RA_QUEUE_LEVEL_HANDOFF);
	CHECK(RA_QUEUE_LEVEL_MAX >= RA_QUEUE_LEVEL_NOSTAMP);
	/* The session block's field has to hold the top of the ladder. */
	CHECK(RA_QUEUE_LEVEL_MAX <= 255);

	*DISPCNT = 0x1F40;

	/*
	    Run before the real startup, because ra_startup() only does its work once per boot
	    and this needs to be that once.
	*/
	printf("\nthe allocator hands out the arena, and reclaims it\n");
	{
		static char pool[4096];
		void* a;
		void* bb;
		void* c;

		ra_alloc_init(pool, pool + sizeof(pool));
		CHECK(ra_alloc_size() == sizeof(pool));
		CHECK(ra_alloc_used() == 0);
		CHECK(ra_alloc_failures() == 0);

		a  = ra_alloc_malloc(100);
		bb = ra_alloc_malloc(100);
		CHECK(a != 0 && bb != 0);
		/* 100 rounds up to 104, plus an 8-byte header each. */
		CHECK(ra_alloc_used() == 2 * (104 + 8));
		/* Payloads 8-aligned, which rcheevos' 64-bit typed values require. */
		CHECK(((u32)a & 7) == 0 && ((u32)bb & 7) == 0);
		/* And they do not overlap. */
		CHECK((char*)bb >= (char*)a + 104);

		memset(a, 0xAA, 100);
		memset(bb, 0x55, 100);
		CHECK(((unsigned char*)a)[99] == 0xAA);   /* neither wrote into the other */
		CHECK(((unsigned char*)bb)[0] == 0x55);

		ra_alloc_free(a);
		CHECK(ra_alloc_used() == 104 + 8);
		ra_alloc_free(bb);
		CHECK(ra_alloc_used() == 0);
		/* Coalesced back into one block, or the next big request would fail. */
		CHECK(ra_alloc_largest() == sizeof(pool) - 8);

		c = ra_alloc_malloc(sizeof(pool) - 8);
		CHECK(c != 0);
		CHECK(ra_alloc_largest() == 0);
		ra_alloc_free(c);
		CHECK(ra_alloc_largest() == sizeof(pool) - 8);
	}

	printf("\nthe allocator refuses rather than overruns\n");
	{
		static char pool[512];
		void* a;

		ra_alloc_init(pool, pool + sizeof(pool));
		CHECK(ra_alloc_malloc(sizeof(pool)) == 0);          /* no room for the header */
		CHECK(ra_alloc_malloc(0xFFFFFFF0u) == 0);           /* would wrap the alignment */
		CHECK(ra_alloc_failures() == 2);

		a = ra_alloc_malloc(0);                             /* legal, and not a failure */
		CHECK(a != 0);
		ra_alloc_free(a);

		ra_alloc_free(0);                                   /* all no-ops, not corruption */
		ra_alloc_free(pool - 64);
		ra_alloc_free(pool + sizeof(pool) + 64);
		a = ra_alloc_malloc(64);
		ra_alloc_free(a);
		ra_alloc_free(a);                                    /* double free */
		CHECK(ra_alloc_used() == 0);
		CHECK(ra_alloc_largest() == sizeof(pool) - 8);        /* arena still whole */
	}

	printf("\nrealloc grows, copies, and leaves the original alone on failure\n");
	{
		static char pool[4096];
		void* a;
		void* grown;

		ra_alloc_init(pool, pool + sizeof(pool));
		a = ra_alloc_malloc(32);
		memset(a, 0x5A, 32);

		grown = ra_alloc_realloc(a, 64);
		CHECK(grown != 0);
		CHECK(((unsigned char*)grown)[0] == 0x5A && ((unsigned char*)grown)[31] == 0x5A);

		/* Already big enough: kept in place rather than moved. */
		CHECK(ra_alloc_realloc(grown, 40) == grown);

		/* Cannot be satisfied, so the original must survive. */
		CHECK(ra_alloc_realloc(grown, sizeof(pool) * 2) == 0);
		CHECK(((unsigned char*)grown)[0] == 0x5A);

		CHECK(ra_alloc_realloc(0, 16) != 0);        /* realloc(0, n) is malloc */
		CHECK(ra_alloc_realloc(grown, 0) == 0);     /* realloc(p, 0) is free */
	}

	printf("\ncalloc zeroes, and refuses an overflowing product\n");
	{
		static char pool[4096];
		unsigned char* a;

		ra_alloc_init(pool, pool + sizeof(pool));
		a = (unsigned char*)ra_alloc_malloc(64);
		memset(a, 0xFF, 64);
		ra_alloc_free(a);

		a = (unsigned char*)ra_alloc_calloc(8, 8);   /* same block back, previously 0xFF */
		CHECK(a != 0);
		CHECK(a[0] == 0 && a[63] == 0);
		CHECK(ra_alloc_calloc(0x10000000u, 0x40u) == 0);   /* product overflows 32 bits */
	}

	printf("\nan exhausted arena is a refusal, not a wild write\n");
	{
		static char pool[1024];
		int taken = 0;
		void* p;

		ra_alloc_init(pool, pool + sizeof(pool));
		while ((p = ra_alloc_malloc(64)) != 0) {
			memset(p, 0xC3, 64);   /* would corrupt the arena if it handed out too much */
			taken++;
			if (taken > 100) {
				break;             /* it should have refused long before here */
			}
		}
		CHECK(taken > 0 && taken <= sizeof(pool) / (64 + 8));
		CHECK(ra_alloc_failures() > 0);
		CHECK(ra_alloc_used() <= sizeof(pool));
	}

	printf("\na failed allocator stays failed, on every call\n");
	{
		/*
		    A window with no room past .bss, which is the one way the probe can fail without
		    mocking anything: ra_alloc_init() cannot take an arena, so the probe's
		    allocation is refused and ra_startup() must say RA_STAGE_HEAP -- and keep saying
		    it.

		    Run before the real startup, because ra_startup() only does its work once per
		    boot and this needs to be that once.
		*/
		char* const bssEnd = FAKE_BSS_END;

		CHECK(ra_startup(FAKE_BSS, bssEnd, bssEnd) == RA_STAGE_HEAP);
		/*
		    The regression. This second call used to return RA_STAGE_ALLOC and claim a
		    working heap, because the "already ran" flag is set before the probe.
		*/
		CHECK(ra_startup(FAKE_BSS, bssEnd, bssEnd) == RA_STAGE_HEAP);
		CHECK(ra_malloc_probe() == 0);
		CHECK(ra_sbrk_probe() == 0);   /* no arena was taken, which is why it failed */
		CHECK(ra_heap_size() == 0);

		/* Back to a fresh boot for the tests below. */
		startupState = RA_STARTUP_FRESH;
		startupStage = RA_STAGE_NONE;
	}

	printf("\nstartup zeroes .bss once and measures the arena\n");
	memset(FAKE_BSS, 0xA5, FAKE_BSS_BYTES);
	CHECK(ra_startup(FAKE_BSS, FAKE_BSS_END, FAKE_TOP) == RA_STAGE_ALLOC);
	CHECK(FAKE_BSS[0] == 0 && FAKE_BSS[FAKE_BSS_BYTES - 1] == 0);
	CHECK(ra_heap_size() == FAKE_ARENA_BYTES);
	/* The probe allocated, wrote, read back and freed, so the arena is whole again. */
	CHECK(ra_heap_used() == 0);
	CHECK(ra_malloc_probe() != 0);
	/*
	    Second call must be a no-op. The flag that says so lives in .data, so the zeroing
	    cannot clear it -- which is the point of putting it there.
	*/
	FAKE_BSS[0] = 0x42;
	CHECK(ra_startup(FAKE_BSS, FAKE_BSS_END, FAKE_TOP) == RA_STAGE_ALLOC);
	CHECK(FAKE_BSS[0] == 0x42);

	printf("\n_sbrk refuses everything, so the arena has one owner\n");
	{
		/*
		    ra_alloc.c owns the arena in one piece now. If _sbrk() still handed it out,
		    newlib's allocator and ours would be writing over each other -- which is the
		    kind of corruption that only appears under load.
		*/
		CHECK(_sbrk(16) == (void*)-1);
		CHECK(_sbrk(0) == (void*)-1);
		CHECK(_sbrk(-16) == (void*)-1);
		CHECK(ra_heap_used() == 0);   /* and nothing was handed out behind our back */
	}

	printf("\nthe first tick claims .bss and installs the defaults\n");
	/* Neither side's .bss is ever zeroed on the target, so start from garbage. */
	memset(&snapshot, 0xA5, sizeof(snapshot));
	stateMagic = 0;   /* startup zeroed .bss on the target; do the same here */
	snapshot.ticks = 7;
	ra_wram_tick(&snapshot);
	CHECK(snapshot.watchCount == 2);
	CHECK(snapshot.resolved == 2);
	CHECK(snapshot.wramMagic == RA_WRAM_MAGIC);
	CHECK(snapshot.wramTicks == 1);
	CHECK(snapshot.wramStage == RA_STAGE_WATCHES);
	CHECK(snapshot.heapSize == 0x3B000);

	/*
	    rcheevos, on the same tick. This is the first thing in the project that turns a
	    server-side definition string into an evaluated condition, and all of it is logic
	    a host can check: whether the definition parses, whether the console-to-DS
	    address translation lands on the right word, and whether the peek path refuses
	    anything it should not.
	*/
	printf("\nthe frame throttle keeps the reader inside the blanking period\n");
	{
		/*
		    The steady-state cost was measured at 28-31 scanlines for 45 definitions against 71 of
		    blanking, and Chrono Trigger's larger set spilled past it -- tearing on the world map, and
		    a dead ARM9 entering Leene Square where the game had no slack. This is the arithmetic that
		    keeps the average inside whatever room was actually left.
		*/
		CHECK(ra_rc_frame_skip(30, 40) == 0);    /* fits, so every frame */
		CHECK(ra_rc_frame_skip(40, 40) == 0);    /* exactly fits */
		CHECK(ra_rc_frame_skip(41, 40) == 1);    /* just over: every other frame */
		CHECK(ra_rc_frame_skip(80, 40) == 2);    /* twice the room: every third */
		CHECK(ra_rc_frame_skip(200, 40) == RA_RC_FRAME_SKIP_MAX);   /* capped, not stalled */
		/*
		    A zero cost never throttles whatever the room says -- and this is the case the host runs
		    in, since RA_VCOUNT never advances here. Get it wrong and the suite's own frame ticks stop
		    evaluating, which is exactly how this was caught.
		*/
		CHECK(ra_rc_frame_skip(0, 0) == 0);
		CHECK(ra_rc_frame_skip(0, 40) == 0);
		/* Started outside blanking with real work done: over budget, and no denominator. */
		CHECK(ra_rc_frame_skip(10, 0) == RA_RC_FRAME_SKIP_MAX);
		/* The floor is well inside what the old VCOUNT hook shipped, which ran on 8% of frames. */
		CHECK(RA_RC_FRAME_SKIP_MAX <= 11);

		/*
		    ...and the half the throttle above could not do, because hardware said so: skipping frames
		    lowers the average and leaves the peak alone, so the frame that does run still overruns by
		    exactly as much as it always did. Leene Square stopped freezing and the world map went on
		    tearing, which is what a bounded reactive throttle looks like -- the same glitch, one frame
		    in four instead of every frame.

		    ra_rc_frame_fits() decides *before* the work whether there is room for it.
		*/
		CHECK(ra_rc_frame_fits(30, 40, 0) == 1);   /* room to spare */
		CHECK(ra_rc_frame_fits(40, 40, 0) == 1);   /* exactly fits, so take it */
		CHECK(ra_rc_frame_fits(41, 40, 0) == 0);   /* one scanline over is one scanline of tearing */
		CHECK(ra_rc_frame_fits(200, 40, 0) == 0);
		CHECK(ra_rc_frame_fits(10, 0, 0) == 0);    /* began outside blanking: no room at all */
		/*
		    A cost of zero always runs, for the same reason it never throttles above -- and again this
		    is the case the host runs in. Both of this function's inputs are zero here forever, so
		    getting it wrong stops the suite's own frame ticks silently.
		*/
		CHECK(ra_rc_frame_fits(0, 0, 0) == 1);
		CHECK(ra_rc_frame_fits(0, 40, 0) == 1);
		/*
		    Starvation always runs. A game whose own handler leaves nothing behind would otherwise stop
		    the reader for the whole session, and a reader that never evaluates is the worse bug.
		*/
		CHECK(ra_rc_frame_fits(200, 0, RA_RC_FRAME_STARVE_MAX) == 1);
		CHECK(ra_rc_frame_fits(200, 0, RA_RC_FRAME_STARVE_MAX - 1) == 0);
		CHECK(ra_rc_frame_fits(200, 0, 255) == 1);
		/* And the forced run is rare enough to be worth forcing: 1 in 17 frames, near the 8% shipped. */
		CHECK(RA_RC_FRAME_STARVE_MAX >= 8 && RA_RC_FRAME_STARVE_MAX <= 32);

		/*
		    ...and then hardware said neither of the two above can work on this set, with two numbers
		    read off the snapshot on Chrono Trigger's world map: rcLinesMax 101, rcRoomMax 70.

		    70 of a possible 71 means the game leaves the reader essentially the whole blanking period
		    and the work still needs 40% more than the hardware has. There is no schedule for a job
		    that exceeds the budget on its cheapest frame, so the set is divided instead.
		*/
		CHECK(ra_rc_frame_parts(1, 101, 70) == 2);   /* the measurement that forced this */
		CHECK(ra_rc_frame_parts(2, 69, 70) == 2);    /* fits now: settle here */
		CHECK(ra_rc_frame_parts(2, 71, 70) == 3);    /* one over is still over */
		/* It only rises, so a piece that fits is never enlarged on a guess about the memref pass. */
		CHECK(ra_rc_frame_parts(4, 10, 70) == 4);
		CHECK(ra_rc_frame_parts(1, 10, 70) == 1);
		/* Pinned at the ceiling is a reading, not a loop: no division of triggers fits, so stop. */
		CHECK(ra_rc_frame_parts(RA_RC_PARTS_MAX, 200, 70) == RA_RC_PARTS_MAX);
		/* Zero either side says nothing, which is the case the host lives in. */
		CHECK(ra_rc_frame_parts(3, 0, 70) == 3);
		CHECK(ra_rc_frame_parts(3, 200, 0) == 3);
		/* A zero parts is 1, because ra_rc_do_frame_slice() treats it that way too. */
		CHECK(ra_rc_frame_parts(0, 0, 0) == 1);
		/* The floor on sample rate stays inside the 8% of frames the old VCOUNT hook shipped on. */
		CHECK(RA_RC_PARTS_MAX <= 12);
	}

	printf("\nthe MPU report names the region and refuses the one holding the game's code\n");
	{
		/*
		    47 of 70 scanlines go on the memref pass -- ~840 ARM9 cycles for a translate, a range
		    check and a load -- and the trigger loop shows the same ratio. Two unrelated halves slow
		    by the same factor is a property of how the code executes, not of what it does: this
		    binary runs from DSi WRAM at 0x03740000, which a DS game's MPU has no reason to have
		    marked cacheable. This is the region arithmetic that finds out.
		*/
		u32 regs[8];
		u32 base = 0;
		u8  n;

		for (n = 0; n < 8; n++) {
			regs[n] = 0;
		}
		/* Region 1: main RAM, 4M at 0x02000000. Size 21 -> 2^22. */
		regs[1] = 0x02000000 | (21 << 1) | 1;
		/* Region 2: the 0x03000000 page, 16M -> covers our window and the I/O registers with it. */
		regs[2] = 0x03000000 | (23 << 1) | 1;

		CHECK(ra_mpu_region_pick(regs, 0x03740000, &base) == 2);
		CHECK(base == 0x03000000);
		CHECK(ra_mpu_region_pick(regs, 0x02000000, &base) == 1);
		CHECK(base == 0x02000000);
		/* Nothing maps this, and that would be a surprise worth reporting rather than assuming. */
		CHECK(ra_mpu_region_pick(regs, 0x06000000, &base) == -1);

		/* The highest matching region wins, whatever order they are declared in. */
		regs[5] = 0x03740000 | (17 << 1) | 1;   /* 256K, exactly our window */
		CHECK(ra_mpu_region_pick(regs, 0x03740000, &base) == 5);
		CHECK(base == 0x03740000);

		/* A disabled region does not win, however well it matches. */
		regs[5] &= ~1u;
		CHECK(ra_mpu_region_pick(regs, 0x03740000, &base) == 2);

		/* Below the 4 KB minimum is not a region. */
		regs[5] = 0x03740000 | (8 << 1) | 1;
		CHECK(ra_mpu_region_pick(regs, 0x03740000, &base) == 2);

		/*
		    Size 31 is the whole address space and its length cannot be shifted for -- 1u << 32 is
		    undefined, and getting it wrong here would mean picking a background region for every
		    address or for none.
		*/
		for (n = 0; n < 8; n++) {
			regs[n] = 0;
		}
		regs[0] = 0x00000000 | (31 << 1) | 1;
		CHECK(ra_mpu_region_pick(regs, 0x03740000, &base) == 0);
		CHECK(base == 0);

		/* The report. Region 2, instruction cache off for it, globally on, data cache off. */
		CHECK(ra_mpu_report(2, 0, 0, 1u << 12, 0) == (2 | RA_MPU_ICACHE_GLOBAL));
		CHECK(ra_mpu_report(2, 1u << 2, 0, 1u << 12, 0)
		      == (2 | RA_MPU_ICACHE_WAS_ON | RA_MPU_ICACHE_GLOBAL));
		CHECK(ra_mpu_report(2, 0, 1u << 2, 1u << 12, 0)
		      == (2 | RA_MPU_DCACHE_ON | RA_MPU_ICACHE_GLOBAL));
		/*
		    The safety case. A region that reaches main RAM governs how the *game's* code is
		    fetched, and a loader that DMAs overlays into main RAM is the last program that should
		    have an instruction cache switched on underneath it.
		*/
		CHECK(ra_mpu_report(1, 0, 0, 1u << 12, 1)
		      == (1 | RA_MPU_SPANS_MAIN_RAM | RA_MPU_ICACHE_GLOBAL));
		/* Bit 6 clear makes every other bit moot, and it is the first thing to read. */
		CHECK((ra_mpu_report(2, 0, 0, 0, 0) & RA_MPU_ICACHE_GLOBAL) == 0);
		/* No region at all is a sentinel, not a bit, and cannot be confused with a real report. */
		CHECK(ra_mpu_report(-1, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 1) == RA_MPU_NO_REGION);
		CHECK(ra_mpu_report(7, 1u << 7, 1u << 7, 1u << 12, 1) != RA_MPU_NO_REGION);
	}

	printf("\nthe self-test's own id is one the unlock guard refuses\n");
	{
		/*
		    The bug this pins reached a real account, and it reached it past a guard written for it.

		    ra_rc_queue_unlock() refuses an id at or above RA_SYNTHETIC_ID_BASE, because that is what
		    a definition with no id of its own gets. The **built-in** self-test is the other kind of
		    idless definition and carried its own constant, RA_TEST_ACHIEVEMENT_ID, four hundred lines
		    away -- which was 1. So on every game the server does not know, the self-test fired,
		    passed the guard, was queued, and was submitted. Achievement 1 is published on a Mega
		    Drive game, so the server accepted it.

		    Checked by *calling the guard* rather than by comparing the two constants, because what
		    has to hold is that nothing this binary invents can be queued -- not that two numbers
		    happen to be ordered. A future third source of made-up ids fails here the same way.
		*/
		const u8 queuedBefore    = unlockQueued;
		const u8 syntheticBefore = unlockSynthetic;

		ra_rc_queue_unlock(RA_TEST_ACHIEVEMENT_ID);
		CHECK(unlockQueued == queuedBefore);
		CHECK(unlockSynthetic == syntheticBefore + 1);
		/* And stated as the relationship too, so a failure says which half moved. */
		CHECK(RA_TEST_ACHIEVEMENT_ID >= RA_SYNTHETIC_ID_BASE);
	}

	printf("\nrcheevos parses the definition and evaluates it\n");
	/*
	    rcheevos comes up over several ticks now -- see RA_RC_LOADING. Ticked until it settles,
	    and bounded, so a state machine that never reports ACTIVE fails here in milliseconds
	    rather than by wedging a console inside an interrupt handler.
	*/
	{
		int ticks = 0;

		while (snapshot.rcStage < RA_RC_ACTIVE && ticks < RA_DEFS_MAX_LINES + 8) {
			ra_wram_tick(&snapshot);
			ticks++;
		}
		/*
		    Settled, and in a bounded number of ticks -- not in a *specific* number. Activation is
		    budgeted by scanlines now (see RA_RC_INIT_BUDGET_LINES), and on a host RA_VCOUNT is a
		    mapped register that never advances, so every activation measures as free and the batch
		    finishes in one pass. Pinning 2 would pin that accident rather than the behaviour.
		*/
		CHECK(ticks >= 1);
		CHECK(snapshot.rcStage >= RA_RC_ACTIVE);
	}
	CHECK(snapshot.rcActivate == 0);                  /* RC_OK */
	CHECK(snapshot.rcStage == RA_RC_FRAME);
	CHECK(snapshot.rcTriggerState != RC_TRIGGER_STATE_DISABLED);
	CHECK(snapshot.rcTarget == 600);
	/*
	    The definition reads one address, so a peek count of zero would mean do_frame
	    never reached memory -- which is the failure that would otherwise look like an
	    achievement that simply never unlocks.
	*/
	CHECK(snapshot.rcPeeks > 0);
	/*
	    Zero refusals is the whole point of routing peek() through ra_readable(): the
	    definition points at the snapshot, which is in main RAM, so nothing here should be
	    turned away. A non-zero count means the translation is wrong, not that the check is
	    too strict.
	*/
	CHECK(snapshot.rcPeeksRejected == 0);
	/*
	    Not disabled, which is what rc_runtime_validate_addresses() would have done had any
	    address in the definition been one this console cannot supply. So this asserts the
	    validation ran and passed, rather than that it was never wired up.
	*/
	CHECK(snapshot.rcTriggerState != RC_TRIGGER_STATE_DISABLED);
	CHECK(snapshot.rcTriggerState != RC_TRIGGER_STATE_INACTIVE);

	/*
	    The bug that disabled the achievement on the first hardware run. The cardengine
	    lives at 0x027FC000, inside main RAM's *second* 4M mirror, so subtracting the base
	    gives 0x7Fxxxx -- past the 4M RetroAchievements maps, and correctly refused. The
	    mask fixes it, but only where the region really is a mirror, so it is proved rather
	    than assumed.
	*/
	/*
	    The self-test definition is anchored at console address 0 -- the first word of the
	    game's own RAM -- because the snapshot has no console address on real hardware. Main
	    RAM there is 16M and RetroAchievements maps 4M, so the cardengine at 0x027FC000 is
	    eight megabytes past the end of the map, and not a mirror of 0x023FC000 either: that
	    was tested directly on hardware with a sentinel and they are separate memory.
	*/
	/*
	    The staged definition, which is how a real achievement gets tried without a rebuild.
	    Exercised here because the alternative -- finding out on hardware -- costs a flash
	    cycle per mistake, which is exactly what the file was introduced to stop.
	*/
	printf("\na staged definition is used, and a missing one falls back\n");
	{
		char* const  block = (char*)CARDENGINEI_ARM9_RA_DEFS_LOCATION;
		char* const  text  = block + CARDENGINEI_ARM9_RA_DEFS_HEADER;
		raSnapshot   probe;

		memset(&probe, 0, sizeof(probe));

		/* No magic: the built-in definition, and the snapshot says so. */
		*(u32*)block = 0;
		CHECK(strcmp(ra_definition(&probe), RA_TEST_DEFINITION) == 0);
		CHECK(probe.rcFromFile == 0);

		/* A staged definition is used verbatim. */
		strcpy(text, "0xH00c0fe=7");
		*(u32*)(block + 4) = strlen(text);
		*(u32*)block = CARDENGINEI_ARM9_RA_DEFS_MAGIC;
		CHECK(strcmp(ra_definition(&probe), "0xH00c0fe=7") == 0);
		CHECK(probe.rcFromFile == 1);
		CHECK(probe.rcDefLength == strlen("0xH00c0fe=7"));

		/*
		    A file typed in a text editor ends with a newline, and rcheevos would reject the
		    whitespace as syntax. Trimmed rather than blamed on the user.
		*/
		strcpy(text, "0xH00c0fe=7\r\n");
		*(u32*)(block + 4) = strlen(text);
		CHECK(strcmp(ra_definition(&probe), "0xH00c0fe=7") == 0);
		CHECK(probe.rcDefLength == strlen("0xH00c0fe=7"));

		/* Empty and oversized both fall back rather than being handed to the parser. */
		*(u32*)(block + 4) = 0;
		CHECK(strcmp(ra_definition(&probe), RA_TEST_DEFINITION) == 0);
		CHECK(probe.rcFromFile == 0);
		*(u32*)(block + 4) = CARDENGINEI_ARM9_RA_DEFS_MAX;
		CHECK(strcmp(ra_definition(&probe), RA_TEST_DEFINITION) == 0);

		/*
		    Several definitions in one file, because a hardware session is the scarce
		    resource and testing one per session wastes it. Comments and blank lines are
		    skipped so the file can say what each line is meant to do.
		*/
		{
			char*       lines[RA_DEFS_MAX_LINES];
			const char* titles[RA_DEFS_MAX_LINES];
			const char  file[] =
				"# what this is for\n"
				"0xH0012a4=1\n"
				"\n"
				"  I:0xW159164_0xX00009c=2  \r\n"
				"0x159992>d0x159992\n";
			u8 count;

			memcpy(text, file, sizeof(file));
			count = ra_split_definitions(text, sizeof(file) - 1, lines, titles);
			CHECK(count == 3);
			CHECK(strcmp(lines[0], "0xH0012a4=1") == 0);
			/* Leading and trailing whitespace and the CR all gone. */
			CHECK(strcmp(lines[1], "I:0xW159164_0xX00009c=2") == 0);
			CHECK(strcmp(lines[2], "0x159992>d0x159992") == 0);
			/* A hand-written file carries no titles, and that is not an error. */
			CHECK(titles[0] == 0 && titles[1] == 0 && titles[2] == 0);
		}

		/*
		    ...and the record the launcher actually writes: `<memaddr>\t<title>`, the id having already
		    been taken off by ra_take_id(). The tab becomes the memaddr's terminator, so rcheevos sees
		    exactly what it always saw and the title is a C string sitting just past it -- nothing is
		    copied.
		*/
		{
			char*       lines[RA_DEFS_MAX_LINES];
			const char* titles[RA_DEFS_MAX_LINES];
			const char  file[] =
				"0xH0012a4=1\tFirst Star\n"
				"0xH1=1\tChapter 1: Beginnings\n"
				"0xH2=2\n"
				"0xH3=3\ttrailing spaces   \n";
			u8 count;

			memcpy(text, file, sizeof(file));
			count = ra_split_definitions(text, sizeof(file) - 1, lines, titles);
			CHECK(count == 4);
			CHECK(strcmp(lines[0], "0xH0012a4=1") == 0);
			CHECK(titles[0] && strcmp(titles[0], "First Star") == 0);
			/* A colon in a title is the whole reason the delimiter is a tab. */
			CHECK(titles[1] && strcmp(titles[1], "Chapter 1: Beginnings") == 0);
			/* No tab, no title, and the line is unchanged. */
			CHECK(strcmp(lines[2], "0xH2=2") == 0 && titles[2] == 0);
			/*
			    Trailing whitespace is trimmed off the whole line before the tab is found, so a title
			    an editor padded does not arrive padded.
			*/
			CHECK(titles[3] && strcmp(titles[3], "trailing spaces") == 0);
		}

		/*
		    A file with more lines than there are slots stops rather than overruns.

		    The fixture is sized *from* the limit rather than to a round number, which it was
		    not: `many[256]` held twelve 12-byte lines comfortably while RA_DEFS_MAX_LINES was
		    8, and overflowed the moment it became 128. glibc's fortify check caught it, which
		    is the system working -- but a buffer whose size has to be re-derived by hand every
		    time a constant moves is a fixture waiting to lie.
		*/
		{
			enum { LINE_BYTES = sizeof("0xH00ffff=1\n") };
			static char*       lines[RA_DEFS_MAX_LINES];
			static const char* titles[RA_DEFS_MAX_LINES];
			static char        many[(RA_DEFS_MAX_LINES + 4) * LINE_BYTES];
			int          n;
			u32          len = 0;

			for (n = 0; n < RA_DEFS_MAX_LINES + 4; n++) {
				len += sprintf(many + len, "0xH00%04x=1\n", n);
			}
			/* And it has to fit where it is about to be copied. */
			CHECK(len < CARDENGINEI_ARM9_RA_DEFS_MAX - CARDENGINEI_ARM9_RA_DEFS_HEADER);
			memcpy(text, many, len + 1);
			CHECK(ra_split_definitions(text, len, lines, titles) == RA_DEFS_MAX_LINES);
		}

		/* Back to no file, so the tests after this see the built-in. */
		*(u32*)block = 0;
	}

	printf("\nthe viewer's index is taken before the block is cut up\n");
	{
		/*
		    The case the index exists for. After ra_split_definitions() has run, the record
		    terminator and the field separators are all NUL, and an earned `#!` record still has its
		    tabs -- two shapes in one buffer. So this asserts the offsets *against the mutated block*,
		    which is the state the menu actually sees, using a set that carries both.
		*/
		char* const  block = (char*)CARDENGINEI_ARM9_RA_DEFS_LOCATION;
		char* const  text  = block + CARDENGINEI_ARM9_RA_DEFS_HEADER;
		const raViewerBlock* v = (const raViewerBlock*)CARDENGINEI_ARM9_RA_VIEWER_LOCATION;
		static const char set[] =
			"#!302329\tWelcome to the Jungle\t3\tClear Stage 1\n"
			"302330:0xH1=1\tBack to the Lab Again\t5\tClear Stage 2\n"
			"# a comment somebody typed\n"
			"0xH2=2\tNo id at all\n";
		char*        lines[RA_DEFS_MAX_LINES];
		const char*  titles[RA_DEFS_MAX_LINES];
		const u32    len = (u32)strlen(set);

		memcpy(text, set, len + 1);
		CHECK(ra_split_definitions(text, len, lines, titles) == 2);   /* the comment is not one */

		CHECK(v->magic == RA_VIEWER_MAGIC);
		/* Three records indexed, and the typed comment is not among them. */
		CHECK(v->count == 3);
		CHECK(v->earned == 1);

		/* The earned one keeps its id, its flag, and all three fields. */
		CHECK(v->entry[0].id == 302329);
		CHECK(v->entry[0].flags == RA_VIEWER_EARNED);
		CHECK(strcmp(text + v->entry[0].titleOff, "Welcome to the Jungle") == 0);
		CHECK(strcmp(text + v->entry[0].pointsOff, "3") == 0);
		CHECK(strcmp(text + v->entry[0].descOff, "Clear Stage 1") == 0);

		/*
		    And the armed one, whose fields the split has just turned into NUL-terminated strings --
		    which is why reading them back through the index is the assertion worth making rather
		    than reading them before it ran.
		*/
		CHECK(v->entry[1].id == 302330);
		CHECK(v->entry[1].flags == 0);
		CHECK(strcmp(text + v->entry[1].titleOff, "Back to the Lab Again") == 0);
		CHECK(strcmp(text + v->entry[1].pointsOff, "5") == 0);
		CHECK(strcmp(text + v->entry[1].descOff, "Clear Stage 2") == 0);

		/*
		    A record with no id and no points is indexed with what it has. Offset 0 says absent, and
		    it is unambiguous because offset 0 is the first record's first byte.
		*/
		CHECK(v->entry[2].id == 0);
		CHECK(strcmp(text + v->entry[2].titleOff, "No id at all") == 0);
		CHECK(v->entry[2].pointsOff == 0 && v->entry[2].descOff == 0);

		*(u32*)block = 0;
	}

	printf("\nthe notification's pixels come out the way the DS reads them\n");
	{
		/*
		    The nibble order is the one thing here that cannot be checked on hardware without
		    spending a run. A 4bpp DS tile stores the *leftmost* pixel in the low nibble of each
		    word; getting it backwards renders every glyph mirrored, which on a photograph of a
		    three-second toaster reads as "the font is wrong" rather than as an ordering bug.

		    So it is checked against a glyph whose shape is known: 'L' is `.##.....` six times over a
		    foot of `.######.`, which is asymmetric in both axes and cannot pass by accident. Reversing
		    the order would move the upright's pixels from nibbles 1 and 2 to nibbles 5 and 6.

		    Which pixel column a glyph starts in is *not* asserted -- that is the font's left bearing
		    and it is free to change. What is asserted is where the ink sits relative to the word.
		*/
		const u32* strip = (const u32*)ra_text_render("L", 0);
		const int  col   = RA_TEXT_COLS - RA_TEXT_MARGIN - 1;   /* one character, right-aligned */
		const u32* tile  = &strip[col * 8];
		int        y;
		int        uprightRows = 0;

		/* The foot spans nibbles 1 to 6. */
		CHECK(nibbleSpanOf(tile[6], RA_TEXT_INK) == span(1, 6));
		/* The upright spans nibbles 1 to 2, on every row above the foot. */
		for (y = 0; y < 6; y++) {
			if (nibbleSpanOf(tile[y], RA_TEXT_INK) == span(1, 2)) {
				uprightRows++;
			}
		}
		CHECK(uprightRows == 6);
		/* Row 7 carries no ink: it is the gap the font leaves between lines. */
		CHECK(nibbleSpanOf(tile[7], RA_TEXT_INK) == 0);
		/* Every set pixel is one of the two entries the overlay borrows, and nothing else. */
		{
			int bad = 0;
			for (y = 0; y < 8; y++) {
				int n;
				for (n = 0; n < 8; n++) {
					const u32 nib = (tile[y] >> (n * 4)) & 0xF;
					if (nib != 0 && nib != RA_TEXT_INK && nib != RA_TEXT_SHADOW) {
						bad++;
					}
				}
			}
			CHECK(bad == 0);
		}

		/*
		    And the drop shadow, on the same glyph, which is where it can be stated exactly.

		    'L' is an upright at columns 1-2 over a foot at columns 1-6, so its shadow is an upright
		    at 2-3 over a foot at 2-7, one row lower. Ink is drawn second, so wherever the two want
		    the same pixel the ink has it -- which on rows 1 to 5 leaves the shadow a single visible
		    column at 3, and on row 6 leaves it nothing at all. That asymmetry is the assertion
		    worth making: a shadow drawn *after* the ink would read as span(2,3) there and would
		    look, on a photograph, like a font with a notch bitten out of it.
		*/
		CHECK(nibbleSpanOf(tile[0], RA_TEXT_SHADOW) == 0);          /* nothing above the first row */
		for (y = 1; y <= 5; y++) {
			CHECK(nibbleSpanOf(tile[y], RA_TEXT_SHADOW) == span(3, 3));
		}
		CHECK(nibbleSpanOf(tile[6], RA_TEXT_SHADOW) == 0);          /* the foot's ink covers it */
		CHECK(nibbleSpanOf(tile[7], RA_TEXT_SHADOW) == span(2, 7)); /* the foot's own shadow */
	}

	printf("\n...right-aligned, clipped, and never showing the previous message\n");
	{
		const u32* strip;
		const int  last = RA_TEXT_COLS - RA_TEXT_MARGIN - 1;

		/* Two lines land on the two rows, and each is right-aligned on its own. */
		strip = (const u32*)ra_text_render("A", "A");
		CHECK(strip[last * 8 + 2] != 0);
		CHECK(strip[(RA_TEXT_COLS + last) * 8 + 2] != 0);

		/*
		    The margin column is what the shadow hangs into, and 'd' is the proof rather than an
		    illustration: its bowl reaches pixel 7 of its cell, so its shadow reaches pixel 8 --
		    which is column 31, the margin. Right-aligned flush against the edge instead, that pixel
		    would fall off the strip, and the last character of every message would be the one
		    missing its shadow.
		*/
		strip = (const u32*)ra_text_render(0, "d");
		CHECK(nibbleSpanOf(strip[(RA_TEXT_COLS + last) * 8], RA_TEXT_INK) == span(6, 7));
		CHECK(nibbleSpanOf(strip[(RA_TEXT_COLS + RA_TEXT_COLS - 1) * 8 + 1], RA_TEXT_SHADOW)
		      == span(0, 0));

		/*
		    A long title is clipped to the strip rather than wrapping into the row below or running
		    off the end of the array. RA_TEXT_COLS + 8 characters, so an unclipped write would be
		    visible in the next row's tiles.
		*/
		strip = (const u32*)ra_text_render(0, "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
		{
			int i;
			int row0 = 0;

			for (i = 0; i < RA_TEXT_COLS * 8; i++) {
				if (strip[i]) {
					row0++;
				}
			}
			CHECK(row0 == 0);   /* line1 was NULL, so row 0 stays blank */
		}

		/*
		    And the strip is cleared whole between calls. A shorter message after a longer one would
		    otherwise keep the tail of the previous *achievement name* -- which reads as a plausible
		    message rather than as obvious corruption, and is the worst way for this to fail.
		*/
		strip = (const u32*)ra_text_render("WWWWWWWWWWWWWWWWWWWWWWWWWWWWWW", 0);
		strip = (const u32*)ra_text_render("W", 0);
		{
			int i;
			int set = 0;

			for (i = 0; i < RA_TEXT_WORDS; i++) {
				if (strip[i]) {
					set++;
				}
			}
			/* One glyph is at most 8 non-zero words, so anything more is leftover text. */
			CHECK(set > 0 && set <= 8);
		}

		/* A character outside the font is a space, not a box and not a wrong letter. */
		strip = (const u32*)ra_text_render("\x01\x02\x03", 0);
		{
			int i;
			int set = 0;

			for (i = 0; i < RA_TEXT_WORDS; i++) {
				if (strip[i]) {
					set++;
				}
			}
			CHECK(set == 0);
		}
	}

	printf("\nthe self-test definition is anchored where the map reaches\n");
	CHECK(ra_rc_translate(0, 1) == RA_DS_SYSTEM_RAM_BASE);
	CHECK(ra_rc_validate_address(0) != 0);
	/*
	    And the reason it is anchored there: an address eight megabytes into main RAM --
	    where the cardengine actually lives on hardware -- has no console address at all.
	    The snapshot in this test links low enough to be inside the map, which the target's
	    never is.
	*/
	CHECK(ra_rc_console_unmapped(0x027FED54));
	CHECK(ra_rc_validate_address(0x027FED54 - RA_DS_SYSTEM_RAM_BASE) == 0);

	printf("\nthe definition's address translates to the snapshot's own ticks\n");
	CHECK(ra_rc_translate(0, 4) == RA_DS_SYSTEM_RAM_BASE);
	CHECK(ra_rc_translate((u32)&snapshot.ticks - RA_DS_SYSTEM_RAM_BASE, 4)
	      == (u32)&snapshot.ticks);
	/*
	    Console addresses this console does not have. The DS's 4M of system RAM is the
	    only region translated: the RetroAchievements map also lists data TCM at console
	    0x1000000, which has no fixed address on hardware and so is deliberately refused
	    rather than guessed at.
	*/
	CHECK(ra_rc_translate(RA_DS_SYSTEM_RAM_SIZE, 1) == 0);
	CHECK(ra_rc_translate(RA_DS_SYSTEM_RAM_SIZE - 1, 4) == 0);   /* would run off the end */
	CHECK(ra_rc_translate(RA_DS_SYSTEM_RAM_SIZE - 4, 4) != 0);   /* last aligned word fits */
	CHECK(ra_rc_translate(0x1000000, 1) == 0);                   /* data TCM */
	CHECK(ra_rc_translate(0xFFFFFFFC, 4) == 0);                  /* cannot wrap the check */

	/*
	    Unaligned multi-byte reads, assembled from bytes because the ARM9 would rotate the
	    word instead of faulting. Achievement authors write these and the server serves
	    them, so getting it wrong means plausible-looking wrong values rather than a crash.
	*/
	printf("\nunaligned reads are assembled little-endian, not rotated\n");
	{
		/*
		    Two adjacent words, so a read straddling the boundary has a known answer in
		    every byte. In memory that is 44 33 22 11 88 77 66 55, and a 32-bit read one
		    byte in must therefore be 0x88112233. An ARM9 LDR at that address would return
		    0x44112233 -- the aligned word, rotated -- which is why this is assembled from
		    bytes rather than left to the hardware.
		*/
		static u32 straddle[2];
		const u32 base = (u32)&straddle[0] - RA_DS_SYSTEM_RAM_BASE;
		straddle[0] = 0x11223344;
		straddle[1] = 0x55667788;

		CHECK(ra_rc_peek(base, 4, 0) == 0x11223344);
		CHECK(ra_rc_peek(base + 1, 2, 0) == 0x2233);
		CHECK(ra_rc_peek(base + 1, 4, 0) == 0x88112233);
		CHECK(ra_rc_peek(base + 3, 1, 0) == 0x11);
		CHECK(ra_rc_peek(base + 4, 4, 0) == 0x55667788);
	}

	printf("\na refused peek reads as zero and is counted, not dereferenced\n");
	{
		const u32 before = peeksRejected;
		CHECK(ra_rc_peek(0x1000000, 4, 0) == 0);   /* data TCM: unmapped here */
		CHECK(peeksRejected == before + 1);
	}

	printf("\nthe self-test cells point where the chain needs them to\n");
	CHECK(snapshot.selfCell == (u32)&snapshot);
	CHECK(snapshot.selfCellPtr == (u32)&snapshot.selfCell);

	printf("\ndefault watch 0 reads a register directly\n");
	expect_status("direct", 0, RA_WATCH_OK);
	CHECK(snapshot.results[0].address == RA_DEFAULT_WATCH_ADDRESS);
	CHECK(snapshot.results[0].value == 0x1F40);
	CHECK(snapshot.results[0].depth == 0);
	CHECK(snapshot.results[0].size == 2);

	printf("\ndefault watch 1 walks two indirections to ticks\n");
	expect_status("two steps", 1, RA_WATCH_OK);
	CHECK(snapshot.results[1].address == (u32)&snapshot.ticks);
	CHECK(snapshot.results[1].value == 7);
	CHECK(snapshot.results[1].depth == 2);

	printf("\nthe WRAM binary's own counter persists across ticks\n");
	*DISPCNT = 0x0100;
	/*
	    Relative, not absolute. This used to assert `== 10`, which encoded how many ticks
	    happened earlier in this file -- so adding the two ticks rcheevos now needs to come up
	    broke a test about a counter persisting. What is being checked is that nine ticks add
	    nine, and that is what it says now.
	*/
	{
		const u32 before = snapshot.wramTicks;

		for (i = 0; i < 9; i++) {
			snapshot.ticks++;
			ra_wram_tick(&snapshot);
		}
		CHECK(snapshot.wramTicks == before + 9);
	}
	CHECK(snapshot.results[1].value == 16);   /* ticks went 7 -> 16 */
	CHECK(snapshot.results[0].value == 0x0100);

	printf("\nthe chains are re-walked every tick, not cached\n");
	snapshot.ticks = 0x1234;
	ra_wram_tick(&snapshot);
	CHECK(snapshot.results[1].value == 0x1234);

	printf("\nan emptied list resolves nothing\n");
	ra_watch_clear();
	ra_wram_tick(&snapshot);
	CHECK(snapshot.watchCount == 0);
	CHECK(snapshot.resolved == 0);

	printf("\nmalformed requests are refused\n");
	CHECK(ra_watch_add((u32)&target, 3, 0, 0) == -1);   /* size not 1/2/4 */
	CHECK(ra_watch_add((u32)&target, 0, 0, 0) == -1);
	CHECK(ra_watch_add((u32)&target, 8, 0, 0) == -1);
	offsets[0] = 0;
	offsets[1] = 0;
	CHECK(ra_watch_add((u32)&target, 4, RA_CHAIN_MAX + 1, offsets) == -1);
	CHECK(ra_watch_add((u32)&target, 4, 1, 0) == -1);   /* depth without offsets */
	CHECK(watchCount == 0);

	printf("\nsized reads take the right width\n");
	target = 0x11223344;
	ra_watch_clear();
	CHECK(ra_watch_add((u32)&target, 1, 0, 0) == 0);
	CHECK(ra_watch_add((u32)&target, 2, 0, 0) == 1);
	CHECK(ra_watch_add((u32)&target, 4, 0, 0) == 2);
	ra_wram_tick(&snapshot);
	CHECK(snapshot.results[0].value == 0x44);      /* little-endian, as the DS is */
	CHECK(snapshot.results[1].value == 0x3344);
	CHECK(snapshot.results[2].value == 0x11223344);

	printf("\nthe list holds many more watches than the cardengine could\n");
	ra_watch_clear();
	for (i = 0; i < RA_WATCH_MAX; i++) {
		if (ra_watch_add((u32)&target, 4, 0, 0) != i) {
			printf("  FAIL  slot %d\n", i);
			failures++;
		}
	}
	CHECK(RA_WATCH_MAX == 16);
	CHECK(ra_watch_add((u32)&target, 4, 0, 0) == -1);   /* and then refuses */
	ra_wram_tick(&snapshot);
	CHECK(snapshot.watchCount == RA_WATCH_MAX);
	CHECK(snapshot.resolved == RA_WATCH_MAX);

	printf("\nonly the first RA_RESULT_MAX are mirrored into the snapshot\n");
	CHECK(RA_RESULT_MAX == 4);
	CHECK(snapshot.results[RA_RESULT_MAX - 1].status == RA_WATCH_OK);

	printf("\nan unreadable base is reported, not dereferenced\n");
	ra_watch_clear();
	ra_watch_add(0x09000000, 4, 0, 0);   /* GBA cart space: not in the list */
	ra_wram_tick(&snapshot);
	expect_status("bad base", 0, RA_WATCH_BAD_BASE);
	CHECK(snapshot.results[0].address == 0);

	printf("\nthe window this code runs from is not readable either\n");
	ra_watch_clear();
	ra_watch_add(CARDENGINEI_ARM9_RA_LOCATION, 4, 0, 0);
	ra_wram_tick(&snapshot);
	expect_status("own window", 0, RA_WATCH_BAD_BASE);

	printf("\na misaligned target is reported rather than silently rotated\n");
	ra_watch_clear();
	ra_watch_add((u32)&target + 1, 4, 0, 0);
	ra_watch_add((u32)&target + 1, 2, 0, 0);
	ra_watch_add((u32)&target + 1, 1, 0, 0);   /* bytes are never misaligned */
	ra_wram_tick(&snapshot);
	expect_status("misaligned word", 0, RA_WATCH_MISALIGNED);
	expect_status("misaligned halfword", 1, RA_WATCH_MISALIGNED);
	expect_status("byte", 2, RA_WATCH_OK);

	/*
	    The failure statuses say where the chain broke, which is the useful thing to know
	    from a RAM viewer: BAD_POINTER means a word the walker was about to dereference for
	    a *further* step was unusable, while a bad final address is BAD_TARGET or
	    MISALIGNED however many steps preceded it. So a one-step chain whose pointer goes
	    null reports BAD_TARGET -- there was no further step -- and reaching BAD_POINTER
	    takes at least two.
	*/
	printf("\na one-step chain that stops resolving reports its target\n");
	targetPtr = (u32)&target;
	ra_watch_clear();
	offsets[0] = 0;
	offsets[1] = 0;
	ra_watch_add((u32)&targetPtr, 4, 1, offsets);
	ra_wram_tick(&snapshot);
	expect_status("chain resolves", 0, RA_WATCH_OK);
	CHECK(snapshot.results[0].value == 0x11223344);

	targetPtr = 0;                      /* what a game does between scenes */
	ra_wram_tick(&snapshot);
	expect_status("null pointer", 0, RA_WATCH_BAD_TARGET);
	CHECK(snapshot.results[0].address == 0);

	targetPtr = 0xFFFFFFFC;             /* addr + len would wrap past the check */
	ra_wram_tick(&snapshot);
	expect_status("wrapping pointer", 0, RA_WATCH_BAD_TARGET);

	targetPtr = (u32)&target + 1;
	ra_wram_tick(&snapshot);
	expect_status("misaligned resolved address", 0, RA_WATCH_MISALIGNED);

	printf("\nit recovers when the pointer comes back\n");
	targetPtr = (u32)&target;
	ra_wram_tick(&snapshot);
	expect_status("recovered", 0, RA_WATCH_OK);
	CHECK(snapshot.results[0].value == 0x11223344);

	printf("\na pointer the walker would have to follow again is BAD_POINTER\n");
	ra_watch_clear();
	targetPtr    = (u32)&target;
	targetPtrPtr = (u32)&targetPtr;
	offsets[0] = 0;
	offsets[1] = 0;
	ra_watch_add((u32)&targetPtrPtr, 4, 2, offsets);
	ra_wram_tick(&snapshot);
	expect_status("two steps resolve", 0, RA_WATCH_OK);
	CHECK(snapshot.results[0].value == 0x11223344);

	targetPtrPtr = 0;                    /* step 0 yields a pointer step 1 must follow */
	ra_wram_tick(&snapshot);
	expect_status("null mid-chain", 0, RA_WATCH_BAD_POINTER);
	CHECK(snapshot.results[0].address == 0);

	targetPtrPtr = (u32)&targetPtr + 1;  /* unaligned, so unusable as a pointer */
	ra_wram_tick(&snapshot);
	expect_status("misaligned mid-chain", 0, RA_WATCH_BAD_POINTER);

	targetPtrPtr = 0x04000000;           /* readable, but no game pointer lives in I/O */
	ra_wram_tick(&snapshot);
	expect_status("mid-chain outside main RAM", 0, RA_WATCH_BAD_POINTER);

	printf("\nan unreadable base is BAD_BASE even on a chain\n");
	ra_watch_clear();
	ra_watch_add(0x09000000, 4, 2, offsets);
	ra_wram_tick(&snapshot);
	expect_status("bad base, depth 2", 0, RA_WATCH_BAD_BASE);

	printf("\noffsets are added at each step\n");
	{
		static u32 pair[2];
		pair[0] = 0;
		pair[1] = 0x0BADF00D;
		targetPtr = (u32)&pair[0];
		ra_watch_clear();
		offsets[0] = 4;             /* one word into pair[] */
		offsets[1] = 0;
		ra_watch_add((u32)&targetPtr, 4, 1, offsets);
		ra_wram_tick(&snapshot);
		expect_status("offset applied", 0, RA_WATCH_OK);
		CHECK(snapshot.results[0].address == (u32)&pair[1]);
		CHECK(snapshot.results[0].value == 0x0BADF00D);
	}

	/*
	    Last, because it advances snapshot.ticks and every assertion above about the
	    counters depends on how many times ra_wram_tick() has run.
	*/
	/*
	    Last, because it advances the frame count and every assertion above about the
	    counters depends on how many times ra_wram_tick() has run.
	*/
	printf("\nmeasured progress climbs one per frame, and triggers at the target\n");
	{
		const u32 startMeasured = snapshot.rcMeasured;
		for (i = 0; i < 5; i++) {
			ra_wram_tick(&snapshot);
		}
		/*
		    One hit per frame regardless of what memory does, which is what the always-true
		    comparison buys: the count is a property of rcheevos running, not of the game.
		*/
		CHECK(snapshot.rcMeasured == startMeasured + 5);
		CHECK(snapshot.rcTarget == 600);
		CHECK(snapshot.rcTriggered == 0);          /* still short of 600 */
		CHECK(snapshot.rcPeeks > 0);               /* and it really is reading memory */

		/* Run it out to the target and check the trigger actually fires. */
		while (snapshot.rcMeasured < snapshot.rcTarget && snapshot.rcTriggered == 0) {
			ra_wram_tick(&snapshot);
		}
		CHECK(snapshot.rcTriggered == 1);
		CHECK(snapshot.rcEvents > 0);
		/*
		    rcheevos stops reporting progress once a trigger fires, so these are latched.
		    Without that a snapshot read after the unlock shows two zeros and no evidence
		    of how it got there -- which is exactly what the first hardware reading showed.
		*/
		CHECK(snapshot.rcTarget == 600);
		/* The last value reported while active, which is the frame before it fired. */
		CHECK(snapshot.rcMeasured == snapshot.rcTarget - 1);
		/* Only the refusal forced by hand above; nothing in normal operation was denied. */
		CHECK(snapshot.rcPeeksRejected == 1);
	}

	/*
	    Watch lines and 24-bit console pointers, which is how the next hardware session
	    measures memory rather than guessing at another definition. Last, because it takes
	    the watchlist over and ticks -- every assertion above reads results[].
	*/
	printf("\nwatch lines parse, and 24-bit console pointers resolve\n");
	{
		const char* p;
		u32         v;

		p = "1593d0"; CHECK(ra_parse_hex(&p, &v) && v == 0x1593d0);
		p = "0x9c";   CHECK(ra_parse_hex(&p, &v) && v == 0x9c);
		p = ":4";     CHECK(!ra_parse_hex(&p, &v));   /* empty field is not a zero */

		ra_watch_clear();
		CHECK(ra_add_watch_line("1593d0:4", 0));
		CHECK(watchCount == 1);
		CHECK(watches[0].base == RA_DS_SYSTEM_RAM_BASE + 0x1593d0);
		CHECK(watches[0].size == 4 && watches[0].depth == 0);

		CHECK(ra_add_watch_line("159164:4:9c", 0));
		CHECK(watches[1].base == RA_DS_SYSTEM_RAM_BASE + 0x159164);
		CHECK(watches[1].depth == 1 && watches[1].offsets[0] == 0x9c);

		/* Malformed and out-of-range both refused rather than half-installed. */
		CHECK(!ra_add_watch_line("1593d0", 0));          /* no size */
		CHECK(!ra_add_watch_line("400000:4", 0));        /* past DS system RAM */
		CHECK(!ra_add_watch_line("1593d0:3", 0));        /* size not 1, 2 or 4 */
		CHECK(watchCount == 2);                        /* nothing was added */

		/*
		    A 24-bit console pointer, which is what RetroAchievements' DS notes mean and
		    what hardware showed the walker was getting wrong. The game stores a word
		    whose low 24 bits are the console address; read as a DS address it resolves
		    nowhere, which is exactly the BAD_TARGET the first run reported.
		*/
		ra_watch_clear();
		{
			static u32 fakePointerCell;
			static u32 fakeStruct[64];
			const u32  consoleBase = (u32)&fakePointerCell - RA_DS_SYSTEM_RAM_BASE;
			const u32  structConsole = (u32)&fakeStruct[0] - RA_DS_SYSTEM_RAM_BASE;

			fakeStruct[4] = 0xFEEDFACE;   /* at +0x10 */

			/* Top byte deliberately not 0x02, the way the game stores it. */
			fakePointerCell = 0x7F000000 | structConsole;

			CHECK(ra_add_watch_line("1593d0:4", RA_WATCH_FLAG_PTR24));   /* syntax ok */
			ra_watch_clear();

			{
				char line[32];
				sprintf(line, "%x:4:10", consoleBase);
				CHECK(ra_add_watch_line(line, RA_WATCH_FLAG_PTR24));
				ra_wram_tick(&snapshot);
				/* Masked, so it resolves into the struct and reads the value there. */
				CHECK(snapshot.results[0].status == RA_WATCH_OK);
				CHECK(snapshot.results[0].address == (u32)&fakeStruct[4]);
				CHECK(snapshot.results[0].value == 0xFEEDFACE);

				/* Without the flag, the same word is not a usable DS address. */
				ra_watch_clear();
				CHECK(ra_add_watch_line(line, 0));
				ra_wram_tick(&snapshot);
				CHECK(snapshot.results[0].status == RA_WATCH_BAD_TARGET);
			}
		}

		/*
		    Put the self-test watches back. Later tests read results[] and this block
		    borrowed the list to check the parser -- leaving it emptied would fail them
		    for a reason that has nothing to do with what they test.
		*/
		ra_watch_clear();
		ra_install_defaults(&snapshot);
	}

	/*
	    The shipped example file, parsed exactly as the WRAM binary would parse it.

	    This exists because of a mistake it would have caught. The Super Mario 64 DS round
	    was written with `0xH08e43c=0x38`, and in memaddr an operand starting `0x` is a
	    *memory read*, not a hex constant -- so that line compared Screen ID against the
	    16-bit word at address 0x38 and would have parsed cleanly, activated cleanly, and
	    quietly never fired. Hex constants take an `h` prefix.

	    The file had been rewritten four times by then and had never once been through the
	    parser. It is the document the user edits and the only part of this system whose
	    errors are silent, so it is now checked here rather than on hardware.
	*/
	printf("\nthe shipped example file parses\n");
	{
		char* const  block = (char*)CARDENGINEI_ARM9_RA_DEFS_LOCATION;
		char* const  text  = block + CARDENGINEI_ARM9_RA_DEFS_HEADER;
		const char*  path  = "tools/ra_achievements.example.txt";
		FILE*        f     = fopen(path, "rb");
		raSnapshot   probe;
		size_t       len;

		if (!f) {
			printf("  cannot open %s -- run from the repository root\n", path);
			failures++;
		} else {
			len = fread(text, 1, CARDENGINEI_ARM9_RA_DEFS_MAX
			                     - CARDENGINEI_ARM9_RA_DEFS_HEADER - 1, f);
			fclose(f);
			text[len] = 0;

			CHECK(len > 0);
			CHECK(len < CARDENGINEI_ARM9_RA_DEFS_MAX - CARDENGINEI_ARM9_RA_DEFS_HEADER);

			memset(&probe, 0, sizeof(probe));
			*(u32*)(block + 4) = (u32)len;
			*(u32*)block       = CARDENGINEI_ARM9_RA_DEFS_MAGIC;

			/*
			    Driven the way hardware drives it: prepare once, then one definition per
			    call. That is not a mechanical adaptation of the old single ra_rc_init() --
			    it is the test for the amortisation itself. The loop is bounded so a state
			    machine that never reports RA_RC_ACTIVE fails here in milliseconds instead
			    of hanging a console inside an interrupt handler.
			*/
			{
				u8  stage  = ra_rc_prepare(&probe);
				int passes = 0;

				CHECK(stage == RA_RC_LOADING);
				while (stage == RA_RC_LOADING && passes < RA_DEFS_MAX_LINES + 4) {
					stage = ra_rc_activate_next(&probe);
					passes++;
				}
				CHECK(stage == RA_RC_ACTIVE);
				/*
				    The count of passes is no longer the property worth pinning. Activation is
				    budgeted by scanlines now -- as many definitions per call as fit under
				    RA_RC_INIT_BUDGET_LINES -- because one-per-frame left a real set unarmed:
				    hardware read rcActivated 14 of 45 with rcPeeks still 0, so the reader's
				    fifteen frames never covered forty-five definitions.
				    
				    On a host RA_VCOUNT is a mapped register that does not advance, so every
				    activation measures as free and the whole set lands in one pass. Pinning a
				    number here would pin that accident. What matters is that nothing was
				    skipped, and rcActivated says so directly.
				*/
				CHECK(passes >= 1);
				CHECK(probe.rcActivated == 3);   /* the three definitions this fixture stages */
			}
			CHECK(probe.rcFromFile == 1);
			/*
			    Every line accounted for: no line was rejected, and the definitions that
			    activated are the file's own rather than the built-in fallback, which
			    ra_rc_init() substitutes when nothing parses.
			*/
			CHECK(probe.rcBadLine == 0);
			CHECK(probe.rcActivate == RC_OK);
			CHECK(probe.rcActivated == 3);
			/* The file's watches replaced the defaults. The count lives in the module
			   until a tick copies it out, and ticking here would dereference the game's
			   addresses, which this host does not have mapped. */
			CHECK(watchCount == 4);
			/*
			    Comments and watches together must still fit the split limit, and the file
			    should not be sitting exactly on it -- 7 of 8 leaves room for one more line
			    without a code change, and pins the fact that there is a limit at all.
			*/
			CHECK(watchCount + probe.rcActivated <= RA_DEFS_MAX_LINES);
			/*
			    And the two halves of the limit have to stay consistent with each other. 128
			    definitions have to fit the 32,760-byte block, which caps the average memaddr
			    string; raising the line count without checking that is how the block would
			    quietly become the real limit instead. RA strings run from tens to a few
			    hundred characters, so 255 average is the number to know.
			*/
			CHECK(RA_DEFS_MAX_LINES == 128);
			CHECK((CARDENGINEI_ARM9_RA_DEFS_MAX - CARDENGINEI_ARM9_RA_DEFS_HEADER)
			      / RA_DEFS_MAX_LINES >= 200);

			*(u32*)block = 0;
			ra_watch_clear();
			ra_install_defaults(&snapshot);
		}
	}

	printf("\nan id prefix is digits then a colon, and nothing else is\n");
	{
		/*
		    The discriminator for step 5's block format, and the case that decides it is the real
		    set's own first line: `1=1.300.` begins with a digit and is not an id. Every memaddr
		    prefix flag that ends in a colon is a letter, so digits-then-colon cannot collide with
		    memaddr syntax -- checked against the shipped set, where the character before the first
		    colon is M, N, O, P, R or T on all 47 lines that have one.
		*/
		char        line[64];
		char*       at;

		strcpy(line, "123456:0xH1=1");
		at = line;
		CHECK(ra_take_id(&at) == 123456);
		CHECK(strcmp(at, "0xH1=1") == 0);

		/* A definition that merely starts with a digit keeps all of itself. */
		strcpy(line, "1=1.300.");
		at = line;
		CHECK(ra_take_id(&at) == 0);
		CHECK(strcmp(at, "1=1.300.") == 0);

		/* A letter-prefixed flag is not an id, however many colons follow. */
		strcpy(line, "A:0xH1=1_M:0xH2=2");
		at = line;
		CHECK(ra_take_id(&at) == 0);
		CHECK(strcmp(at, "A:0xH1=1_M:0xH2=2") == 0);

		/* A colon with no digits before it, and digits with no colon after them. */
		strcpy(line, ":0xH1=1");
		at = line;
		CHECK(ra_take_id(&at) == 0);
		strcpy(line, "0xH000010>d0xH000010");
		at = line;
		CHECK(ra_take_id(&at) == 0);
		CHECK(strcmp(at, "0xH000010>d0xH000010") == 0);

		/*
		    Nine digits, because the real set has one: line 1 of GameID 14856 is
		    `101000001:1=1.300.`. And ten, which a u32 holds and the first clamp would have
		    silently shortened by a digit -- naming a different achievement.
		*/
		strcpy(line, "101000001:1=1.300.");
		at = line;
		CHECK(ra_take_id(&at) == 101000001);
		CHECK(strcmp(at, "1=1.300.") == 0);

		strcpy(line, "4294967295:0xH1=1");
		at = line;
		CHECK(ra_take_id(&at) == 4294967295u);
		CHECK(strcmp(at, "0xH1=1") == 0);

		/*
		    Past a u32 the id is refused -- but the prefix is still stripped, because a line left
		    with digits and a colon on the front is not memaddr syntax and rcheevos would refuse the
		    whole definition. Losing the ability to report one achievement beats losing the
		    achievement.
		*/
		strcpy(line, "99999999999:0xH1=1");
		at = line;
		CHECK(ra_take_id(&at) == 0);
		CHECK(strcmp(at, "0xH1=1") == 0);

		/* An id of 1 is a real id, and is not the same thing as no id. */
		strcpy(line, "1:0xH1=1");
		at = line;
		CHECK(ra_take_id(&at) == 1);
		CHECK(strcmp(at, "0xH1=1") == 0);
	}

	test_overlay_survey();

	test_wifi_verdict();

	printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
	       failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
