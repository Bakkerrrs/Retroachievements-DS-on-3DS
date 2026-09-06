/*
    Host-side checks for the launcher's own pure logic: the ROM hash (3b), the configuration
    file and the query encoding (3c). What they have in common is that every one of them fails
    *silently* -- a hash over the wrong bytes, a password truncated at an `&`, a config key that
    parses as something else -- and every one of those looks, from the console, exactly like a
    wrong password or a game with no achievement set.

    A second binary rather than a section of tools/ra_reader_test.c, and that separation is
    load-bearing. That file computes the WRAM allocator's arena from the addresses of
    `__bss_start`, `__bss_end` and `__vram_top`, which it defines as 1-byte dummies -- so the
    arena is whatever the linker's ordering of those three happens to make it. Adding
    rcheevos' hash.c and hash_rom.c to that link flipped `__vram_top` below `__bss_end`, the
    span became -1, and `ra_startup()` zeroed and handed out memory across the whole process.
    The suite passed at -O0 and segfaulted at -O1, in a test several sections before the new
    code was even reached.

    That fragility is worth fixing on its own terms and is not this file's business, so this
    file simply does not join that link: no cardengine sources, no fixed link address, no
    mapped pages. It needs none of them -- the hash is file I/O and an MD5.

    Run through tools/ra_reader_test.sh, which builds both binaries.

    This file is part of nds-bootstrap and is licensed under the GPL-3.0,
    the same terms as the rest of the project.
*/

#include <stdio.h>
#include <string.h>

#include "ra_wifi.h"
/* For the staging block's size, which step 3d's scanner has to fit a real set into. */
#include "locations.h"

/*
    raNetUrlEncode() and raNetJsonString() are string logic; the rest of ra_net.c is lwip
    sockets. Rather than link lwip into a host test to reach two pure functions, the two are
    lifted in by including the file with its socket half compiled out. RA_NET_HOST_TEST is
    defined nowhere else, so the target build always gets the whole thing.
*/
#define RA_NET_HOST_TEST 1
#include "../retail/arm9/source/ra_net.c"

/*
    ------------------------------------------------------------------------------------
    Step 3b's hash, checked against the only reference that matters.

    ra_hash.c streams the four ranges rcheevos hashes through a 1 K buffer, because
    rc_hash_nintendo_ds() allocates max(0xA00, arm9_size, arm7_size) in one block -- 353,164
    bytes for nds-bootstrap's own .nds -- and the launcher's heap is 352 K before lwip and
    191 K after. It does not fit, so it had to be reimplemented.

    Which makes divergence the whole risk: a hash over almost-the-right bytes is a
    well-formed MD5 that the server does not recognise, and on hardware that is
    indistinguishable from a game with no achievement set. Nothing about a play session
    could tell those apart.

    So the real rc_hash_nintendo_ds() is compiled here -- only here, never into the
    launcher -- and the two are required to agree on real .nds files. rcheevos stays the
    definition; ours is an implementation of it that cannot drift in silence.
    ------------------------------------------------------------------------------------
*/
#include "rc_hash.h"
#include "rhash/rc_hash_internal.h"

static void* rcHashOpen(const char* path)                        { return fopen(path, "rb"); }
static void  rcHashSeek(void* h, int64_t o, int w)                { fseek((FILE*)h, (long)o, w); }
static int64_t rcHashTell(void* h)                                { return ftell((FILE*)h); }
static size_t rcHashRead(void* h, void* b, size_t n)              { return fread(b, 1, n, (FILE*)h); }
static void  rcHashClose(void* h)                                 { fclose((FILE*)h); }

/* rcheevos' answer for the same file, through its own allocating implementation. */
static int rc_reference_hash(const char* path, char out[33]) {
	rc_hash_iterator_t it;

	memset(&it, 0, sizeof(it));
	it.path = path;
	it.callbacks.filereader.open  = rcHashOpen;
	it.callbacks.filereader.seek  = rcHashSeek;
	it.callbacks.filereader.tell  = rcHashTell;
	it.callbacks.filereader.read  = rcHashRead;
	it.callbacks.filereader.close = rcHashClose;
	return rc_hash_nintendo_ds(out, &it);
}

static int failures;

