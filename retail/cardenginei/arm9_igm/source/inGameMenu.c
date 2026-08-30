#include "inGameMenu.h"

#include <nds/bios.h>
#include <nds/ndstypes.h>
#include <nds/input.h>
#include <nds/interrupts.h>
#include <nds/system.h>
#include <nds/arm9/background.h>
#include <nds/arm9/video.h>

#include "igm_text.h"
#include "locations.h"
#include "ra_wifi.h"   /* raPendingBlock -- the tally the launcher staged */
#include "cardengine_header_arm9.h"
#include "nds_header.h"
#include "tonccpy.h"

void DC_InvalidateRange(const void *base, u32 size);
void DC_FlushRange(const void *base, u32 size);

#ifndef B4DS
	#define MAX_BRIGHTNESS 5
#else
	#define MAX_BRIGHTNESS 4
#endif

static u8 bgBak[sizeof(igmText.font) * 4];
static u16 bgMapBak[0x300];
static u16 palBak[256];

// For RAM viewer, global so it's persistant
vu32 *address = (vu32*)0x02000000;

/*
    Ranges the ARM9 can actually read here. The RAM viewer's `address` is edited a hex
    digit at a time and then dereferenced with no check at all, so a single mistyped
    digit -- 0x52413153 instead of 0x027FEF10, say -- is a Data Abort and the red
    exception screen.

    Worse, `address` is a deliberately persistent global so the viewer reopens where you
    left it, which means a poisoned value faults again on every re-entry, before you can
    reach the keys to correct it. The only way out is rebooting the game.

    So an address outside every real region snaps back to the start of main RAM instead.
    The list is deliberately generous -- viewing I/O, VRAM, DSi WRAM and the extended RAM
    above 0x0C000000 are all legitimate things to do with this tool -- because the point
    is to catch a typo, not to police where you look.
*/
static const u32 ramViewerRanges[][2] = {
	{ 0x02000000, 0x03000000 },  /* main RAM */
	{ 0x03000000, 0x04000000 },  /* shared WRAM and DSi WRAM */
	{ 0x04000000, 0x04001100 },  /* I/O registers */
	{ 0x05000000, 0x05000800 },  /* palette RAM */
	{ 0x06000000, 0x07000000 },  /* VRAM, all banks */
	{ 0x07000000, 0x07000800 },  /* OAM */
	{ 0x08000000, 0x0A000000 },  /* GBA slot */
	{ 0x0C000000, 0x0E000000 },  /* the extended RAM a DSi and 3DS expose */
};

/*
    Called before every read and after every navigation step. One screen is 23 rows of
    16 bytes, so the whole of what is about to be displayed has to be inside a range,
    not just the first byte.
*/
static void clampAddress(void) {
	const u32 span = 23 * 0x10;
	unsigned int i;

	for (i = 0; i < sizeof(ramViewerRanges) / sizeof(ramViewerRanges[0]); i++) {
		if ((u32)address >= ramViewerRanges[i][0]
		 && (u32)address <= ramViewerRanges[i][1] - span) {
			return;
		}
	}

	address = (vu32*)0x02000000;
}

static bool arm7Ram = false;
static u8 arm7RamBak[0xC0];

#ifndef B4DS
static u16* vramBak = (u16*)INGAME_MENU_EXT_LOCATION+(0x18200/sizeof(u16));
static u16* bmpBuffer = (u16*)INGAME_MENU_EXT_LOCATION;

#define refreshRateCount 5
static const char* refreshRateText[refreshRateCount] = {"29.9 Hz", "44.9 Hz", "50 Hz", "59.9 Hz", "74.9 Hz"};
static int refreshRates[refreshRateCount] = {30000, 45000, 50, 60000, 75000};
static int refreshRate = 3;
#else
cardengineArm9* volatile ce9 = NULL;

static u16* vramBak = (u16*)INGAME_MENU_EXT_LOCATION_B4DS+(0x18200/sizeof(u16));
static u16* bmpBuffer = (u16*)INGAME_MENU_EXT_LOCATION_B4DS;
#endif

