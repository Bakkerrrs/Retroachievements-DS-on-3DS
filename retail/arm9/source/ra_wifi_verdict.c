/*
    Reading how the Atheros chip arrived out of dsiwifi's own narration.

    dsiwifi tells you everything that matters about the bring-up and exposes none of it:
    the chip string, the host-interest word at +0x58 that says whether firmware was already
    uploaded, the BMI version and the EEPROM version are all statics inside the ARM7 half of
    libs/dsiwifi. What crosses to the ARM9 is the printf text. So the text is what gets read.

    That is a coupling to a third-party library's log strings, and it is the reason for two
    deliberate choices. The verbatim log is the artifact -- the flags below only summarise
    it, and every conclusion in docs/retroachievements.md is meant to be checked against the
    file. And every string matched here is pinned by tools/ra_reader_test.c against the log
    a real console produced, so bumping the submodule fails a host test in seconds rather
    than reporting the wrong world after a play session.

    One thing worth being precise about, because it is easy to read the log the other way:
    dsiwifi resets the chip into its BMI bootloader and relaunches the firmware *every*
    time, warm or cold. `Reset cause`, `BMI version` and `Launching!` therefore prove
    nothing about how the chip arrived. Exactly one line does -- "needs firmware upload",
    printed only when the host-interest ready flag read zero -- and that is what coldStart
    is.

    This file is part of nds-bootstrap and is licensed under the GPL-3.0,
    the same terms as the rest of the project.
*/

#include "ra_wifi.h"

/*
    Guarded like everything else the switch adds, so that a default build of the launcher is
    byte-for-byte what it was: `RA_LAUNCHER_WIFI` off has to mean nothing was added, not
    "a few hundred bytes nobody calls". tools/ra_reader_test.c defines it and includes this
    file directly, the way it already does for the cardengine's sources.
*/
#if RA_LAUNCHER_WIFI

#include <string.h>

/*
    The strings, in one place, named for what they mean rather than for what they say. Kept
    as short as they can be while staying unambiguous: dsiwifi formats the chip name into
    several of these lines, so matching on the invariant part survives a different chip.
*/
#define RA_WIFI_SAY_CHIP       "Mfg "
#define RA_WIFI_SAY_COLD       "needs firmware upload"
#define RA_WIFI_SAY_BMI        "BMI version:"
#define RA_WIFI_SAY_LAUNCH     "Launching!"
#define RA_WIFI_SAY_FW_READY   "ready, handshaking"
#define RA_WIFI_SAY_WMI_READY  "fully initialized!"
#define RA_WIFI_SAY_BAD_MBOX   "bad mbox alloc"
/*
    Association and the end of the handshake used to arrive as IPC messages, which was
    better -- they were the driver's own signals rather than its prose. Step 3 hands
    FIFO_DSWIFI to dsiwifi's ARM9 half, because two owners of one channel is not a thing, so
    these two rungs are read out of the text like the rest.

    "Done auth" rather than "Added GTK": the GTK line is WPA2-only, and wmi_post_handshake()
    prints "Done auth" on the open and WEP paths too. A rung that silently cannot be reached
    on some networks is worse than a slightly weaker one.
*/
#define RA_WIFI_SAY_ASSOC      "WMI_CONNECT_EVENT"
#define RA_WIFI_SAY_AUTH_DONE  "Done auth"

void raWifiVerdictReset(raWifiVerdict* v) {
	memset(v, 0, sizeof(*v));
}

/*
    Pull the chip name out of "Mfg 02010271 Cid 0d000001 (AR6014)".

    Bounded on both ends and it simply gives up rather than guessing: the name is for the
    summary line on screen, and a summary that says nothing is better than one that says
    something wrong. The log has the full line either way.
*/
static void raWifiVerdictChip(raWifiVerdict* v, const char* line) {
	const char* open = strchr(line, '(');
	const char* close;
	size_t      length;

	if (!open) {
		return;
	}
	close = strchr(open + 1, ')');
	if (!close) {
		return;
	}
	length = (size_t)(close - open - 1);
	if (length == 0 || length >= sizeof(v->chip)) {
		return;
	}
	memcpy(v->chip, open + 1, length);
	v->chip[length] = 0;
}

void raWifiVerdictLine(raWifiVerdict* v, const char* line) {
	if (!line[0]) {
		return;
	}
	v->lines++;

	if (strstr(line, RA_WIFI_SAY_CHIP)) {
		v->chipSeen = 1;
		raWifiVerdictChip(v, line);
	}
	if (strstr(line, RA_WIFI_SAY_COLD)) {
		v->coldStart = 1;
	}
	if (strstr(line, RA_WIFI_SAY_BMI)) {
		v->bmiSeen = 1;
	}
	if (strstr(line, RA_WIFI_SAY_LAUNCH)) {
		v->firmwareLaunched = 1;
	}
	if (strstr(line, RA_WIFI_SAY_FW_READY)) {
		v->firmwareReady = 1;
	}
	if (strstr(line, RA_WIFI_SAY_WMI_READY)) {
		v->wmiReady = 1;
	}
	if (strstr(line, RA_WIFI_SAY_BAD_MBOX)) {
		v->mboxAllocFailed = 1;
	}
	if (strstr(line, RA_WIFI_SAY_ASSOC)) {
		v->associated = 1;
	}
	if (strstr(line, RA_WIFI_SAY_AUTH_DONE)) {
		v->linkReady = 1;
	}
}

