/*
    The ARM7 side of the WiFi probe -- a stock DSi-mode core with dsiwifi's FIFO installed.

    Almost all of this is the standard libnds ARM7 template, and it is here rather than
    reduced because the point of the probe is to test WiFi under *ordinary* conditions.
    Trimming the core to the minimum would introduce a second variable: a failure could then
    be the chip, or it could be something the template does that we removed. So the core
    stays conventional and the only unusual line is installWifiFIFO().

    The contrast with the real project is the whole reason this exists. Here the ARM7 is
    ours, idle, and running a normal main loop. Inside nds-bootstrap it belongs to the game,
    it is already carrying the cardengine's own hooks, and nothing may block. If WiFi cannot
    be made to work in this easy case, the hard case is not worth attempting.

    This file is part of nds-bootstrap and is licensed under the GPL-3.0,
    the same terms as the rest of the project.
*/

#include <nds.h>
#include <string.h>

#include "dsiwifi7.h"
#include "wifiprobe_fifo.h"

/*
    dsiwifi's own view of the firmware's WiFi slots. A private header of the library,
    included because we build it in-tree -- the same call made for rcheevos' rc_internal.h,
    and for the same reason: reading the real layout beats copying it and letting the copy
    rot.
*/
#include "wifi_card.h"

/*
    Report what the console actually has configured, before any attempt to connect.

    This exists because of how dsiwifi chooses an access point: it scans, and for each
    beacon it walks the firmware's slots looking for a matching SSID, skipping empty ones
    with a bare `continue`. A console with nothing configured therefore behaves exactly like
    a console whose WiFi chip never came up -- no association, no message, nothing to tell
    the two apart. For a probe whose entire job is to distinguish causes, that is the one
    ambiguity worth spending code to remove.

    The two tables are different generations and it matters which one is populated:

      slots 4-6, at NVRAM 0x1F400 -- the DSi-era config, WPA and WPA2 capable. These are
      what dsiwifi tries first, and the only ones that can give a modern network.

      slots 1-3, at the end of the user settings area -- the original DS config, WEP or
      open only. dsiwifi falls back to these, and on a 3DS they are what the
      "Nintendo DS Connections" settings screen writes.

    So the answer to "which WiFi do I need configured?" is visible in the result rather than
    guessed at: if the DSi slots come back empty, the console never wrote them, and no
    amount of chip debugging will help.
*/
static void report_wifi_slots(void) {
	static nvram_cfg     dsiCfg[3];
	static nvram_cfg_wep wepCfg[3];
	probeSlots           report;
	u32                  endAddr;
	int                  i;

	memset(&report, 0, sizeof(report));

	readFirmware(0x20, &endAddr, sizeof(u32));
	endAddr *= 8;

	readFirmware(0x1F400, dsiCfg, sizeof(dsiCfg));
	readFirmware(endAddr - 0x400, wepCfg, sizeof(wepCfg));

	for (i = 0; i < 3; i++) {
		/* The same three tests dsiwifi applies before it will consider a slot usable. */
		if (dsiCfg[i].ssid[0] && dsiCfg[i].wpa_mode != 0xFF && dsiCfg[i].slot_idx) {
			report.dsiSlots |= (1 << i);
			if (!report.firstSsid[0]) {
				u8 len = dsiCfg[i].ssid_len;
				if (len > 0x20) {
					len = 0x20;
				}
				memcpy(report.firstSsid, dsiCfg[i].ssid, len);
				report.firstWpaMode = dsiCfg[i].wpa_mode;
				report.firstWepMode = dsiCfg[i].wep_mode;
			}
		}
		if (wepCfg[i].ssid[0] && wepCfg[i].status != 0xFF && wepCfg[i].slot_idx) {
			report.wepSlots |= (1 << i);
		}
	}

	fifoSendDatamsg(WIFIPROBE_FIFO_CHANNEL, sizeof(report), (u8*)&report);
}

static volatile bool exitflag = false;

static void VcountHandler(void) {
	inputGetAndSend();
}

static void VblankHandler(void) {
}

static void powerButtonCB(void) {
	exitflag = true;
}

int main(void) {
	/*
	    The extended TWL I/O, opened the same way the reference ARM7 core does it. This is
	    what puts the WiFi SDIO block at 0x04004A00 in reach at all -- and it is the same
	    SCFG state nds-bootstrap keeps open for its own reasons, which is why the question
	    this probe answers is worth asking of nds-bootstrap afterwards.
	*/
	if (isDSiMode()) {
		REG_SCFG_ROM = 0x101;
		REG_SCFG_CLK = (BIT(0) | BIT(1) | BIT(2) | BIT(7) | BIT(8));
		REG_SCFG_EXT = 0x93FFFB06;
		*(vu16*)(0x04004012) = 0x1988;
		*(vu16*)(0x04004014) = 0x264C;
		*(vu16*)(0x04004C02) = 0x4000;   /* power button IRQ, for Unlaunch 1.3 */
	}

	*(vu16*)(0x04004700) |= BIT(13);     /* 48kHz sound/mic */

	dmaFillWords(0, (void*)0x04000400, 0x100);
	REG_SOUNDCNT |= SOUND_ENABLE;
	writePowerManagement(PM_CONTROL_REG,
	                     (readPowerManagement(PM_CONTROL_REG) & ~PM_SOUND_MUTE) | PM_SOUND_AMP);
	powerOn(POWER_SOUND);

	readUserSettings();
	ledBlink(0);

	irqInit();
	initClockIRQ();
	fifoInit();
	touchInit();

	SetYtrigger(80);

	installSoundFIFO();
	installSystemFIFO();

	irqSet(IRQ_VCOUNT, VcountHandler);
	irqSet(IRQ_VBLANK, VblankHandler);
	irqEnable(IRQ_VBLANK | IRQ_VCOUNT | IRQ_NETWORK);

	setPowerButtonCB(powerButtonCB);

	/*
	    Before the stack touches anything: say what the firmware has configured. If this
	    comes back empty, everything after it fails for a reason that is not the hardware's.
	*/
	report_wifi_slots();

	/* The one line that is not template. Everything dsiwifi does on this CPU hangs off it. */
	installWifiFIFO();

	while (!exitflag) {
		const u32 keys = REG_KEYINPUT;
		if (0 == (keys & (KEY_SELECT | KEY_START | KEY_L | KEY_R))) {
			exitflag = true;
		}
		swiWaitForVBlank();
	}
	return 0;
}