#define CHECK(cond) do { \
	if (cond) { \
		printf("  ok    %s\n", #cond); \
	} else { \
		printf("  FAIL  %s\n", #cond); \
		failures++; \
	} \
} while (0)

/*
    Real .nds files, because a synthetic one would only prove the two implementations agree
    about a header this test wrote. These are whatever the build produced -- nds-bootstrap
    itself and the WiFi probe -- so the ARM9 and ARM7 sizes, the icon block and the offsets
    are all a linker's rather than a fixture's.

    Skipped, not failed, when they are absent: the host test is meant to run before any
    devkitARM build, and it says so rather than demanding one.
*/
static void test_rom_hash(void) {
	static const char* const roms[] = {
		"retail/bin/nds-bootstrap-nightly.nds",
		"retail/bin/nds-bootstrap.nds",
		"tools/wifiprobe/wifiprobe.nds",
		NULL,
	};
	int tested = 0;
	int i;

	printf("\nthe popup is opt-in per game, and matching is whole-number\n");
	{
		const char* list = "16710,3405";

		/* The ladder's ends. */
		CHECK(raOverlayAllowed(RA_OVERLAY_OFF,    list, 16710) == 0);
		CHECK(raOverlayAllowed(RA_OVERLAY_ALWAYS, list, 16710) == 1);
		/* ...and ALWAYS does not consult the list at all. */
		CHECK(raOverlayAllowed(RA_OVERLAY_ALWAYS, "",   99999) == 1);
		CHECK(raOverlayAllowed(RA_OVERLAY_ALWAYS, 0,    0)     == 1);

		/* The default rung: listed draws, unlisted does not. */
		CHECK(raOverlayAllowed(RA_OVERLAY_LISTED, list, 16710) == 1);
		CHECK(raOverlayAllowed(RA_OVERLAY_LISTED, list, 3405)  == 1);
		CHECK(raOverlayAllowed(RA_OVERLAY_LISTED, list, 9999)  == 0);

		/*
		    Whole-number, which is the reason this is a parser and not strstr(): a prefix or a suffix
		    of a listed id must not enable a different game.
		*/
		CHECK(raOverlayAllowed(RA_OVERLAY_LISTED, list, 1671)   == 0);
		CHECK(raOverlayAllowed(RA_OVERLAY_LISTED, list, 6710)   == 0);
		CHECK(raOverlayAllowed(RA_OVERLAY_LISTED, list, 167100) == 0);

		/* Any non-digit separates, so spacing cannot silently drop an entry. */
		CHECK(raOverlayAllowed(RA_OVERLAY_LISTED, "16710 3405",  3405) == 1);
		CHECK(raOverlayAllowed(RA_OVERLAY_LISTED, "16710, 3405", 3405) == 1);
		CHECK(raOverlayAllowed(RA_OVERLAY_LISTED, "  16710  ",  16710) == 1);
		/* The last entry counts even with no trailing separator. */
		CHECK(raOverlayAllowed(RA_OVERLAY_LISTED, "1,2,3405",   3405) == 1);

		/* An empty or absent list draws on nothing. */
		CHECK(raOverlayAllowed(RA_OVERLAY_LISTED, "",  16710) == 0);
		CHECK(raOverlayAllowed(RA_OVERLAY_LISTED, 0,   16710) == 0);
		/* A ROM the server does not know has no id to be listed under. */
		CHECK(raOverlayAllowed(RA_OVERLAY_LISTED, list, 0) == 0);
		CHECK(raOverlayAllowed(RA_OVERLAY_LISTED, "0",  0) == 0);

		/* A number too long for a u32 saturates rather than wrapping onto a real id. */
		CHECK(raOverlayAllowed(RA_OVERLAY_LISTED, "99999999999999", 16710) == 0);
	}

	printf("\nthe ROM hash agrees with rcheevos' own, on real .nds files\n");

	for (i = 0; roms[i]; i++) {
		char       ours[33], theirs[33];
		raHashInfo info;
		FILE*      f = fopen(roms[i], "rb");

		if (!f) {
			continue;
		}
		fclose(f);
		tested++;

		CHECK(raHashRom(roms[i], ours, &info) == true);
		CHECK(rc_reference_hash(roms[i], theirs) != 0);
		if (strcmp(ours, theirs) == 0) {
			printf("  ok    %s\n        %s\n", roms[i], ours);
		} else {
			printf("  FAIL  %s\n        ours   %s\n        rcheevos %s\n",
			       roms[i], ours, theirs);
			failures++;
		}

		/*
		    And the number that forced the streaming implementation, printed rather than
		    asserted -- it is a property of whatever ROM happened to be here, not of the
		    code. The launcher has ~191 K of heap once lwip is up, so anything approaching
		    that is the point being made.
		*/
		printf("        rcheevos would have malloc'd %lu bytes (arm9 %lu, arm7 %lu)\n",
		       (unsigned long)info.bufferBytes,
		       (unsigned long)info.arm9Size, (unsigned long)info.arm7Size);
	}

	if (!tested) {
		printf("  no .nds built yet -- skipped (run make first to cover this)\n");
	}

	/*
	    A file that is not a ROM has to fail rather than return a confident hash of whatever
	    was there. This test file is the nearest thing to hand.
	*/
	{
		char       ours[33];
		raHashInfo info;

		CHECK(raHashRom("tools/ra_reader_test.c", ours, &info) == false
		   || strlen(ours) == 32);
		CHECK(raHashRom("does/not/exist.nds", ours, &info) == false);
		CHECK(ours[0] == 0);
	}
}

/*
    The query encoder, and it is the one function in 3c that can lie convincingly.

    A password is user text. An `&` ends the parameter early, so the server sees a shorter
    password; a `+` decodes as a space; a `%` opens an escape that is not there. Every one of
    those builds a well-formed request that comes back `invalid_credentials`, which from the
    console is indistinguishable from the password simply being wrong -- so the user would be
    told their password is wrong when it is not.
*/
static void test_url_encode(void) {
	char buf[64];

	printf("\nthe query encoder survives a password with punctuation in it\n");

	CHECK(raNetUrlEncode("plain", buf, sizeof(buf)) && strcmp(buf, "plain") == 0);
	/* Unreserved per RFC 3986 pass through untouched. */
	CHECK(raNetUrlEncode("aZ09-_.~", buf, sizeof(buf)) && strcmp(buf, "aZ09-_.~") == 0);
	/* The three that would silently corrupt a login. */
	CHECK(raNetUrlEncode("a&b", buf, sizeof(buf)) && strcmp(buf, "a%26b") == 0);
	CHECK(raNetUrlEncode("a+b", buf, sizeof(buf)) && strcmp(buf, "a%2Bb") == 0);
	CHECK(raNetUrlEncode("100%", buf, sizeof(buf)) && strcmp(buf, "100%25") == 0);
	CHECK(raNetUrlEncode("a b", buf, sizeof(buf)) && strcmp(buf, "a%20b") == 0);
	CHECK(raNetUrlEncode("=?#/", buf, sizeof(buf)) && strcmp(buf, "%3D%3F%23%2F") == 0);
	/* High bytes, in case someone's password is not ASCII. */
	CHECK(raNetUrlEncode("\xc3\xb1", buf, sizeof(buf)) && strcmp(buf, "%C3%B1") == 0);
	CHECK(raNetUrlEncode("", buf, sizeof(buf)) && buf[0] == 0);

	/*
	    Truncation has to be reported, not swallowed: a silently shortened password is the same
	    bug as a badly escaped one.
	*/
	{
		char tiny[4];

		CHECK(raNetUrlEncode("abc", tiny, sizeof(tiny)) && strcmp(tiny, "abc") == 0);
		CHECK(raNetUrlEncode("abcd", tiny, sizeof(tiny)) == false);
		/* One %XX needs four bytes of room including the terminator. */
		CHECK(raNetUrlEncode("&", tiny, sizeof(tiny)) && strcmp(tiny, "%26") == 0);
		CHECK(raNetUrlEncode("&&", tiny, sizeof(tiny)) == false);
	}
}

/*
    The config file, fed odelot's own example verbatim -- because "his file works here" is the
    actual requirement, and a fixture I invent cannot check it.
*/
static void test_config(void) {
	const char* path = "/tmp/ra_cfg_test.cfg";
	raConfig    cfg;
	FILE*       f = fopen(path, "w");

	printf("\nthe config file reads odelot's format\n");
	if (!f) {
		printf("  cannot write %s -- skipped\n", path);
		return;
	}
	fputs("# RetroAchievements configuration file\n"
	      "\n"
	      "# RetroAchievements credentials\n"
	      "username=odelot\n"
	      "password=hunter2&co\n"
	      "\n"
	      "show_challenge_show_popup=1\n"
	      "show_progress_popups=1\n"
	      "debug=0\n"
	      "hardcore=1\n"
	      "force_hardcore=0\n"
	      "list_hotkey=0\n"
	      "   spaced   =   yes   \n"
	      "a line with no equals sign\n",
	      f);
	fclose(f);

	CHECK(raConfigRead(path, &cfg) == true);
	CHECK(cfg.found == 1);
	CHECK(strcmp(cfg.username, "odelot") == 0);
	/* Kept verbatim, punctuation and all -- the encoder is what makes it safe to send. */
	CHECK(strcmp(cfg.password, "hunter2&co") == 0);
	CHECK(cfg.usable == 1);
	CHECK(cfg.hardcore == 1);
	CHECK(cfg.debug == 0);
	/* His popup and leaderboard keys are recognised and not acted on, not rejected. */
	CHECK(cfg.notYet == 4);
	/* `spaced` is nobody's key: a typo has to be visible. */
	CHECK(cfg.unknown == 1);
	CHECK(cfg.badLines == 1);
	/*
	    A file that has never heard of verbose_log gets the quiet screen -- the one default in the
	    parser that is not "behave as before", and worth pinning because it is a visible change to
	    every existing card.
	*/
	CHECK(cfg.verboseLog == 0);
	/*
	    ...and the two switches that gate the only parts of this fork that touch a running game: the
	    popup, which negotiates for its hardware, and the queue write, which uses the card the game is
	    streaming from. Both default on, so a card that has never heard of either key behaves as it did.
	*/
	CHECK(cfg.overlay == 1);
	CHECK(cfg.queue == 1);

	printf("\nverbose_log is off unless the file asks for it\n");
	{
		raConfig vcfg;
		FILE*    vf = fopen(path, "w");

		fputs("username=u\npassword=p\nverbose_log=1\n", vf);
		fclose(vf);
		CHECK(raConfigRead(path, &vcfg) == true);
		CHECK(vcfg.verboseLog == 1);
		/* Recognised, so it must not be counted as somebody's typo. */
		CHECK(vcfg.unknown == 0);
		CHECK(vcfg.notYet == 0);

		vf = fopen(path, "w");
		fputs("username=u\npassword=p\nverbose_log=0\n", vf);
		fclose(vf);
		CHECK(raConfigRead(path, &vcfg) == true);
		CHECK(vcfg.verboseLog == 0);
		/* And it changes nothing else: sync and submit keep their own defaults. */
		CHECK(vcfg.sync == 1);
		CHECK(vcfg.submit == 1);

		/* overlay and queue are independent of each other and of everything above. */
		vf = fopen(path, "w");
		fputs("username=u\npassword=p\noverlay=0\n", vf);
		fclose(vf);
		CHECK(raConfigRead(path, &vcfg) == true);
		CHECK(vcfg.overlay == 0 && vcfg.queue == 1);
		CHECK(vcfg.unknown == 0);

		vf = fopen(path, "w");
		fputs("username=u\npassword=p\nqueue=0\n", vf);
		fclose(vf);
		CHECK(raConfigRead(path, &vcfg) == true);
		CHECK(vcfg.queue == 0 && vcfg.overlay == 1);
		CHECK(vcfg.unknown == 0);
		CHECK(vcfg.sync == 1 && vcfg.submit == 1);
	}

	printf("\nthe body's end is read from the headers, not waited for\n");
	{
		/*
		    This is where fifty of the sixty seconds were. Reading until the peer closes means
		    waiting for the peer to close, and the timing line put every one-kilobyte request at
		    10.2 seconds against a chip bring-up and a DHCP lease that together cost 5.

		    Driven on the host because every part of it is a string problem: the header is
		    case-insensitive by the standard, Cloudflare does not send it the way anyone expects, and
		    a `Content-Length` appearing in the *body* must never be found.
		*/
		const char* r1 = "HTTP/1.1 200 OK\r\nContent-Length: 1087\r\n\r\n{\"Success\":true}";
		const char* r2 = "HTTP/1.1 200 OK\r\ncontent-length:42\r\n\r\nbody";
		const char* r3 = "HTTP/1.1 200 OK\r\nCONTENT-LENGTH:   7\r\n\r\nbody";
		const char* r4 = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nbody\r\n0\r\n\r\n";
		const char* r5 = "HTTP/1.1 200 OK\r\nServer: cloudflare\r\n\r\nbody";

		CHECK(raNetHeaderLength(r1, (u32)(strstr(r1, "\r\n\r\n") - r1 + 4)) == 1087);
		CHECK(raNetHeaderLength(r2, (u32)(strstr(r2, "\r\n\r\n") - r2 + 4)) == 42);
		CHECK(raNetHeaderLength(r3, (u32)(strstr(r3, "\r\n\r\n") - r3 + 4)) == 7);
		/* A reply that does not say has no length, and falls back to ending at the close. */
		CHECK(raNetHeaderLength(r5, (u32)(strstr(r5, "\r\n\r\n") - r5 + 4)) == 0);

		CHECK(raNetHeaderChunked(r4, (u32)(strstr(r4, "\r\n\r\n") - r4 + 4)) == 1);
		CHECK(raNetHeaderChunked(r1, (u32)(strstr(r1, "\r\n\r\n") - r1 + 4)) == 0);
		/* `identity` is a Transfer-Encoding and is not chunked. */
		{
			const char* r6 = "HTTP/1.1 200 OK\r\nTransfer-Encoding: identity\r\n\r\nbody";

			CHECK(raNetHeaderChunked(r6, (u32)(strstr(r6, "\r\n\r\n") - r6 + 4)) == 0);
		}
		/*
		    And the one that would be a real bug: a body mentioning the header must not be mistaken
		    for the header. The bound is what stops it, and a reply whose body is a JSON object full
		    of achievement descriptions is exactly the kind of body that could contain anything.
		*/
		{
			const char* r7 = "HTTP/1.1 200 OK\r\n\r\n{\"d\":\"Content-Length: 99\"}";

			CHECK(raNetHeaderLength(r7, (u32)(strstr(r7, "\r\n\r\n") - r7 + 4)) == 0);
		}
		/* A header name that only starts a line counts; one in the middle of a line does not. */
		{
			const char* r8 = "HTTP/1.1 200 OK\r\nX: content-length: 5\r\n\r\nbody";

			CHECK(raNetHeaderLength(r8, (u32)(strstr(r8, "\r\n\r\n") - r8 + 4)) == 0);
		}
	}

	printf("\nan earned date packs into a word and comes back out\n");
	{
		/*
		    Two sources feed this -- a server timestamp converted to local time, and the queue file's
		    own stamp -- and the menu unpacks it with shifts because it has no calendar library. A
		    packing that lost a field would show a plausible wrong date, which is the failure mode
		    this project has now been bitten by twice on this very page.
		*/
		const u32 w = raWhenPack(2026, 8, 22, 14, 7);

		CHECK(w != 0);
		CHECK(RA_WHEN_YEAR(w) == 2026);
		CHECK(RA_WHEN_MONTH(w) == 8);
		CHECK(RA_WHEN_DAY(w) == 22);
		CHECK(RA_WHEN_HOUR(w) == 14);
		CHECK(RA_WHEN_MINUTE(w) == 7);
		/* Both ends of every field, since each one is a different width. */
		{
			const u32 lo = raWhenPack(2000, 1, 1, 0, 0);
			const u32 hi = raWhenPack(2127, 12, 31, 23, 59);

			CHECK(lo != 0 && RA_WHEN_YEAR(lo) == 2000 && RA_WHEN_MONTH(lo) == 1
			      && RA_WHEN_DAY(lo) == 1 && RA_WHEN_HOUR(lo) == 0 && RA_WHEN_MINUTE(lo) == 0);
			CHECK(hi != 0 && RA_WHEN_YEAR(hi) == 2127 && RA_WHEN_MONTH(hi) == 12
			      && RA_WHEN_DAY(hi) == 31 && RA_WHEN_HOUR(hi) == 23 && RA_WHEN_MINUTE(hi) == 59);
		}
		/* Out of range is 0, not a wrapped date: the menu prints nothing for 0 and that is correct. */
		CHECK(raWhenPack(1999, 1, 1, 0, 0) == 0);
		CHECK(raWhenPack(2128, 1, 1, 0, 0) == 0);
		CHECK(raWhenPack(2026, 0, 1, 0, 0) == 0);
		CHECK(raWhenPack(2026, 13, 1, 0, 0) == 0);
		CHECK(raWhenPack(2026, 1, 0, 0, 0) == 0);
		CHECK(raWhenPack(2026, 1, 32, 0, 0) == 0);
		CHECK(raWhenPack(2026, 1, 1, 24, 0) == 0);
		CHECK(raWhenPack(2026, 1, 1, 0, 60) == 0);
		/*
		    Zero has to be unreachable from a real date, because that is what "not known" is. Month
		    and day are 1-based, so the month field can never be zero on a packed value.
		*/
		CHECK(raWhenPack(2000, 1, 1, 0, 0) != 0);

		/* ...and from the queue file's own stamp, which is where a locally earned unlock's time is. */
		CHECK(raWhenFromStamp("20260822140755") == raWhenPack(2026, 8, 22, 14, 7));
		/* Seconds are read and dropped -- the field has no room and a minute is the useful unit. */
		CHECK(raWhenFromStamp("20260822140700") == raWhenFromStamp("20260822140759"));
		/* Anything that is not fourteen digits is refused rather than half-read. */
		CHECK(raWhenFromStamp("2026082214075") == 0);
		CHECK(raWhenFromStamp("2026-08-22 14:07") == 0);
		CHECK(raWhenFromStamp("") == 0);
		CHECK(raWhenFromStamp(NULL) == 0);
		/* A stamp that parses but is not a date is still refused, by raWhenPack's ranges. */
		CHECK(raWhenFromStamp("20269922140700") == 0);
	}

	printf("\nthe progress bar agrees with its own percentage\n");
	{
		char bar[RA_BAR_MIN];

		/*
		    A full bar beside "95%" is the kind of thing that gets reported as a bug, so the cells and
		    the number come from the same rounded division and both ends are pinned.
		*/
		raWifiBar(bar, sizeof(bar), 0, RA_STEP_MAX);
		CHECK(strcmp(bar, "[----------------------]   0%") == 0);
		raWifiBar(bar, sizeof(bar), RA_STEP_MAX, RA_STEP_MAX);
		CHECK(strcmp(bar, "[######################] 100%") == 0);
		raWifiBar(bar, sizeof(bar), 5, 10);
		CHECK(strcmp(bar, "[###########-----------]  50%") == 0);
		/* Rounded, not truncated: 3 of 10 is 6.6 cells and reads as 7. */
		raWifiBar(bar, sizeof(bar), 3, 10);
		CHECK(strcmp(bar, "[#######---------------]  30%") == 0);
		/* It fits the console: 32 columns, and the bar must not be the thing that wraps. */
		CHECK(strlen(bar) <= 32);

		/* A step past the end is clamped rather than overrunning the buffer. */
		raWifiBar(bar, sizeof(bar), 200, RA_STEP_MAX);
		CHECK(strcmp(bar, "[######################] 100%") == 0);
		/* ...and zero steps is one step, because a bar with no denominator still has to draw. */
		raWifiBar(bar, sizeof(bar), 1, 0);
		CHECK(strcmp(bar, "[######################] 100%") == 0);

		/*
		    The spinner is one cell that pulses. A wrong mask here shows a space, and a spinner that
		    stops is exactly what it exists to rule out -- a stalled run looking identical to a
		    waiting one.
		*/
		{
			u8 t;

			CHECK(raWifiSpinFrame(0) == '.');
			CHECK(raWifiSpinFrame(1) == 'o');
			CHECK(raWifiSpinFrame(2) == 'O');
			/* Symmetric: it returns through 'o' rather than snapping back, so it breathes. */
			CHECK(raWifiSpinFrame(3) == 'o');
			CHECK(raWifiSpinFrame(4) == '.');
			/* Every tick in a byte prints something, including the wrap. */
			for (t = 0; t < 255; t++) {
				if (raWifiSpinFrame(t) <= ' ') {
					break;
				}
			}
			CHECK(t == 255);
			CHECK(raWifiSpinFrame(255) > ' ');
		}

		/*
		    Centring, and it is the fix for the layout coming apart rather than a nicety.

		    Writing the console's final cell advances the cursor past the end, the console wraps to
		    the next row, and every absolute row this code has addressed is then one out. So nothing
		    may be placed where it could reach that column -- which is what the `cells + 1 >= width`
		    guard is, and why the divisor is `width - 1`.
		*/
		CHECK(raWifiCentre(32, 24) == 3);      /* the bar: 24 cells of 32 */
		CHECK(raWifiCentre(32, 1) == 15);      /* the pulse: one cell, its own row */
		CHECK(raWifiCentre(32, 4) == 13);      /* " 45%" */
		CHECK(raWifiCentre(32, 0) == 15);
		/* A string that would reach the last column starts at 0 rather than being pushed off it. */
		CHECK(raWifiCentre(32, 31) == 0);
		CHECK(raWifiCentre(32, 32) == 0);
		CHECK(raWifiCentre(32, 99) == 0);
		/* And whatever it returns, the string still ends before the final column. */
		{
			u32 w, c;

			for (w = 8; w <= 64; w++) {
				for (c = 0; c <= w; c++) {
					const u32 at = raWifiCentre(w, c);

					if (at != 0 && at + c > w - 1) {
						break;
					}
				}
				if (c <= w) {
					break;
				}
			}
			CHECK(w == 65);
		}
		/* A zero-width console asks for nothing sensible and must not divide by it. */
		CHECK(raWifiCentre(0, 4) == 0);

		/* Too small a buffer terminates rather than writing what it cannot fit. */
		{
			char small[8];

			memset(small, 0x5A, sizeof(small));
			raWifiBar(small, sizeof(small), 5, 10);
			CHECK(small[0] == 0);
		}
	}

	/*
	    And the secret must never be printable. The log is a file that gets sent to someone.
	*/
	printf("\nsecrets are not printable\n");
	CHECK(strstr(raConfigRedact(cfg.password), "hunter2") == NULL);
	CHECK(strstr(raConfigRedact(cfg.password), "10") != NULL);   /* its length, though */
	CHECK(strcmp(raConfigRedact(""), "(empty)") == 0);
	CHECK(strcmp(raConfigRedact(NULL), "(empty)") == 0);

	printf("\nthe login reply gives up its token and nothing else\n");
	{
		const char* reply =
			"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n"
			"{\"Success\":true,\"User\":\"odelot\",\"Token\":\"abcDEF123\",\"Score\":42}";
		char token[40];
		char user[40];

		CHECK(strstr(raNetBody(reply), "{\"Success\"") == raNetBody(reply));
		CHECK(raNetJsonString(reply, "Token", token, sizeof(token))
		   && strcmp(token, "abcDEF123") == 0);
		CHECK(raNetJsonString(reply, "User", user, sizeof(user))
		   && strcmp(user, "odelot") == 0);
		/* Absent, and a non-string field, must both fail rather than return rubbish. */
		CHECK(raNetJsonString(reply, "Nope", token, sizeof(token)) == false);
		CHECK(raNetJsonString(reply, "Score", token, sizeof(token)) == false);
		/* A token longer than the buffer is a failure, not a truncation. */
		{
			char tiny[4];
			CHECK(raNetJsonString(reply, "Token", tiny, sizeof(tiny)) == false);
		}
	}

	/*
	    r=gameid's reply is a bare number, and the number 0 is an *answer*: it is what the API
	    returns for a hash it has never seen. Reading it as "no field" would turn "your dump is
	    not the supported one" into "something went wrong", which are different problems with
	    different next moves.
	*/
	printf("\nthe unlocks reply gives up a list, and empty is not the same as absent\n");
	{
		u32 ids[8];
		int n;

		n = raNetJsonIdList("{\"Success\":true,\"UserUnlocks\":[93119,93120,94160],\"GameID\":14856}",
		                    "UserUnlocks", ids, 8);
		CHECK(n == 3);
		CHECK(ids[0] == 93119 && ids[1] == 93120 && ids[2] == 94160);

		/*
		    The distinction the whole function exists for. A player who has earned nothing and a
		    request that failed both produce no ids -- and they mean different things: the first is an
		    answer, the second is "stage everything because we do not know". Conflating them would
		    make a failed request look like a fresh account.
		*/
		CHECK(raNetJsonIdList("{\"Success\":true,\"UserUnlocks\":[]}", "UserUnlocks", ids, 8) == 0);
		CHECK(raNetJsonIdList("{\"Success\":false}", "UserUnlocks", ids, 8) == -1);

		/* Whitespace between elements is legal JSON even if this API sends none. */
		n = raNetJsonIdList("{\"UserUnlocks\":[ 1, 22 ,333 ]}", "UserUnlocks", ids, 8);
		CHECK(n == 3 && ids[0] == 1 && ids[1] == 22 && ids[2] == 333);

		/*
		    More than fits is truncation, reported by returning the capacity -- and it is safe: a
		    short skip list only means a few already-earned achievements get staged again.
		*/
		n = raNetJsonIdList("{\"UserUnlocks\":[1,2,3,4,5,6,7,8,9,10]}", "UserUnlocks", ids, 4);
		CHECK(n == 4);
		CHECK(ids[0] == 1 && ids[3] == 4);

		/* A value past a u32 is skipped rather than wrapped, like every other number here. */
		n = raNetJsonIdList("{\"UserUnlocks\":[99999999999,7]}", "UserUnlocks", ids, 8);
		CHECK(n == 1 && ids[0] == 7);
	}

	printf("\nand r=startsession's arrays of objects need a different reader\n");
	{
		/*
		    The shape r=startsession answers with, which is not the shape r=unlocks answers with. The
		    flat reader stops at the `{` and reports an empty list -- and an empty list is a meaningful
		    answer from that endpoint, so the two must not share a reader.
		*/
		static const char SESSION[] =
			"{\"Success\":true,"
			"\"Unlocks\":[{\"ID\":93119,\"When\":1786243173},{\"ID\":93120,\"When\":1786243180}],"
			"\"HardcoreUnlocks\":[{\"ID\":91467,\"When\":1700000000}],"
			"\"ServerNow\":1786243200}";
		u32 ids[8];
		int n;

		n = raNetJsonObjectField(SESSION, "Unlocks", "ID", ids, 8);
		CHECK(n == 2 && ids[0] == 93119 && ids[1] == 93120);

		/*
		    And the two arrays are read separately. `Unlocks` is a prefix of nothing here, but
		    `HardcoreUnlocks` *contains* `Unlocks` as a substring -- the needle is `"Unlocks":[` with
		    the quote, which is what keeps the first lookup from landing inside the second.
		*/
		n = raNetJsonObjectField(SESSION, "HardcoreUnlocks", "ID", ids, 8);
		CHECK(n == 1 && ids[0] == 91467);

		/* Same contract as the flat reader: 0 is an empty array, -1 is no key at all. */
		CHECK(raNetJsonObjectField("{\"Unlocks\":[]}", "Unlocks", "ID", ids, 8) == 0);
		CHECK(raNetJsonObjectField("{\"Success\":true}", "Unlocks", "ID", ids, 8) == -1);

		/* `When` is reachable by the same call, which is what makes the field a parameter. */
		n = raNetJsonObjectField(SESSION, "HardcoreUnlocks", "When", ids, 8);
		CHECK(n == 1 && ids[0] == 1700000000u);

		/*
		    Depth matters. A nested object's `ID` belongs to something else, and reading it would mix
		    two levels of the reply into one list.
		*/
		n = raNetJsonObjectField("{\"Unlocks\":[{\"ID\":5,\"Extra\":{\"ID\":999}},{\"ID\":6}]}",
		                         "Unlocks", "ID", ids, 8);
		CHECK(n == 2 && ids[0] == 5 && ids[1] == 6);

		/* It stops at its own array, not at whatever comes after it. */
		n = raNetJsonObjectField("{\"Unlocks\":[{\"ID\":5}],\"Other\":[{\"ID\":77}]}",
		                         "Unlocks", "ID", ids, 8);
		CHECK(n == 1 && ids[0] == 5);

		/* Truncation reported as the capacity, and overflow refused, like the flat reader. */
		n = raNetJsonObjectField("{\"U\":[{\"ID\":1},{\"ID\":2},{\"ID\":3}]}", "U", "ID", ids, 2);
		CHECK(n == 2);
		n = raNetJsonObjectField("{\"U\":[{\"ID\":99999999999},{\"ID\":7}]}", "U", "ID", ids, 8);
		CHECK(n == 1 && ids[0] == 7);
	}

	printf("\nsubmit defaults to on and only submit=0 turns it off\n");
	{
		const char* sp = "/tmp/ra_cfg_submit.cfg";
		raConfig    scfg;
		FILE*       sf;

		/* Absent: sending stays on, so a card that never heard of the key behaves as it did. */
		sf = fopen(sp, "w");
		if (sf) {
			fputs("username=Bakke\npassword=x\n", sf);
			fclose(sf);
			CHECK(raConfigRead(sp, &scfg) == true);
			CHECK(scfg.submit == 1);

			/* Present and off -- the whole point: an unlock is spent the moment it lands. */
			sf = fopen(sp, "w");
			fputs("username=Bakke\npassword=x\nsubmit=0\n", sf);
			fclose(sf);
			CHECK(raConfigRead(sp, &scfg) == true);
			CHECK(scfg.submit == 0);

			/* And back on explicitly. */
			sf = fopen(sp, "w");
			fputs("username=Bakke\npassword=x\nsubmit=1\n", sf);
			fclose(sf);
			CHECK(raConfigRead(sp, &scfg) == true);
			CHECK(scfg.submit == 1);

			/*
			    sync is the same shape and the same risk: a default that flipped would start bringing
			    the radio up on cards that asked for it to stay off.
			*/
			sf = fopen(sp, "w");
			fputs("username=Bakke\npassword=x\n", sf);
			fclose(sf);
			CHECK(raConfigRead(sp, &scfg) == true);
			CHECK(scfg.sync == 1);

			sf = fopen(sp, "w");
			fputs("username=Bakke\npassword=x\nsync=0\n", sf);
			fclose(sf);
			CHECK(raConfigRead(sp, &scfg) == true);
			CHECK(scfg.sync == 0);
			/* And the two switches are independent -- one is not the other spelled differently. */
			CHECK(scfg.submit == 1);
			remove(sp);
		} else {
			printf("  cannot write %s -- skipped\n", sp);
		}
	}

	printf("\nthe gameid reply gives up a number, and zero is a number\n");
	{
		u32 id = 12345;

		CHECK(raNetJsonNumber("{\"Success\":true,\"GameID\":1448}", "GameID", &id)
		   && id == 1448);
		CHECK(raNetJsonNumber("{\"Success\":true,\"GameID\":0}", "GameID", &id) && id == 0);
		CHECK(raNetJsonNumber("{\"Success\":true, \"GameID\" : 7 }", "GameID", &id) == false);
		/* whitespace after the colon is legal; before it the needle simply will not match */
		CHECK(raNetJsonNumber("{\"GameID\":  7}", "GameID", &id) && id == 7);

		/* A quoted value is not a number. Reading its digits would name the wrong game. */
		id = 999;
		CHECK(raNetJsonNumber("{\"GameID\":\"1448\"}", "GameID", &id) == false);
		CHECK(id == 0);

		/* Absent, and a value that would overflow, both have to fail rather than wrap. */
		CHECK(raNetJsonNumber("{\"Success\":false}", "GameID", &id) == false);
		CHECK(raNetJsonNumber("{\"GameID\":99999999999999}", "GameID", &id) == false);
	}

	remove(path);
}

/*
    ------------------------------------------------------------------------------------
    Step 3d. Two state machines that are fed a socket's byte stream, which means the one thing
    they must survive is arriving in pieces -- and pieces of a size nothing here chooses.

    That is why the tests below do not check one split: they check *every* split. Feeding a
    fixture as two pieces at each of its byte boundaries, and again one byte at a time, and
    requiring the result to be identical every time, is the only form of this test that is worth
    running. A boundary falling between the `\r` and the `\n` of a chunk header, or in the
    middle of `"MemAddr":"`, or between a backslash and the slash it escapes, are all just
    ordinary splits to a network and all silently produce a wrong answer to code that assumes
    otherwise.
    ------------------------------------------------------------------------------------
*/

/* Where the streaming reader's sink puts what it is given, so a test can look at it. */
static char sunk[8192];
static u32  sunkLength;

static void sinkAppend(void* ctx, const char* data, int length) {
	(void)ctx;
	if (sunkLength + (u32)length < sizeof(sunk)) {
		memcpy(sunk + sunkLength, data, (size_t)length);
		sunkLength += (u32)length;
	}
	sunk[sunkLength] = 0;
}

/*
    Feed one reply at every possible split and require the body to come out the same each time.
    Returns 0 when every split agreed, or the split point that did not.
*/
static int streamEverySplit(const char* reply, const char* expectBody, u16 expectStatus) {
	const int    total = (int)strlen(reply);
	raNetStream  s;
	int          split;

	for (split = 0; split <= total; split++) {
		raNetStreamReset(&s, sinkAppend, NULL);
		sunkLength = 0;
		sunk[0]    = 0;

		raNetStreamFeed(&s, reply, split);
		raNetStreamFeed(&s, reply + split, total - split);

		if (strcmp(sunk, expectBody) != 0 || s.status != expectStatus
		 || s.bodyBytes != strlen(expectBody)) {
			return split + 1;
		}
	}

	/* And once byte by byte, which is the same test taken to its limit. */
	raNetStreamReset(&s, sinkAppend, NULL);
	sunkLength = 0;
	sunk[0]    = 0;
	for (split = 0; split < total; split++) {
		raNetStreamFeed(&s, reply + split, 1);
	}
	if (strcmp(sunk, expectBody) != 0 || s.status != expectStatus) {
		return -1;
	}
	return 0;
}

static void test_stream(void) {
	printf("\nthe streaming reader strips headers and chunk framing at any split\n");

	/*
	    Identity encoding, which is what every reply this project has read so far came back as.
	    Proven here rather than assumed, because the two paths have to agree.
	*/
	CHECK(streamEverySplit("HTTP/1.1 200 OK\r\n"
	                       "Content-Type: application/json\r\n"
	                       "\r\n"
	                       "{\"Success\":true}",
	                       "{\"Success\":true}", 200) == 0);

	/*
	    Chunked, which is the reason this file exists. A hex length written into the byte stream
	    would otherwise land inside a definition, and exactly one achievement out of a hundred
	    would be quietly wrong.
	*/
	CHECK(streamEverySplit("HTTP/1.1 200 OK\r\n"
	                       "Transfer-Encoding: chunked\r\n"
	                       "\r\n"
	                       "4\r\nabcd\r\n"
	                       "2\r\nef\r\n"
	                       "0\r\n\r\n",
	                       "abcdef", 200) == 0);

	/* A chunk longer than sixteen bytes, so the hex actually has to be hex. */
	CHECK(streamEverySplit("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
	                       "1a\r\nabcdefghijklmnopqrstuvwxyz\r\n0\r\n\r\n",
	                       "abcdefghijklmnopqrstuvwxyz", 200) == 0);

	/* Header names are case-insensitive and a server owes nobody a particular spelling. */
	CHECK(streamEverySplit("HTTP/1.1 200 OK\r\nTRANSFER-ENCODING: Chunked\r\n\r\n"
	                       "3\r\nxyz\r\n0\r\n\r\n",
	                       "xyz", 200) == 0);

	/* Chunk extensions are legal and nothing here wants them. */
	CHECK(streamEverySplit("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
	                       "3;name=value\r\nxyz\r\n0\r\n\r\n",
	                       "xyz", 200) == 0);

	/* A trailer after the last chunk is read and discarded, not handed on as body. */
	CHECK(streamEverySplit("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
	                       "3\r\nxyz\r\n0\r\nExpires: never\r\n\r\n",
	                       "xyz", 200) == 0);

	/*
	    A status other than 200 has to be *readable*, because that is how "the token expired"
	    stops looking like "the set is empty".
	*/
	CHECK(streamEverySplit("HTTP/1.1 404 Not Found\r\n\r\nno", "no", 404) == 0);
	CHECK(streamEverySplit("HTTP/1.1 401 Unauthorized\r\n"
	                       "Transfer-Encoding: chunked\r\n\r\n"
	                       "2\r\nno\r\n0\r\n\r\n",
	                       "no", 401) == 0);

	/* An empty body is a body. */
	CHECK(streamEverySplit("HTTP/1.1 204 No Content\r\n\r\n", "", 204) == 0);

	/* Headers alone, with the reply cut off before the blank line, must produce nothing. */
	{
		raNetStream s;

		raNetStreamReset(&s, sinkAppend, NULL);
		sunkLength = 0;
		sunk[0]    = 0;
		raNetStreamFeed(&s, "HTTP/1.1 200 OK\r\nContent-Type: text", 35);
		CHECK(sunkLength == 0 && s.status == 200);
	}
}

/*
    An r=patch reply, shaped like the real one: an object per achievement, MemAddr before Flags,
    forward slashes escaped, and unofficial achievements in the same array as published ones.

    The third achievement's title is the adversarial case and it is not hypothetical enough to
    leave out -- it contains the scanner's own needle, as a JSON string would have to write it.
    Because JSON escapes an interior quote as `\"`, the eleven bytes the scanner looks for cannot
    occur in a value, and this fixture is what says so rather than the argument in ra_patch.c.
*/
static const char patchReply[] =
	"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n"
	"{\"Success\":true,\"PatchData\":{\"ID\":14856,\"Title\":\"Super Mario 64 DS\","
	"\"ConsoleID\":18,\"Achievements\":["
	"{\"ID\":1,\"MemAddr\":\"0xH0a1b2c=1_d0xH0a1b2c=0\",\"Title\":\"First Star\","
	"\"Description\":\"Get it\",\"Points\":5,\"Author\":\"someone\",\"Flags\":3,\"Type\":null},"
	"{\"ID\":2,\"MemAddr\":\"A:0xX123456=1_I:0xH99\\/2\",\"Title\":\"Unofficial\","
	"\"Points\":0,\"Flags\":5},"
	"{\"ID\":3,\"MemAddr\":\"0xH000010>d0xH000010\","
	"\"Title\":\"They wrote \\\"MemAddr\\\":\\\" in the title\",\"Flags\":3}"
	"],\"Leaderboards\":[{\"ID\":9,\"Mem\":\"STA:0xH1=1::CAN:0=1\",\"Format\":\"SCORE\"}],"
	"\"RichPresencePatch\":\"Display:\\nStars: @Number(0xH1)\"}}";

/*
    The two published definitions, one per line, each prefixed with the achievement's own
    RetroAchievements id -- 1 and 3 in the fixture; 2 is the unofficial one and is not here -- and
    each followed by a tab and the achievement's title.

    Achievement 3's title is longer than RA_PATCH_TITLE_MAX and so arrives clipped. The expectation
    is *computed* from the constant rather than typed out, for the reason patchFeedAll() exists: a
    fixture measured by hand is a fixture that will eventually measure wrong, and a hand-clipped
    31-character string is exactly that measurement.
*/
#define PATCH_TITLE_1 "First Star"
#define PATCH_TITLE_3 "They wrote \"MemAddr\":\" in the title"
/*
    Achievement 1 carries a Description and Points and so gets the full four-field record;
    achievement 3 carries neither, so it stops after its title -- which is the same shape every
    build before the two fields wrote, and the reason the format degrades by truncation.
*/
#define PATCH_HEAD    "1:0xH0a1b2c=1_d0xH0a1b2c=0\t" PATCH_TITLE_1 "\t5\tGet it" \
                      "\n3:0xH000010>d0xH000010\t"

static char patchExpect[512];

static void patchExpectBuild(void) {
	/* RA_PATCH_TITLE_MAX counts the terminator, so one fewer is what a title can hold. */
	snprintf(patchExpect, sizeof(patchExpect), "%s%.*s\n",
	         PATCH_HEAD, (int)(RA_PATCH_TITLE_MAX - 1), PATCH_TITLE_3);
}

/*
    A whole fixture into the scanner in one call.

    A helper rather than a length at each call site, and that is not tidiness: the first version
    of these tests passed hand-counted byte counts, one of which was short by one. It cut a
    `"Flags":5` to `"Flags":` and turned a test about unofficial achievements into a test about
    missing flags -- which then failed for the right reason on the wrong grounds. A fixture that
    has to be measured by hand is a fixture that will eventually measure wrong.
*/
static void patchFeedAll(raPatch* p, const char* text) {
	raPatchFeed(p, text, (int)strlen(text));
}

/* One reply through the reader and the scanner together, split at one point. */
static void patchRun(raPatch* patch, char* block, u32 blockMax, const char* reply, int split) {
	const int   total = (int)strlen(reply);
	raNetStream s;

	raPatchReset(patch, block, blockMax);
	raNetStreamReset(&s, raPatchFeed, patch);
	raNetStreamFeed(&s, reply, split);
	raNetStreamFeed(&s, reply + split, total - split);
	raPatchFinish(patch);
}

static void test_patch(void) {
	/* Sized from the carry buffer, so the longest definition the scanner will hold still fits. */
	static char block[RA_PATCH_MEMADDR_MAX + 4096];
	raPatch     patch;

	printf("\nthe patch scanner pulls the published set out of a streaming reply\n");

	patchExpectBuild();
	patchRun(&patch, block, sizeof(block) - 1, patchReply, 0);
	CHECK(strcmp(block, patchExpect) == 0);
	CHECK(patch.kept == 2);
	CHECK(patch.unofficial == 1);
	CHECK(patch.dropped == 0 && patch.tooLong == 0 && patch.cutShort == 0);
	CHECK(patch.empty == 0 && patch.oddFlags == 0 && patch.noFlags == 0);
	CHECK(patch.used == strlen(patchExpect));
	CHECK(patch.wanted == strlen(patchExpect));
	/* Decoded length, so the escaped slash counts as the one character it becomes. */
	CHECK(patch.longest == strlen("0xH0a1b2c=1_d0xH0a1b2c=0"));
	/*
	    The Rich Presence script is counted and not kept. Decoded length, so the `\n` in the fixture
	    is the two characters this scanner would have had to store rather than the one byte it
	    represents -- the number has to be comparable with the title's and the description's, which
	    are decoded the same way.
	*/
	CHECK(patch.richSeen == 1);
	CHECK(patch.richBytes == strlen("Display:") + 2 + strlen("Stars: @Number(0xH1)"));
	/* And it disturbed nothing: the block is what it was before the fifth needle existed. */
	CHECK(strcmp(block, patchExpect) == 0);
	CHECK(patch.shortest == strlen("0xH000010>d0xH000010"));
	/*
	    The ids came off the reply, not out of a counter. Both published achievements carried one;
	    the count of id-less definitions is what would say a set cannot be reported on.
	*/
	CHECK(patch.withId == 2);
	CHECK(patch.withoutId == 0);
	/*
	    And both carried a title. Note where they sit in the fixture: `MemAddr` comes *before* `Title`
	    in each object here, and the one real reply this project has captured has them the other way
	    round. Both work, because a title is captured on sight and spent at commit time -- which is
	    the Flags field, after either order has gone past.
	*/
	CHECK(patch.withTitle == 2);
	CHECK(patch.titleCut == 1);        /* achievement 3's, which is 35 characters */
	CHECK(patch.titleNoRoom == 0);

	/*
	    Every split, because the boundary the network chooses is not ours -- and the interesting
	    ones are inside `"MemAddr":"`, between a backslash and the slash it escapes, and between
	    the digits of a Flags value.
	*/
	printf("\n...and does it identically at every split point\n");
	{
		const int total = (int)strlen(patchReply);
		int       bad   = 0;
		int       split;

		for (split = 0; split <= total; split++) {
			patchRun(&patch, block, sizeof(block) - 1, patchReply, split);
			if (strcmp(block, patchExpect) != 0 || patch.kept != 2
			 || patch.unofficial != 1 || patch.dropped || patch.tooLong
			 || patch.cutShort || patch.noFlags || patch.oddFlags) {
				bad = split + 1;
				break;
			}
		}
		CHECK(bad == 0);
	}

	/* And one byte at a time, which is the same requirement taken to its limit. */
	{
		raNetStream s;
		const int   total = (int)strlen(patchReply);
		int         i;

		raPatchReset(&patch, block, sizeof(block) - 1);
		raNetStreamReset(&s, raPatchFeed, &patch);
		for (i = 0; i < total; i++) {
			raNetStreamFeed(&s, patchReply + i, 1);
		}
		raPatchFinish(&patch);
		CHECK(strcmp(block, patchExpect) == 0 && patch.kept == 2 && patch.withTitle == 2);
	}

	printf("\nthe needle restarts correctly, and escapes decode\n");
	{
		/*
		    A doubled quote is the one case restart-and-retest has to get right, and the reason
		    ra_patch.c can use it instead of a failure function: the only character that repeats
		    in `"MemAddr":"` is the quote, at length one.
		*/
		raPatchReset(&patch, block, sizeof(block) - 1);
		patchFeedAll(&patch, "{\"\"MemAddr\":\"0xH1=1\",\"Flags\":3}");
		raPatchFinish(&patch);
		CHECK(patch.kept == 1 && strcmp(block, "0xH1=1\n") == 0);

		/* `\/` is the escape that matters: division is a memaddr operator. */
		raPatchReset(&patch, block, sizeof(block) - 1);
		patchFeedAll(&patch, "\"MemAddr\":\"0xH1\\/0xH2\\/2\",\"Flags\":3");
		raPatchFinish(&patch);
		CHECK(patch.kept == 1 && strcmp(block, "0xH1/0xH2/2\n") == 0);

		/* An unknown escape keeps its backslash rather than being invented away. */
		raPatchReset(&patch, block, sizeof(block) - 1);
		patchFeedAll(&patch, "\"MemAddr\":\"a\\tb\",\"Flags\":3");
		raPatchFinish(&patch);
		CHECK(patch.kept == 1 && strcmp(block, "a\\tb\n") == 0);

		/* A value ending the document, with no Flags field ever arriving, still lands. */
		raPatchReset(&patch, block, sizeof(block) - 1);
		patchFeedAll(&patch, "\"MemAddr\":\"0xH7=7\"}]}");
		raPatchFinish(&patch);
		CHECK(patch.kept == 1 && patch.noFlags == 1 && strcmp(block, "0xH7=7\n") == 0);

		/* Two definitions with no Flags between them: the first commits when the second opens. */
		raPatchReset(&patch, block, sizeof(block) - 1);
		patchFeedAll(&patch, "\"MemAddr\":\"a=1\",\"MemAddr\":\"b=2\"");
		raPatchFinish(&patch);
		CHECK(patch.kept == 2 && patch.noFlags == 2 && strcmp(block, "a=1\nb=2\n") == 0);

		/* An empty value is not a definition, and is counted rather than written. */
		raPatchReset(&patch, block, sizeof(block) - 1);
		patchFeedAll(&patch, "\"MemAddr\":\"\",\"Flags\":3");
		raPatchFinish(&patch);
		CHECK(patch.kept == 0 && patch.empty == 1 && block[0] == 0);

		/* A flag value nobody knows is kept and said out loud. */
		raPatchReset(&patch, block, sizeof(block) - 1);
		patchFeedAll(&patch, "\"MemAddr\":\"a=1\",\"Flags\":7");
		raPatchFinish(&patch);
		CHECK(patch.kept == 1 && patch.oddFlags == 1);

		/*
		    An unofficial definition is not written but is still measured. That matters for the
		    one number this rung is for: `longest` sizes the scanner's carry buffer, and a
		    buffer sized only by what got kept would be the wrong size for the next reply.
		*/
		raPatchReset(&patch, block, sizeof(block) - 1);
		patchFeedAll(&patch, "\"MemAddr\":\"0xH1=1_0xH2=2_0xH3=3\",\"Flags\":5");
		raPatchFinish(&patch);
		CHECK(patch.kept == 0 && patch.unofficial == 1 && block[0] == 0);
		CHECK(patch.longest == strlen("0xH1=1_0xH2=2_0xH3=3"));
	}

	printf("\nthe id needle takes achievement ids and nothing that merely ends in ID\n");
	{
		/*
		    "GameID" and "ConsoleID" both contain `ID":` and neither may match, because the needle
		    carries the opening quote. This is the check that keeps a set from being staged under the
		    game's number instead of each achievement's own.
		*/
		raPatchReset(&patch, block, sizeof(block) - 1);
		patchFeedAll(&patch,
			"{\"GameID\":14856,\"ConsoleID\":18,"
			"\"ID\":9007,\"MemAddr\":\"0xH1=1\",\"Flags\":3}");
		raPatchFinish(&patch);
		CHECK(patch.kept == 1 && patch.withId == 1 && patch.withoutId == 0);
		CHECK(strcmp(block, "9007:0xH1=1\n") == 0);

		/*
		    An achievement with no id of its own must *not* inherit the root object's. The reply
		    really does open with the game's `"ID"`, so without clearing the pending id on commit
		    this would have staged 14856 and counted it as a proper id.
		*/
		raPatchReset(&patch, block, sizeof(block) - 1);
		patchFeedAll(&patch,
			"{\"ID\":14856,\"Achievements\":["
			"{\"MemAddr\":\"0xH1=1\",\"Flags\":3},"
			"{\"ID\":77,\"MemAddr\":\"0xH2=2\",\"Flags\":3}]}");
		raPatchFinish(&patch);
		CHECK(patch.kept == 2);
		CHECK(patch.withId == 1 && patch.withoutId == 1);
		CHECK(strcmp(block, "14856:0xH1=1\n77:0xH2=2\n") != 0);
		CHECK(strcmp(block, "0xH1=1\n77:0xH2=2\n") == 0);

		/*
		    Eight digits, just under the notice threshold, so it is a real id and stages as one. The
		    parser's own boundaries -- nine digits, the u32 maximum, and past it -- are exercised
		    against ra_take_id() in tools/ra_reader_test.c, where no threshold intervenes.
		*/
		raPatchReset(&patch, block, sizeof(block) - 1);
		patchFeedAll(&patch, "{\"ID\":99999999,\"MemAddr\":\"0xH1=1\",\"Flags\":3}");
		raPatchFinish(&patch);
		CHECK(patch.kept == 1 && patch.withId == 1 && patch.oddIds == 0);
		CHECK(strcmp(block, "99999999:0xH1=1\n") == 0);

		/* And one digit more is a server notice: dropped, counted, not staged. */
		raPatchReset(&patch, block, sizeof(block) - 1);
		patchFeedAll(&patch, "{\"ID\":101000001,\"MemAddr\":\"1=1.300.\",\"Flags\":3}");
		raPatchFinish(&patch);
		CHECK(patch.kept == 0 && patch.oddIds == 1);
		CHECK(block[0] == 0);

		/*
		    An id nobody can explain is *counted and captured*, not filtered. The set for GameID
		    14856 staged 56 core definitions against 55 published achievements, and the extra id --
		    101000001 -- returns NOT FOUND on the site. One set and one id is not a rule, so the
		    reply's own bytes around it are kept and the definition still stages. What identifies it
		    is the Title and Points that follow the id in the object, which is exactly what the
		    capture holds.
		*/
		raPatchReset(&patch, block, sizeof(block) - 1);
		patchFeedAll(&patch,
			"{\"ID\":93121,\"MemAddr\":\"0xH1=1\",\"Title\":\"Real\",\"Flags\":3},"
			"{\"ID\":101000001,\"MemAddr\":\"1=1.300.\",\"Title\":\"Odd One\","
			"\"Points\":0,\"Flags\":3}");
		raPatchFinish(&patch);
		/*
		    One kept, not two: the odd one is a server notice and is dropped. What it *is* was
		    established from hardware -- the capture read `"Title":"Warning: Unknown Emulator"` with
		    zero Points and an empty Author -- so this is a filter against evidence rather than
		    against a threshold.
		*/
		CHECK(patch.kept == 1);
		CHECK(patch.withId == 1);
		CHECK(strcmp(block, "93121:0xH1=1\tReal\n") == 0);
		CHECK(patch.oddIds == 1 && patch.oddId == 101000001);
		/* The capture starts at the byte after the id's digits, so the fields follow it verbatim. */
		CHECK(strstr(patch.oddContext, "Odd One") != NULL);
		CHECK(strstr(patch.oddContext, "\"Points\":0") != NULL);
		/* And it is the *first* odd id only -- one example identifies the shape. */
		CHECK(strstr(patch.oddContext, "Real") == NULL);

		/* Ids survive being split across chunks like everything else. */
		{
			const char* reply = "\"ID\":123456,\"MemAddr\":\"0xH9=9\",\"Flags\":3";
			const int   total = (int)strlen(reply);
			int         split, bad = 0;

			for (split = 0; split <= total; split++) {
				raPatchReset(&patch, block, sizeof(block) - 1);
				raPatchFeed(&patch, reply, split);
				raPatchFeed(&patch, reply + split, total - split);
				raPatchFinish(&patch);
				if (strcmp(block, "123456:0xH9=9\n") != 0) {
					bad = split + 1;
					break;
				}
			}
			CHECK(bad == 0);
		}
	}

	printf("\nalready-earned definitions are written for the viewer, not armed\n");
	{
		/*
		    Never armed, because that is the whole reason the skip list exists: an achievement the
		    account holds must not fire again. But it is no longer *dropped* either -- the viewer has
		    to be able to show it, and it can, because what it costs is what a person reads rather
		    than a memaddr. The largest real set averages over 500 bytes a line for the memaddr and
		    about 100 for everything else, so an earned achievement gives the block back four fifths
		    of its space instead of all of it.

		    The marker is `#!`, and the `#` is not a new convention: ra_split_definitions() in
		    cardenginei_arm9_ra has skipped `#` lines since it was written, so these are invisible to
		    rcheevos with nothing changed over there. The `!` separates them from a comment a person
		    typed in a hand-written file.
		*/
		static const u32 earned[] = { 93121 };

		raPatchReset(&patch, block, sizeof(block) - 1);
		patch.skipIds   = earned;
		patch.skipCount = 1;
		patchFeedAll(&patch,
			"{\"ID\":93121,\"MemAddr\":\"0xH1=1\",\"Title\":\"Done\",\"Points\":10,\"Flags\":3},"
			"{\"ID\":93119,\"MemAddr\":\"0xH2=2\",\"Flags\":3}");
		raPatchFinish(&patch);
		CHECK(patch.kept == 1 && patch.alreadyDone == 1 && patch.earned == 1);
		CHECK(strcmp(block, "#!93121\tDone\t10\n93119:0xH2=2\n") == 0);
		CHECK(patch.wanted == strlen("#!93121\tDone\t10\n93119:0xH2=2\n"));
		/*
		    And 93119 has no label of its own, which is the assertion that found a bug older than
		    this change. Every early exit in raPatchCommit() cleared the memaddr and the flags and
		    not the title, so a definition following a discarded one inherited its label -- and RA
		    sends unofficial achievements interleaved with published ones, so it was reachable on a
		    real set. Nothing compared a label to anything until this fixture did.
		*/
		CHECK(strstr(block, "93119:0xH2=2\tDone") == NULL);

		/* An empty skip list is the same as none: everything stages. */
		raPatchReset(&patch, block, sizeof(block) - 1);
		patch.skipIds   = earned;
		patch.skipCount = 0;
		patchFeedAll(&patch,
			"{\"ID\":93121,\"MemAddr\":\"0xH1=1\",\"Flags\":3}");
		raPatchFinish(&patch);
		CHECK(patch.kept == 1 && patch.alreadyDone == 0);
	}

	printf("\nthe carry buffer holds the largest definition hardware has produced\n");
	{
		/*
		    6,264 bytes, measured: the first hardware run of stage 12 against GameID 14856
		    reported that as `longest` and dropped five definitions to a 2,047-byte buffer. A
		    completionist achievement is one condition per collectable, so kilobytes is normal for
		    exactly the achievements a player cares most about.

		    Pinned as a test rather than only as a constant, because the failure it prevents is
		    silent: five achievements that simply never exist, in a set that otherwise loads.
		*/
		enum { RA_MEASURED_LONGEST = 6264 };
		static char huge[RA_MEASURED_LONGEST + 64];
		u32         i;

		strcpy(huge, "\"MemAddr\":\"");
		for (i = strlen(huge); i < strlen("\"MemAddr\":\"") + RA_MEASURED_LONGEST; i++) {
			huge[i] = (i & 1) ? '1' : '0';
		}
		strcpy(huge + i, "\",\"Flags\":3");

		raPatchReset(&patch, block, sizeof(block) - 1);
		patchFeedAll(&patch, huge);
		raPatchFinish(&patch);
		CHECK(patch.kept == 1 && patch.tooLong == 0);
		CHECK(patch.longest == RA_MEASURED_LONGEST);
		CHECK(RA_PATCH_MEMADDR_MAX > RA_MEASURED_LONGEST);
	}

	printf("\nnothing is truncated: a short definition is a different definition\n");
	{
		static char big[RA_PATCH_MEMADDR_MAX + 256];
		u32         i;

		/* A memaddr past the buffer is dropped and counted, never clipped and kept. */
		strcpy(big, "\"MemAddr\":\"");
		for (i = strlen(big); i < RA_PATCH_MEMADDR_MAX + 100; i++) {
			big[i] = 'a';
		}
		strcpy(big + i, "\",\"Flags\":3");

		raPatchReset(&patch, block, sizeof(block) - 1);
		raPatchFeed(&patch, big, (int)strlen(big));
		raPatchFinish(&patch);
		CHECK(patch.kept == 0 && patch.tooLong == 1 && block[0] == 0);
		CHECK(patch.longest > RA_PATCH_MEMADDR_MAX);

		/* A reply cut off inside a value is a prefix, and a prefix goes nowhere. */
		raPatchReset(&patch, block, sizeof(block) - 1);
		patchFeedAll(&patch, "\"MemAddr\":\"0xH1=1_0xH");
		raPatchFinish(&patch);
		CHECK(patch.kept == 0 && patch.cutShort == 1 && block[0] == 0);
	}

	printf("\na full block stops cleanly and says how much it wanted\n");
	{
		/*
		    Room for the first definition and not the second. What matters is that the block
		    holds whole lines only -- a half-written memaddr would parse as a real one -- and
		    that `wanted` reports what a complete set would have needed. That number is the
		    measurement step 3d exists to take.
		*/
		char small[28];   /* "1:" + 24 + newline = 27, and not the 22 the second needs */

		patchRun(&patch, small, sizeof(small) - 1, patchReply, 0);
		CHECK(strcmp(small, "1:0xH0a1b2c=1_d0xH0a1b2c=0\n") == 0);
		CHECK(patch.kept == 1 && patch.dropped == 1);
		CHECK(patch.used == 27 && patch.wanted == strlen(patchExpect));
		CHECK(patch.used <= sizeof(small) - 1);
		/*
		    And this is the case the label guard exists for. With its title the first definition needs
		    38 bytes and there are 27, so the *label* is what gets dropped -- the achievement is kept
		    and still works. Before the guard this read `kept 0, dropped 2`: adding titles would have
		    switched off an achievement that had been fine the day before.
		*/
		CHECK(patch.withTitle == 0 && patch.titleNoRoom == 1);

		/* No room at all: nothing is written and everything is accounted for. */
		patchRun(&patch, small, 0, patchReply, 0);
		CHECK(patch.kept == 0 && patch.dropped == 2);
		CHECK(patch.wanted == strlen(patchExpect));
	}

	printf("\ntitles survive what a title contains, and cannot forge the delimiter\n");
	{
		/*
		    A colon in a title is the reason the delimiter is a tab and not a third colon-separated
		    field. "Chapter 1: Beginnings" is an ordinary achievement name.
		*/
		patchRun(&patch, block, sizeof(block) - 1,
		         "HTTP/1.1 200 OK\r\n\r\n{\"Achievements\":[{\"ID\":7,"
		         "\"Title\":\"Chapter 1: Beginnings\",\"MemAddr\":\"0xH1=1\",\"Flags\":3}]}", 0);
		CHECK(strcmp(block, "7:0xH1=1\tChapter 1: Beginnings\n") == 0);
		CHECK(patch.kept == 1 && patch.withTitle == 1);

		/*
		    An escaped tab stays two characters. That is what makes the record parseable at all: if
		    `\t` in a title decoded to a real tab, a title could split its own record and the text
		    after it would be read as another title -- or worse, the memaddr boundary would move.
		*/
		patchRun(&patch, block, sizeof(block) - 1,
		         "HTTP/1.1 200 OK\r\n\r\n{\"Achievements\":[{\"ID\":8,"
		         "\"Title\":\"a\\tb\",\"MemAddr\":\"0xH1=1\",\"Flags\":3}]}", 0);
		CHECK(strcmp(block, "8:0xH1=1\ta\\tb\n") == 0);
		CHECK(strchr(block + 9, '\t') == NULL);   /* exactly one tab in the record, the delimiter */

		/*
		    Two achievements, the second with no Title at all. It gets *no* label rather than the
		    first one's, because the title is cleared when a definition commits. Pinned, because the
		    alternative -- a label bleeding from one achievement to the next -- would look like the
		    notification naming the wrong thing.
		*/
		patchRun(&patch, block, sizeof(block) - 1,
		         "HTTP/1.1 200 OK\r\n\r\n{\"Achievements\":["
		         "{\"ID\":10,\"Title\":\"Named\",\"MemAddr\":\"0xH1=1\",\"Flags\":3},"
		         "{\"ID\":11,\"MemAddr\":\"0xH2=2\",\"Flags\":3}]}", 0);
		CHECK(strcmp(block, "10:0xH1=1\tNamed\n11:0xH2=2\n") == 0);
		CHECK(patch.kept == 2 && patch.withTitle == 1);

		/*
		    The one case that does bleed, pinned so it is a decision: an *untitled first* achievement
		    inherits the game's own Title from the root object. The id is still its own, which is the
		    half that matters.
		*/
		patchRun(&patch, block, sizeof(block) - 1,
		         "HTTP/1.1 200 OK\r\n\r\n{\"PatchData\":{\"ID\":14856,"
		         "\"Title\":\"Contra 4\",\"Achievements\":["
		         "{\"ID\":20,\"MemAddr\":\"0xH1=1\",\"Flags\":3}]}}", 0);
		CHECK(strcmp(block, "20:0xH1=1\tContra 4\n") == 0);

		/*
		    And the reverse of the fixture's order, which is the order the one captured reply uses:
		    Title first, MemAddr second. The root object's own Title must not leak into it -- and the
		    brace inside the Description must not cost the achievement its id, which it did until the
		    scanner learned to tell a brace in a string from a brace that opens an object.
		*/
		patchRun(&patch, block, sizeof(block) - 1,
		         "HTTP/1.1 200 OK\r\n\r\n{\"PatchData\":{\"ID\":14856,"
		         "\"Title\":\"Contra 4\",\"Achievements\":[{\"ID\":12,"
		         "\"Title\":\"Stage 1 Clear\",\"Description\":\"has a { brace\","
		         "\"MemAddr\":\"0xH3=3\",\"Flags\":3}]}}", 0);
		CHECK(strcmp(block, "12:0xH3=3\tStage 1 Clear\n") == 0);

		/*
		    An achievement with an empty title writes no tab rather than a trailing one, so the reader
		    never has to tell "no label" from "empty label".
		*/
		patchRun(&patch, block, sizeof(block) - 1,
		         "HTTP/1.1 200 OK\r\n\r\n{\"Achievements\":[{\"ID\":13,"
		         "\"Title\":\"\",\"MemAddr\":\"0xH1=1\",\"Flags\":3}]}", 0);
		CHECK(strcmp(block, "13:0xH1=1\n") == 0);
		CHECK(patch.kept == 1 && patch.withTitle == 0);
	}

	printf("\nthe real block is big enough for the set this fork is aiming at\n");
	/*
	    The two constants that have to agree, pinned where a change to either is visible: the
	    reader splits at most RA_DEFS_MAX_LINES definitions out of a block of
	    CARDENGINEI_ARM9_RA_DEFS_MAX bytes, so the average definition length that fits is what
	    decides whether a real set arrives whole. tools/ra_reader_test.c pins the same pair from
	    the reader's side; this is the writer's.
	*/
	/* 128 is RA_DEFS_MAX_LINES, which lives in the cardengine and is pinned to that value there. */
	CHECK((CARDENGINEI_ARM9_RA_DEFS_MAX - CARDENGINEI_ARM9_RA_DEFS_HEADER - 1)
	      / 128 >= 200);
	/* And a single definition can be as long as the scanner will carry. */
	CHECK(RA_PATCH_MEMADDR_MAX
	      < CARDENGINEI_ARM9_RA_DEFS_MAX - CARDENGINEI_ARM9_RA_DEFS_HEADER);
}

/*
    The unlock queue.

    Two halves worth different amounts. The parser and the packer are ordinary logic and are tested
    the ordinary way. The signature is the reason this function exists: RetroAchievements answers a
    wrong `v=` with a generic refusal that says nothing about hashing, so a mistake there would look
    exactly like a wrong achievement id, from a console, one boot at a time. The digests below were
    computed by coreutils' md5sum and pasted in -- an oracle this code had no part in producing.
*/
static void test_queue(void) {
	printf("\nthe queue reads what a human or the cardengine writes\n");
	{
		raQueue q;

		/* Fixed 16-byte NUL-padded records, which is what the cardengine will write. */
		{
			char file[RA_QUEUE_BYTES];

			memset(file, 0, sizeof(file));
			memcpy(file + 0 * RA_QUEUE_RECORD, "93119\n", 6);
			memcpy(file + 1 * RA_QUEUE_RECORD, "93121\n", 6);

			raQueueScan(&q, file, sizeof(file));
			CHECK(q.count == 2);
			CHECK(q.ids[0] == 93119 && q.ids[1] == 93121);
			/*
			    The NUL padding must not read as anything. A parser that counted it would report
			    hundreds of dropped values on every boot and bury a real one.
			*/
			CHECK(q.dropped == 0);
		}

		/* ...and the same ids typed by hand, in any of the shapes a person would type them. */
		{
			static const char* const shapes[] = {
				"93119\n93121\n",
				"93119 93121",
				"93119\r\n93121\r\n",
				"  93119,93121  ",
				"# earned tonight\n93119\n93121\n",
			};
			size_t i;

			for (i = 0; i < sizeof(shapes) / sizeof(shapes[0]); i++) {
				raQueueScan(&q, shapes[i], (int)strlen(shapes[i]));
				CHECK(q.count == 2 && q.ids[0] == 93119 && q.ids[1] == 93121);
			}
		}

		/* A comment cannot smuggle an id in, which is the only reason comments are parsed at all. */
		raQueueScan(&q, "# 93119 was earned\n93121\n", 25);
		CHECK(q.count == 1 && q.ids[0] == 93121);

		/* Duplicates collapse: awarding one twice is not how you find out the writer misbehaved. */
		raQueueScan(&q, "93119 93119 93119", 17);
		CHECK(q.count == 1);

		/*
		    Refused rather than truncated, the same correction ra_patch.c and ra_take_id() needed.
		    One digit past a u32 is a different number and must not be silently shortened into a
		    real one. Stated on RA_SYNTHETIC_ID_BASE - 1 rather than on 4294967295, which used to be
		    the largest value that had to survive and is now refused for a different reason -- see
		    the synthetic block below.
		*/
		raQueueScan(&q, "4026531839", 10);
		CHECK(q.count == 1 && q.ids[0] == RA_SYNTHETIC_ID_BASE - 1);
		raQueueScan(&q, "4294967296", 10);
		CHECK(q.count == 0 && q.dropped == 1);
		raQueueScan(&q, "99999999999", 11);
		CHECK(q.count == 0 && q.dropped == 1);

		/* Zero is not an id -- rcheevos refuses it -- and a file of zeros is not a file of unlocks. */
		raQueueScan(&q, "0\n00\n", 5);
		CHECK(q.count == 0 && q.dropped == 2);

		/*
		    A synthetic id is not an achievement, and this is the case a card taught us.

		    cardenginei_arm9_ra gives an id-less definition RA_SYNTHETIC_ID_BASE + index, and the
		    built-in self-test is exactly such a definition -- so on every game RetroAchievements
		    does not know, the self-test fired, was queued as a real unlock, showed up in the in-game
		    menu as work waiting to sync, and was submitted on the next boot. The server's answer, off
		    a real log: `4026531840 refused: Unknown achievement`, a 404 per boot forever.

		    The ARM9 guard stops new ones. This stops the ones already on cards, and it is the half
		    that has to be right about *consumption*: the drop happens after the record's stamp, code
		    and title have been taken, or `20260815183256` and `PICROSS3D` become two more ids.
		*/
		raQueueScan(&q, "4026531840\t20260815183256\tC6PJ\tPICROSS3D\n", 41);
		CHECK(q.count == 0);
		CHECK(q.synthetic == 1);
		CHECK(q.dropped == 0);   /* well-formed, not corrupt -- the counts say different things */

		/* A real unlock in the same file survives it, which is the case that actually matters. */
		raQueueScan(&q, "4026531840\t20260815183256\tC6PJ\tPICROSS3D\n"
		                "302329\t20260815183300\tYCTE\tCONTRA 4\n", 78);
		CHECK(q.count == 1 && q.ids[0] == 302329);
		CHECK(q.synthetic == 1);
		CHECK(strcmp(q.titles[0], "CONTRA 4") == 0);

		/* More than the queue holds is reported, not silently dropped. */
		{
			char many[RA_QUEUE_MAX * 8 + 64];
			int  n = 0;
			int  i;

			for (i = 0; i < RA_QUEUE_MAX + 3; i++) {
				n += sprintf(many + n, "%d\n", 1000 + i);
			}
			raQueueScan(&q, many, n);
			CHECK(q.count == RA_QUEUE_MAX);
			CHECK(q.truncated == 3);
		}
	}

	printf("\nand writes back only what is still owed, at the same length\n");
	{
		raQueue q;
		char    out[RA_QUEUE_BYTES];
		int     keep[2];
		int     written;

		memset(&q, 0, sizeof(q));

		/* The clearing case, which is the common one: everything went, nothing is owed. */
		memset(out, 'x', sizeof(out));
		written = raQueuePack(&q, NULL, 0, out, sizeof(out));
		CHECK(written == 0);
		{
			size_t i;
			int    clean = 1;

			for (i = 0; i < sizeof(out); i++) {
				if (out[i]) {
					clean = 0;
				}
			}
			CHECK(clean);
		}

		/* Two owed: they land at record boundaries the cardengine can compute without reading. */
		raQueueScan(&q, "93119\n4026531839\n", 17);
		keep[0] = 0;
		keep[1] = 1;
		written = raQueuePack(&q, keep, 2, out, sizeof(out));
		CHECK(written == 2);
		CHECK(memcmp(out + 0 * RA_QUEUE_RECORD, "93119\n", 6) == 0);
		CHECK(memcmp(out + 1 * RA_QUEUE_RECORD, "4026531839\n", 11) == 0);
		CHECK(out[2 * RA_QUEUE_RECORD] == 0);

		/* And what it wrote is what the parser reads back -- the round trip, not two guesses. */
		raQueueScan(&q, out, sizeof(out));
		CHECK(q.count == 2 && q.ids[0] == 93119 && q.ids[1] == RA_SYNTHETIC_ID_BASE - 1);

		/*
		    Refused rather than half-written. A partial queue is indistinguishable from a whole one
		    on the next boot, so a buffer that is not a whole number of records is an error.
		*/
		CHECK(raQueuePack(&q, keep, 2, out, RA_QUEUE_RECORD - 1) == -1);
		CHECK(raQueuePack(&q, keep, 2, out, RA_QUEUE_RECORD * 2 + 1) == -1);
		/* More to keep than the file can hold is also an error rather than a truncation. */
		CHECK(raQueuePack(&q, keep, 2, out, RA_QUEUE_RECORD) == -1);

		/* A record has to hold the longest u32 plus its newline, or ids get mis-parsed. */
		CHECK(RA_QUEUE_RECORD >= 11);
	}

	printf("\na record remembers the mode it was earned in, and the sender cannot overrule it\n");
	{
		raQueue q;
		char    out[RA_QUEUE_BYTES];
		int     keep[2];

		/*
		    The bug this whole field exists for: the mode used to come from ra.cfg at submission
		    time, so earning in softcore and then setting hardcore=1 sent them as hardcore. The
		    record now says, and the record is what raWifiSubmitOne() signs and sends.
		*/
		raQueueScan(&q, "302329\t20260815183300\tYCTE\tCONTRA 4\t1\n"
		                "302330\t20260815183301\tYCTE\tCONTRA 4\t0\n", 76);
		CHECK(q.count == 2);
		CHECK(q.ids[0] == 302329 && q.hardcore[0] == 1);
		CHECK(q.ids[1] == 302330 && q.hardcore[1] == 0);
		CHECK(q.hard == 1);
		/* The fields before it still arrive: appending must not have shifted anything. */
		CHECK(strcmp(q.titles[0], "CONTRA 4") == 0 && strcmp(q.codes[0], "YCTE") == 0);
		CHECK(q.stamped == 2 && q.named == 2);

		/*
		    **The consumption test, and it is the one that matters.** A mode digit left unconsumed is
		    read by the outer loop as the next record's id -- exactly the failure a title ending in a
		    digit would have caused, which is why that lesson is written down at takeField(). Two
		    records must parse as two, not four.
		*/
		CHECK(q.dropped == 0);
		CHECK(q.count == 2);

		/*
		    **The record as the ARM7 actually writes it**, which is not the shape every test above
		    uses, and that difference cost a real account an achievement it never earned.

		    raUnlockAppend() copies gameCode and gameTitle as fixed 4 and 12 bytes straight out of the
		    ROM header, padding included, because trimming them costs bytes the ARM7 does not have.
		    Contra 4's header pads its eight-character title with NULs. takeField() stops at the first
		    NUL, four bytes short of the tab, so the mode field was never found and its `1` was read by
		    the outer loop as an achievement id -- and submitted, and accepted.

		    Every other test here was written against raQueuePack()'s output, which trims. Two writers,
		    one reader, and the tests only ever exercised the writer that agreed with them.
		*/
		{
			static const char arm7[RA_QUEUE_RECORD] = {
				'3','0','2','3','2','9','\t',
				'2','0','2','6','0','8','1','5','1','8','3','3','0','0','\t',
				'Y','C','T','E','\t',
				'C','O','N','T','R','A',' ','4',0,0,0,0,'\t',
				'1','\n'
			};

			raQueueScan(&q, arm7, RA_QUEUE_RECORD);
			CHECK(q.count == 1);
			CHECK(q.ids[0] == 302329);
			CHECK(q.hardcore[0] == 1 && q.hard == 1);
			CHECK(strcmp(q.titles[0], "CONTRA 4") == 0);
			CHECK(strcmp(q.codes[0], "YCTE") == 0);
			/* The one that failed on hardware: the mode digit must not become a second id. */
			CHECK(q.dropped == 0);
		}

		/* The same record from a build with no mode field, which must still read as one unlock. */
		{
			static const char arm7old[RA_QUEUE_RECORD] = {
				'3','0','2','3','2','9','\t',
				'2','0','2','6','0','8','1','5','1','8','3','3','0','0','\t',
				'Y','C','T','E','\t',
				'C','O','N','T','R','A',' ','4',0,0,0,0,'\n'
			};

			raQueueScan(&q, arm7old, RA_QUEUE_RECORD);
			CHECK(q.count == 1 && q.ids[0] == 302329 && q.hardcore[0] == 0);
			CHECK(strcmp(q.titles[0], "CONTRA 4") == 0);
		}

		/*
		    And two of them back to back, at record boundaries, with padding to the record length --
		    the file as it exists on a card. Skipping padding must not run into the record after it.
		*/
		{
			static char two[RA_QUEUE_RECORD * 2];
			static const char a[] = "302329\t20260815183300\tYCTE\tCONTRA 4";
			static const char b[] = "302330\t20260815183301\tYCTE\tCONTRA 4";

			memset(two, 0, sizeof(two));
			memcpy(two, a, sizeof(a) - 1);
			two[sizeof(a) - 1 + 4] = '\t';       /* past the title's four NUL pad bytes */
			two[sizeof(a) - 1 + 5] = '1';
			two[sizeof(a) - 1 + 6] = '\n';
			memcpy(two + RA_QUEUE_RECORD, b, sizeof(b) - 1);
			two[RA_QUEUE_RECORD + sizeof(b) - 1 + 4] = '\t';
			two[RA_QUEUE_RECORD + sizeof(b) - 1 + 5] = '0';
			two[RA_QUEUE_RECORD + sizeof(b) - 1 + 6] = '\n';

			raQueueScan(&q, two, sizeof(two));
			CHECK(q.count == 2);
			CHECK(q.ids[0] == 302329 && q.hardcore[0] == 1);
			CHECK(q.ids[1] == 302330 && q.hardcore[1] == 0);
			CHECK(q.hard == 1 && q.dropped == 0);
		}

		/* Absent means softcore: a bare id, and a record from a build before the field existed. */
		raQueueScan(&q, "93119\n93120\t20260815183300\tYCTE\tCONTRA 4\n", 41);
		CHECK(q.count == 2);
		CHECK(q.hardcore[0] == 0 && q.hardcore[1] == 0);
		CHECK(q.hard == 0);

		/*
		    Only `1` is a claim. A hand edit that put something else there is not a hardcore unlock,
		    and the character is still consumed so it cannot become an id.
		*/
		raQueueScan(&q, "93119\t20260815183300\tYCTE\tCONTRA 4\tx\n", 37);
		CHECK(q.count == 1 && q.hardcore[0] == 0 && q.dropped == 0);

		/*
		    And a kept record does not lose it. This is the population most likely to be retried --
		    the server never answered -- so a pack that dropped the field would downgrade exactly the
		    unlocks that get a second chance.
		*/
		raQueueScan(&q, "302329\t20260815183300\tYCTE\tCONTRA 4\t1\n"
		                "302330\t20260815183301\tYCTE\tCONTRA 4\t0\n", 76);
		keep[0] = 0;
		keep[1] = 1;
		CHECK(raQueuePack(&q, keep, 2, out, sizeof(out)) == 2);
		CHECK(memcmp(out, "302329\t20260815183300\tYCTE\tCONTRA 4\t1\n", 38) == 0);
		raQueueScan(&q, out, sizeof(out));
		CHECK(q.count == 2 && q.hardcore[0] == 1 && q.hardcore[1] == 0);
		CHECK(q.hard == 1);

		/* The longest record the writer can produce still fits, with the field on the end. */
		CHECK(10 + 1 + RA_QUEUE_STAMP + 1 + RA_QUEUE_CODE + 1 + RA_QUEUE_TITLE + 1 + 1 + 1
		      <= RA_QUEUE_RECORD);

		/*
		    The two magics differ, and softcore is still the value an older ARM7 half compares
		    against. They are read as a pair by cardenginei_arm7 and written as a pair by
		    cardenginei_arm9_ra, in two binaries that share only this header.
		*/
		CHECK(RA_SHARED_UNLOCK_MAGIC != RA_SHARED_UNLOCK_HARDCORE);
		CHECK(RA_SHARED_UNLOCK_MAGIC == 0x4C554152u);
		CHECK(RA_SHARED_UNLOCK_HARDCORE != 0);
	}

	printf("\nand signs r=awardachievement the way rcheevos does\n");
	{
		char v[33];

		/*
		    md5 of the id, the username and the hardcore flag as decimal text with no separators.
		    Oracle: printf '93119Bakke0' | md5sum
		*/
		raQueueSign(93119, "Bakke", 0, 0, v);
		CHECK(strcmp(v, "d9ac96231a45f0f275747a84a4c9271d") == 0);

		/* Oracle: printf '1Cheevos1' | md5sum -- and it proves the flag is '1' and not 1. */
		raQueueSign(1, "Cheevos", 1, 0, v);
		CHECK(strcmp(v, "4787f01ee76713835a4f3bd5de506ec1") == 0);

		/* Hardcore has to change it, or the flag is not in the hash at all. */
		{
			char soft[33];
			char hard[33];

			raQueueSign(93119, "Bakke", 0, 0, soft);
			raQueueSign(93119, "Bakke", 1, 0, hard);
			CHECK(strcmp(soft, hard) != 0);
		}

		/* Always 32 lowercase hex digits, whatever the digest happens to be. */
		{
			int  i;
			int  ok = 1;

			raQueueSign(4294967295u, "a", 0, 0, v);
			CHECK(strlen(v) == 32);
			for (i = 0; i < 32; i++) {
				if (!((v[i] >= '0' && v[i] <= '9') || (v[i] >= 'a' && v[i] <= 'f'))) {
					ok = 0;
				}
			}
			CHECK(ok);
		}

		/*
		    The username goes in raw, not percent-encoded -- the server hashes what it decoded from
		    u=. This is the case that would break only for accounts with a space in the name, which
		    is exactly the kind of bug that ships, so the distinction is pinned rather than trusted.
		*/
		{
			char raw[33];
			char encoded[33];

			raQueueSign(93119, "two words", 0, 0, raw);
			raQueueSign(93119, "two%20words", 0, 0, encoded);
			CHECK(strcmp(raw, encoded) != 0);
		}
	}

	printf("\nreads a record's stamp as seconds, pinned against date -u +%%s\n");
	{
		/*
		    Every expected value here came out of coreutils, not out of this code:
		        date -u -d "2024-02-29 12:00:00" +%s
		    That is the whole point of the exercise. A calendar checked against itself proves that
		    two functions agree, which is not the same as proving either one is right, and `o=` is a
		    number nothing downstream can sanity-check -- the server's reply does not echo it back.
		*/
		CHECK(raQueueStampToUnix("20010101000000") == 978307200u);
		CHECK(raQueueStampToUnix("20250101000000") == 1735689600u);
		CHECK(raQueueStampToUnix("20260810142345") == 1786371825u);
		/* A leap day, which is the case a wrong month table gets wrong by exactly one. */
		CHECK(raQueueStampToUnix("20240229120000") == 1709208000u);
		/* The last second of a leap year, one before the next year's first. */
		CHECK(raQueueStampToUnix("20241231235959") == 1735689599u);
		/*
		    2100 is refused, and that is the contract rather than a limitation. The RTC reports two
		    digits and the writer prefixes "20", so a stamp is 2000-2099 by construction and nothing
		    outside it can be real. Refusing beyond 2099 is the same rule that refuses year 2000.
		*/
		CHECK(raQueueStampToUnix("21000101000000") == 0);

		printf("\nand refuses a date it cannot vouch for rather than guessing\n");
		/*
		    Zero means "no stamp" everywhere downstream, so every one of these ends up sending the
		    unlock without o= -- the behaviour every unlock had before this existed.
		*/
		CHECK(raQueueStampToUnix("20000101000000") == 0);  /* a clock that was never set */
		CHECK(raQueueStampToUnix("20241301000000") == 0);  /* month 13 */
		CHECK(raQueueStampToUnix("20241200000000") == 0);  /* day 0 */
		CHECK(raQueueStampToUnix("20241232000000") == 0);  /* day 32 */
		CHECK(raQueueStampToUnix("20241201240000") == 0);  /* hour 24 */
		CHECK(raQueueStampToUnix("20241201006000") == 0);  /* minute 60 */
		CHECK(raQueueStampToUnix("20241201000060") == 0);  /* second 60 */
		CHECK(raQueueStampToUnix("2024120100000x") == 0);  /* not all digits */
		CHECK(raQueueStampToUnix("") == 0);

		printf("\nand the two conversions are exact inverses\n");
		{
			/*
			    Round trip over a spread of real instants. Both directions have their own month
			    table and their own leap rule, so agreeing is evidence rather than tautology.
			*/
			static const char* const when[] = {
				"20010101000000", "20240229120000", "20241231235959",
				"20260810142345", "20990630181530", "20991231235959",
			};
			size_t k;

			for (k = 0; k < sizeof(when) / sizeof(when[0]); k++) {
				char back[RA_QUEUE_STAMP + 1];

				memset(back, 0, sizeof(back));
				raQueueUnixToStamp(raQueueStampToUnix(when[k]), back);
				CHECK(strcmp(back, when[k]) == 0);
			}
		}
	}

	printf("\nthe queue carries when an unlock was earned, and survives being kept\n");
	{
		raQueue q;
		char    out[RA_QUEUE_RECORD * 4];
		int     keepIdx[2];

		/* A stamped record: the fourteen digits after the tab are a date, not a second id. */
		raQueueScan(&q, "93119\t20260810142345\n", 21);
		CHECK(q.count == 1);
		CHECK(q.ids[0] == 93119);
		CHECK(q.times[0] == 1786371825u);
		CHECK(q.stamped == 1);

		/*
		    The regression this delimiter exists to prevent. Under the old rule -- every non-digit is
		    a separator -- those digits would have parsed as a second unlock with an enormous id.
		*/
		CHECK(q.count == 1);

		/* A bare id still works, which is what keeps the file hand-writable and old queues valid. */
		raQueueScan(&q, "93119\n", 6);
		CHECK(q.count == 1 && q.ids[0] == 93119);
		CHECK(q.times[0] == 0 && q.stamped == 0);

		/* Mixed, in one file: the launcher has to cope with both on the boot after an upgrade. */
		raQueueScan(&q, "93119\n93121\t20260810142345\n", 27);
		CHECK(q.count == 2);
		CHECK(q.ids[0] == 93119 && q.times[0] == 0);
		CHECK(q.ids[1] == 93121 && q.times[1] == 1786371825u);
		CHECK(q.stamped == 1);

		/* An unusable date is consumed as a date and reported as no stamp, not read as an id. */
		raQueueScan(&q, "93119\t20001301000000\n", 21);
		CHECK(q.count == 1 && q.ids[0] == 93119 && q.times[0] == 0);

		/* A truncated stamp at the end of the buffer must not read past it -- nor become an id. */
		raQueueScan(&q, "93119\t2026", 10);
		CHECK(q.count == 1 && q.ids[0] == 93119 && q.times[0] == 0);

		/*
		    And a short stamp mid-file must not swallow the record after it. Counting off a fixed
		    fourteen bytes here ate the next id instead, which turned two owed unlocks into one.
		*/
		raQueueScan(&q, "93119\t2026\n93121\n", 17);
		CHECK(q.count == 2);
		CHECK(q.ids[0] == 93119 && q.times[0] == 0);
		CHECK(q.ids[1] == 93121);

		/*
		    Kept records keep their stamps. This is the case the whole feature turns on: an unlock
		    whose request got no answer is retried on a later boot, and that is precisely when the
		    difference between "earned then" and "submitted now" is largest.
		*/
		raQueueScan(&q, "93119\t20260810142345\tYZ4E\tCONTRA4\n93121\n", 43);
		CHECK(q.count == 2);
		keepIdx[0] = 0;
		keepIdx[1] = 1;
		CHECK(raQueuePack(&q, keepIdx, 2, out, sizeof(out)) == 2);
		/*
		    Written back with an explicit `\t0` although the record arrived without one. The packer
		    always states the mode when the game is present, which normalises an older record rather
		    than preserving its silence -- and the two mean the same thing, since absent has always
		    read as softcore.
		*/
		CHECK(memcmp(out + 0 * RA_QUEUE_RECORD,
		             "93119\t20260810142345\tYZ4E\tCONTRA4\t0\n", 36) == 0);
		/* The unstamped one is written the old way, not with a bogus date or an invented game. */
		CHECK(memcmp(out + 1 * RA_QUEUE_RECORD, "93121\n", 6) == 0);

		/* And read back: the round trip, through the file format rather than around it. */
		raQueueScan(&q, out, sizeof(out));
		CHECK(q.count == 2);
		CHECK(q.ids[0] == 93119 && q.times[0] == 1786371825u);
		CHECK(strcmp(q.codes[0], "YZ4E") == 0);
		CHECK(strcmp(q.titles[0], "CONTRA4") == 0);
		CHECK(q.ids[1] == 93121 && q.times[1] == 0);
		CHECK(q.codes[1][0] == 0);

		/*
		    A record must hold id, stamp, code, title and mode with their delimiters:
		    10+1+14+1+4+1+12+1+1+1.
		*/
		CHECK(RA_QUEUE_RECORD >= 46);
	}

	printf("\na record names the game it came from, straight out of the ROM header\n");
	{
		raQueue q;
		char    out[RA_QUEUE_RECORD * 4];
		int     keep[2];

		raQueueScan(&q, "302329\t20260810142345\tYZ4E\tCONTRA4\n", 35);
		CHECK(q.count == 1);
		CHECK(q.ids[0] == 302329);
		CHECK(q.times[0] == 1786371825u);
		CHECK(strcmp(q.codes[0], "YZ4E") == 0);
		CHECK(strcmp(q.titles[0], "CONTRA4") == 0);
		CHECK(q.named == 1);

		/*
		    **The reason the title is consumed as a field rather than skipped over.** A DS game title
		    routinely ends in a digit -- CONTRA4 does -- and left unconsumed that `4` parses as an
		    achievement id and gets submitted as an unlock the player never earned. One record in, one
		    id out, and the id is the one the cardengine wrote.
		*/
		CHECK(q.count == 1);

		/* Two games in one file, which is the whole case: play A offline, then boot B. */
		raQueueScan(&q,
		            "302329\t20260810142345\tYZ4E\tCONTRA4\n"
		            "118842\t20260809090200\tAMCE\tMARIOKART\n", 71);
		CHECK(q.count == 2);
		CHECK(q.ids[0] == 302329 && strcmp(q.titles[0], "CONTRA4") == 0);
		CHECK(q.ids[1] == 118842 && strcmp(q.titles[1], "MARIOKART") == 0);
		CHECK(strcmp(q.codes[1], "AMCE") == 0);
		CHECK(q.named == 2);

		/* Titles are padded with spaces in the header; the padding is not part of the name. */
		raQueueScan(&q, "302329\t20260810142345\tYZ4E\tCONTRA4     \n", 40);
		CHECK(strcmp(q.titles[0], "CONTRA4") == 0);

		/* A title with a space inside it keeps it -- only the trailing padding goes. */
		raQueueScan(&q, "302329\t20260810142345\tAMCE\tMARIO KART\n", 38);
		CHECK(strcmp(q.titles[0], "MARIO KART") == 0);

		/* An over-long title is clipped in the copy but still consumed whole. */
		raQueueScan(&q, "302329\t20260810142345\tYZ4E\tABCDEFGHIJKLMNOP\n", 44);
		CHECK(q.count == 1);
		CHECK(strcmp(q.titles[0], "ABCDEFGHIJKL") == 0);

		/* A stamp with no game after it is still a stamp -- every field is optional in order. */
		raQueueScan(&q, "302329\t20260810142345\n", 22);
		CHECK(q.count == 1 && q.times[0] == 1786371825u);
		CHECK(q.codes[0][0] == 0 && q.named == 0);

		/*
		    And the round trip through the file, which is what the launcher actually does to whatever
		    it could not send: a kept record has to come back naming the same game, or the menu would
		    lose track of which unlocks belong where after a single failed boot.
		*/
		raQueueScan(&q,
		            "302329\t20260810142345\tYZ4E\tCONTRA4\n"
		            "118842\t20260809090200\tAMCE\tMARIO KART\n", 72);
		keep[0] = 1;   /* only the second one is still owed */
		CHECK(raQueuePack(&q, keep, 1, out, sizeof(out)) == 1);
		raQueueScan(&q, out, sizeof(out));
		CHECK(q.count == 1);
		CHECK(q.ids[0] == 118842);
		CHECK(q.times[0] == raQueueStampToUnix("20260809090200"));
		CHECK(strcmp(q.codes[0], "AMCE") == 0);
		CHECK(strcmp(q.titles[0], "MARIO KART") == 0);

		/* An index outside the queue is refused rather than read. */
		keep[0] = 5;
		CHECK(raQueuePack(&q, keep, 1, out, sizeof(out)) == -1);
		keep[0] = -1;
		CHECK(raQueuePack(&q, keep, 1, out, sizeof(out)) == -1);
	}

	printf("\ngroups the queue by game, which is what the menu shows\n");
	{
		raQueue        q;
		raPendingBlock b;
		const u32      now = raQueueStampToUnix("20260810142345");

		raQueueScan(&q,
		            "302329\t20260810120000\tYZ4E\tCONTRA4\n"
		            "302330\t20260810130000\tYZ4E\tCONTRA4\n"
		            "118842\t20260809090200\tAMCE\tMARIO KART\n", 108);
		CHECK(q.count == 3);

		raQueueTally(&q, now, &b);
		CHECK(b.magic == RA_PENDING_MAGIC);
		CHECK(b.games == 2);
		CHECK(b.total == 3);
		CHECK(strcmp(b.game[0].title, "CONTRA4") == 0 && b.game[0].count == 2);
		CHECK(strcmp(b.game[1].title, "MARIO KART") == 0 && b.game[1].count == 1);

		/*
		    Both of Contra 4's are from today, so the column reads "today"; Mario Kart's is from
		    yesterday. This is the case that made a date column wrong in the first place -- written
		    08/09 it is the ninth of August to one reader and the eighth of September to another,
		    and what the column is actually for is "how long has this been stuck".
		*/
		CHECK(b.game[0].waitDays == 0);
		CHECK(b.game[1].waitDays == 1);

		/* The oldest of a game's unlocks sets the number, not the newest and not the last read. */
		raQueueScan(&q,
		            "302329\t20260801120000\tYZ4E\tCONTRA4\n"
		            "302330\t20260810130000\tYZ4E\tCONTRA4\n", 71);
		raQueueTally(&q, now, &b);
		CHECK(b.games == 1 && b.game[0].count == 2);
		CHECK(b.game[0].waitDays == 9);

		printf("\nand refuses to report an age it does not have\n");
		{
			/*
			    A record whose stamp is fourteen digits but not a real date -- month 13 here -- still
			    names its game and is still owed, so it counts. It contributes no age: reporting it as
			    zero days would print "today", which is a claim about when it was earned made from a
			    record that does not say.

			    Note this is the only shape that case can take. The fields are positional, so a record
			    cannot carry a game without a stamp in front of it; one with neither is a bare id and
			    lands in `unnamed` instead.
			*/
			raQueueScan(&q,
			            "302329\t20001301000000\tYZ4E\tCONTRA4\n"
			            "302330\t20260809090200\tYZ4E\tCONTRA4\n", 70);
			raQueueTally(&q, now, &b);
			CHECK(b.total == 2);
			CHECK(b.games == 1 && b.game[0].count == 2);
			CHECK(b.game[0].waitDays == 1);

			/* A clock that moved backwards wraps the subtraction, so it contributes nothing. */
			raQueueScan(&q, "302329\t20260812120000\tYZ4E\tCONTRA4\n", 35);
			raQueueTally(&q, now, &b);
			CHECK(b.games == 1 && b.game[0].count == 1);
			CHECK(b.game[0].waitDays == 0);
		}

		printf("\nand counts what it had no room to show rather than hiding it\n");
		{
			/*
			    A bare id has no game to be grouped under. Counted apart rather than filed beneath a
			    blank title, so the menu can admit it instead of drawing an empty row.
			*/
			raQueueScan(&q, "302329\n302330\t20260809090200\tYZ4E\tCONTRA4\n", 42);
			raQueueTally(&q, now, &b);
			CHECK(b.total == 2);
			CHECK(b.unnamed == 1);
			CHECK(b.games == 1 && b.game[0].count == 1);
		}
	}

	printf("\nand the block the bootloader copies is the one the launcher wrote\n");
	{
		/*
		    Two constants for one value, in two headers that never include each other: the bootloader
		    checks the magic from locations.h without knowing what a raPendingBlock is, and the
		    launcher writes it from ra_wifi.h. Nothing else would notice them drifting apart, and the
		    failure would be a page that silently never appears.
		*/
		CHECK(RA_PENDING_MAGIC == CARDENGINEI_ARM9_RA_PENDING_MAGIC);
		/* And the block has to fit the reservation the heap was shortened to make room for. */
		CHECK(sizeof(raPendingBlock) <= CARDENGINEI_ARM9_RA_PENDING_MAX);
		/*
		    Same pin for the viewer's index, and it earns it more: raViewerEntry is 16 bytes and 128
		    of them plus the header is 2,060, so the reservation had to grow from 0x800 to 0xA00 when
		    the entry gained the date an achievement was earned. Without this check that growth would
		    have overrun the pending tally directly above it, with nothing failing to compile.
		*/
		CHECK(sizeof(raViewerBlock) <= CARDENGINEI_ARM9_RA_VIEWER_MAX);
		CHECK(sizeof(raViewerEntry) == 16);
		CHECK(RA_VIEWER_MAGIC == CARDENGINEI_ARM9_RA_VIEWER_MAGIC);
		/*
		    Same pin again for the session block, and this is the one where the two constants are
		    read by code that can never be compared by a compiler: the launcher writes RA_SESSION_-
		    MAGIC from ra_wifi.h and the bootloader checks CARDENGINEI_ARM9_RA_SESSION_MAGIC from
		    locations.h. Drifted apart, the bootloader would zero the destination on every boot, the
		    menu would read "nobody told me", and a hardcore session would get its RAM editor back
		    with nothing failing to compile and no message anywhere.
		*/
		CHECK(RA_SESSION_MAGIC == CARDENGINEI_ARM9_RA_SESSION_MAGIC);
		/*
		    The overlay switch rides in a byte this block already had spare, so the bootloader's two
		    offsets are undisturbed -- and that matters because it reaches them by offset from
		    locations.h rather than through this struct, so a shift would be silent.
		*/
		CHECK(__builtin_offsetof(raSessionBlock, hardcore)
		      == CARDENGINEI_ARM9_RA_SESSION_HARDCORE_OFFSET);
		CHECK(__builtin_offsetof(raSessionBlock, refusal)
		      == CARDENGINEI_ARM9_RA_SESSION_REFUSAL_OFFSET);
		CHECK(__builtin_offsetof(raSessionBlock, overlay) == 6);
		/*
		    ...and the queue switch in the last of the two bytes this block had spare. Pinned because
		    the bootloader reaches into this block by offset from locations.h rather than through the
		    struct, so a field that shifted would be silent on that side.
		*/
		CHECK(__builtin_offsetof(raSessionBlock, queue) == 7);
		CHECK(sizeof(raSessionBlock) <= CARDENGINEI_ARM9_RA_SESSION_MAX);
		/*
		    And the two facts the bootloader needs about the *inside* of that block, which it reaches
		    without including ra_wifi.h. The offset is the sharper of the two: written through a
		    literal on one side and a struct member on the other, a field inserted before `hardcore`
		    would move the flag and the bootloader would go on clearing a byte of the magic --
		    zeroing a quarter of it, so the menu would read "nobody told me" and hand a cheating
		    session its RAM editor.
		*/
		CHECK(offsetof(raSessionBlock, hardcore) == CARDENGINEI_ARM9_RA_SESSION_HARDCORE_OFFSET);
		CHECK(offsetof(raSessionBlock, refusal) == CARDENGINEI_ARM9_RA_SESSION_REFUSAL_OFFSET);
		CHECK(RA_REFUSED_CHEATS == CARDENGINEI_ARM9_RA_SESSION_REFUSED_CHEATS);
		CHECK(RA_CHEATS_MIN_BYTES == CARDENGINEI_ARM9_RA_CHEATS_MIN_BYTES);
		/*
		    And the flag really is a flag. The RAM viewer and the unlock path both read it as a
		    capability with `!= 0`, so a field that grew a third value would grant hardcore to it.
		*/
		CHECK(sizeof(((raSessionBlock*)0)->hardcore) == 1);
		/*
		    And the four regions do not overlap: the definitions end where the session block begins,
		    that ends where the viewer's index begins, and that ends where the pending tally does.
		    Stated as arithmetic because the launcher's blockMax is the only thing keeping the
		    scanner out of all three.
		*/
		CHECK(CARDENGINEI_ARM9_RA_SESSION_LOCATION + CARDENGINEI_ARM9_RA_SESSION_MAX
		      == CARDENGINEI_ARM9_RA_VIEWER_LOCATION);
		CHECK(CARDENGINEI_ARM9_RA_VIEWER_LOCATION + CARDENGINEI_ARM9_RA_VIEWER_MAX
		      == CARDENGINEI_ARM9_RA_PENDING_LOCATION);
		CHECK(CARDENGINEI_ARM9_RA_PENDING_LOCATION + CARDENGINEI_ARM9_RA_PENDING_MAX
		      == CARDENGINEI_ARM9_RA_DEFS_LOCATION + CARDENGINEI_ARM9_RA_DEFS_MAX);
		/*
		    The staging copies are laid out differently from the destinations -- the pending tally is
		    staged well past the definitions rather than inside them -- so their non-overlap is a
		    separate fact and is checked separately.
		*/
		CHECK(CARDENGINEI_ARM9_RA_PENDING_BUFFERED_LOCATION + CARDENGINEI_ARM9_RA_PENDING_MAX
		      <= CARDENGINEI_ARM9_RA_SESSION_BUFFERED_LOCATION);
		CHECK(CARDENGINEI_ARM9_RA_DEFS_BUFFERED_LOCATION + CARDENGINEI_ARM9_RA_DEFS_MAX
		      <= CARDENGINEI_ARM9_RA_PENDING_BUFFERED_LOCATION);
	}

	printf("\nand a set can never be large enough to reach any of them\n");
	{
		/*
		    The cap the launcher applies to a fetched set, restated here against the reservation it
		    is protecting. This exists because the cap is three subtractions in one function and the
		    reservation is four constants in another, and the arithmetic that ties them was wrong for
		    the hand-written file for as long as that file was assumed to be small -- see
		    loadRaDefinitions() in conf_sd.cpp, which now subtracts the same three.
		*/
		const unsigned long blockMax = CARDENGINEI_ARM9_RA_DEFS_MAX
		                               - CARDENGINEI_ARM9_RA_PENDING_MAX
		                               - CARDENGINEI_ARM9_RA_VIEWER_MAX
		                               - CARDENGINEI_ARM9_RA_SESSION_MAX
		                               - CARDENGINEI_ARM9_RA_DEFS_HEADER - 1;

		CHECK(CARDENGINEI_ARM9_RA_DEFS_LOCATION + CARDENGINEI_ARM9_RA_DEFS_HEADER + blockMax
		      < CARDENGINEI_ARM9_RA_SESSION_LOCATION);
		/*
		    And it is still big enough to be worth having: the largest real set measured on this
		    project is 9,662 bytes.
		*/
		CHECK(blockMax > 16384);
	}

	printf("\nand o= changes the signature as well as the URL\n");
	{
		char with[33];
		char without[33];

		/*
		    The trap in this whole change. rcheevos appends the id a *second* time and then the
		    seconds when o= is sent -- its own comment says the server overloads the hash that way --
		    so a request that carries o= with the old three-field digest is refused, and refused with
		    a message that does not say which half was wrong.
		*/
		raQueueSign(93119, "Bakke", 0, 0, without);
		raQueueSign(93119, "Bakke", 0, 3600, with);
		CHECK(strcmp(with, without) != 0);

		/*
		    Pinned against the formula rather than against this code: md5 of
		    id + user + hardcore + id + seconds, which is what rc_api_runtime.c builds.
		        printf '93119Bakke0931193600' | md5sum
		*/
		CHECK(strcmp(with, "52d7d7ff9f64ceb0784a29d2035133fa") == 0);

		/* Zero seconds is "no o=", matching rcheevos, which writes the parameter only when non-zero. */
		raQueueSign(93119, "Bakke", 0, 0, with);
		CHECK(strcmp(with, without) == 0);

		/* The seconds are part of it, so two different offsets cannot share a signature. */
		{
			char a[33];
			char b[33];

			raQueueSign(93119, "Bakke", 0, 60, a);
			raQueueSign(93119, "Bakke", 0, 61, b);
			CHECK(strcmp(a, b) != 0);
		}
	}

	printf("\nand Success is read as a literal, not as a word in the reply\n");
	{
		CHECK(raNetJsonTrue("{\"Success\":true,\"Score\":1}", "Success"));
		CHECK(raNetJsonTrue("{\"Success\": true}", "Success"));
		CHECK(!raNetJsonTrue("{\"Success\":false,\"Error\":\"nope\"}", "Success"));
		/*
		    The failure this reader exists to avoid. `false` here, and the word `true` sitting in a
		    title -- a search for "true" anywhere in the body would call this an awarded unlock.
		*/
		CHECK(!raNetJsonTrue("{\"Success\":false,\"Error\":\"is it true? no\"}", "Success"));
		/* A missing key is false, which is the safe direction: refused-and-logged, not awarded. */
		CHECK(!raNetJsonTrue("{\"Error\":\"x\"}", "Success"));
		/* And a string that merely starts with the letters is not the literal. */
		CHECK(!raNetJsonTrue("{\"Success\":\"true\"}", "Success"));
	}
}

int main(void) {
	test_rom_hash();
	test_url_encode();
	test_config();
	test_stream();
	test_patch();
	test_queue();

	printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
	       failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
