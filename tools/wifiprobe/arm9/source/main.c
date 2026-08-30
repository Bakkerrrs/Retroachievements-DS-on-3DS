/*
    Does WiFi work on this console at all, before any of it is nds-bootstrap's problem?

    This is step one of the ladder in docs/retroachievements.md, and it is deliberately not
    part of nds-bootstrap. It is an ordinary DSi-mode homebrew .nds: no cardengine, no
    injected code, no game running. If WiFi does not work *here*, on this exact console and
    this exact WiFi board, then it certainly will not work from inside a game's interrupt
    handler, and the answer is in without touching the loader.

    Three questions, in the order they can kill the idea:

      1. Does the Atheros chip come up? The AR6002/AR6013/AR6014 keeps no firmware in
         flash -- the Xtensa core's code is uploaded to RAM on every boot, normally by the
         system menu. Booting through ntrboot may not pass through it. Whether the chip
         arrives warm or cold is the highest risk in the whole plan, and dsiwifi's log is
         where it shows.

      2. Does it associate and get an address? WPA2 needs the Atheros path; if this falls
         back or fails, the only alternative is WEP, which routers stopped offering.

      3. Can it reach RetroAchievements over plain HTTP? Already known to work from a PC --
         `dorequest.php` answers on port 80 with the same JSON as HTTPS -- but "the server
         allows it" and "this console can do it" are different claims.

    Deliberately asks for no credentials. The request below logs in as a user that does not
    exist, because a well-formed `invalid_credentials` reply proves the whole path -- DNS,
    TCP, HTTP, and the API parsing our query -- without this program ever handling a real
    password. Over cleartext, that distinction is worth keeping.

    The log goes to a file on the SD card as well as the screen. Every hardware answer in
    this project so far has been read by photographing a hex viewer, and forty lines of
    stack log is where that stops being reasonable.

    This file is part of nds-bootstrap and is licensed under the GPL-3.0,
    the same terms as the rest of the project.
*/

#include <nds.h>
#include <fat.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "dsiwifi9.h"

#include "wifiprobe_fifo.h"

#define PROBE_LOG_PATH "/wifiprobe.log"

#define RA_HOST "retroachievements.org"
#define RA_PORT 80
/*
    A login for a user that does not exist. The reply is a 401 with a JSON body, which is a
    complete proof of reach: anything less specific -- a timeout, a Cloudflare page, an
    empty read -- fails differently and says so.
*/
#define RA_PATH "/dorequest.php?r=login&u=ndsbootstrap_probe&p=x"

/* Same shape as the snapshot's stages: it only moves forward, and each value is a place. */
#define STAGE_START      0
#define STAGE_INIT       1  /* DSiWifi_InitDefault() returned */
#define STAGE_ASSOCIATED 2  /* an IP address was assigned */
#define STAGE_RESOLVED   3  /* DNS answered for retroachievements.org */
#define STAGE_CONNECTED  4  /* TCP to port 80 */
#define STAGE_REQUESTED  5  /* the request was written */
#define STAGE_ANSWERED   6  /* the API replied, and the reply looks like the API */

static FILE*      logFile;
static int        stage = STAGE_START;
static probeSlots slots;
static volatile bool slotsKnown = false;

/*
    Everything goes to both the screen and the file. The screen is what you watch; the file
    is what you send back, because a stack log is too long to photograph and the
    interesting line is never the last one.
*/
static void probe_log(const char* fmt, ...) {
	char line[256];
	va_list args;

	va_start(args, fmt);
	vsniprintf(line, sizeof(line), fmt, args);
	va_end(args);

	iprintf("%s", line);
	if (logFile) {
		fputs(line, logFile);
		fflush(logFile);
	}
}

/*
    dsiwifi's own log, verbatim. This is the channel that answers question 1: firmware
    upload, BMI, WMI init and association all narrate themselves here, and the difference
    between "the chip was already warm" and "we had to boot it ourselves" is visible in
    these lines and nowhere else. So it is passed through unedited rather than summarised.
*/
static void wifi_log(const char* s) {
	iprintf("%s", s);
	if (logFile) {
		fputs(s, logFile);
		fflush(logFile);
	}
}

