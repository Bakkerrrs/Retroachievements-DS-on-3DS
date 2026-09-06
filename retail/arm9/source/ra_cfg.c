/*
    Step 3c's half that needs no network: the RetroAchievements configuration file.

    Deliberately shaped like the one odelot ships for his MiSTer core -- `key=value`, `#` for
    comments, credentials at the top -- because that is the file this project's user already
    knows, and a format someone can copy from a working example is worth more than a tidier one
    they have to learn.

    ## The password lives in the file

    That is a decision on the record rather than an oversight: it is how odelot's works, and it
    was chosen knowingly after the alternative -- log in once, keep only the token, never write
    the password to the card -- was offered and declined. `r=login` therefore sends it in the
    clear on every boot, and the file sits readable on the SD.

    What this code does *not* do is put it anywhere else. It never goes in the log, which is a
    file that gets shared: see raConfigRedact(). Nothing else about the choice is this file's
    business.

    ## Everything unknown is tolerated, and everything recognised-but-unused says so

    odelot's file carries a dozen keys about popups, leaderboards and hardcore behaviour. Most
    of them describe an overlay this fork does not have yet. Rejecting them would make his file
    unusable here for no reason; accepting them silently would make a typo indistinguishable
    from a feature that is simply not built. So the ones we know about are counted as
    recognised-but-not-yet, and only genuinely unknown keys are reported.

    This file is part of nds-bootstrap and is licensed under the GPL-3.0,
    the same terms as the rest of the project.
*/

#include "ra.h"
#include "ra_wifi.h"

#if RA_LAUNCHER_WIFI

#include <stdio.h>
#include <string.h>

/*
    Keys this fork acts on today. Anything here changes behaviour.
*/
#define RA_CFG_USERNAME  "username"
#define RA_CFG_PASSWORD  "password"
#define RA_CFG_HARDCORE  "hardcore"
#define RA_CFG_DEBUG     "debug"
/*
    Not one of odelot's -- this fork's own, and it exists for testing rather than for players.

    `submit=0` reads the unlock queue and reports it, and then does not send it and does not clear
    it. That matters because an achievement is spent the moment it lands: the server returns it in
    r=unlocks, the scanner leaves it out of the block, and the cheapest repeatable test case on the
    card is gone. Contra 4's stage 1 was exactly that case.

    Defaults to 1, so a card without the key behaves as before.
*/
#define RA_CFG_SUBMIT    "submit"
/*
    Also this fork's own. `sync=0` skips the whole network ladder and plays from the cached set.

    The reason it is worth having is a measurement rather than a preference: a boot with no access point
    in range spends **forty seconds** failing to associate before the game starts, and a boot with one
    spends about fifteen doing work it already did yesterday. Neither is a cost worth paying to play.

    Nothing about detecting achievements needs the radio -- the cardengine never had it -- so with the
    ladder skipped the set comes from the per-game cache, unlocks are still detected, still notified and
    still queued, and one later boot with sync on drains the queue and refreshes the set. Syncing and
    playing become separate activities, which is what they always were.

    Defaults to 1.
*/
#define RA_CFG_SYNC      "sync"
/*
    This fork's own as well, and the only key here that changes nothing about what the loader does --
    only about what it says while doing it.

    `verbose_log=1` puts the whole ladder on the screen, which is what it did before this key existed:
    stage headings, the SCFG registers, every line dsiwifi narrates on its way up, a heap report
    between rungs and a twenty-line summary. That is the right screen for finding out why a boot
    failed and the wrong one for a person waiting to play.

    Defaults to **0**: a progress bar, the current step in ordinary words, and a few lines of essential
    result at the end. It governs the screen only -- the log file gets everything either way, because
    that file is how every hardware finding in this project arrived and a setting that silenced it
    would mean a reflash before anything could be diagnosed.
*/
#define RA_CFG_VERBOSE   "verbose_log"
/*
    Also this fork's own. `overlay=0` stops the on-screen notification from drawing.

    The overlay is the one part of this fork that negotiates with a running game for its hardware: it
    finds object VRAM and an OAM entry the game appears not to be using, and appears-not-to-be-using
    is an inference rather than a fact. Getting it wrong has broken a game's graphics and hung one.

    Off, an unlock is still detected, still queued, still submitted, and still listed in the in-game
    menu with the date it was earned. Only the popup goes -- which is the only part of the feature
    that can touch the game at all.

    Defaults to 1, because for every game measured so far it works and is worth having.
*/
#define RA_CFG_OVERLAY   "overlay"
/*
    And this fork's own again. `queue=0` stops an unlock from being written to sd:/ra_unlocks.txt.

    A diagnostic before it is a feature, and it exists because a game froze at the exact moment an
    achievement fired, with the popup already switched off -- so what was left at that instant was the
    id crossing to the ARM7 and the ARM7 writing a record to the SD card while the game runs. The card
    is the ARM7's, and it is also what the game is streaming from: this project has already watched
    that contention corrupt the queue file, and a blocking transaction there is a plausible way to
    stop a game dead.

    Off, an unlock is still detected, still counted, still shown, and still listed in the menu for the
    session -- and it is **lost** at power off, because the file is the only thing that survives a
    session. That is the cost, and it is why this defaults to 1.
*/
/*
    The allow-list the key above consults at level 1, by RetroAchievements GameID. See
    RA_OVERLAY_LISTED for why the popup is opt-in per game now.
*/
#define RA_CFG_OVERLAY_GAMES "overlay_games"