// Header for a 256x192 16 bit (RGBA 565) BMP
const static u8 bmpHeader[] = {
	0x42, 0x4D, 0x46, 0x80, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46, 0x00,
	0x00, 0x00, 0x38, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0xC0, 0x00,
	0x00, 0x00, 0x01, 0x00, 0x10, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x80,
	0x01, 0x00, 0x13, 0x0B, 0x00, 0x00, 0x13, 0x0B, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF8, 0x00, 0x00, 0xE0, 0x07,
	0x00, 0x00, 0x1F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

#define KEYS sharedAddr[5]

bool swiDelayEnabled = true;
void mySwiDelay(int delay) {
	if (!swiDelayEnabled) {
		return;
	}

	swiDelay(delay);
}

void SetBrightness(u8 screen, s8 bright) {
	u8 mode = 1;

	if (bright < 0) {
		mode = 2;
		bright = -bright;
	}
	if (bright > 31) {
		bright = 31;
	}
	*(vu16*)(0x0400006C + (0x1000 * screen)) = bright | (mode << 14);
}

void print(int x, int y, const unsigned char *str, FontPalette palette, bool main) {
	u16 *dst = (main ? BG_MAP_RAM(15) : BG_MAP_RAM_SUB(15)) + y * 0x20 + x;
	while(*str)
		*(dst++) = *(str++) | palette << 12;
}

void printCenter(int x, int y, const unsigned char *str, FontPalette palette, bool main) {
	u16 *dst = (main ? BG_MAP_RAM(15) : BG_MAP_RAM_SUB(15)) + y * 0x20 + x;
	const unsigned char *start = str;
	while(*str)
		str++;
	dst += (str - start) / 2;
	while(str != start)
		*(--dst) = *(--str) | palette << 12;
}

void printRight(int x, int y, const unsigned char *str, FontPalette palette, bool main) {
	u16 *dst = (main ? BG_MAP_RAM(15) : BG_MAP_RAM_SUB(15)) + y * 0x20 + x;
	const unsigned char *start = str;
	while(*str)
		str++;
	while(str != start)
		*(dst--) = *(--str) | palette << 12;
}

int printMsg(int y, const unsigned char *str, FontPalette palette, bool main) {
	u16 *dst = main ? BG_MAP_RAM(15) : BG_MAP_RAM_SUB(15);
	while(*str) {
		bool endFound = false;
		int lineLen = 0;
		if (str[0] == 0x20) {
			str++;
		}
		for (int i = 0; i < 0x20; i++) {
			endFound = str[i] == 0;
			if (endFound) {
				break;
			}
			lineLen++;
		}
		if (!endFound) {
			for (int i = 0x20; i >= 0; i--) {
				if (str[i] == 0x20) {
					lineLen = i;
					break;
				}
			}
		}
		if (lineLen > 0) {
			if (igmText.rtl) {
				for (int i = 0; i < lineLen; i++) {
					dst[(y * 0x20) + (0x20 - lineLen) + i] = *(str++) | palette << 12;
				}
			} else {
				for (int i = 0; i < lineLen; i++) {
					dst[(y * 0x20) + i] = *(str++) | palette << 12;
				}
			}
			y++;
		}
		if (y == 0x18) break;
	}
	return y;
}

void printChar(int x, int y, unsigned char c, FontPalette palette, bool main) {
	(main ? BG_MAP_RAM(15) : BG_MAP_RAM_SUB(15))[y * 0x20 + x] = c | palette << 12;
}

void printDec(int x, int y, u32 val, int digits, FontPalette palette, bool main) {
	u16 *dst = (main ? BG_MAP_RAM(15) : BG_MAP_RAM_SUB(15)) + y * 0x20 + x;
	for(int i = digits - 1; i >= 0; i--) {
		*(dst + i) = ('0' + (val % 10)) | palette << 12;
		val /= 10;
	}
}

void printHex(int x, int y, u32 val, u8 bytes, FontPalette palette, bool main) {
	u16 *dst = (main ? BG_MAP_RAM(15) : BG_MAP_RAM_SUB(15)) + y * 0x20 + x;
	for(int i = bytes * 2 - 1; i >= 0; i--) {
		*(dst + i) = ((val & 0xF) >= 0xA ? 'A' + (val & 0xF) - 0xA : '0' + (val & 0xF)) | palette << 12;
		val >>= 4;
	}
}

static void printTime(void) {
	while (!(sharedAddr[7] & 0x10000000)) { // Wait for time to be received
		while (REG_VCOUNT != 191) mySwiDelay(100);
		while (REG_VCOUNT == 191) mySwiDelay(100);
	}
	#ifndef B4DS
	#define timeYpos 3
	#else
	#define timeYpos 2
	#endif

	u8 hours = (u8)sharedAddr[7];
	u8 minutes = (u8)sharedAddr[8];
	printDec(0x20 - 6, 0x18 - timeYpos, hours, 2, FONT_LIGHT_BLUE, false);
	printChar(0x20 - 4, 0x18 - timeYpos, ':', FONT_LIGHT_BLUE, false);
	printDec(0x20 - 3, 0x18 - timeYpos, minutes, 2, FONT_LIGHT_BLUE, false);
}

#ifndef B4DS
static void printBattery(void) {
	while ((u8)sharedAddr[6] == 0) { // Wait for battery level to be received
		while (REG_VCOUNT != 191) mySwiDelay(100);
		while (REG_VCOUNT == 191) mySwiDelay(100);
	}
	u8 batteryLevel = (u8)sharedAddr[6];
	const char *bars = "\3\3";
	if (batteryLevel & BIT(7)) {
		bars = "\6\6";	// Charging
	} else {
		switch (batteryLevel) {
			default:
				break;
			case 0x1:
			case 0x3:
				bars = "\3\4";
				break;
			case 0x7:
				bars = "\3\5";
				break;
			case 0xB:
				bars = "\4\5";
				break;
			case 0xF:
				bars = "\5\5";
				break;
		}
	}
	print(0x20 - 4, 0x18 - 2, (const unsigned char *)bars, FONT_LIGHT_BLUE, false);
}
#endif

static void waitKeys(u16 keys) {
	// Prevent key repeat for 10 frames
	for(int i = 0; i < 10 && (KEYS & keys); i++) {
		while (REG_VCOUNT != 191) mySwiDelay(100);
		while (REG_VCOUNT == 191) mySwiDelay(100);
	}

	u32 status = sharedAddr[6]; // Battery, brightness, volume
	u8 hours = (u8)sharedAddr[7];
	u8 minutes = (u8)sharedAddr[8];

	do {
		while (REG_VCOUNT != 191) mySwiDelay(100);
		while (REG_VCOUNT == 191) mySwiDelay(100);
	} while(!(KEYS & keys) && sharedAddr[6] == status && (u8)sharedAddr[7] == hours && (u8)sharedAddr[8] == minutes);
}

void clearScreen(bool main) {
	toncset16(main ? BG_MAP_RAM(15) : BG_MAP_RAM_SUB(15), 0, 0x300);
}

bool boolQuestion(const unsigned char *str) {
	clearScreen(false);

	const int y = printMsg(0, str, FONT_WHITE, false) + 1;
	if (igmText.rtl) {
		printRight(0x20 - 1, y, igmText.aYes, FONT_WHITE, false);
		printRight(0x20 - 1, y + 1, igmText.bNo, FONT_WHITE, false);
	} else {
		print(0, y, igmText.aYes, FONT_WHITE, false);
		print(0, y + 1, igmText.bNo, FONT_WHITE, false);
	}

	do {
		while (REG_VCOUNT != 191) mySwiDelay(100);
		while (REG_VCOUNT == 191) mySwiDelay(100);
	} while(KEYS & KEY_A);

	waitKeys(KEY_A | KEY_B);

	if (KEYS & KEY_A) {
		do {
			while (REG_VCOUNT != 191) mySwiDelay(100);
			while (REG_VCOUNT == 191) mySwiDelay(100);
		} while(KEYS & KEY_A);
		return true;
	} else {
		do {
			while (REG_VCOUNT != 191) mySwiDelay(100);
			while (REG_VCOUNT == 191) mySwiDelay(100);
		} while(KEYS & KEY_B);
	}
	return false;
}

#define VRAM_x(bank) ((u16*)(0x6800000 + (0x0020000 * (bank))))
#define VRAM_x_CR(bank) (((vu8*)0x04000240)[bank])

static void screenshot(void) {
	// Try to find the safest bank to capture to
	u8 vramBank = 2;
	if((VRAM_D_CR & 1) == 0) {
		vramBank = 3;
	} else if((VRAM_C_CR & 1) == 0) {
		vramBank = 2;
	} else if((VRAM_B_CR & 7) == 0) {
		vramBank = 1;
	} else if((VRAM_A_CR & 7) == 0) {
		vramBank = 0;
	}

	// Use capture mode B if no banks are mapped for main engine
	u8 captureMode = DCAP_MODE_B;
	for(int i = 0; i <= 6; i++) {
		if(VRAM_x_CR(i) & 1)
			captureMode = DCAP_MODE_A;
	}

	u8 vramCr = VRAM_x_CR(vramBank);

	// Select bank
	u8 cursorPosition = vramBank;
	while(1) {
		// Configure VRAM
		VRAM_x_CR(vramBank) = vramCr; // LCD
		vramCr = VRAM_x_CR(cursorPosition);
		VRAM_x_CR(cursorPosition) = VRAM_ENABLE; // LCD
		vramBank = cursorPosition;

		clearScreen(false);

		toncset16(BG_MAP_RAM_SUB(15) + 0x20 * 9 + 5, '-', 20);
		printCenter(15, 10, igmText.selectBank, FONT_WHITE, false);
		printChar(15, 12, 'A' + vramBank, FONT_LIGHT_BLUE, false);
		toncset16(BG_MAP_RAM_SUB(15) + 0x20 * 13 + 5, '-', 20);

		FontPalette color = igmText.currentScreenshot == 50 ? FONT_RED : FONT_LIME;
		if(igmText.rtl) {
			printDec(6, 14, igmText.currentScreenshot, 2, color, false);
			printChar(8, 14, '/', color, false);
			printDec(9, 14, 50, 2, color, false);
			printRight(23, 14, igmText.count, 0, false);
		} else {
			print(6, 14, igmText.count, 0, false);
			printDec(19, 14, igmText.currentScreenshot, 2, color, false);
			printChar(21, 14, '/', color, false);
			printDec(22, 14, 50, 2, color, false);

		}

		waitKeys(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_A | KEY_B);

		if (KEYS & (KEY_UP | KEY_LEFT)) {
			if(vramBank > 0)
				cursorPosition--;
		} else if (KEYS & (KEY_DOWN | KEY_RIGHT)) {
			if(vramBank < 3)
				cursorPosition++;
		} else if(KEYS & KEY_A && igmText.currentScreenshot < 50) {
			break;
		} else if(KEYS & KEY_B) {
			VRAM_x_CR(vramBank) = vramCr;
			return;
		}
	}

	#ifdef B4DS
	codeJumpWord = ce9->prepareScreenshot;
	(*codeJump)();
	#else
	sharedAddr[4] = 0x50505353;
	while (sharedAddr[4] == 0x50505353) {
		while (REG_VCOUNT != 191) mySwiDelay(100);
		while (REG_VCOUNT == 191) mySwiDelay(100);
	}
	#endif

	// Backup VRAM bank
	tonccpy(vramBak, VRAM_x(vramBank), 0x18000);

	REG_DISPCAPCNT = DCAP_BANK(vramBank) | DCAP_SIZE(DCAP_SIZE_256x192) | DCAP_MODE(captureMode) | DCAP_ENABLE;
	while(REG_DISPCAPCNT & DCAP_ENABLE);

	tonccpy(bmpBuffer, bmpHeader, sizeof(bmpHeader));

	// ABGR 1555 -> RGB 565
	for (int i = 0; i < 256 * 192; i++) {
		u16 val = VRAM_x(vramBank)[i];
		VRAM_x(vramBank)[i] = ((val >> 10) & 31) | ((val & (31 << 5)) << 1) | ((val & 31) << 11);
	}

	// Write image data, upside down as that's how BMPs want it
	u16 *bmp = bmpBuffer + sizeof(bmpHeader) / sizeof(u16);
	for(int i = 191; i >= 0; i--) {
		tonccpy(bmp, VRAM_x(vramBank) + (i * 256), 256 * sizeof(u16));
		bmp += 256;
	}

	// Restore VRAM bank
	tonccpy(VRAM_x(vramBank), vramBak, 0x18000);
	VRAM_x_CR(vramBank) = vramCr;

	#ifdef B4DS
	codeJumpWord = ce9->saveScreenshot;
	(*codeJump)();
	#else
	sharedAddr[4] = 0x544F4853;
	while (sharedAddr[4] == 0x544F4853) {
		while (REG_VCOUNT != 191) mySwiDelay(100);
		while (REG_VCOUNT == 191) mySwiDelay(100);
	}
	DC_InvalidateRange(&igmText.currentScreenshot, 1);
	DC_InvalidateRange((char*)INGAME_MENU_EXT_LOCATION, 0x40000);
	#endif
}

static void manual(void) {
	#ifdef B4DS
	codeJumpWord = ce9->prepareManual;
	(*codeJump)();
	#else
	sharedAddr[4] = 0x4E414D50; // PMAN
	do {
		while (REG_VCOUNT != 191) mySwiDelay(100);
		while (REG_VCOUNT == 191) mySwiDelay(100);
	} while (sharedAddr[4] == 0x4E414D50);
	#endif

	while (1) {
		#ifdef B4DS
		codeJumpWord = ce9->readManual;
		(*codeJump1)(igmText.manualLine);

		print(0, 0, (unsigned char *)0x027FF200, FONT_WHITE, false);
		#else
		DC_InvalidateRange((unsigned char *)INGAME_MENU_EXT_LOCATION, 32 * 24);
		sharedAddr[0] = igmText.manualLine;
		sharedAddr[4] = 0x554E414D; // MANU
		do {
			while (REG_VCOUNT != 191) mySwiDelay(100);
			while (REG_VCOUNT == 191) mySwiDelay(100);
		} while (sharedAddr[4] == 0x554E414D);

		print(0, 0, (unsigned char *)INGAME_MENU_EXT_LOCATION, FONT_WHITE, false);
		#endif

		waitKeys(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_B);
		if(KEYS & KEY_UP) {
			igmText.manualLine -= (KEYS & KEY_R) ? 100 : 1;
		} else if(KEYS & KEY_DOWN) {
			igmText.manualLine += (KEYS & KEY_R) ? 100 : 1;
		} else if(KEYS & KEY_LEFT) {
			igmText.manualLine -= (KEYS & KEY_R) ? 1000 : 23;
			
		} else if(KEYS & KEY_RIGHT) {
			igmText.manualLine += (KEYS & KEY_R) ? 1000 : 23;
		} else if(KEYS & KEY_B) {
			break;
		}

		if(igmText.manualLine < 0)
			igmText.manualLine = 0;
		else if(igmText.manualLine > igmText.manualMaxLine - 23)
			igmText.manualLine = igmText.manualMaxLine - 23 > 0 ? igmText.manualMaxLine - 23 : 0;
	}

	#ifdef B4DS
	codeJumpWord = ce9->restorePreManual;
	(*codeJump)();
	#else
	sharedAddr[4] = 0x4E414D52; // RMAN
	do {
		while (REG_VCOUNT != 191) mySwiDelay(100);
		while (REG_VCOUNT == 191) mySwiDelay(100);
	} while (sharedAddr[4] == 0x4E414D52);
	#endif
}

static void drawCursor(u8 line) {
	u8 pos = igmText.rtl ? 0x1F : 0;
	// Clear other cursors
	for(int i = 0; i < 0x18; i++)
		BG_MAP_RAM_SUB(15)[i * 0x20 + pos] = 0;

	// Set cursor on the selected line
	BG_MAP_RAM_SUB(15)[line * 0x20 + pos] = igmText.rtl ? '<' : '>';
}

static void drawMainMenu(MenuItem *menuItems, int menuItemCount) {
	clearScreen(false);

	// Print labels
	for(int i = 0; i < menuItemCount; i++) {
		if(igmText.rtl)
			printRight(0x1D, i, igmText.menu[menuItems[i]], FONT_WHITE, false);
		else
			print(2, i, igmText.menu[menuItems[i]], FONT_WHITE, false);
	}

	// Print info
	print(1, 0x18 - 3, igmText.ndsBootstrap, FONT_LIGHT_GRAY, false);
	print(1, 0x18 - 2, igmText.version, FONT_LIGHT_GRAY, false);

	#ifndef B4DS
	// Print battery
	printChar(0x20 - 5, 0x18 - 2, '\2', FONT_LIGHT_BLUE, false);
	printChar(0x20 - 2, 0x18 - 2, '\7', FONT_LIGHT_BLUE, false);
	#endif
}

/* Two game codes, compared without pulling string.h into a cardengine. Both are NUL-terminated. */
static bool sameCode(const char* a, const char* b) {
	int i;

	for (i = 0; i <= RA_QUEUE_CODE; i++) {
		if (a[i] != b[i]) {
			return false;
		}
		if (a[i] == 0) {
			return true;
		}
	}
	return true;
}

/*
    A number right-aligned in `width` cells, without printDec()'s leading zeros.

    printDec() writes exactly the digits it is asked for, taken from the low end -- so a count of 101
    in two cells prints `01`, which is what the achievements header did on *Chrono Trigger*: a set of
    a hundred and one read as a set of one. It was never a counting fault, and it would have gone on
    reading plausibly wrong on every set past ninety-nine.

    Widening the field alone is not the fix, because printDec() pads with zeros: three cells turn a
    forty-five-achievement set into `045`. The screen is cleared at the top of every draw, so the
    leading cells are already blank and the number only has to be placed -- which is all this does.

    Clamped at `width` cells rather than overrunning: a value too large for the field loses its top
    digits, which is the old bug in miniature, but RA_VIEWER_MAX_ENTRIES is 128 and every caller here
    passes 3.
*/
/*
    Three cells is enough for every count this menu shows, and it is checked rather than believed:
    the field that was too small is exactly what this function exists to stop happening again.
*/
typedef char raCountFitsThreeCells[(RA_VIEWER_MAX_ENTRIES <= 999 && RA_QUEUE_MAX <= 99) ? 1 : -1];

static void raPrintNum(int x, int y, int width, u32 val, FontPalette palette) {
	int digits = 1;
	u32 rest   = val;

	while (rest >= 10) {
		rest /= 10;
		digits++;
	}
	if (digits > width) {
		digits = width;
	}
	printDec(x + width - digits, y, val, digits, palette, false);
}

/*
    RetroAchievements: what is earned and not yet sent.

    One line per game, three fields -- the game, how many of its unlocks are still owed, and how long
    the oldest has waited. Counts rather than a list of achievements, because achievement titles only
    exist in the staged definitions of the game that is *running*: a list could name the current game's
    unlocks and could only ever print bare ids for every other game, and the other games are exactly
    what this page exists to show. The player was already told which achievement by the notification,
    with its name, when it fired.

    The block is read where the bootloader put it, and its magic is what says a boot actually staged
    one. An unset window reads as whatever the previous occupant left, so "no magic" and "nothing
    pending" have to look different here -- the first is a boot that never looked.
*/
static void raPendingPage(void) {
	const raPendingBlock* b = (const raPendingBlock*)CARDENGINEI_ARM9_RA_PENDING_LOCATION;
	int line;
	int shown = 0;

	clearScreen(false);
	print(0, 0, igmText.raMenu[RA_MENU_SYNC_PENDING], FONT_WHITE, false);

	if (b->magic != RA_PENDING_MAGIC) {
		print(0, 2, (unsigned char*)"No queue was read this boot", FONT_LIGHT_GRAY, false);
	} else if (b->total + b->session == 0) {
		print(0, 2, (unsigned char*)"Nothing waiting to sync", FONT_LIME, false);
	} else {
		/*
		    Two cells, not one. The queue holds up to RA_QUEUE_MAX records and a single cell showed
		    the low digit of anything past nine -- twelve waiting read as two. Same defect as the
		    achievements header, one page away, found while fixing that one.
		*/
		raPrintNum(0, 2, 2, (u32)(b->total + b->session), FONT_WHITE);
		print(3, 2, (unsigned char*)"waiting to sync", FONT_WHITE, false);

		line = 4;
		for (int i = 0; i < b->games && line < 19; i++, line++) {
			/*
			    This session's unlocks belong to the running game, and they are not in the block's
			    counts: the launcher tallied the queue as it stood at boot, and everything earned
			    since then was written by the ARM7 afterwards. Folded in here rather than staged,
			    because the number changes while the menu is closed.
			*/
			const int mine  = (b->thisCode[0] && sameCode(b->game[i].code, b->thisCode));
			const int count = b->game[i].count + (mine ? b->session : 0);

			print(1, line, (unsigned char*)b->game[i].title, FONT_WHITE, false);
			printDec(15, line, count, 2, FONT_LIGHT_BLUE, false);
			if (mine)
				shown = 1;

			/*
			    Days, not a date. Five characters cannot tell 08/09 the ninth of August from the
			    eighth of September, and what this column is for is how long something has been
			    stuck. The launcher does the subtraction at boot -- it has a real clock, which in
			    here nothing does: sharedAddr[7]/[8] carry hours and minutes and no date at all.
			*/
			if (b->game[i].waitDays == 0)
				printRight(31, line, (unsigned char*)"today", FONT_LIGHT_GRAY, false);
			else if (b->game[i].waitDays == 1)
				printRight(31, line, (unsigned char*)"yesterday", FONT_LIGHT_GRAY, false);
			else {
				printDec(24, line, b->game[i].waitDays, 3, FONT_LIGHT_GRAY, false);
				print(28, line, (unsigned char*)"days", FONT_LIGHT_GRAY, false);
			}
		}

		/*
		    The running game earned its first unlocks this session, so the queue had no row for it at
		    boot and the loop above drew none. Without this the most common case of all -- a fresh
		    game, one achievement, open the menu to check -- would show an empty page.
		*/
		if (!shown && b->session && b->thisTitle[0] && line < 19) {
			print(1, line, (unsigned char*)b->thisTitle, FONT_WHITE, false);
			printDec(15, line, b->session, 2, FONT_LIGHT_BLUE, false);
			printRight(31, line, (unsigned char*)"today", FONT_LIGHT_GRAY, false);
			line++;
		}

		/* Said rather than folded away: a screen that under-reports what is owed is worse. */
		if (b->dropped) {
			raPrintNum(1, line + 1, 2, b->dropped, FONT_RED);
			print(4, line + 1, (unsigned char*)"more game(s) not shown", FONT_RED, false);
			line++;
		}
		if (b->unnamed) {
			raPrintNum(1, line + 1, 2, b->unnamed, FONT_LIGHT_GRAY);
			print(4, line + 1, (unsigned char*)"of unknown origin", FONT_LIGHT_GRAY, false);
		}
	}

	print(0, 21, (unsigned char*)"Sent on the next boot with wifi", FONT_DARKER_GRAY, false);
	print(0, 23, igmText.bNo, FONT_WHITE, false);

	waitKeys(KEY_B);
}

/*
    RetroAchievements: this game's set, earned and still to earn.

    Read through the index cardenginei_arm9_ra built at init, never by parsing the block -- see
    CARDENGINEI_ARM9_RA_VIEWER_LOCATION for why that is not a preference. The offsets are relative
    to the definitions text, which is the one address both binaries can name.

    Two levels, because 32 columns cannot hold a title, its points and its description at once. The
    list gives one line each -- a mark, the title, the points -- and A opens the description of the
    line under the cursor. That split is also what keeps the page useful on a large set: forty
    achievements are eight screens of one line and forty screens of three.
*/
static void raAchievementsPage(void) {
	const raViewerBlock* const v = (const raViewerBlock*)CARDENGINEI_ARM9_RA_VIEWER_LOCATION;
	const char* const          text = (const char*)(CARDENGINEI_ARM9_RA_DEFS_LOCATION
	                                                + CARDENGINEI_ARM9_RA_DEFS_HEADER);
	/* Rows 3 to 21 hold the list, so nineteen entries a screen. */
	const int rows = 19;
	int       top = 0;
	int       cursor = 0;

	if (v->magic != RA_VIEWER_MAGIC || v->count == 0) {
		clearScreen(false);
		print(1, 0, igmText.raMenu[RA_MENU_ACHIEVEMENTS], FONT_WHITE, false);
		/*
		    Two states and they are not the same. No magic means this boot staged nothing -- no
		    network, or a game the server does not know. A magic with no entries would mean a set
		    that arrived empty, which is what an unsupported ROM's own set looks like.
		*/
		print(1, 2, (v->magic == RA_VIEWER_MAGIC)
		            ? (unsigned char*)"This game has no achievements"
		            : (unsigned char*)"No set was staged this boot", FONT_LIGHT_GRAY, false);
		print(1, 23, igmText.bNo, FONT_WHITE, false);
		waitKeys(KEY_B);
		return;
	}

	while (1) {
		int i;

		clearScreen(false);
		/*
		    Column 1, not 0, and the header is the reason this note is here twice. drawCursor()
		    clears column 0 of every row to erase the previous caret, so a title printed at 0 loses
		    its first letter the moment the cursor is drawn -- which on hardware read `chievements`,
		    and `02 of 45` as `2 of 45`. Nothing on this page may start at column 0.
		*/
		print(1, 0, igmText.raMenu[RA_MENU_ACHIEVEMENTS], FONT_WHITE, false);
		/*
		    Three cells each, because RA_VIEWER_MAX_ENTRIES is 128 and two cells silently showed the
		    low two digits of anything larger -- see raPrintNum().
		*/
		raPrintNum(1, 1, 3, v->earned, FONT_LIME);
		print(5, 1, (unsigned char*)"of", FONT_LIGHT_GRAY, false);
		raPrintNum(8, 1, 3, v->count, FONT_WHITE);
		print(12, 1, (unsigned char*)"earned", FONT_LIGHT_GRAY, false);
		if (v->queued) {
			raPrintNum(19, 1, 3, v->queued, FONT_RED);
			print(23, 1, (unsigned char*)"sync", FONT_LIGHT_GRAY, false);
		}
		/*
		    How far through the set this account is, at the right-hand end of the same line.

		    **Queued counts.** An achievement that has fired but not been sent is earned -- it is the
		    same thing the list marks with a star -- and a percentage that ignored it would fall
		    behind the stars on the page under it. The two counts are disjoint in practice: QUEUED is
		    this console's word for an unlock, EARNED is the server's answer, and an unlock crosses
		    from one to the other on the boot that submits it.

		    Clamped at 100 rather than trusted: the counts come from a block another binary wrote.
		*/
		{
			const u32 done  = (u32)v->earned + (u32)v->queued;
			u32       percent = v->count ? (done * 100) / v->count : 0;

			if (percent > 100) {
				percent = 100;
			}
			raPrintNum(28, 1, 3, percent, FONT_LIGHT_BLUE);
			print(31, 1, (unsigned char*)"%", FONT_LIGHT_BLUE, false);
		}

		for (i = 0; i < rows && top + i < v->count; i++) {
			const raViewerEntry* const e = &v->entry[top + i];
			const int                  earned = (e->flags & RA_VIEWER_EARNED) != 0;
			const int                  queued = (e->flags & RA_VIEWER_QUEUED) != 0;

			/*
			    A mark rather than a colour alone: the two palettes are close enough on a lit DS
			    screen that a photograph of this page could not settle which row was which, and this
			    project reads a lot of photographs.

			    Column 0 is not available for it. drawCursor() writes the caret there and *clears
			    that column on every row* to erase the previous one, so a mark at 0 would survive
			    exactly until the cursor moved.
			*/
			/*
			    Three states, and the middle one is the one a player actually wants: earned on the
			    server, earned and still in the queue, not earned. Queued keeps the star because it
			    *is* earned -- what it is missing is the server's acknowledgement, and red says that
			    without pretending it did not happen.
			*/
			print(1, 3 + i, (unsigned char*)((earned || queued) ? "*" : "-"),
			      earned ? FONT_LIME : (queued ? FONT_RED : FONT_DARKER_GRAY), false);
			if (e->titleOff) {
				print(3, 3 + i, (unsigned char*)(text + e->titleOff),
				      earned ? FONT_LIGHT_GRAY : FONT_WHITE, false);
			}
			if (e->pointsOff) {
				printRight(31, 3 + i, (unsigned char*)(text + e->pointsOff),
				           FONT_LIGHT_BLUE, false);
			}
		}
		drawCursor((u8)(3 + cursor));
		/*
		    One line rather than two, because the row it used to take is a row of the list -- and on
		    a 24-row screen with forty-five achievements to show, a row is worth more than a second
		    hint. Left and right page through, which is the only way a set this size is walkable.
		*/
		print(1, 23, (unsigned char*)"A: OK / B: Back", FONT_WHITE, false);

		waitKeys(KEY_A | KEY_B | KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);
		if (KEYS & KEY_B) {
			return;
		}
		if (KEYS & KEY_UP) {
			if (cursor > 0) {
				cursor--;
			} else if (top > 0) {
				top--;
			}
			continue;
		}
		if (KEYS & KEY_DOWN) {
			if (cursor < rows - 1 && top + cursor + 1 < v->count) {
				cursor++;
			} else if (top + rows < v->count) {
				top++;
			}
			continue;
		}
		/*
		    A page at a time. Clamped rather than wrapped, and the cursor is pulled back to the last
		    real row on the final page -- a caret sitting on a blank line below the set would be the
		    page inviting a press that does nothing.
		*/
		if (KEYS & KEY_LEFT) {
			top = (top > rows) ? top - rows : 0;
			continue;
		}
		if (KEYS & KEY_RIGHT) {
			if (top + rows < v->count) {
				top += rows;
				if (top + cursor >= v->count) {
					cursor = v->count - top - 1;
				}
			}
			continue;
		}

		/* A: the description of the line under the cursor, on a page of its own. */
		{
			const raViewerEntry* const e = &v->entry[top + cursor];

			clearScreen(false);
			if (e->titleOff) {
				print(1, 0, (unsigned char*)(text + e->titleOff), FONT_WHITE, false);
			}
			if (e->pointsOff) {
				print(1, 2, (unsigned char*)(text + e->pointsOff), FONT_LIGHT_BLUE, false);
				print(6, 2, (unsigned char*)"points", FONT_LIGHT_GRAY, false);
			}
			/*
			    The same three states as the list, in the same place, so moving between the two
			    pages does not move the fact. `(sync pending)` rather than a second word for earned:
			    it is the *sending* that is outstanding, and saying so is what stops a player
			    wondering whether the achievement itself is in doubt.
			*/
			if (e->flags & RA_VIEWER_EARNED) {
				printRight(31, 2, (unsigned char*)"earned", FONT_LIME, false);
			} else if (e->flags & RA_VIEWER_QUEUED) {
				printRight(31, 2, (unsigned char*)"(sync pending)", FONT_RED, false);
			}
			/*
			    When it was earned, under the description rather than beside the status: the status
			    is one word and this is a sentence, and a line of its own is the only place a date
			    and a time both fit on a 32-column screen.

			    Printed only when there is one, and there are three ways for there not to be. The
			    account does not hold it, which the status line above has already said. It was earned
			    during *this* session, where the flag flips while the game runs and nothing in that
			    context has a date -- only hours and minutes on sharedAddr. Or the block filled and
			    the date was the first field dropped, which is what raPatchWriteEarned() trims first.

			    Nothing is printed for any of them, deliberately: an empty row says less than a wrong
			    one, and "unknown" beside an achievement a player is looking at would invite the
			    question of what else is unknown about it.
			*/
			if (e->when) {
				print(1, 10, (unsigned char*)"Earned", FONT_LIGHT_GRAY, false);
				raPrintNum(8, 10, 4, RA_WHEN_YEAR(e->when), FONT_WHITE);
				print(12, 10, (unsigned char*)"-", FONT_DARKER_GRAY, false);
				printDec(13, 10, RA_WHEN_MONTH(e->when), 2, FONT_WHITE, false);
				print(15, 10, (unsigned char*)"-", FONT_DARKER_GRAY, false);
				printDec(16, 10, RA_WHEN_DAY(e->when), 2, FONT_WHITE, false);
				printDec(20, 10, RA_WHEN_HOUR(e->when), 2, FONT_WHITE, false);
				print(22, 10, (unsigned char*)":", FONT_DARKER_GRAY, false);
				printDec(23, 10, RA_WHEN_MINUTE(e->when), 2, FONT_WHITE, false);
			}
			if (e->descOff) {
				/*
				    Wrapped by hand at 30 columns, because print() does not wrap and a description
				    is up to 64 characters. 30 rather than 32: the text starts at column 1, since
				    column 0 belongs to drawCursor() everywhere on this page, and the last column is
				    left clear so a full line does not touch the bezel. Broken at a space where
				    there is one within reach, so a word is not cut in half.
				*/
				const char* d = text + e->descOff;
				int         line;

				for (line = 0; line < 4 && *d; line++) {
					unsigned char row[31];
					int           n = 0;
					int           cut;

					while (n < 30 && d[n]) {
						n++;
					}
					cut = n;
					if (d[n]) {
						while (cut > 0 && d[cut] != ' ') {
							cut--;
						}
						if (cut == 0) {
							cut = n;   /* one long word: cut it rather than loop forever */
						}
					}
					for (n = 0; n < cut; n++) {
						row[n] = (unsigned char)d[n];
					}
					row[cut] = 0;
					print(1, 5 + line, row, FONT_LIGHT_GRAY, false);
					d += cut;
					while (*d == ' ') {
						d++;
					}
				}
			}
			print(1, 23, igmText.bNo, FONT_WHITE, false);
			waitKeys(KEY_B);
		}
	}
}

/*
    Which mode this session is in, and when it is not the one the player asked for, why.

    A line in the folder rather than a page of its own: it is one fact, and a page for one fact is a
    button press charged for nothing. It sits above the folder's own name so the two read as one
    footer -- what this is, and what it is doing right now.

    **The reason is the half that earns it.** "Softcore" alone answers a question nobody was asking;
    a player who set `hardcore=1` and is looking at this screen wants to know what took it away, and
    without this the only answer available is to power off, take the card out and read
    `ra_wifi_launcher.log` on a PC.

    The `hardcore` case says what changed rather than only what it is called, because the visible
    consequence of hardcore in this menu is two pages away in the RAM viewer, and a player who finds
    that refusal first should not have to guess which setting caused it.
*/
static void raSessionLine(int row) {
	const raSessionBlock* const s = (const raSessionBlock*)CARDENGINEI_ARM9_RA_SESSION_LOCATION;

	if (s->magic != RA_SESSION_MAGIC) {
		/*
		    Same distinction the pending page draws, and for the same reason: no launcher told this
		    boot anything, which is a different state from being told softcore.
		*/
		print(1, row, (unsigned char*)"No session this boot", FONT_DARKER_GRAY, false);
	} else if (s->hardcore) {
		print(1, row, (unsigned char*)"Hardcore -- RAM editing locked", FONT_LIME, false);
	} else if (s->refusal == RA_REFUSED_CHEATS) {
		/*
		    Red, although enabling cheats is a deliberate act and not an error. What is not
		    deliberate is the consequence, and a player who turned on one cheat months ago is
		    exactly the one who has not connected it to the mode this session is running in.
		*/
		print(1, row, (unsigned char*)"Softcore -- cheats are on", FONT_RED, false);
	} else {
		print(1, row, (unsigned char*)"Softcore", FONT_LIGHT_GRAY, false);
	}
}

/*
    The folder. Two entries today, and it is a folder rather than a page so that forcing a sync or
    reading the launcher's log have somewhere to go that is not the root menu.
*/
static void raMenu(void) {
	int cursor = 0;

	while (1) {
		clearScreen(false);
		print(2, 0, igmText.raMenu[RA_MENU_SYNC_PENDING], FONT_WHITE, false);
		print(2, 1, igmText.raMenu[RA_MENU_ACHIEVEMENTS], FONT_WHITE, false);
		raSessionLine(0x18 - 5);
		/*
		    The folder names itself down here, where the main menu names the program, rather than as
		    a header above its own items -- which read as the same page twice on the way to them.
		*/
		print(1, 0x18 - 3, (unsigned char*)"RetroAchievements", FONT_LIGHT_GRAY, false);
		print(0, 23, igmText.bNo, FONT_WHITE, false);
		drawCursor((u8)cursor);

		waitKeys(KEY_A | KEY_B | KEY_UP | KEY_DOWN);
		if (KEYS & KEY_B)
			return;
		if (KEYS & KEY_UP) {
			if (cursor > 0)
				cursor--;
			continue;
		}
		if (KEYS & KEY_DOWN) {
			if (cursor < RA_MENU_ACHIEVEMENTS)
				cursor++;
			continue;
		}
		if (cursor == RA_MENU_SYNC_PENDING)
			raPendingPage();
		else
			raAchievementsPage();
	}
}

static void optionsMenu(s32 *mainScreen, u32 consoleModel) {
	OptionsItem optionsItems[8];
	int optionsItemCount = 0;
	optionsItems[optionsItemCount++] = OPTIONS_MAIN_SCREEN;
	#ifndef B4DS
	if(consoleModel < 2) // Not on 3DS
	#endif
		optionsItems[optionsItemCount++] = OPTIONS_BRIGHTNESS;
	#ifndef B4DS
	optionsItems[optionsItemCount++] = OPTIONS_VOLUME;
	optionsItems[optionsItemCount++] = OPTIONS_REFRESH_RATE;
	optionsItems[optionsItemCount++] = OPTIONS_CLOCK_SPEED;
	optionsItems[optionsItemCount++] = OPTIONS_VRAM_MODE;
	#endif

	bool mainScreenChanged = false;

	u8 cursorPosition = 0;
	while(1) {
		clearScreen(false);

		// Print options
		for(int i = 0; i < optionsItemCount; i++) {
			const unsigned char *optionValue = NULL;
			int optionPercent = 0;
			bool isString = true;
			switch(optionsItems[i]) {
				case OPTIONS_MAIN_SCREEN:
					optionValue = igmText.optionsValues[*mainScreen];
					break;
				case OPTIONS_BRIGHTNESS:
					optionPercent = (u8)(sharedAddr[6] >> 8) * 100 / MAX_BRIGHTNESS;
					isString = false;
					break;
				#ifndef B4DS
				case OPTIONS_VOLUME:
					optionPercent = (u8)(sharedAddr[6] >> 16) * 100 / 31;
					isString = false;
					break;
				case OPTIONS_REFRESH_RATE:
					optionValue = (unsigned char*)refreshRateText[refreshRate];
					break;
				case OPTIONS_CLOCK_SPEED:
					optionValue = igmText.optionsValues[3 + ((REG_SCFG_CLK == 0 ? scfgClkBak : REG_SCFG_CLK) & 1)];
					break;
				case OPTIONS_VRAM_MODE:
					optionValue = igmText.optionsValues[5 + (((REG_SCFG_EXT == 0 ? scfgExtBak : REG_SCFG_EXT) & BIT(13)) >> 13)];
					break;
				#endif
			}

			int digits = optionPercent == 100 ? 3 : (optionPercent >= 10 ? 2 : 1);
			if(igmText.rtl) {
				printRight(0x1D, i, igmText.optionsLabels[optionsItems[i]], FONT_WHITE, false);
				if(isString) {
					print(0, i, optionValue, FONT_WHITE, false);
				} else {
					printDec(0, i, optionPercent, digits, FONT_WHITE, false);
					printChar(0 + digits, i, '%', FONT_WHITE, false);
				}
			} else {
				print(2, i, igmText.optionsLabels[optionsItems[i]], FONT_WHITE, false);
				if(isString) {
					printRight(0x1E, i, optionValue, FONT_WHITE, false);
				} else {
					printDec(0x1E - digits, i, optionPercent, digits, FONT_WHITE, false);
					printChar(0x1E, i, '%', FONT_WHITE, false);
				}
			}
		}
		drawCursor(cursorPosition);
		printMsg(17, igmText.optionsDescriptions[cursorPosition], FONT_WHITE, false);

		waitKeys(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_B | KEY_SELECT);

		if (KEYS & KEY_UP) {
			if(cursorPosition > 0)
				cursorPosition--;
			else
				cursorPosition = optionsItemCount - 1;
		} else if (KEYS & KEY_DOWN) {
			if(cursorPosition < (optionsItemCount - 1))
				cursorPosition++;
			else
				cursorPosition = 0;
		} else if (KEYS & (KEY_LEFT | KEY_RIGHT)) {
			switch(optionsItems[cursorPosition]) {
				case OPTIONS_MAIN_SCREEN:
					(KEYS & KEY_LEFT) ? (*mainScreen)-- : (*mainScreen)++;
					if(*mainScreen > 2)
						*mainScreen = 0;
					else if(*mainScreen < 0)
						*mainScreen = 2;
					mainScreenChanged = true;
					break;
				case OPTIONS_BRIGHTNESS:
				{
					u8 brightness = (u8)(sharedAddr[6] >> 8);
					if(KEYS & KEY_LEFT && brightness > 0)
						brightness--;
					else if (KEYS & KEY_RIGHT && brightness < MAX_BRIGHTNESS)
						brightness++;

					sharedAddr[0] = brightness;
					sharedAddr[4] = 0x4554494C; // LITE
					while(sharedAddr[4] == 0x4554494C) {
						while (REG_VCOUNT != 191) mySwiDelay(100);
						while (REG_VCOUNT == 191) mySwiDelay(100);
					}
					break;
				}
				#ifndef B4DS
				case OPTIONS_VOLUME:
				{
					u8 volume = (u8)(sharedAddr[6] >> 16);
					if(KEYS & KEY_LEFT && volume > 0)
						volume--;
					else if (KEYS & KEY_RIGHT && volume < 31)
						volume++;

					sharedAddr[0] = volume;
					sharedAddr[4] = 0x554C4F56; // VOLU
					while(sharedAddr[4] == 0x554C4F56) {
						while (REG_VCOUNT != 191) mySwiDelay(100);
						while (REG_VCOUNT == 191) mySwiDelay(100);
					}
					break;
				}
				case OPTIONS_REFRESH_RATE:
					const int prevRefreshRate = refreshRate;
					if (KEYS & KEY_LEFT) {
						refreshRate--;
						if (refreshRate < 0) refreshRate = 0;
					} else {
						refreshRate++;
						if (refreshRate == refreshRateCount) refreshRate = refreshRateCount-1;
					}
					if (refreshRate != prevRefreshRate) {
						sharedAddr[0] = refreshRates[refreshRate];
						sharedAddr[4] = 0x41535046; // FPSA
						while(sharedAddr[4] == 0x41535046) {
							while (REG_VCOUNT != 191) mySwiDelay(100);
							while (REG_VCOUNT == 191) mySwiDelay(100);
						}
						if (sharedAddr[0] == 0xFFFFFFFF) {
							refreshRate = prevRefreshRate;
						} else {
							DC_FlushRange(&refreshRate, 4);
						}
					}
					break;
				case OPTIONS_CLOCK_SPEED:
					REG_SCFG_CLK ^= 1;
					if (waitSysCyclesLoc) {
						if (waitSysCyclesLoc[0] == 0xE92D4008) {
							waitSysCyclesLoc[1] = (REG_SCFG_CLK & BIT(1)) ? 0xE1A00100 : 0xE1A00080;
						} else {
							u16* offsetThumb = (u16*)waitSysCyclesLoc;
							offsetThumb[1] = (REG_SCFG_CLK & BIT(1)) ? 0x0080 : 0x0040;
						}
					}
					break;
				case OPTIONS_VRAM_MODE:
					REG_SCFG_EXT ^= BIT(13);
					break;
				#endif
				default:
					break;
			}
		} else if (KEYS & KEY_B) {
			if (mainScreenChanged) {
				sharedAddr[0] = *mainScreen;
				sharedAddr[4] = 0x53435049; // IPCS
				while(sharedAddr[4] == 0x53435049) {
					while (REG_VCOUNT != 191) mySwiDelay(100);
					while (REG_VCOUNT == 191) mySwiDelay(100);
				}
				#ifdef B4DS
				codeJumpWord = ce9->saveMainScreenSetting;
				(*codeJump)();
				#endif
			}
			return;
		}
	}
}

static void jumpToAddress(void) {
	clearScreen(false);

	u8 cursorPosition = 0;
	while(1) {
		toncset16(BG_MAP_RAM_SUB(15) + 0x20 * 9 + 5, '-', 20);
		printCenter(15, 10, igmText.jumpAddress, FONT_WHITE, false);
		printHex(11, 12, (u32)address, 4, FONT_LIGHT_BLUE, false);
		BG_MAP_RAM_SUB(15)[0x20 * 12 + 11 + 6 - cursorPosition] = (BG_MAP_RAM_SUB(15)[0x20 * 12 + 11 + 6 - cursorPosition] & ~(0xF << 12)) | 4 << 12;
		toncset16(BG_MAP_RAM_SUB(15) + 0x20 * 13 + 5, '-', 20);

		waitKeys(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_A | KEY_B);

		if(KEYS & KEY_UP) {
			address = (vu32*)(((u32)address & ~(0xF0 << cursorPosition * 4)) | (((u32)address + (0x10 << (cursorPosition * 4))) & (0xF0 << cursorPosition * 4)));
		} else if(KEYS & KEY_DOWN) {
			address = (vu32*)(((u32)address & ~(0xF0 << cursorPosition * 4)) | (((u32)address - (0x10 << (cursorPosition * 4))) & (0xF0 << cursorPosition * 4)));
		} else if(KEYS & KEY_LEFT) {
			if(cursorPosition < 6)
				cursorPosition++;
		} else if(KEYS & KEY_RIGHT) {
			if(cursorPosition > 0)
				cursorPosition--;
		} else if(KEYS & (KEY_A | KEY_B)) {
			clampAddress();
			return;
		}
	}
}

/*
    Is this boot a hardcore RetroAchievements session.

    The launcher settles that before it touches the radio and stages the answer where the bootloader
    copies it, exactly as it stages the pending tally -- see CARDENGINEI_ARM9_RA_SESSION_LOCATION.
    Read here rather than inferred from anything else in the window: a set of definitions being
    present says achievements are being evaluated, which is true in softcore too.

    **No magic means no.** That is the safe default and not merely the convenient one. The word is
    zeroed by the bootloader on every boot that stages no session block, so a plain nds-bootstrap
    build, a build with the launcher's network compiled out, and a console where the RA window never
    loads all read false -- and none of those can submit a hardcore unlock. The failure this
    ordering avoids is the other one: a stale word from a previous boot deciding that a hardcore
    session may edit its own memory.
*/
static bool raHardcoreSession(void) {
	const raSessionBlock* const s = (const raSessionBlock*)CARDENGINEI_ARM9_RA_SESSION_LOCATION;

	return s->magic == RA_SESSION_MAGIC && s->hardcore != 0;
}

/*
    Why A did nothing, said where the player is looking when they press it.

    Drawn over the last two rows of the dump and left until a key is pressed; the loop redraws every
    row from memory each pass, so nothing has to clean this up. Two lines because one of them has to
    be the way out -- a refusal that does not say what to change is a bug report.

    A is waited out before the message is dismissible, and that is not fussiness. waitKeys() gives up
    suppressing a held key after ten frames and then returns on the next one it sees, so a press held
    for more than about a sixth of a second would dismiss the very message it opened -- the player
    would see a red flash and nothing they could read. Releasing first makes the dismissal a fresh
    press whatever they did with the first one.
*/
static void ramEditRefused(void) {
	print(0, 22, (unsigned char*)"Hardcore: RAM editing is locked", FONT_RED, false);
	print(0, 23, (unsigned char*)"Set hardcore=0 in ra.cfg to edit", FONT_LIGHT_GRAY, false);

	do {
		while (REG_VCOUNT != 191) mySwiDelay(100);
		while (REG_VCOUNT == 191) mySwiDelay(100);
	} while (KEYS & KEY_A);

	waitKeys(KEY_A | KEY_B);
}

static void ramViewer(void) {
	bool hardcore;

	clearScreen(false);
	(*changeMpu)();

	/*
	    Read once, and after changeMpu() rather than before it.

	    After, because the block lives in DSi WRAM and this function is compiled into builds that
	    have no such thing -- the RAM viewer is offered on every console, the RA window is not. Under
	    the widened regions the read is the same read the viewer itself is about to make of anywhere
	    the player types, so it cannot fault where the viewer would not.

	    Once, because nothing can change the answer while the menu is open -- the launcher wrote it
	    at boot and the game has been stopped since -- and one read means the check below and the
	    write it guards cannot disagree.
	*/
	hardcore = raHardcoreSession();

	u8 *arm7RamBuffer = ((u8*)sharedAddr) - 0x74C;
	tonccpy(arm7RamBak, arm7RamBuffer, 0xC0);
	bool ramLoaded = false;
	u8 cursorPosition = 0, mode = 0;
	while(1) {
		u8 *ramPtr;

		clampAddress();
		ramPtr = arm7Ram ? arm7RamBuffer : (u8*)address;

		unsigned char armText[5] = {'A', 'R', 'M', arm7Ram ? '7' : '9', 0};
		printCenter(14, 0, igmText.ramViewer, FONT_WHITE, false);
		print(27, 0, armText, FONT_LIGHT_BLUE, false);
		printHex(0, 0, (u32)address >> 0x10, 2, FONT_LIGHT_BLUE, false);

		if (arm7Ram && !ramLoaded) {
			sharedAddr[0] = (vu32)arm7RamBuffer;
			sharedAddr[1] = (vu32)address;
			sharedAddr[4] = 0x524D4152; // RAMR
			while (sharedAddr[4] == 0x524D4152) {
				while (REG_VCOUNT != 191) mySwiDelay(100);
				while (REG_VCOUNT == 191) mySwiDelay(100);
			}
		}
		ramLoaded = true;

		for(int i = 0; i < 23; i++) {
			printHex(0, i + 1, (u32)(address + (i * 2)) & 0xFFFF, 2, FONT_LIGHT_BLUE, false);
			for(int j = 0; j < 4; j++)
				printHex(5 + (j * 2), i + 1, ramPtr[(i * 8) + j], 1, 1 + j % 2, false);
			for(int j = 0; j < 4; j++)
				printHex(14 + (j * 2), i + 1, ramPtr[4 + (i * 8) + j], 1, 1 + j % 2, false);
			for(int j = 0; j < 8; j++)
				printChar(23 + j, i + 1, ramPtr[i * 8 + j], FONT_WHITE, false);
		}

		// Change color of selected byte
		if(mode > 0) {
			// Hex
			u16 loc = 0x20 * (1 + (cursorPosition / 8)) + 5 + ((cursorPosition % 8) * 2) + (cursorPosition % 8 >= 4);
			BG_MAP_RAM_SUB(15)[loc] = (BG_MAP_RAM_SUB(15)[loc] & ~(0xF << 12)) | (3 + mode) << 12;
			BG_MAP_RAM_SUB(15)[loc + 1] = (BG_MAP_RAM_SUB(15)[loc + 1] & ~(0xF << 12)) | (3 + mode) << 12;

			// Text
			loc = 0x20 * (1 + (cursorPosition / 8)) + 23 + (cursorPosition % 8);
			BG_MAP_RAM_SUB(15)[loc] = (BG_MAP_RAM_SUB(15)[loc] & ~(0xF << 12)) | (3 + mode) << 12;
		}

		waitKeys(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_A | KEY_B | KEY_Y | KEY_SELECT);

		if(mode == 0) {
			if(KEYS & KEY_R && KEYS & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)) {
				if (KEYS & KEY_UP) {
					address -= 0x400;
					ramLoaded = false;
				} else if (KEYS & KEY_DOWN) {
					address += 0x400;
					ramLoaded = false;
				} else if (KEYS & KEY_LEFT) {
					address -= 0x4000;
					ramLoaded = false;
				} else if (KEYS & KEY_RIGHT) {
					address += 0x4000;
					ramLoaded = false;
				}
			} else {
				if (KEYS & KEY_UP) {
					address -= 2;
					ramLoaded = false;
				} else if (KEYS & KEY_DOWN) {
					address += 2;
					ramLoaded = false;
				} else if (KEYS & KEY_LEFT) {
					address -= 2 * 23;
					ramLoaded = false;
				} else if (KEYS & KEY_RIGHT) {
					address += 2 * 23;
					ramLoaded = false;
				} else if (KEYS & KEY_A) {
					mode = 1;
				} else if (KEYS & KEY_B) {
					return;
				} else if(KEYS & KEY_Y) {
					jumpToAddress();
					clearScreen(false);
					ramLoaded = false;
				}else if (KEYS & KEY_SELECT) {
					arm7Ram = !arm7Ram;
					ramLoaded = false;
				}
			}
		} else if(mode == 1) {
			if (KEYS & KEY_UP) {
				if(cursorPosition >= 8)
					cursorPosition -= 8;
				else
					address -= 2;
			} else if (KEYS & KEY_DOWN) {
				if(cursorPosition < 8 * 22)
					cursorPosition += 8;
				else
					address += 2;
			} else if (KEYS & KEY_LEFT) {
				if(cursorPosition > 0)
					cursorPosition--;
			} else if (KEYS & KEY_RIGHT) {
				if(cursorPosition < 8 * 23 - 1)
					cursorPosition++;
			} else if (KEYS & KEY_A) {
				/*
				    The one door into edit mode, and where a hardcore session is turned away.

				    Here rather than at the viewer's entrance on purpose: reading memory is not
				    what RetroAchievements' rules are about, and a hex dump of a running game is a
				    debugging tool this fork has no reason to take away from anybody. What is
				    forbidden is changing it, so that is what is refused -- navigation, the ARM7
				    window and the cursor all still work.
				*/
				if (hardcore) {
					ramEditRefused();
				} else {
					mode = 2;
				}
			} else if (KEYS & KEY_B) {
				mode = 0;
			} else if(KEYS & KEY_Y) {
				jumpToAddress();
				clearScreen(false);
			}
		} else if(mode == 2) {
			/*
			    Gated twice, and the second gate is not decoration.

			    Everything below this line writes: the four directions edit the byte under the
			    cursor in place -- on the ARM9 `ramPtr` *is* the game's memory, not a copy of it --
			    and A or B pushes the edited window back across to the ARM7 with RAMW. Today the
			    branch above is the only way into this mode, so this cannot fire. A later change
			    that adds a second way in would reopen all of it at once, and the symptom would not
			    be a crash somebody notices: it would be an unlock claimed as hardcore.
			*/
			if (hardcore) {
				mode = 1;
				continue;
			}
			if (KEYS & KEY_UP) {
				ramPtr[cursorPosition]++;
			} else if (KEYS & KEY_DOWN) {
				ramPtr[cursorPosition]--;
			} else if (KEYS & KEY_LEFT) {
				ramPtr[cursorPosition] -= 0x10;
			} else if (KEYS & KEY_RIGHT) {
				ramPtr[cursorPosition] += 0x10;
			} else if (KEYS & (KEY_A | KEY_B)) {
				if(arm7Ram) {
					sharedAddr[0] = (vu32)arm7RamBuffer;
					sharedAddr[1] = (vu32)address;
					sharedAddr[2] = cursorPosition;
					sharedAddr[4] = 0x574D4152; // RAMW
					while (sharedAddr[4] == 0x574D4152) {
						while (REG_VCOUNT != 191) mySwiDelay(100);
						while (REG_VCOUNT == 191) mySwiDelay(100);
					}
					ramLoaded = false;
				}
				mode = 1;
			}
		}
	}
	tonccpy(arm7RamBuffer, arm7RamBak, 0xC0);
	(*revertMpu)();
}