/*
    The ARM7's report on what the firmware has configured. Arrives once, before the stack
    starts, and it is what separates "the hardware failed" from "there was nothing to
    connect to" -- which look identical from here otherwise.
*/
static void slots_handler(int bytes, void* userdata) {
	if (bytes >= (int)sizeof(slots)) {
		fifoGetDatamsg(WIFIPROBE_FIFO_CHANNEL, sizeof(slots), (u8*)&slots);
		slotsKnown = true;
	}
}

static void probe_report_slots(void) {
	if (!slotsKnown) {
		probe_log("\x1b[33mARM7 sent no slot report\x1b[37m\n");
		return;
	}

	probe_log("DSi slots (WPA2-capable): %s%s%s\n",
	          (slots.dsiSlots & 1) ? "4 " : "",
	          (slots.dsiSlots & 2) ? "5 " : "",
	          (slots.dsiSlots & 4) ? "6 " : "");
	probe_log("DS slots  (WEP/open):     %s%s%s\n",
	          (slots.wepSlots & 1) ? "1 " : "",
	          (slots.wepSlots & 2) ? "2 " : "",
	          (slots.wepSlots & 4) ? "3 " : "");

	if (slots.dsiSlots) {
		probe_log("first DSi SSID: \x1b[36m%s\x1b[37m (wpa %u, wep %u)\n",
		          slots.firstSsid, slots.firstWpaMode, slots.firstWepMode);
	}

	/*
	    Said plainly, because it is the single most likely reason for a confusing result and
	    it is entirely fixable by the person holding the console.
	*/
	if (!slots.dsiSlots && !slots.wepSlots) {
		probe_log("\x1b[31mnothing configured for DS/DSi mode.\x1b[37m\n"
		          "set up an internet connection in the\n"
		          "console settings, then run this again.\n");
	} else if (!slots.dsiSlots) {
		probe_log("\x1b[33monly legacy DS slots: WEP or open only.\x1b[37m\n");
	}
}

static void probe_report_ip(u32 addr) {
	const u8* b = (const u8*)&addr;
	probe_log("\x1b[32mIP %u.%u.%u.%u\x1b[37m\n", b[0], b[1], b[2], b[3]);
}

/*
    Wait for something, reporting as it goes, and give up rather than hang. Every step below
    can fail by never completing, and a probe that hangs teaches nothing -- the whole point
    is to come back with a stage number.
*/
static bool wait_for_ip(int seconds) {
	int frames = seconds * 60;

	while (frames-- > 0) {
		const u32 addr = DSiWifi_GetIP();
		if (addr != 0 && addr != 0xFFFFFFFF) {
			probe_report_ip(addr);
			return true;
		}
		swiWaitForVBlank();
	}
	return false;
}

static int http_get(const char* host, const char* path, char* out, int outSize) {
	struct hostent*    he;
	struct sockaddr_in addr;
	char               request[320];
	int                sock;
	int                total = 0;

	he = gethostbyname(host);
	if (!he || !he->h_addr_list[0]) {
		probe_log("\x1b[31mDNS failed for %s\x1b[37m\n", host);
		return -1;
	}
	memcpy(&addr.sin_addr, he->h_addr_list[0], 4);
	probe_log("resolved %s\n", host);
	probe_report_ip(addr.sin_addr.s_addr);
	stage = STAGE_RESOLVED;

	sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0) {
		probe_log("\x1b[31msocket() failed\x1b[37m\n");
		return -1;
	}
	addr.sin_family = AF_INET;
	addr.sin_port   = htons(RA_PORT);

	if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
		probe_log("\x1b[31mconnect() to port %d failed\x1b[37m\n", RA_PORT);
		close(sock);
		return -1;
	}
	probe_log("connected, port %d\n", RA_PORT);
	stage = STAGE_CONNECTED;

	/*
	    A user agent is not optional: RetroAchievements requires one on every Connect API
	    call, and a request without it is the client's fault rather than the network's.
	*/
	siprintf(request,
	         "GET %s HTTP/1.1\r\n"
	         "Host: %s\r\n"
	         "User-Agent: nds-bootstrap-ra-probe/0.1\r\n"
	         "Connection: close\r\n"
	         "\r\n",
	         path, host);

	if (send(sock, request, strlen(request), 0) < 0) {
		probe_log("\x1b[31msend() failed\x1b[37m\n");
		close(sock);
		return -1;
	}
	probe_log("request sent\n");
	stage = STAGE_REQUESTED;

	while (total < outSize - 1) {
		const int got = recv(sock, out + total, outSize - 1 - total, 0);
		if (got <= 0) {
			break;
		}
		total += got;
	}
	out[total] = 0;
	close(sock);
	return total;
}