#define RA_CFG_QUEUE     "queue"

/*
    Keys odelot's file has that this fork parses and then does nothing with, because what they
    control does not exist here yet -- the overlay has no font, so there are no popups to show
    or hide, and there are no leaderboards. Listed so his file loads without complaint and a
    misspelling still gets one.
*/
static const char* const raCfgNotYet[] = {
	"show_challenge_show_popup",
	"show_challenge_hide_popup",
	"show_progress_popups",
	"show_progress_name",
	"show_leaderboards_updates",
	"show_leaderboards_submission",
	"force_hardcore",
	"multiline_desc",
	"list_desc_ticker",
	"list_hotkey",
	NULL,
};

static char* raCfgTrim(char* s) {
	char* end;

	while (*s == ' ' || *s == '\t') {
		s++;
	}
	end = s + strlen(s);
	while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
		end--;
	}
	*end = 0;
	return s;
}

static void raCfgCopy(char* dst, size_t size, const char* src) {
	size_t n = strlen(src);

	if (n > size - 1) {
		n = size - 1;
	}
	memcpy(dst, src, n);
	dst[n] = 0;
}

static bool raCfgFlag(const char* value) {
	/* `1` is yes in odelot's file. Anything else, including empty, is no. */
	return value[0] == '1' && value[1] == 0;
}

/*
    A single digit 0..max, for a key that is a ladder rather than a switch. Anything else -- empty, a
    second character, out of range -- is the default, and the default is what ships.

    Separate from raCfgFlag() rather than a generalisation of it: `1` means yes and everything else
    means no is odelot's rule for his keys and this file matches it deliberately. Only `queue` is a
    ladder, and only because it is an instrument.
*/
static u8 raCfgLevel(const char* value, u8 max, u8 dflt) {
	if (value[0] < '0' || value[0] > '0' + max || value[1] != 0) {
		return dflt;
	}
	return (u8)(value[0] - '0');
}

/*
    Whether the popup may draw this session. See RA_OVERLAY_LISTED for why the default is "no".

    Whole-number matching, deliberately: a substring search would let `1671` in the list turn the
    popup on for game 16710. Any non-digit separates, so `16710,3405`, `16710 3405` and
    `16710, 3405` all work and a stray space cannot silently drop an entry.

    Overflow saturates rather than wraps. A number longer than a u32 can hold is a typo, and a
    saturated value simply fails to match every real GameID -- which is the safe direction here,
    because the failure is "no popup" rather than "popup on the wrong game".
*/
int raOverlayAllowed(u8 level, const char* list, u32 gameId) {
	u32 value = 0;
	int haveDigits = 0;

	if (level == RA_OVERLAY_OFF) {
		return 0;
	}
	if (level >= RA_OVERLAY_ALWAYS) {
		return 1;
	}
	/* A ROM the server does not know has no id to be listed under. */
	if (gameId == 0 || list == 0) {
		return 0;
	}

	for (;; list++) {
		if (*list >= '0' && *list <= '9') {
			value = (value <= (0xFFFFFFFFu - 9) / 10)
			        ? (value * 10 + (u32)(*list - '0'))
			        : 0xFFFFFFFFu;
			haveDigits = 1;
			continue;
		}
		if (haveDigits && value == gameId) {
			return 1;
		}
		if (*list == 0) {
			return 0;
		}
		value = 0;
		haveDigits = 0;
	}
}

