/*
    What the ARM7 tells the ARM9 about the console's WiFi configuration.

    Only the ARM7 can call readFirmware(), and only the ARM9 has a screen and the SD card,
    so the one fact that decides how to read every later failure has to cross between them.
    One message, sent once, before the stack starts.

    This file is part of nds-bootstrap and is licensed under the GPL-3.0,
    the same terms as the rest of the project.
*/

#ifndef WIFIPROBE_FIFO_H
#define WIFIPROBE_FIFO_H

#include <nds/ndstypes.h>

#define WIFIPROBE_FIFO_CHANNEL FIFO_USER_01

typedef struct probeSlots {
	/*
	    Bit per slot. dsiSlots are the DSi-era entries at NVRAM 0x1F400 -- WPA and WPA2
	    capable, and the only ones that can reach a modern network. wepSlots are the
	    original DS entries, WEP or open only, which dsiwifi falls back to.

	    Both zero is the answer to a whole class of confusing failures: the console has no
	    WiFi configured for DS/DSi mode, and nothing downstream can work regardless of how
	    healthy the hardware is.
	*/
	u8   dsiSlots;
	u8   wepSlots;
	u8   firstWpaMode;
	u8   firstWepMode;
	char firstSsid[33];   /* of the first usable DSi slot, for confirmation by eye */
	u8   reserved[3];
} probeSlots;

#endif /* WIFIPROBE_FIFO_H */