int main(void) {
	static char response[2048];
	int         got;

	defaultExceptionHandler();
	consoleDemoInit();

	iprintf("\x1b[33mnds-bootstrap RA WiFi probe\x1b[37m\n");
	iprintf("plain DSi mode, no game running\n\n");

	/*
	    The SD first, so the log has somewhere to go before anything interesting happens.
	    A probe that cannot write its log is still worth running -- it just has to be
	    photographed -- so this is reported and not treated as fatal.
	*/
	if (fatInitDefault()) {
		logFile = fopen(PROBE_LOG_PATH, "w");
	}
	if (!logFile) {
		iprintf("\x1b[33mno SD log; screen only\x1b[37m\n");
	} else {
		iprintf("logging to %s\n", PROBE_LOG_PATH);
	}

	/*
	    Ask the ARM7 what the firmware holds before anything else runs, so the answer is at
	    the top of the log where it belongs rather than inferred from a silence further down.
	*/
	fifoSetDatamsgHandler(WIFIPROBE_FIFO_CHANNEL, slots_handler, 0);
	{
		int frames = 60;
		while (!slotsKnown && frames-- > 0) {
			swiWaitForVBlank();
		}
	}
	probe_log("\n-- stage 0: what is configured --\n");
	probe_report_slots();

	probe_log("\n-- stage 1: bring the chip up --\n");
	DSiWifi_SetLogHandler(wifi_log);
	DSiWifi_InitDefault(WFC_CONNECT);
	stage = STAGE_INIT;

	probe_log("\n-- stage 2: associate --\n");
	if (!wait_for_ip(30)) {
		probe_log("\x1b[31mno IP after 30s\x1b[37m\n");
		goto done;
	}
	stage = STAGE_ASSOCIATED;

	probe_log("\n-- stage 3: reach the API over plain HTTP --\n");
	got = http_get(RA_HOST, RA_PATH, response, sizeof(response));
	if (got <= 0) {
		probe_log("\x1b[31mno response\x1b[37m\n");
		goto done;
	}
	probe_log("%d bytes back\n", got);

	/*
	    Checked for content, not just for bytes. A captive portal, a proxy or a Cloudflare
	    interstitial would all "succeed" at the socket level and mean nothing; the API's own
	    error code coming back is what proves we reached RetroAchievements and not something
	    that answered on its behalf.
	*/
	if (strstr(response, "invalid_credentials")) {
		probe_log("\x1b[32mthe API answered over plain HTTP\x1b[37m\n");
		stage = STAGE_ANSWERED;
	} else {
		probe_log("\x1b[31mreply is not the API\x1b[37m\n");
	}
	{
		const char* body = strstr(response, "\r\n\r\n");
		probe_log("body: %s\n", body ? body + 4 : response);
	}

done:
	probe_log("\n\x1b[33mreached stage %d of %d\x1b[37m\n", stage, STAGE_ANSWERED);
	probe_log(stage == STAGE_ANSWERED
	          ? "live unlocks are reachable from DSi mode.\n"
	          : "stopped here -- see the log above.\n");

	/*
	    The log stays open until you leave, deliberately. dsiwifi narrates asynchronously and
	    keeps talking after the last stage finishes -- the first run on hardware printed its
	    WMI_BSSINFO line, the one naming the access point and its security mode, *after* the
	    summary. Closing the file at the summary would have dropped exactly the lines that
	    say how the chip came up, which is the question the next step depends on.
	*/
	probe_log("\nSTART to exit.\n");

	while (1) {
		swiWaitForVBlank();
		scanKeys();
		if (keysDown() & KEY_START) {
			break;
		}
	}

	if (logFile) {
		fclose(logFile);
		logFile = 0;
	}
	return 0;
}