void raWifiVerdictFlush(raWifiVerdict* v) {
	if (v->lineLength == 0) {
		return;
	}
	v->line[v->lineLength] = 0;
	raWifiVerdictLine(v, v->line);
	v->lineLength = 0;
}

/*
    Reassemble lines out of the FIFO's 59-byte chunks.

    A chunk can hold several newlines or none, and a line can be split across two chunks --
    which is not hypothetical: the strings this file looks for are 12 to 21 characters and
    the log has lines longer than a chunk. An over-long line is flushed where it is rather
    than dropped, because a truncated line still matches anything that fits in it, and
    losing the tail of one debug line is not worth losing the rest.
*/
void raWifiVerdictChunk(raWifiVerdict* v, const char* chunk) {
	int i;

	for (i = 0; chunk[i]; i++) {
		if (chunk[i] == '\n' || chunk[i] == '\r') {
			raWifiVerdictFlush(v);
			continue;
		}
		if (v->lineLength >= sizeof(v->line) - 1) {
			raWifiVerdictFlush(v);
		}
		v->line[v->lineLength++] = chunk[i];
	}
}

/*
    Highest rung the evidence supports, and only the evidence: each test is the thing that
    proves that rung, not an inference from the one after it. A run that reaches WMI but
    never associates reports 3, which is the useful answer.
*/
int raWifiVerdictStage(const raWifiVerdict* v) {
	if (v->patched) {
		return RA_WIFI_STAGE_PATCHED;
	}
	/*
	    r=unlocks answering is a rung of its own, and it is below the fetch rather than above it
	    because the fetch is what produces something a game runs. A run that knows the account's
	    unlocks but never got the set has got further than one that did neither, and less far than one
	    that staged definitions.
	*/
	if (v->unlocksKnown) {
		return RA_WIFI_STAGE_UNLOCKS;
	}
	/*
	    Reporting the queue is its own rung, and "there was nothing to report" reaches it: the rung
	    measures whether the pass over the queue completed, not whether the player earned anything.
	    Most boots earn nothing, and a ladder that read 11 on those would be describing a failure that
	    did not happen. See raWifiVerdict.submitDone.
	*/
	if (v->submitDone) {
		return RA_WIFI_STAGE_SUBMIT;
	}
	/*
	    r=startsession, below the award because the award is what the session is for. A run that started
	    a session and then could not report anything has got further than one that never started it.
	*/
	if (v->sessionOk) {
		return RA_WIFI_STAGE_SESSION;
	}
	if (v->identified) {
		return RA_WIFI_STAGE_IDENTIFIED;
	}
	if (v->loggedIn) {
		return RA_WIFI_STAGE_LOGGED_IN;
	}
	if (v->apiOk) {
		return RA_WIFI_STAGE_ANSWERED;
	}
	if (v->tcpOk) {
		return RA_WIFI_STAGE_CONNECTED;
	}
	if (v->dnsOk) {
		return RA_WIFI_STAGE_RESOLVED;
	}
	if (v->gotIp) {
		return RA_WIFI_STAGE_IP;
	}
	if (v->linkReady) {
		return RA_WIFI_STAGE_READY;
	}
	if (v->associated) {
		return RA_WIFI_STAGE_ASSOCIATED;
	}
	if (v->wmiReady) {
		return RA_WIFI_STAGE_WMI;
	}
	if (v->firmwareReady) {
		return RA_WIFI_STAGE_FIRMWARE;
	}
	if (v->chipSeen) {
		return RA_WIFI_STAGE_CHIP;
	}
	return RA_WIFI_STAGE_START;
}

/*
    How the chip arrived, in the three words that decide whether step two changed anything.

    "cold" is what the probe saw on this console and what the plan expected to be the
    project's biggest risk; it turned out not to matter, because dsiwifi's firmware-upload
    path is `#if 0` and BMI alone starts what is already there. "warm" would mean the flag
    was already set on arrival -- a different boot path, and interesting precisely because
    it would differ from the control.
*/
const char* raWifiVerdictArrival(const raWifiVerdict* v) {
	if (!v->chipSeen) {
		return "never answered";
	}
	if (v->coldStart) {
		return "cold";
	}
	return "warm";
}

#endif /* RA_LAUNCHER_WIFI */