u32 inGameMenu(s32 *mainScreen, u32 consoleModel, s32 *exceptionRegisters) {
	// If we were given exception registers, then we're handling an exception
	bool exception = (exceptionRegisters != 0);

	#ifdef B4DS
	static bool ce9Set = false;
	if (!ce9Set) {
		ce9 = (cardengineArm9*)consoleModel;
		ce9Set = true;
	}
	#endif

	u32 res = 0;

	u32 dispcnt = REG_DISPCNT_SUB;
	u16 bg0cnt = REG_BG0CNT_SUB;
	u16 bg1cnt = REG_BG1CNT_SUB;
	u16 bg2cnt = REG_BG2CNT_SUB;
	u16 bg3cnt = REG_BG3CNT_SUB;

	u8 vramCCr = VRAM_C_CR;
	u8 vramHCr = VRAM_H_CR;

	u16 powercnt = REG_POWERCNT;

	u16 masterBright = *(vu16*)0x0400106C;

	REG_DISPCNT_SUB = MODE_0_2D | DISPLAY_BG3_ACTIVE;
	REG_BG0CNT_SUB = 0;
	REG_BG1CNT_SUB = 0;
	REG_BG2CNT_SUB = 0;
	REG_BG3CNT_SUB = (u16)(BG_MAP_BASE(15) | BG_TILE_BASE(0) | BgSize_T_256x256);

	if(VRAM_C_CR & 4) // If VRAM C is mapped to sub bg, unmap it
		VRAM_C_CR = VRAM_C_LCD;
	VRAM_H_CR = VRAM_ENABLE | VRAM_H_SUB_BG;

	REG_BG3VOFS_SUB = 0;
	REG_BG3HOFS_SUB = 0;

	// If main screen is on auto, then force the bottom
	REG_POWERCNT |= POWER_SWAP_LCDS;

	SetBrightness(1, 0);
	REG_MOSAIC_SUB = 0; // Register is write only, can't back up
	REG_BLDCNT_SUB = 0; // Register is write only, can't back up
	REG_BLDALPHA_SUB = 0; // Register is write only, can't back up
	REG_BLDY_SUB = 0; // Register is write only, can't back up

	tonccpy(bgMapBak, BG_MAP_RAM_SUB(15), sizeof(bgMapBak));	// Backup BG_MAP_RAM
	clearScreen(false);

	tonccpy(palBak, BG_PALETTE_SUB, sizeof(palBak));	// Backup the palette
	toncset16(BG_PALETTE_SUB, igmPal[exception ? 7 : 6], 256);
	for(int i = 0; i < 6; i++) {
		BG_PALETTE_SUB[i * 0x10 + 1] = igmPal[i];
	}

	tonccpy(bgBak, BG_GFX_SUB, sizeof(igmText.font) * 4);	// Backup the original graphics
	for(int i = 0; i < sizeof(igmText.font); i++) {	// Load font from 1bpp to 4bpp
		u8 val = igmText.font[i];
		BG_GFX_SUB[i * 2]     = (val & 1) | ((val & 2) << 3) | ((val & 4) << 6) | ((val & 8) << 9);
		val >>= 4;
		BG_GFX_SUB[i * 2 + 1] = (val & 1) | ((val & 2) << 3) | ((val & 4) << 6) | ((val & 8) << 9);
	}

	// Let ARM7 know the menu loaded
	sharedAddr[5] = 0x59444552; // 'REDY'

	MenuItem menuItems[9];
	int menuItemCount = 0;
	if(!exception)
		menuItems[menuItemCount++] = MENU_EXIT;
	menuItems[menuItemCount++] = MENU_RESET;
	menuItems[menuItemCount++] = MENU_SCREENSHOT;
	if(igmText.manualMaxLine > 0 && !exception)
		menuItems[menuItemCount++] = MENU_MANUAL;
	menuItems[menuItemCount++] = MENU_RAM_DUMP;
	if(!exception)
		menuItems[menuItemCount++] = MENU_OPTIONS;
	menuItems[menuItemCount++] = MENU_RAM_VIEWER;
	/*
	    Not offered on an exception screen, like every other entry that reads staged memory: the
	    block lives in cardenginei_arm9_ra's window and a crashed game is the one case where nothing
	    about that window can be trusted.
	*/
	if (!exception)
		menuItems[menuItemCount++] = MENU_RETROACHIEVEMENTS;
	menuItems[menuItemCount++] = MENU_QUIT;

	if(exception) {
		showException(exceptionRegisters);
	}

	// Wait for keys to be released
	drawMainMenu(menuItems, menuItemCount);
	drawCursor(0);
	do {
		printTime();
		#ifndef B4DS
		printBattery();
		#endif
		while (REG_VCOUNT != 191) mySwiDelay(100);
		while (REG_VCOUNT == 191) mySwiDelay(100);
	} while(KEYS & igmText.hotkey);

	u8 cursorPosition = 0;
	while (sharedAddr[4] == 0x554E454D) {
		drawMainMenu(menuItems, menuItemCount);
		drawCursor(cursorPosition);
		printTime();
#ifndef B4DS
		printBattery();
#endif

		if(exception)
			waitKeys(KEY_UP | KEY_DOWN | KEY_A);
		else
			waitKeys(KEY_UP | KEY_DOWN | KEY_A | KEY_B);

		if (KEYS & KEY_UP) {
			if (cursorPosition > 0)
				cursorPosition--;
			else
				cursorPosition = menuItemCount - 1;
		} else if (KEYS & KEY_DOWN) {
			if (cursorPosition < (menuItemCount - 1))
				cursorPosition++;
			else
				cursorPosition = 0;
		} else if (KEYS & KEY_A) {
			switch(menuItems[cursorPosition]) {
				case MENU_EXIT:
					do {
						while (REG_VCOUNT != 191) mySwiDelay(100);
						while (REG_VCOUNT == 191) mySwiDelay(100);
					} while(KEYS & KEY_A);
					// sharedAddr[1] = 0;
					sharedAddr[4] = 0x54495845; // EXIT
					while (sharedAddr[4] != 0) swiDelay(100);
					break;
				case MENU_RESET:
					if (boolQuestion(igmText.resetGameMessage)) {
						extern bool exceptionPrinted;
						exceptionPrinted = false;
						res = 0x52534554; // TESR
						sharedAddr[3] = res;
						sharedAddr[4] = 0x54455352; // RSET
					}
					break;
				case MENU_SCREENSHOT:
					screenshot();
					break;
				case MENU_MANUAL:
					manual();
					break;
				case MENU_RAM_DUMP:
					#ifndef B4DS
					sharedAddr[4] = 0x444D4152; // RAMD
					while (sharedAddr[4] == 0x444D4152) {
						while (REG_VCOUNT != 191) mySwiDelay(100);
						while (REG_VCOUNT == 191) mySwiDelay(100);
					}
					#else
					// sharedAddr[1] = 0;
					res = 0x444D4152; // RAMD
					sharedAddr[3] = res;
					sharedAddr[4] = 0x54495845; // EXIT
					while (sharedAddr[4] != 0) swiDelay(100);
					#endif
					break;
				case MENU_OPTIONS:
					optionsMenu(mainScreen, consoleModel);
					break;
				case MENU_RAM_VIEWER:
					ramViewer();
					break;
				case MENU_RETROACHIEVEMENTS:
					raMenu();
					break;
				case MENU_QUIT:
					if (boolQuestion(igmText.quitGameMessage)) {
						res = 0x54495845; // EXIT
						sharedAddr[3] = res;
						sharedAddr[4] = 0x54495551; // QUIT
					}
					break;
				default:
					break;
			}
		} else if (KEYS & KEY_B && !exception) {
			do {
				while (REG_VCOUNT != 191) mySwiDelay(100);
				while (REG_VCOUNT == 191) mySwiDelay(100);
			} while(KEYS & KEY_B);
			// sharedAddr[1] = 0;
			sharedAddr[4] = 0x54495845; // EXIT
			while (sharedAddr[4] != 0) swiDelay(100);
		} /* else if (KEYS & KEY_R && !exception) {
			do {
				while (REG_VCOUNT != 191) mySwiDelay(100);
				while (REG_VCOUNT == 191) mySwiDelay(100);
			} while(KEYS & KEY_R);
			sharedAddr[1] = 1;
			sharedAddr[4] = 0x54495845; // EXIT
			while (sharedAddr[4] != 0) swiDelay(100);
		} */
	}

	tonccpy(BG_MAP_RAM_SUB(15), bgMapBak, sizeof(bgMapBak));	// Restore BG_MAP_RAM
	tonccpy(BG_PALETTE_SUB, palBak, sizeof(palBak));	// Restore the palette
	tonccpy(BG_GFX_SUB, bgBak, sizeof(igmText.font) * 4);	// Restore the original graphics

	*(vu16*)0x0400106C = masterBright;

	REG_DISPCNT_SUB = dispcnt;
	REG_BG0CNT_SUB = bg0cnt;
	REG_BG1CNT_SUB = bg1cnt;
	REG_BG2CNT_SUB = bg2cnt;
	REG_BG3CNT_SUB = bg3cnt;

	VRAM_C_CR = vramCCr;
	VRAM_H_CR = vramHCr;

	REG_POWERCNT = powercnt;

	if(*mainScreen == 1)
		REG_POWERCNT &= ~POWER_SWAP_LCDS;
	else if(*mainScreen == 2)
		REG_POWERCNT |= POWER_SWAP_LCDS;

	return res;
}