static bool raCfgKnownUnused(const char* key) {
	int i;

	for (i = 0; raCfgNotYet[i]; i++) {
		if (strcmp(key, raCfgNotYet[i]) == 0) {
			return true;
		}
	}
	return false;
}

bool raConfigRead(const char* path, raConfig* cfg) {
	char  line[192];
	FILE* file;

	memset(cfg, 0, sizeof(*cfg));
	/*
	    Sending is the default, so a card that has never heard of the key behaves as it did. Set after
	    the memset and before parsing, so `submit=0` can turn it off and nothing else can.
	*/
	cfg->submit  = 1;
	cfg->sync    = 1;
	cfg->overlay = RA_OVERLAY_LISTED;
	cfg->overlayGames[0] = 0;
	cfg->queue   = 1;
	/*
	    verboseLog is left at the memset's zero deliberately, and it is the one default in here that
	    is not "behave as before". The quiet screen is what a card with no opinion should get; the
	    verbose one is a diagnostic mode you ask for.
	*/

	file = fopen(path, "r");
	if (!file) {
		return false;
	}
	cfg->found = 1;

	while (fgets(line, sizeof(line), file)) {
		char* key;
		char* value;
		char* eq;

		key = raCfgTrim(line);
		if (key[0] == 0 || key[0] == '#') {
			continue;
		}

		eq = strchr(key, '=');
		if (!eq) {
			cfg->badLines++;
			continue;
		}
		*eq   = 0;
		value = raCfgTrim(eq + 1);
		key   = raCfgTrim(key);

		if (strcmp(key, RA_CFG_USERNAME) == 0) {
			raCfgCopy(cfg->username, sizeof(cfg->username), value);
		} else if (strcmp(key, RA_CFG_PASSWORD) == 0) {
			raCfgCopy(cfg->password, sizeof(cfg->password), value);
		} else if (strcmp(key, RA_CFG_HARDCORE) == 0) {
			cfg->hardcore = raCfgFlag(value);
		} else if (strcmp(key, RA_CFG_DEBUG) == 0) {
			cfg->debug = raCfgFlag(value);
		} else if (strcmp(key, RA_CFG_SUBMIT) == 0) {
			cfg->submit = raCfgFlag(value);
		} else if (strcmp(key, RA_CFG_SYNC) == 0) {
			cfg->sync = raCfgFlag(value);
		} else if (strcmp(key, RA_CFG_VERBOSE) == 0) {
			cfg->verboseLog = raCfgFlag(value);
		} else if (strcmp(key, RA_CFG_OVERLAY) == 0) {
			cfg->overlay = raCfgLevel(value, RA_OVERLAY_MAX, RA_OVERLAY_LISTED);
		} else if (strcmp(key, RA_CFG_OVERLAY_GAMES) == 0) {
			raCfgCopy(cfg->overlayGames, sizeof(cfg->overlayGames), value);
		} else if (strcmp(key, RA_CFG_QUEUE) == 0) {
			cfg->queue = raCfgLevel(value, RA_QUEUE_LEVEL_MAX, RA_QUEUE_LEVEL_FULL);
		} else if (raCfgKnownUnused(key)) {
			cfg->notYet++;
		} else {
			cfg->unknown++;
		}
	}
	fclose(file);

	cfg->usable = (cfg->username[0] != 0 && cfg->password[0] != 0);
	return true;
}

/*
    What may be said about the file out loud.

    The log is written to the SD card and then sent to whoever is reading the round -- that is
    the entire point of it -- so the password must not be in it, and neither must the token,
    which grants exactly the same power over the account. A length is enough to tell "the field
    is empty" from "the field is set", which is the only question a log needs to answer.
*/
const char* raConfigRedact(const char* secret) {
	static char shown[24];

	if (!secret || !secret[0]) {
		return "(empty)";
	}
	sniprintf(shown, sizeof(shown), "(set, %u chars)", (unsigned)strlen(secret));
	return shown;
}

#endif /* RA_LAUNCHER_WIFI */
