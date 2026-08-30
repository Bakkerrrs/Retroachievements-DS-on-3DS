/*
	NitroHax -- Cheat tool for the Nintendo DS
	Copyright (C) 2008  Michael "Chishm" Chisholm

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful, 
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <string.h>
#include <nds/ndstypes.h>
#include <nds/fifomessages.h>
#include <nds/dma.h>
#include <nds/ipc.h>
#include <nds/system.h>
#include <nds/interrupts.h>
#include <nds/input.h>
#include <nds/timers.h>
#include <nds/arm7/audio.h>
#include <nds/arm7/clock.h>   /* rtcGetTimeAndDate(), to stamp an unlock with when it was earned */
#include <nds/arm7/i2c.h>
#include <nds/memory.h> // tNDSHeader
/* Step 3b: RA_SHARED_UNLOCK_* -- the contract with the ARM9, in one place rather than two. */
#include "ra.h"
#include "ra_wifi.h"   /* RA_QUEUE_RECORD, RA_QUEUE_MAX -- defined whether or not the WiFi switch is on */
#include <nds/debug.h>

#include "ndma.h"
#include "dmaTwl.h"
#include "tonccpy.h"
#include "my_sdmmc.h"
#include "my_fat.h"
#include "locations.h"
#include "module_params.h"
#include "unpatched_funcs.h"
#include "debug_file.h"
#include "cardengine.h"
#include "fpsAdjust.h"
#include "nds_header.h"
#include "igm_text.h"

#ifndef TWLSDK
// Patcher
#include "common.h"
#include "decompress.h"
#include "patch.h"
#include "find.h"
#include "hook.h"
#endif

// TWL soft-reset
#include "sr_data_error.h"      // For showing an error screen

#define gameOnFlashcard BIT(0)
#define saveOnFlashcard BIT(1)
#define eSdk2 BIT(1)
#define ROMinRAM BIT(3)
#define dsiMode BIT(4)
#define b_dsiSD BIT(5)
#define preciseVolumeControl BIT(6)
#define powerCodeOnVBlank BIT(7)
#define delayWrites BIT(8)
#define igmAccessible BIT(9)
#define quitOnFlashcard BIT(10)
#define slowSoftReset BIT(11)
#define wideCheatUsed BIT(12)
#define isSdk5 BIT(13)
#define hasVramWifiBinary BIT(14)
#define twlTouch BIT(15)
#define cloneboot BIT(16)
#define sleepMode BIT(17)
#define b_dsiBios BIT(18)
#define bootstrapOnFlashcard BIT(19)
#define ndmaDisabled BIT(20)
#define isDlp BIT(21)
#define useColorLut BIT(22)
#define clearRamOnReset BIT(23)
#define i2cBricked BIT(30)
#define scfgLocked BIT(31)

#define	REG_EXTKEYINPUT	(*(vuint16*)0x04000136)
#define	REG_WIFIIRQ	(*(vuint16*)0x04808012)

extern u32 ce7;

static const char *unlaunchAutoLoadID = "AutoLoadInfo";

extern u32 srBackendId[2];
extern u32 srFrontendId[2];

extern void ndsCodeStart(u32* addr);
extern int tryLockMutex(int* addr);
extern int lockMutex(int* addr);
extern int unlockMutex(int* addr);

extern vu32* volatile cardStruct;
extern u32 cheatEngineAddr;
extern u32 quitFileCluster;
extern u32 fileCluster;
extern u32 saveCluster;
extern u32 saveSize;
extern u32 patchOffsetCacheFileCluster;
extern u32 srParamsCluster;
extern u32 raUnlocksCluster;   /* step 3b; 0 when the launcher found no queue file */
extern u32 ramDumpCluster;
extern u32 screenshotCluster;
extern u32 pageFileCluster;
extern u32 manualCluster;
extern module_params_t* moduleParams;
extern u32 valueBits;
extern s32 mainScreen;
extern u32* languageAddr;
extern u8 language;
extern u8 consoleModel;
extern u8 romRead_LED;
extern u8 dmaRomRead_LED;
extern u8 remappedKeys[12];
extern u16 igmHotkey;
extern u16 screenSwapHotkey;

#ifdef TWLSDK
vu32* volatile sharedAddr = (vu32*)CARDENGINE_SHARED_ADDRESS_SDK5;
#else
vu32* volatile sharedAddr = (vu32*)CARDENGINE_SHARED_ADDRESS_SDK1;
#endif

struct IgmText *igmText = (struct IgmText *)INGAME_MENU_LOCATION;

static bool initialized = false;
static bool driveInited = false;
#ifdef TWLSDK
static bool sixInHeader = false;
#endif
static bool bootloaderCleared = false;
static bool funcsUnpatched = false;
//static bool initializedIRQ = false;
//static bool calledViaIPC = false;
//static bool ipcSyncHooked = false;
//static bool dmaLed = false;
static bool powerLedChecked = false;
static bool powerLedIsPurple = false;
static bool wifiIrq = false;
static int wifiIrqTimer = 0;
//static bool saveInRam = false;

#ifdef TWLSDK
static aFile* romFile = (aFile*)ROM_FILE_LOCATION_TWLSDK;
static aFile* savFile = (aFile*)SAV_FILE_LOCATION_TWLSDK;
//static aFile* gbaFile = (aFile*)GBA_FILE_LOCATION_TWLSDK;
static aFile* apFixOverlaysFile = (aFile*)OVL_FILE_LOCATION_TWLSDK;
static aFile* sharedFontFile = (aFile*)FONT_FILE_LOCATION_TWLSDK;
#else
#ifdef ALTERNATIVE
static aFile* romFile = (aFile*)ROM_FILE_LOCATION_ALT;
static aFile* savFile = (aFile*)SAV_FILE_LOCATION_ALT;
//static aFile* gbaFile = (aFile*)GBA_FILE_LOCATION_ALT;
static aFile* apFixOverlaysFile = (aFile*)OVL_FILE_LOCATION_ALT;
#else
static aFile* romFile = (aFile*)ROM_FILE_LOCATION;
static aFile* savFile = (aFile*)SAV_FILE_LOCATION;
//static aFile* gbaFile = (aFile*)GBA_FILE_LOCATION;
static aFile* apFixOverlaysFile = (aFile*)OVL_FILE_LOCATION;
#endif
#endif
static aFile patchOffsetCacheFile;
static aFile ramDumpFile;
static aFile srParamsFile;
static aFile raUnlocksFile;
/*
    The running game's identity, captured at init because the header it comes from does not survive
    play. See where these are filled.
*/
static char raGameTitle[RA_QUEUE_TITLE];
static char raGameCode[RA_QUEUE_CODE];
static aFile screenshotFile;
static aFile pageFile;
static aFile manualFile;

static int saveTimer = 0;

static int languageTimer = 0;
static int swapTimer = 0;
int afterSwapTimer = 0;
static int returnTimer = 0;
static int softResetTimer = 0;
// static int ramDumpTimer = 0;
static int noI2CVolLevel = 127; // Volume workaround for bricked I2C chips
static int volumeAdjustDelay = 0;
static bool volumeAdjustActivated = false;

#ifndef TWLSDK
fpsa_t sActiveFpsa;
#endif

//static bool ndmaUsed = false;

// static int cardEgnineCommandMutex = 0;
static int saveMutex = 0;

bool returnToMenu = false;

#ifdef TWLSDK
static tNDSHeader* ndsHeader = (tNDSHeader*)NDS_HEADER_SDK5;
static PERSONAL_DATA* personalData = (PERSONAL_DATA*)((u8*)NDS_HEADER_SDK5-0x180);
#else
static tNDSHeader* ndsHeader = (tNDSHeader*)NDS_HEADER;
static PERSONAL_DATA* personalData = (PERSONAL_DATA*)((u8*)NDS_HEADER-0x180);
#endif

extern u32 romLocation;

extern u32 romMapLines;
// 0: ROM part start, 1: ROM part start in RAM, 2: ROM part end in RAM
extern u32 romMap[][3];

u32 currentSrlAddr = 0;

void i2cIRQHandler(void);

static void unlaunchSetFilename(void) {
	const u8* filename = 
	#ifdef TWLSDK
	(u8*)(ce7+0x8400);
	#else
	(u8*)(ce7+0xF400);
	#endif

	if (filename[0] == 0) return;

	tonccpy((u8*)0x02000800, unlaunchAutoLoadID, 12);
	*(u16*)(0x0200080C) = 0x3F0;		// Unlaunch Length for CRC16 (fixed, must be 3F0h)
	*(u16*)(0x0200080E) = 0;			// Unlaunch CRC16 (empty)
	*(u32*)(0x02000810) = (BIT(0) | BIT(1));		// Load the title at 2000838h
													// Use colors 2000814h
	*(u16*)(0x02000814) = 0x7FFF;		// Unlaunch Upper screen BG color (0..7FFFh)
	*(u16*)(0x02000816) = 0x7FFF;		// Unlaunch Lower screen BG color (0..7FFFh)
	toncset((u8*)0x02000818, 0, 0x20+0x208+0x1C0);		// Unlaunch Reserved (zero)
	int i2 = 0;
	for (int i = 0; i < 256; i++) {
		*(u8*)(0x02000838+i2) = filename[i];		// Unlaunch Device:/Path/Filename.ext (16bit Unicode,end by 0000h)
		i2 += 2;
	}
	*(u16*)(0x0200080E) = swiCRC16(0xFFFF, (void*)0x02000810, 0x3F0);		// Unlaunch CRC16
}

static void readSoftResetId(const bool front) {
	if (front) {
		if (srFrontendId[0] == 0 && srFrontendId[1] == 0) return;
	} else {
		if (srBackendId[0] == 0 && srBackendId[1] == 0) return;
	}

	// Use soft-reset ID
	*(u32*)(0x02000300) = 0x434E4C54;	// 'CNLT'
	*(u16*)(0x02000304) = 0x1801;
	*(u32*)(0x02000308) = 0;
	*(u32*)(0x0200030C) = 0;
	*(u32*)(0x02000310) = front ? srFrontendId[0] : srBackendId[0];
	*(u32*)(0x02000314) = front ? srFrontendId[1] : srBackendId[1];
	*(u32*)(0x02000318) = *(u32*)(0x02000314) == 0x00030000 ? 0x13 : 0x17;
	*(u32*)(0x0200031C) = 0;
	*(u16*)(0x02000306) = swiCRC16(0xFFFF, (void*)0x02000308, 0x18);
}

// Alternative to swiWaitForVBlank()
static inline void waitFrames(int count) {
	for (int i = 0; i < count; i++) {
		while (REG_VCOUNT != 191);
		while (REG_VCOUNT == 191);
	}
}

static inline bool isSdEjected(void) {
	return (*(vu32*)(0x400481C) & BIT(3));
}

static void driveInitialize(void) {
	if (driveInited) {
		return;
	}

	if (valueBits & b_dsiSD) {
		if (sdmmc_read16(REG_SDSTATUS0) != 0) {
			sdmmc_init();
			SD_Init();
		}
		FAT_InitFiles(false, false);
	}
	if (((valueBits & gameOnFlashcard) && !(valueBits & ROMinRAM)) || (valueBits & saveOnFlashcard)) {
		FAT_InitFiles(false, true);
	}

	getFileFromCluster(&patchOffsetCacheFile, patchOffsetCacheFileCluster, (valueBits & gameOnFlashcard));
	getFileFromCluster(&ramDumpFile, ramDumpCluster, (valueBits & bootstrapOnFlashcard));
	getFileFromCluster(&srParamsFile, srParamsCluster, (valueBits & gameOnFlashcard));
	/*
	    Step 3b. Zero when the launcher found no queue file, and getFileFromCluster on zero yields a
	    file the append path refuses -- see raUnlockAppend(). Same flashcard flag as srParams, because
	    the queue lives beside the game rather than beside nds-bootstrap.
	*/
	getFileFromCluster(&raUnlocksFile, raUnlocksCluster, (valueBits & gameOnFlashcard));
	/*
	    And which game this is, taken *now* rather than when an achievement fires.

	    ndsHeader points at the header the loader left in main RAM, and the game reuses that memory
	    once it is running. Reading it at unlock time therefore works for an achievement earned in the
	    first seconds and returns nothing for one earned at a stage end minutes later -- which is
	    exactly what hardware showed: an early unlock carried YCTE and CONTRA 4, a late one carried
	    neither, from the same build.

	    Copied raw, with the tidying left to raQueueScan() on the launcher side; see raUnlockAppend()
	    for why this binary does no trimming of its own.
	*/
	tonccpy(raGameTitle, ndsHeader->gameTitle, RA_QUEUE_TITLE);
	tonccpy(raGameCode, ndsHeader->gameCode, RA_QUEUE_CODE);
	getFileFromCluster(&screenshotFile, screenshotCluster, (valueBits & bootstrapOnFlashcard));
	getFileFromCluster(&pageFile, pageFileCluster, (valueBits & bootstrapOnFlashcard));
	getFileFromCluster(&manualFile, manualCluster, (valueBits & bootstrapOnFlashcard));

	//romFile = getFileFromCluster(fileCluster);
	//buildFatTableCache(&romFile, 0);
	#ifdef DEBUG	
	if (romFile->fatTableCached) {
		nocashMessage("fat table cached");
	} else {
		nocashMessage("fat table not cached"); 
	}
	#endif
	
	/*if (saveCluster > 0) {
		savFile = getFileFromCluster(saveCluster);
	} else {
		savFile.firstCluster = CLUSTER_FREE;
	}*/

	#ifdef DEBUG		
	aFile myDebugFile;
	getBootFileCluster(&myDebugFile, "NDSBTSRP.LOG", 0, !(valueBits & b_dsiSD));
	enableDebug(&myDebugFile);
	dbg_printf("logging initialized\n");		
	dbg_printf("sdk version :");
	dbg_hexa(moduleParams->sdk_version);		
	dbg_printf("\n");	
	dbg_printf("rom file :");
	dbg_hexa(fileCluster);	
	dbg_printf("\n");	
	dbg_printf("save file :");
	dbg_hexa(saveCluster);	
	dbg_printf("\n");
	#endif

	if (valueBits & ndmaDisabled) {
		sdmmc_lock_ndma_slot();
	}

	sdmmc_set_ndma_slot(0);
	driveInited = true;
}

/* Step 3b: defined far below, beside the rest of the queue code it belongs to. */
static __attribute__((noinline)) void raStampCapture(void);

static void initialize(void) {
	if (initialized) {
		return;
	}

	#ifdef TWLSDK
	if (*(u8*)(DSI_HEADER_SDK5+0x234) == 6) {
		*(u8*)(DSI_HEADER_SDK5+0x234) = 0;
		sixInHeader = true;
	}
	#else
	if (valueBits & isSdk5) {
		sharedAddr = (vu32*)CARDENGINE_SHARED_ADDRESS_SDK5;
		ndsHeader = (tNDSHeader*)NDS_HEADER_SDK5;
		personalData = (PERSONAL_DATA*)((u8*)NDS_HEADER_SDK5-0x180);
	} else {
		sharedAddr = (vu32*)CARDENGINE_SHARED_ADDRESS_SDK1;
		ndsHeader = (tNDSHeader*)NDS_HEADER;
		personalData = (PERSONAL_DATA*)((u8*)NDS_HEADER-0x180);
	}
	#endif

	if (language >= 0 && language <= 7) {
		// Change language
		personalData->language = language;
	}

	if (!bootloaderCleared) {
		toncset((u8*)0x06000000, 0, 0x40000);	// Clear bootloader
		bootloaderCleared = true;
	}

	/*
	    Step 3b: the console's clock, taken here and nowhere else. This is the only moment in this
	    binary's life when reading the RTC is safe -- the game is setting up its interrupts and has no
	    main loop to interrupt. See raStampCapture().
	*/
	raStampCapture();

	initialized = true;
}

#ifdef TWLSDK
/*u32 auxIeBak = 0;
u32 sdStatBak = 0;
u32 sdMaskBak = 0;

void bakSdData(void) {
	auxIeBak = REG_AUXIE;
	sdStatBak = *(vu32*)0x400481C;
	sdMaskBak = *(vu32*)0x4004820;

	REG_AUXIE &= ~(1UL << 8);
	*(vu32*)0x400481C = 0;
	*(vu32*)0x4004820 = 0;
}

void restoreSdBakData(void) {
	REG_AUXIE = auxIeBak;
	*(vu32*)0x400481C = sdStatBak;
	*(vu32*)0x4004820 = sdMaskBak;
}*/
#else
bool buttonsRemapped(void) {
	for (int i = 0; i < 12; i++) {
		if (remappedKeys[i] != i) {
			return true;
		}
	}
	return false;
}

static module_params_t* getModuleParams(const tNDSHeader* ndsHeader) {
	//nocashMessage("Looking for moduleparams...\n");

	u32* moduleParamsOffset = findModuleParamsOffset(ndsHeader);

	//module_params_t* moduleParams = (module_params_t*)((u32)moduleParamsOffset - 0x1C);
	return moduleParamsOffset ? (module_params_t*)(moduleParamsOffset - 7) : NULL;
}
#endif

static bool cardReadRAM(u8* dst, u32 src, u32 len/*, int romPartNo*/) {
	if (!(valueBits & ROMinRAM)) {
		return false;
	}

	// Copy directly
	#ifdef TWLSDK
	u32 newSrc = romLocation/*[romPartNo]*/+src;
	if (src > *(u32*)0x02FFE1C0) {
		newSrc -= *(u32*)0x02FFE1CC;
	}
	tonccpy(dst, (u8*)newSrc, len);
	#else
	// tonccpy(dst, (u8*)romLocation/*[romPartNo]*/+src, len);
	u32 newSrc = 0;
	u32 newLen = 0;
	bool srcFound = false;
	int i = 0;
	for (i = 0; i < romMapLines; i++) {
		if (src >= romMap[i][0] && (i == romMapLines-1 || src < romMap[i+1][0])) {
			srcFound = true;
			break;
		}
	}
	if (!srcFound) {
		return false;
	}
	while (len > 0) {
		newSrc = (romMap[i][1]-romMap[i][0])+src;
		if (newSrc >= 0x03000000) {
			return false; // Unable to read from ARM9-exclusive areas
		}
		newLen = len;
		while (newSrc+newLen > romMap[i][2]) {
			newLen--;
		}
		tonccpy(dst, (u8*)newSrc, newLen);
		src += newLen;
		dst += newLen;
		len -= newLen;
		i++;
	}
	#endif
	return true;
}

void reset(const bool downloadedSrl) {
	register int i, reg;

#ifndef TWLSDK
	u32 resetParam = ((valueBits & isSdk5) ? RESET_PARAM_SDK5 : RESET_PARAM);
	if (((valueBits & isDlp) && *(u32*)(NDS_HEADER_SDK5+0xC) == 0) || (valueBits & slowSoftReset) || (*(u32*)(resetParam+0xC) > 0 && (valueBits & isSdk5))) {
		REG_MASTER_VOLUME = 0;
		int oldIME = enterCriticalSection();
		//driveInitialize();
		if (downloadedSrl) {
			*(u32*)resetParam = 0;
			*(u32*)(resetParam+8) = 0x44414F4C; // 'LOAD'
			fileWrite((char*)ndsHeader, &pageFile, 0x2BFE00, 0x160);
			fileWrite((char*)ndsHeader->arm9destination, &pageFile, 0x14000, ndsHeader->arm9binarySize);
			fileWrite((char*)ndsHeader->arm7destination, &pageFile, 0x2C0000, ndsHeader->arm7binarySize);
		}
		fileWrite((char*)resetParam, &srParamsFile, 0, 0x10);
		fileWrite((char*)resetParam+0x20, &srParamsFile, 0x10, 0x40);
		toncset((u32*)0x02000000, 0, 0x400);
		*(u32*)0x02000000 = BIT(3);
		*(u32*)0x02000004 = 0x54455352; // 'RSET'
		unlaunchSetFilename();
		readSoftResetId(false);
		i2cWriteRegister(0x4A, 0x70, 0x01);
		i2cWriteRegister(0x4A, 0x11, 0x01);			// Reboot game
		leaveCriticalSection(oldIME);
		while (1);
	}

	if (!(valueBits & isDlp)) {
#endif

	REG_IME = 0;

	for (i = 0; i < 16; i++) {
		SCHANNEL_CR(i) = 0;
		SCHANNEL_TIMER(i) = 0;
		SCHANNEL_SOURCE(i) = 0;
		SCHANNEL_LENGTH(i) = 0;
	}

	REG_SOUNDCNT = 0;
	REG_SNDCAP0CNT = 0;
	REG_SNDCAP1CNT = 0;

	REG_SNDCAP0DAD = 0;
	REG_SNDCAP0LEN = 0;
	REG_SNDCAP1DAD = 0;
	REG_SNDCAP1LEN = 0;

	// Clear out ARM7 DMA channels and timers
	for (i = 0; i < 4; i++) {
		DMA_CR(i) = 0;
		DMA_SRC(i) = 0;
		DMA_DEST(i) = 0;
		TIMER_CR(i) = 0;
		TIMER_DATA(i) = 0;
	}

	// Clear out FIFO
	REG_IPC_SYNC = 0;
	REG_IPC_FIFO_CR = IPC_FIFO_ENABLE | IPC_FIFO_SEND_CLEAR;
	REG_IPC_FIFO_CR = 0;

	REG_IE = 0;
	REG_IF = ~0;
	*(vu32*)0x0380FFFC = 0;  // IRQ_HANDLER ARM7 version
	*(vu32*)0x0380FFF8 = 0; // VBLANK_INTR_WAIT_FLAGS, ARM7 version
	REG_POWERCNT = 1;  // Turn off power to stuff

	funcsUnpatched = false;

#ifndef TWLSDK
	}
#endif

	initialized = false;
	//ipcSyncHooked = false;
	languageTimer = 0;
	unlockMutex(&saveMutex);

	#ifndef TWLSDK
	if ((valueBits & isDlp) || currentSrlAddr != *(u32*)(resetParam+0xC) || downloadedSrl) {
		currentSrlAddr = *(u32*)(resetParam+0xC);
		if ((valueBits & isDlp) || downloadedSrl) {
			// ndmaCopyWordsAsynch(1, (u32*)0x022C0000, ndsHeader->arm7destination, ndsHeader->arm7binarySize);
		} else {
			if (!cardReadRAM((u8*)ndsHeader, currentSrlAddr, 0x160)) {
				fileRead((char*)ndsHeader, romFile, currentSrlAddr, 0x160);
			}
			if (!cardReadRAM((u8*)ndsHeader->arm9destination, currentSrlAddr+ndsHeader->arm9romOffset, ndsHeader->arm9binarySize)) {
				fileRead((char*)ndsHeader->arm9destination, romFile, currentSrlAddr+ndsHeader->arm9romOffset, ndsHeader->arm9binarySize);
			}
			if (!cardReadRAM((u8*)ndsHeader->arm7destination, currentSrlAddr+ndsHeader->arm7romOffset, ndsHeader->arm7binarySize)) {
				fileRead((char*)ndsHeader->arm7destination, romFile, currentSrlAddr+ndsHeader->arm7romOffset, ndsHeader->arm7binarySize);
			}
		}

		moduleParams = getModuleParams(ndsHeader);
		/*dbg_printf("sdk_version: ");
		dbg_hexa(moduleParams->sdk_version);
		dbg_printf("\n");*/ 
		if (moduleParams->sdk_version > 0x5000000) {
			valueBits |= isSdk5;
		} else {
			valueBits &= ~isSdk5;
		}
		/* if ((moduleParams->sdk_version >= 0x2008000 && moduleParams->sdk_version != 0x2012774) || moduleParams->sdk_version == 0x20029A8) {
			valueBits &= ~eSdk2;
		} else {
			valueBits |= eSdk2;
		} */

		ensureBinaryDecompressed(ndsHeader, moduleParams);

		const bool buttonsRemappedBool = buttonsRemapped();

		patchCardNdsArm9(
			(cardengineArm9*)CARDENGINEI_ARM9_LOCATION,
			ndsHeader,
			moduleParams,
			1,
			buttonsRemappedBool
		);
		patchCardNdsArm7(
			(cardengineArm7*)ce7,
			ndsHeader,
			moduleParams,
			buttonsRemappedBool
		);

		hookNdsRetailArm7(
			(cardengineArm7*)ce7,
			ndsHeader
		);
		hookNdsRetailArm9(
			(cardengineArm9*)CARDENGINEI_ARM9_LOCATION,
			ndsHeader
		);

		extern u32 iUncompressedSize;

		fileWrite((char*)ndsHeader->arm9destination, &pageFile, 0x14000, iUncompressedSize);
		fileWrite((char*)ndsHeader->arm7destination, &pageFile, 0x2C0000, ndsHeader->arm7binarySize);
		fileWrite((char*)&iUncompressedSize, &pageFile, 0x5FFFF0, sizeof(u32));
		fileWrite((char*)&ndsHeader->arm7binarySize, &pageFile, 0x5FFFF4, sizeof(u32));
		/* } else {
			*(u32*)ARM9_DEC_SIZE_LOCATION = iUncompressedSize;
			ndmaCopyWordsAsynch(0, ndsHeader->arm9destination, (char*)ndsHeader->arm9destination+0x400000, *(u32*)ARM9_DEC_SIZE_LOCATION);
			ndmaCopyWordsAsynch(1, ndsHeader->arm7destination, (char*)DONOR_ROM_ARM7_LOCATION, ndsHeader->arm7binarySize);
			while (ndmaBusy(0) || ndmaBusy(1));
		} */
		if ((valueBits & isDlp) && !(valueBits & isSdk5)) {
			tonccpy((u8*)0x027FF000, (u8*)0x02FFF000, 0x1000);
		}
		valueBits &= ~isDlp;
	} else {
		//driveInitialize();

		u32 iUncompressedSize = 0;
		u32 newArm7binarySize = 0;
		fileRead((char*)&iUncompressedSize, &pageFile, 0x5FFFF0, sizeof(u32));
		fileRead((char*)&newArm7binarySize, &pageFile, 0x5FFFF4, sizeof(u32));
		if (valueBits & clearRamOnReset) {
			dma_twlFill32Async(1, 0, (void*)0x02000000+iUncompressedSize, 0x3E0000-iUncompressedSize);
		}
		fileRead((char*)ndsHeader->arm9destination, &pageFile, 0x14000, iUncompressedSize);
		if (valueBits & clearRamOnReset) {
			while (ndmaBusy(1));
		}
		fileRead((char*)ndsHeader->arm7destination, &pageFile, 0x2C0000, newArm7binarySize);
	} /* else {
		ndmaCopyWordsAsynch(0, (char*)ndsHeader->arm9destination+0x400000, ndsHeader->arm9destination, *(u32*)ARM9_DEC_SIZE_LOCATION);
		ndmaCopyWordsAsynch(1, (char*)DONOR_ROM_ARM7_LOCATION, ndsHeader->arm7destination, ndsHeader->arm7binarySize);
		while (ndmaBusy(0) || ndmaBusy(1));
	} */
	#else
	//bool doBak = ((valueBits & gameOnFlashcard) && (valueBits & b_dsiSD));
	//if (doBak) bakSdData();

	//driveInitialize();

	u32 iUncompressedSize = 0;
	u32 iUncompressedSizei = 0;
	u32 newArm7binarySize = 0;
	u32 newArm7ibinarySize = 0;

	fileRead((char*)&iUncompressedSize, &pageFile, 0x5FFFF0, sizeof(u32));
	fileRead((char*)&newArm7binarySize, &pageFile, 0x5FFFF4, sizeof(u32));
	fileRead((char*)&iUncompressedSizei, &pageFile, 0x5FFFF8, sizeof(u32));
	fileRead((char*)&newArm7ibinarySize, &pageFile, 0x5FFFFC, sizeof(u32));
	fileRead((char*)ndsHeader->arm9destination, &pageFile, 0x14000, iUncompressedSize);
	fileRead((char*)ndsHeader->arm7destination, &pageFile, 0x2C0000, newArm7binarySize);
	fileRead((char*)(*(u32*)0x02FFE1C8), &pageFile, 0x300000, iUncompressedSizei);
	fileRead((char*)(*(u32*)0x02FFE1D8), &pageFile, 0x580000, newArm7ibinarySize);

	if (sixInHeader) {
		*(u8*)(DSI_HEADER_SDK5+0x234) = 6;
	}
	//if (doBak) restoreSdBakData();
	#endif
	toncset((char*)((valueBits & isSdk5) ? 0x02FFFD80 : 0x027FFD80), 0, 0x80);
	toncset((char*)((valueBits & isSdk5) ? 0x02FFFF80 : 0x027FFF80), 0, 0x80);

	sharedAddr[0] = 0x44414F4C; // 'LOAD'

	for (i = 0; i < 4; i++) {
		for(reg=0; reg<0x1c; reg+=4)*((vu32*)(0x04004104 + ((i*0x1c)+reg))) = 0;//Reset NDMA.
	}

	while (sharedAddr[0] != 0x544F4F42) { // 'BOOT'
		while (REG_VCOUNT != 191) swiDelay(100);
		while (REG_VCOUNT == 191) swiDelay(100);
	}

	// Start ARM7
	ndsCodeStart(ndsHeader->arm7executeAddress);
}

static void cardReadLED(const bool on, const bool dmaLed) {
	if (!(valueBits & i2cBricked) && consoleModel < 2) { /* Proceed below */ } else { return; }

	/* static bool ledIsOn = false;
	static bool dmaLedIsOn = false;
	if (dmaLed ? dmaLedIsOn == on : ledIsOn == on) {
		return;
	}
	dmaLed
		? (dmaLedIsOn = on)
		: (ledIsOn = on); */

	if (dmaRomRead_LED == -1) dmaRomRead_LED = romRead_LED;
	if (!powerLedChecked && (romRead_LED || dmaRomRead_LED)) {
		const u8 byte = i2cReadRegister(0x4A, 0x63);
		powerLedIsPurple = (byte == 0xFF);
		powerLedChecked = true;
	}
	if (on) {
		switch(dmaLed ? dmaRomRead_LED : romRead_LED) {
			case 0:
			default:
				break;
			case 1:
				i2cWriteRegister(0x4A, 0x30, 0x13);    // Turn WiFi LED on
				break;
			case 2:
				i2cWriteRegister(0x4A, 0x63, powerLedIsPurple ? 0x00 : 0xFF);    // Turn power LED purple
				break;
			case 3:
				i2cWriteRegister(0x4A, 0x31, 0x01);    // Turn Camera LED on
				break;
		}
	} else {
		switch(dmaLed ? dmaRomRead_LED : romRead_LED) {
			case 0:
			default:
				break;
			case 1:
				i2cWriteRegister(0x4A, 0x30, 0x12);    // Turn WiFi LED off
				break;
			case 2:
				i2cWriteRegister(0x4A, 0x63, powerLedIsPurple ? 0xFF : 0x00);    // Revert power LED to normal
				break;
			case 3:
				i2cWriteRegister(0x4A, 0x31, 0x00);    // Turn Camera LED off
				break;
		}
	}
}

/*static void asyncCardReadLED(bool on) {
	if (consoleModel < 2) {
		if (on) {
			switch(romread_LED) {
				case 0:
				default:
					break;
				case 1:
					i2cWriteRegister(0x4A, 0x63, 0xFF);    // Turn power LED purple
					break;
				case 2:
					i2cWriteRegister(0x4A, 0x30, 0x13);    // Turn WiFi LED on
					break;
			}
		} else {
			switch(romread_LED) {
				case 0:
				default:
					break;
				case 1:
					i2cWriteRegister(0x4A, 0x63, 0x00);    // Revert power LED to normal
					break;
				case 2:
					i2cWriteRegister(0x4A, 0x30, 0x12);    // Turn WiFi LED off
					break;
			}
		}
	}
}*/

extern void inGameMenu(void);

static inline void rebootConsole(void) {
	if (valueBits & i2cBricked) {
		u8 readCommand = readPowerManagement(0x10);
		readCommand |= BIT(0);
		writePowerManagement(0x10, readCommand); // Reboot console
		return;
	}
	i2cWriteRegister(0x4A, 0x70, 0x01);
	i2cWriteRegister(0x4A, 0x11, 0x01);
}

void forceGameReboot(void) {
	toncset((u32*)0x02000000, 0, 0x400);
	*(u32*)0x02000000 = BIT(3);
	*(u32*)0x02000004 = 0x54455352; // 'RSET'
	sharedAddr[4] = 0x57534352;
	IPC_SendSync(0x8);
	u32 clearBuffer = 0;
	#ifdef TWLSDK
	//bool doBak = ((valueBits & gameOnFlashcard) && (valueBits & b_dsiSD));
	//if (doBak) bakSdData();
	#endif
	//driveInitialize();
	fileWrite((char*)&clearBuffer, &srParamsFile, 0, 0x4);
  	#ifdef TWLSDK
	//if (doBak) restoreSdBakData();
	#endif
	if (consoleModel < 2) {
		unlaunchSetFilename();
		readSoftResetId(false);
		waitFrames(5);							// Wait for DSi screens to stabilize
	} else {
		readSoftResetId(false);
		waitFrames(1);
	}
	rebootConsole();		// Force-reboot game
}

#ifdef TWLSDK
/* static void initMBK_dsiMode(void) {
	// This function has no effect with ARM7 SCFG locked
	*(vu32*)REG_MBK1 = *(u32*)0x02FFE180;
	*(vu32*)REG_MBK2 = *(u32*)0x02FFE184;
	*(vu32*)REG_MBK3 = *(u32*)0x02FFE188;
	*(vu32*)REG_MBK4 = *(u32*)0x02FFE18C;
	*(vu32*)REG_MBK5 = *(u32*)0x02FFE190;
	REG_MBK6 = *(u32*)0x02FFE1A0;
	REG_MBK7 = *(u32*)0x02FFE1A4;
	REG_MBK8 = *(u32*)0x02FFE1A8;
	REG_MBK9 = *(u32*)0x02FFE1AC;
} */

extern bool dldiPatchBinary (unsigned char *binData, u32 binSize);
#endif

void returnToLoader(bool reboot) {
	toncset((u32*)0x02000000, 0, 0x400);
	*(u32*)0x02000000 = BIT(0) | BIT(1) | BIT(2);
	*(u32*)0x02000004 = 0x54455352; // 'RSET'
	sharedAddr[4] = 0x57534352;
#ifdef TWLSDK
	u32 twlCfgLoc = *(u32*)0x02FFFDFC;
	if (twlCfgLoc != 0x02000400) {
		tonccpy((u8*)0x02000400, (u8*)twlCfgLoc, 0x128);
	}

	if (reboot || !(valueBits & b_dsiBios) || ((valueBits & twlTouch) && !(*(u8*)0x02FFE1BF & BIT(0))) || ((valueBits & b_dsiSD) && (valueBits & wideCheatUsed))) {
		if (consoleModel < 2) {
			unlaunchSetFilename();
			readSoftResetId(true);
			waitFrames(5);							// Wait for DSi screens to stabilize
		} else {
			readSoftResetId(true);
			waitFrames(1);
		}
		rebootConsole();
	}

	register int i, reg;

	REG_IME = 0;

	for (i = 0; i < 16; i++) {
		SCHANNEL_CR(i) = 0;
		SCHANNEL_TIMER(i) = 0;
		SCHANNEL_SOURCE(i) = 0;
		SCHANNEL_LENGTH(i) = 0;
	}

	REG_SOUNDCNT = 0;
	REG_SNDCAP0CNT = 0;
	REG_SNDCAP1CNT = 0;

	REG_SNDCAP0DAD = 0;
	REG_SNDCAP0LEN = 0;
	REG_SNDCAP1DAD = 0;
	REG_SNDCAP1LEN = 0;

	// Clear out ARM7 DMA channels and timers
	for (i = 0; i < 4; i++) {
		DMA_CR(i) = 0;
		DMA_SRC(i) = 0;
		DMA_DEST(i) = 0;
		TIMER_CR(i) = 0;
		TIMER_DATA(i) = 0;
	}

	// Clear out FIFO
	REG_IPC_SYNC = 0;
	REG_IPC_FIFO_CR = IPC_FIFO_ENABLE | IPC_FIFO_SEND_CLEAR;
	REG_IPC_FIFO_CR = 0;

	REG_IE = 0;
	REG_IF = ~0;
	REG_AUXIE = 0;
	REG_AUXIF = ~0;
	*(vu32*)0x0380FFFC = 0;  // IRQ_HANDLER ARM7 version
	*(vu32*)0x0380FFF8 = 0; // VBLANK_INTR_WAIT_FLAGS, ARM7 version
	REG_POWERCNT = 1;  // Turn off power to stuff

	REG_AUXIE &= ~(1UL << 8);
	*(vu32*)0x400481C = 0;
	*(vu32*)0x4004820 = 0;

	//driveInitialize();

	aFile file;
	getFileFromCluster(&file, quitFileCluster, (valueBits & quitOnFlashcard));
	if (file.firstCluster == CLUSTER_FREE) {
		// File not found, so reboot console instead
		i2cWriteRegister(0x4A, 0x70, 0x01);
		i2cWriteRegister(0x4A, 0x11, 0x01);
	}

	fileRead((char*)__DSiHeader, &file, 0, sizeof(tDSiHeader));
	*ndsHeader = __DSiHeader->ndshdr;

	fileRead(__DSiHeader->ndshdr.arm9destination, &file, (u32)__DSiHeader->ndshdr.arm9romOffset, __DSiHeader->ndshdr.arm9binarySize);
	fileRead(__DSiHeader->ndshdr.arm7destination, &file, (u32)__DSiHeader->ndshdr.arm7romOffset, __DSiHeader->ndshdr.arm7binarySize);
	if (ndsHeader->unitCode > 0) {
		fileRead(__DSiHeader->arm9idestination, &file, (u32)__DSiHeader->arm9iromOffset, __DSiHeader->arm9ibinarySize);
		fileRead(__DSiHeader->arm7idestination, &file, (u32)__DSiHeader->arm7iromOffset, __DSiHeader->arm7ibinarySize);

		// Disabled due to ce7 code taking place in DSi WRAM
		// initMBK_dsiMode();
	}

	if (!(valueBits & b_dsiSD)) {
		dldiPatchBinary(ndsHeader->arm9destination, ndsHeader->arm9binarySize);
	}

	sharedAddr[0] = 0x44414F4C; // 'LOAD'

	for (i = 0; i < 4; i++) {
		for(reg=0; reg<0x1c; reg+=4)*((vu32*)(0x04004104 + ((i*0x1c)+reg))) = 0;//Reset NDMA.
	}

	while (sharedAddr[0] != 0x544F4F42) { // 'BOOT'
		while (REG_VCOUNT != 191) swiDelay(100);
		while (REG_VCOUNT == 191) swiDelay(100);
	}

	// Start ARM7
	ndsCodeStart(ndsHeader->arm7executeAddress);
#else
	IPC_SendSync(0x8);
	if (consoleModel < 2) {
		unlaunchSetFilename();
		readSoftResetId(true);
		waitFrames(5);							// Wait for DSi screens to stabilize
	} else {
		readSoftResetId(true);
		waitFrames(1);
	}

	rebootConsole();		// Reboot into TWiLight Menu++
#endif
}

/*
    Step 3b: append one earned achievement id to sd:/ra_unlocks.txt.

    This is the half of the loop with no network in it. rcheevos fires on the ARM9, inside the game's
    VCOUNT handler, where there is no SD card -- on a DSi the card is this CPU's. So the id crosses
    through sharedAddr and lands here, and the *next* boot's launcher sends it. See RA_QUEUE_PATH.

    Fixed 16-byte records, so record N is at offset N*16 and no index has to be stored anywhere: the
    append point is found once, by reading the first byte of each record until one is not a digit. The
    launcher zero-fills the file and packs the ids it could not send to the front, so that byte is
    exactly the boundary. Sixty-four one-record reads, on the frame of the first unlock of a session,
    and never again.

    Refuses rather than grows. The file's length is fixed because this CPU can only write into clusters
    that already exist -- it cannot allocate -- so a full queue is dropped and counted rather than
    extended. A session earning sixty-four achievements before a reboot is not a thing.

    The statics are magic-guarded for the same reason the overlay's are: this binary is copied in
    without a crt0, so .bss holds whatever the previous occupant left and a plain zero means nothing.
*/
#define RA_UNLOCK_STATE_MAGIC 0x314C5541   /* 'AUL1' */

static u32 raUnlockStateMagic;
static u8  raUnlockSlot;      /* next record to write, RA_QUEUE_MAX when full */

/*
    Initialised, and **in .data rather than .bss**, which is what lets the read path be a bare copy
    with no validity magic and no branch in front of it.

    Every other static this feature owns is magic-guarded, because .bss in an injected binary holds
    whatever the previous occupant left and a plain zero means nothing. An initialised array is a
    different thing: it is part of the image the bootloader copies in, so it arrives holding exactly
    these fourteen bytes whether or not anything has run yet.

    And they are chosen to be the safe answer. `20000000000000` is well formed and has month 00, so
    raQueueStampToUnix() refuses it and the launcher submits without `o=` -- the same outcome
    `queue=2` produces on purpose. So a capture that somehow never happened costs an unlock its date
    and nothing else, with no code spent checking for a case that cannot occur.
*/
static char raStampCache[RA_QUEUE_STAMP] = "20000000000000";

/*
    The console's clock as `YYYYMMDDhhmmss`, **read once, at init, and never again while a game runs.**

    That last part is the whole point of this function existing separately, and it is a fix rather than
    a tidy-up. Reading the RTC on the frame an achievement fires is what froze Ketsui, confirmed by
    bisection: `queue=1` hangs on the boss kill, `queue=2` -- identical in every respect except that it
    skips this read -- clears the same stage. `queue=3` and `queue=0` clear it too. One variable.

    Two ways it can hurt and this fork does not need to know which, because the fix removes both:

      duration   rtcTransaction() bit-bangs RTC_CR8 with swiDelay(48) on every clock edge, and
                 rtcGetTimeAndDate() does two of them -- about 166 delays, near a millisecond, spent
                 inside the VBlank handler with the I bit set. That is a millisecond in which the FIFO
                 handler is not answering the ARM9's card reads, at the one moment a bullet-hell
                 shooter has least to spare.

      collision  the RTC is a serial bus with a chip select, and plenty of DS games read it from their
                 own ARM7. Our handler interrupts the game's main loop; if that loop was mid
                 transaction, driving CS and SCK underneath it destroys the transaction, and the reply
                 the game is waiting for never comes.

    Called from initialize(), which runs when the game sets up its interrupts -- before its main loop
    exists, so neither hazard applies there.

    **What this costs is accuracy, and it is stated rather than hidden.** Every unlock in a session is
    now stamped with the moment the session started, not the moment it fired. An hour into a long
    session that is an hour of error. Measured against what it replaces that is still the right trade:
    without `o=` at all the server dates an unlock by the boot that *reported* it, which with this
    client can be the next day. Session-start is never worse than that and usually far better.

    The exact version is designed and not built, because it does not fit here: the ARM7 would count
    VBlanks and add the elapsed seconds to the captured time, and the day rollover would go to the
    launcher, which owns the calendar and has host tests behind it. That is 60-100 bytes of division
    by constants on a binary with sixty spare. See "What is left" in docs/retroachievements.md.

    rtcGetTimeAndDate() hands back plain integers, not BCD -- it masks the 12/24-hour bit and calls
    BCDToInteger(t, 7) itself before returning, which is what the disassembly of libnds7 shows. The one
    thing it does not normalise is the PM flag in 12-hour mode, where the hour comes back with 40 added
    to it (RTCtime's own comment: "0 to 11 for AM, 52 to 63 for PM"). Subtracting it is a no-op on a
    console set to 24 hours and the difference between 21:xx and 61:xx on one set to 12.

    **Not validated here, on purpose.** raQueueStampToUnix() already refuses any date it will not vouch
    for -- a console whose clock was never set reads as year 0 and comes out as `2000...`, below its
    floor -- and it is the copy with a host test behind it. Checking the same ranges twice cost this
    binary bytes it does not have.

    Only the digits are written here and none of the arithmetic. Turning them into seconds is the
    launcher's job, which keeps the calendar -- leap years, month lengths, the epoch -- on the side
    that has a host test, and keeps this side to a copy loop.
*/
static __attribute__((noinline)) void raStampCapture(void) {
	char* const out = raStampCache;
	RTCtime now;
	u8      field[5];
	u8      hours;
	int     f;
	int     at = 0;

	rtcGetTimeAndDate((uint8*)&now);
	hours = now.hours;
	if (hours >= 40) {
		hours -= 40;
	}

	field[0] = now.month;
	field[1] = now.day;
	field[2] = hours;
	field[3] = now.minutes;
	field[4] = now.seconds;

	out[at++] = '2';
	out[at++] = '0';
	out[at++] = (char)('0' + (now.year / 10));
	out[at++] = (char)('0' + (now.year % 10));
	for (f = 0; f < 5; f++) {
		out[at++] = (char)('0' + (field[f] / 10));
		out[at++] = (char)('0' + (field[f] % 10));
	}
}


/* my_fat.c's sector cache; the queue may be on either card, so both are cleared. */
extern int prevSect[2];

static void raUnlockAppend(u32 id, const char* stamp, int hardcore) {
	char record[RA_QUEUE_RECORD];
	char digits[11];
	u32  value = id;
	int  n = 0;
	int  i;

	if (raUnlocksCluster == 0 || id == 0) {
		return;
	}

	if (raUnlockStateMagic != RA_UNLOCK_STATE_MAGIC) {
		u8 slot;

		raUnlockStateMagic = RA_UNLOCK_STATE_MAGIC;

		/*
		    Once before the first read, for the reason fileWrite() forces its own: a partial read is
		    served out of a buffer shared with the running game's card traffic, and this scan read a
		    digit where the file held a NUL and put the record in slot 1 with slot 0 empty. One clear
		    rather than one per iteration -- the first read is the one that decides, and this binary
		    is the tightest in the tree. Written inline rather than through resetPrevSect() for the
		    same reason: the call plus the retained function body did not fit.
		*/
		prevSect[0] = -1;
		prevSect[1] = -1;

		for (slot = 0; slot < RA_QUEUE_MAX; slot++) {
			fileRead(record, &raUnlocksFile, slot * RA_QUEUE_RECORD, 1);
			if (record[0] < '0' || record[0] > '9') {
				break;
			}
		}
		raUnlockSlot = slot;
	}

	if (raUnlockSlot >= RA_QUEUE_MAX) {
		return;
	}

	while (value && n < (int)sizeof(digits)) {
		digits[n++] = (char)('0' + (value % 10));
		value /= 10;
	}
	toncset(record, 0, RA_QUEUE_RECORD);
	for (i = 0; i < n; i++) {
		record[i] = digits[n - 1 - i];
	}
	record[n] = '\n';

	/*
	    Stamp it with when, not only what, so the launcher can send `o=` and the server dates the
	    unlock by the moment it was earned instead of the boot that reported it -- which with this
	    client can be a day later. See RA_QUEUE_PATH for the record layout.

	    `stamp` is read by the caller, outside the critical section, and is NULL when the clock could
	    not be believed -- in which case the record is a bare id and is sent without `o=`, which is
	    exactly what every unlock did before this existed.
	*/
	if (stamp) {
		int at = n;

		record[at++] = '\t';
		for (i = 0; i < RA_QUEUE_STAMP; i++) {
			record[at++] = stamp[i];
		}

		/*
		    And which game it came from -- `gameCode` identifies the release and `gameTitle` is what a
		    human reads. No network and nothing passed down from the launcher, which is what the case
		    this exists for demands: a queue full of one game's unlocks while another is running and
		    there is no WiFi to drain it.

		    Read from the copies taken at init, **not** from ndsHeader here. That header is the
		    loader's and the game reuses its memory once running, so reading it on the frame an
		    achievement fires works early in a session and returns nothing later in one.

		    Both are fixed-width fields that are *not* NUL-terminated and are padded with spaces, and
		    neither is guaranteed to be text at all -- a homebrew ROM can put anything there. So
		    anything outside printable ASCII stops the copy, which also keeps a stray tab or newline
		    from inventing a field boundary in a record that is delimited by them.
		*/
		/*
		    And which game it came from, out of the running ROM's own header -- `gameCode` identifies
		    the release and `gameTitle` is what a human reads. No network, nothing passed down from the
		    launcher, and available on the frame the achievement fires, which is what the case this
		    exists for demands: a queue full of one game's unlocks while another is running and there
		    is no WiFi to drain it.

		    **Copied raw, and the tidying happens in the launcher.** Both fields are fixed width, are
		    not NUL-terminated, are padded with spaces or NULs, and are not guaranteed to be text at
		    all. Trimming and range-checking them here cost more than this binary has: cardenginei_arm7
		    for TWL-SDK games links into 33K and had **76 bytes** spare before this field existed. So
		    the bytes go out as they are and raQueueScan() stops at the first one that is not printable
		    and drops trailing padding -- which it already did.

		    Copied with tonccpy() rather than a loop, and that is a size decision measured rather than
		    guessed: a loop with a constant bound of twelve is one gcc unrolls into twelve load/store
		    pairs, which cost this binary 264 bytes it does not have.

		    It is the same split as the stamp, for the same reason: this side writes bytes, the side
		    with a host test interprets them. The worst a hostile header can do is garble its own
		    display name -- the id and the stamp are written before it and are not reachable from here.
		*/
		record[at++] = '\t';
		tonccpy(record + at, raGameCode, RA_QUEUE_CODE);
		at += RA_QUEUE_CODE;
		record[at++] = '\t';
		tonccpy(record + at, raGameTitle, RA_QUEUE_TITLE);
		at += RA_QUEUE_TITLE;

		/*
		    And which mode it was earned in, so the boot that sends it cannot decide that for itself.

		    Without this the mode came from ra.cfg at submission time, which meant a player could earn
		    unlocks in softcore with the menu's RAM editor open, set hardcore=1, boot, and watch them
		    go out as hardcore. The record is the only place that can hold the truth, because it is
		    the only thing that survives from the session that earned it to the boot that sends it.

		    Last, because the fields are positional and everything before it already shipped. A record
		    that lost its stamp has no game either and now has no mode either, and reads as softcore:
		    the fail-safe direction, and the same thing an older build's records read as.

		    Two characters, and the record was sized for them -- 44 bytes used of 48 before this, 46
		    after. This binary is the tightest in the tree, so it writes a byte it is handed rather
		    than deciding anything: `hardcore` came across in the request's own magic.
		*/
		record[at++] = '\t';
		record[at++] = hardcore ? '1' : '0';
		record[at] = '\n';
	}

	fileWrite(record, &raUnlocksFile, raUnlockSlot * RA_QUEUE_RECORD, RA_QUEUE_RECORD);
	raUnlockSlot++;
}

void dumpRam(void) {
	#ifdef TWLSDK
	//bool doBak = ((valueBits & gameOnFlashcard) && (valueBits & b_dsiSD));
	//if (doBak) bakSdData();
	#endif
	//driveInitialize();
	sharedAddr[3] = 0x444D4152;
	// Dump RAM
	// #ifdef TWLSDK
	fileWrite((char*)0x0C000000, &ramDumpFile, 0, (consoleModel==0 ? 0x01000000 : 0x02000000));
	/* #else
	if (valueBits & dsiMode) {
		// Dump full RAM
		fileWrite((char*)0x0C000000, &ramDumpFile, 0, (consoleModel==0 ? 0x01000000 : 0x02000000));
	} else if (valueBits & isSdk5) {
		// Dump RAM used in DS mode (SDK5)
		fileWrite((char*)0x02000000, &ramDumpFile, 0, 0x3E0000);
		fileWrite((char*)(ndsHeader->unitCode==2 ? 0x02FE0000 : 0x027E0000), &ramDumpFile, 0x3E0000, 0x1F000);
		fileWrite((char*)0x02FFF000, &ramDumpFile, 0x3FF000, 0x1000);
	} else if (moduleParams->sdk_version >= 0x2008000) {
		// Dump RAM used in DS mode (SDK2.1+)
		fileWrite((char*)0x02000000, &ramDumpFile, 0, 0x3E0000);
		fileWrite((char*)0x027E0000, &ramDumpFile, 0x3E0000, 0x20000);
	} else {
		// Dump RAM used in DS mode (SDK2.0)
		fileWrite((char*)0x02000000, &ramDumpFile, 0, 0x3C0000);
		fileWrite((char*)0x027C0000, &ramDumpFile, 0x3C0000, 0x40000);
	}
	#endif */
	sharedAddr[3] = 0;
  	#ifdef TWLSDK
	//if (doBak) restoreSdBakData();
	#endif
}

void prepareScreenshot(void) {
#ifdef TWLSDK
	//bool doBak = ((valueBits & gameOnFlashcard) && (valueBits & b_dsiSD));
	//if (doBak) bakSdData();
#endif
		//driveInitialize();
		fileWrite((char*)INGAME_MENU_EXT_LOCATION, &pageFile, 0x540000, 0x40000);
#ifdef TWLSDK
	//if (doBak) restoreSdBakData();
#endif
}

void saveScreenshot(void) {
	if (igmText->currentScreenshot >= 50) return;

#ifdef TWLSDK
	//bool doBak = ((valueBits & gameOnFlashcard) && (valueBits & b_dsiSD));
	//if (doBak) bakSdData();
#endif
	//driveInitialize();
	fileWrite((char*)INGAME_MENU_EXT_LOCATION, &screenshotFile, 0x200 + (igmText->currentScreenshot * 0x18400), 0x18046);

	// Skip until next blank slot
	char magic;
	do {
		igmText->currentScreenshot++;
		fileRead(&magic, &screenshotFile, 0x200 + (igmText->currentScreenshot * 0x18400), 1);
	} while(magic == 'B' && igmText->currentScreenshot < 50);

		fileRead((char*)INGAME_MENU_EXT_LOCATION, &pageFile, 0x540000, 0x40000);
#ifdef TWLSDK
	//if (doBak) restoreSdBakData();
#endif
}

void prepareManual(void) {
#ifdef TWLSDK
	//bool doBak = ((valueBits & gameOnFlashcard) && (valueBits & b_dsiSD));
	//if (doBak) bakSdData();
#endif
		//driveInitialize();
		fileWrite((char*)INGAME_MENU_EXT_LOCATION, &pageFile, 0x540000, 32 * 24);
#ifdef TWLSDK
	//if (doBak) restoreSdBakData();
#endif
}

void readManual(int line) {
	static int currentManualLine = 0;
	static int currentManualOffset = 0;
	char buffer[32];

	// Seek for desired line
	bool firstLoop = true;
	while(currentManualLine != line) {
		if(line > currentManualLine) {
			fileRead(buffer, &manualFile, currentManualOffset, 32);

			for(int i = 0; i < 32; i++) {
				if(buffer[i] == '\n') {
					currentManualOffset += i + 1;
					currentManualLine++;
					break;
				} else if(i == 31) {
					currentManualOffset += i + 1;
					break;
				}
			}
		} else {
			currentManualOffset -= 32;
			fileRead(buffer, &manualFile, currentManualOffset, 32);
			int i = firstLoop ? 30 : 31;
			firstLoop = false;
			for(; i >= 0; i--) {
				if((buffer[i] == '\n') || currentManualOffset + i == -1) {
					currentManualOffset += i + 1;
					currentManualLine--;
					firstLoop = true;
					break;
				}
			}
		}
	}

	toncset((u8*)INGAME_MENU_EXT_LOCATION, ' ', 32 * 24);
	((vu8*)INGAME_MENU_EXT_LOCATION)[32 * 24] = '\0';

	// Read in 24 lines
	u32 tempManualOffset = currentManualOffset;
	bool fullLine = false;
	for(int line = 0; line < 24 && line < igmText->manualMaxLine; line++) {
		fileRead(buffer, &manualFile, tempManualOffset, 32);

		// Fix for exactly 32 char lines
		if(fullLine && buffer[0] == '\n')
			fileRead(buffer, &manualFile, ++tempManualOffset, 32);

		for(int i = 0; i <= 32; i++) {
			if(i == 32 || buffer[i] == '\n' || buffer[i] == '\0') {
				tempManualOffset += i;
				if(buffer[i] == '\n')
					tempManualOffset++;
				fullLine = i == 32;
				tonccpy((char*)INGAME_MENU_EXT_LOCATION + line * 32, buffer, i);
				break;
			}
		}
	}
}

void restorePreManual(void) {
#ifdef TWLSDK
	//bool doBak = ((valueBits & gameOnFlashcard) && (valueBits & b_dsiSD));
	//if (doBak) bakSdData();
#endif
		//driveInitialize();
		fileRead((char*)INGAME_MENU_EXT_LOCATION, &pageFile, 0x540000, 32 * 24);
#ifdef TWLSDK
	//if (doBak) restoreSdBakData();
#endif
}

void saveMainScreenSetting(void) {
	fileWrite((char*)&mainScreen, &patchOffsetCacheFile, 0x1FC, sizeof(u32));
}

void saveMainScreenSettingIgm(void) {
	fileWrite((char*)sharedAddr, &patchOffsetCacheFile, 0x1FC, sizeof(u32));
}

void loadInGameMenu(void) {
	const u32 igmLocation = INGAME_MENU_LOCATION;

	sharedAddr[5] = 0x4C4D4749; // 'IGML'
	fileWrite((char*)igmLocation, &pageFile, 0xA000, 0xA000);	// Backup part of game RAM to page file
	fileRead((char*)igmLocation, &pageFile, 0, 0xA000);	// Read in-game menu
	sharedAddr[5] = 0;
}

void unloadInGameMenu(void) {
	while (REG_VCOUNT != 191) swiDelay(100);
	while (REG_VCOUNT == 191) swiDelay(100);

	const u32 igmLocation = INGAME_MENU_LOCATION;

	sharedAddr[5] = 0x4C4D4749; // 'IGML'
	fileWrite((char*)igmLocation, &pageFile, 0, 0xA000);	// Store in-game menu
	fileRead((char*)igmLocation, &pageFile, 0xA000, 0xA000);	// Restore part of game RAM from page file
	sharedAddr[5] = 0;
}

#ifndef TWLSDK
static void vcountIrqLower()
{
    while (1)
    {
        if (sActiveFpsa.initial)
        {
            sActiveFpsa.initial = FALSE;
            break;
        }

        if (!sActiveFpsa.backJump)
            sActiveFpsa.cycleDelta += sActiveFpsa.targetCycles - ((u64)FPSA_CYCLES_PER_FRAME << 24);
        u32 linesToAdd = 0;
        while (sActiveFpsa.cycleDelta >= (s64)((u64)FPSA_CYCLES_PER_LINE << 23))
        {
            sActiveFpsa.cycleDelta -= (u64)FPSA_CYCLES_PER_LINE << 24;
            if (++linesToAdd == 5)
                break;
        }
        if (linesToAdd == 0)
        {
            sActiveFpsa.backJump = FALSE;
            break;
        }
        if (linesToAdd > 1)
        {
            sActiveFpsa.backJump = TRUE;
        }
        else
        {
            // don't set the backJump flag because the irq is not retriggered if the new vcount
            // is the same as the previous line
            sActiveFpsa.backJump = FALSE;
        }
        // ensure we won't accidentally run out of line time
        while (REG_DISPSTAT & DISP_IN_HBLANK)
            ;
        int curVCount = REG_VCOUNT;
        REG_VCOUNT = curVCount - (linesToAdd - 1);
        if (linesToAdd == 1)
            break;

        while (REG_VCOUNT >= curVCount)//FPSA_ADJUST_MAX_VCOUNT - 5)
            ;
        while (REG_VCOUNT < curVCount)//FPSA_ADJUST_MAX_VCOUNT - 5)
            ;
    }
    REG_IF = IRQ_VCOUNT;
}

static void vcountIrqHigher()
{
    if (sActiveFpsa.initial)
    {
        sActiveFpsa.initial = FALSE;
        return;
    }
    sActiveFpsa.cycleDelta += ((u64)FPSA_CYCLES_PER_FRAME << 24) - sActiveFpsa.targetCycles;
    u32 linesToSkip = 0;
    while (sActiveFpsa.cycleDelta >= (s64)((u64)FPSA_CYCLES_PER_LINE << 23))
    {
        sActiveFpsa.cycleDelta -= (u64)FPSA_CYCLES_PER_LINE << 24;
        if (++linesToSkip == 55)
            break;
    }
    if (linesToSkip == 0)
        return;
    // ensure we won't accidentally run out of line time
    while (REG_DISPSTAT & DISP_IN_HBLANK)
        ;
    REG_VCOUNT = REG_VCOUNT + (linesToSkip + 1);
}

void fpsa_init(fpsa_t* fpsa)
{
    toncset(fpsa, 0, sizeof(fpsa_t));
    fpsa->isStarted = FALSE;
    fpsa_setTargetFrameCycles(fpsa, (u64)FPSA_CYCLES_PER_FRAME << 24); // default to no adjustment
}

void fpsa_start(fpsa_t* fpsa)
{
    // int irq = enterCriticalSection();
    do
    {
        if (fpsa->isStarted)
            break;
        if (fpsa->targetCycles == ((u64)FPSA_CYCLES_PER_FRAME << 24))
            break;
        fpsa->backJump = FALSE;
        fpsa->cycleDelta = 0;
        fpsa->initial = TRUE;
        fpsa->isFpsLower = fpsa->targetCycles >= ((u64)FPSA_CYCLES_PER_FRAME << 24);
        // prevent the irq from immediately happening
        while (REG_VCOUNT != FPSA_ADJUST_MAX_VCOUNT + 2)
            ;
        fpsa->isStarted = TRUE;
        if (fpsa->isFpsLower)
        {
            SetYtrigger(FPSA_ADJUST_MAX_VCOUNT - 5);
        }
        else
        {
            SetYtrigger(FPSA_ADJUST_MIN_VCOUNT);
        }
    } while (0);
    // leaveCriticalSection(irq);
}

void fpsa_stop(fpsa_t* fpsa)
{
    if (!fpsa->isStarted)
        return;
    fpsa->isStarted = FALSE;
}

void fpsa_setTargetFrameCycles(fpsa_t* fpsa, u64 cycles)
{
    fpsa->targetCycles = cycles;
}

void fpsa_setTargetFpsFraction(fpsa_t* fpsa, u32 num, u32 den)
{
    u64 cycles = (((double)FPSA_SYS_CLOCK * den * (1 << 24)) / num) + 0.5;
    fpsa_setTargetFrameCycles(fpsa, cycles);//((((u64)FPSA_SYS_CLOCK * (u64)den) << 24) + ((num + 1) >> 1)) / num);
}

void fpsa_run(void) {
    if (!sActiveFpsa.isStarted) {
        return;
	}
	sActiveFpsa.isFpsLower ? vcountIrqLower() : vcountIrqHigher();
}
#endif

#ifdef DEBUG
static void log_arm9(void) {
	//driveInitialize();
	u32 src = *(vu32*)(sharedAddr+2);
	u32 dst = *(vu32*)(sharedAddr);
	u32 len = *(vu32*)(sharedAddr+1);
	u32 marker = *(vu32*)(sharedAddr+3);

	dbg_printf("\ncard read received\n");

	if (calledViaIPC) {
		dbg_printf("\ntriggered via IPC\n");
	}
	dbg_printf("\nstr : \n");
	dbg_hexa((u32)cardStruct);
	dbg_printf("\nsrc : \n");
	dbg_hexa(src);
	dbg_printf("\ndst : \n");
	dbg_hexa(dst);
	dbg_printf("\nlen : \n");
	dbg_hexa(len);
	dbg_printf("\nmarker : \n");
	dbg_hexa(marker);

	dbg_printf("\nlog only \n");
}
#endif

static void nandRead(void) {
	u32 flash = *(vu32*)(sharedAddr+2);
	u32 memory = *(vu32*)(sharedAddr);
	u32 len = *(vu32*)(sharedAddr+1);
	#ifdef DEBUG
	u32 marker = *(vu32*)(sharedAddr+3);

	dbg_printf("\nnand read received\n");

	if (calledViaIPC) {
		dbg_printf("\ntriggered via IPC\n");
	}
	dbg_printf("\nflash : \n");
	dbg_hexa(flash);
	dbg_printf("\nmemory : \n");
	dbg_hexa(memory);
	dbg_printf("\nlen : \n");
	dbg_hexa(len);
	dbg_printf("\nmarker : \n");
	dbg_hexa(marker);
	#endif

	//driveInitialize();
	//cardReadLED(true, true);    // When a file is loading, turn on LED for card read indicator
	sdmmc_set_ndma_slot(4);
	fileRead((char *)memory, savFile, flash, len);
	sdmmc_set_ndma_slot(0);
	//cardReadLED(false, true);
}

static void nandWrite(void) {
	u32 flash = *(vu32*)(sharedAddr+2);
	u32 memory = *(vu32*)(sharedAddr);
	u32 len = *(vu32*)(sharedAddr+1);
	#ifdef DEBUG
	u32 marker = *(vu32*)(sharedAddr+3);

	dbg_printf("\nnand write received\n");

	if (calledViaIPC) {
		dbg_printf("\ntriggered via IPC\n");
	}
	dbg_printf("\nflash : \n");
	dbg_hexa(flash);
	dbg_printf("\nmemory : \n");
	dbg_hexa(memory);
	dbg_printf("\nlen : \n");
	dbg_hexa(len);
	dbg_printf("\nmarker : \n");
	dbg_hexa(marker);
	#endif

	//driveInitialize();
	saveTimer = 1;			// When we're saving, power button does nothing, in order to prevent corruption.
	//cardReadLED(true, true);    // When a file is loading, turn on LED for card read indicator
	sdmmc_set_ndma_slot(4);
	fileWrite((char *)memory, savFile, flash, len);
	sdmmc_set_ndma_slot(0);
	//cardReadLED(false, true);
}

#ifdef TWLSDK
static void sharedFontRead(void) {
	u32 flash = *(vu32*)(sharedAddr+2);
	u32 memory = *(vu32*)(sharedAddr);
	u32 len = *(vu32*)(sharedAddr+1);
	#ifdef DEBUG
	u32 marker = *(vu32*)(sharedAddr+3);

	dbg_printf("\nshared font read received\n");

	if (calledViaIPC) {
		dbg_printf("\ntriggered via IPC\n");
	}
	dbg_printf("\nflash : \n");
	dbg_hexa(flash);
	dbg_printf("\nmemory : \n");
	dbg_hexa(memory);
	dbg_printf("\nlen : \n");
	dbg_hexa(len);
	dbg_printf("\nmarker : \n");
	dbg_hexa(marker);
	#endif

	//driveInitialize();
	//cardReadLED(true, false);    // When a file is loading, turn on LED for card read indicator
	sdmmc_set_ndma_slot(4);
	fileRead((char *)memory, sharedFontFile, flash, len);
	sdmmc_set_ndma_slot(0);
	//cardReadLED(false, false);
}
#endif

/*static void slot2Read(void) {
	u32 src = *(vu32*)(sharedAddr+2);
	u32 dst = *(vu32*)(sharedAddr);
	u32 len = *(vu32*)(sharedAddr+1);
	#ifdef DEBUG
	u32 marker = *(vu32*)(sharedAddr+3);

	dbg_printf("\nslot2 read received\n");

	if (calledViaIPC) {
		dbg_printf("\ntriggered via IPC\n");
	}
	dbg_printf("\nsrc : \n");
	dbg_hexa(src);
	dbg_printf("\ndst : \n");
	dbg_hexa(dst);
	dbg_printf("\nlen : \n");
	dbg_hexa(len);
	dbg_printf("\nmarker : \n");
	dbg_hexa(marker);
	#endif

	cardReadLED(true);    // When a file is loading, turn on LED for card read indicator
	fileRead((char*)dst, *gbaFile, src, len, -1);
	cardReadLED(false);
}*/

static bool readOngoing = false;
//static bool sdReadOngoing = false;
//static bool ongoingIsDma = false;
//static int currentCmd=0, currentNdmaSlot=0;
//static int timeTillDmaLedOff = 0;

/*
    Step 3b: an achievement the ARM9 earned, on its way to the queue file.

    A critical section and no mutex, and that is a considered choice rather than an omission. The mutex
    the old ramDump path used, cardEgnineCommandMutex, is commented out at its declaration -- dead code
    -- and taking a private one would protect nothing, because the card-read path would not be taking
    it. What IME off does buy is that a competing read cannot *start* underneath this write, and on this
    CPU reads are driven from interrupt handlers.

    That was the whole of the argument, and it was not enough: see raCardReadPending() in ra.h, which
    carries the reasoning and the measurements. A read does not have to start underneath the write to
    be ruined by it -- it can already be half done, outstanding on the SD controller, with nothing but
    a command id and 512 bytes of shared buffer standing in for it. So `readOngoing` is checked here
    as well as IME held off, and the ARM9 does not even raise the request while a read of its own is
    unserved.

    The cost of the section is that an ARM9 read request waits a few milliseconds -- a stall, not
    corruption -- and only on the frame an achievement unlocks.

    The request word is cleared after the append, not before, so a frame that could not do the work
    leaves the request standing and the next one retries. That is what makes waiting free.

    **A function, explicitly noinline, rather than a block inside myIrqHandlerVBlank -- and that is
    measured rather than tidiness.** cardenginei_arm7 for TWL-SDK games links into 33K with 60 bytes
    spare, so where code sits is a size decision here. This block used to be inlined into the handler,
    which was 1860 bytes of straight-line code, and every arrangement was weighed against the region
    (text of this translation unit, baseline 7737):

      one bool test added to the condition, in place       7829   +92
      the two-word predicate added in place                7905  +168
      lifted to a function, gcc inlined it back            7817   +80
      lifted, noinline, both halves of the predicate       7793   +56  -- .bss 4 bytes past the region
      lifted, noinline, `readOngoing` only                 7753   +16  -- this

    Ninety-two bytes for one bool test is register pressure across a 1860-byte function, not the test.
    Lifted out, the handler drops to 1260 and this function is 612, and the test costs what a test
    costs. The last two lines are also why the other half of the wait runs on the ARM9: four bytes.
    See raCardReadPending().

    Two magics, one request, and the mode is which of them arrived -- see RA_SHARED_UNLOCK_HARDCORE.
    Read once into a local because the ARM9 may not write this word while it is non-zero, but reading
    it twice would still be two loads of a volatile for one answer.
*/
static __attribute__((noinline)) void raUnlockService(void) {
	const u32 req = sharedAddr[RA_SHARED_UNLOCK_REQ];
	int       oldIME;

	if (req != RA_SHARED_UNLOCK_MAGIC && req != RA_SHARED_UNLOCK_HARDCORE) {
		return;
	}
	if (readOngoing) {
		return;
	}

#ifndef TWLSDK
	/*
	    How much of it to do -- see RA_SHARED_UNLOCK_LEVEL. Only RA_QUEUE_LEVEL_HANDOFF is tested, so
	    anything else does the work: the failure direction is toward shipping behaviour. The request is
	    acknowledged either way, because a level that skips the write still has to clear the word or
	    the ARM9 stops offering.

	    **Not compiled in for TWL-SDK**, which ignores the level and always does the whole thing. The
	    slot read and its branch cost bytes that binary does not have -- it links into 33K with sixty
	    spare -- and doing more than asked is the safe direction to be wrong in. It is also where it is
	    least missed: the ladder exists to bisect a freeze on an NTR title, which loads a variant with
	    11K spare. Recorded in tools/ra.example.cfg so a level that appears to do nothing on a
	    DSi-enhanced game is explained rather than puzzling.
	*/
	if (sharedAddr[RA_SHARED_UNLOCK_LEVEL] != RA_QUEUE_LEVEL_HANDOFF)
#endif
	{
		oldIME = enterCriticalSection();

		/*
		    raStampCache rather than a local, and passed by pointer rather than copied. It is fourteen
		    bytes of initialised data that nothing in the critical section writes, so there is nothing
		    to copy it for -- and the copy, its stack slot and the function that did it were what put
		    this binary past the end of its region.
		*/
		raUnlockAppend(sharedAddr[RA_SHARED_UNLOCK_ID], raStampCache,
		               req == RA_SHARED_UNLOCK_HARDCORE);
		leaveCriticalSection(oldIME);
	}
	sharedAddr[RA_SHARED_UNLOCK_REQ] = 0;
}

static bool start_cardRead_arm9(void) {
	bool useApFixOverlays = false;
	u32 src = sharedAddr[2];
	u32 dst = sharedAddr[0];
	u32 len = sharedAddr[1];
	if (len >= 0x80000000) {
		len -= 0x80000000;
		useApFixOverlays = true;
	}
	#ifdef DEBUG
	u32 marker = sharedAddr[3];

	dbg_printf("\ncard read received v2\n");

	if (calledViaIPC) {
		dbg_printf("\ntriggered via IPC\n");
	}

	dbg_printf("\nstr : \n");
	dbg_hexa((u32)cardStruct);
	dbg_printf("\nsrc : \n");
	dbg_hexa(src);
	dbg_printf("\ndst : \n");
	dbg_hexa(dst);
	dbg_printf("\nlen : \n");
	dbg_hexa(len);
	dbg_printf("\nmarker : \n");
	dbg_hexa(marker);	
	#endif

	const bool isDma = (sharedAddr[3] == (vu32)0x025FFB09 || sharedAddr[3] == (vu32)0x025FFB0A);

	//driveInitialize();
	cardReadLED(true, isDma);    // When a file is loading, turn on LED for card read indicator
	#ifdef DEBUG
	nocashMessage("fileRead romFile");
	#endif
	if ((dst % 4) == 0) {
		if(!fileReadNonBLocking((char*)dst, useApFixOverlays ? apFixOverlaysFile : romFile, src, len))
		{
			readOngoing = true;
			return false;
			//while(!resumeFileRead()){}
		} 
		else
		{
			readOngoing = false;
			cardReadLED(false, isDma);    // After loading is done, turn off LED for card read indicator
			return true;
		}
	} else {
		fileRead((char*)dst, useApFixOverlays ? apFixOverlaysFile : romFile, src, len);
		cardReadLED(false, isDma);    // After loading is done, turn off LED for card read indicator
		return true;
	}

	#ifdef DEBUG
	dbg_printf("\nread \n");
	if (is_aligned(dst, 4) || is_aligned(len, 4)) {
		dbg_printf("\n aligned read : \n");
	} else {
		dbg_printf("\n misaligned read : \n");
	}
	#endif
}

static bool resume_cardRead_arm9(void) {
	const bool isDma = (sharedAddr[3] == (vu32)0x025FFB09 || sharedAddr[3] == (vu32)0x025FFB0A);
    if(resumeFileRead())
    {
        readOngoing = false;
        cardReadLED(false, isDma);    // After loading is done, turn off LED for card read indicator
        return true;    
    } 
    else
    {
        return false;    
    }
}

#ifndef TWLSDK
/* static bool gsddFix(void) {
	if (sharedAddr[4] != 0x44445347) {
        return false;
    }

	const u32 gsddOverlayChecksumOffset = *(u32*)0x02FFF004;
	// const u32 gsddOverlayFuncOffset = *(u32*)0x02FFF008;
	const u32 oldChecksum = 0x2FBB82E1;
	const u32 newChecksum = *(u32*)0x02FFF17C;

	if (*(u32*)gsddOverlayChecksumOffset == oldChecksum) {
		*(u32*)gsddOverlayChecksumOffset = newChecksum;
		// *(u32*)gsddOverlayFuncOffset = 0xE1A00000; // nop (Start the game past name setting)
	}

	sharedAddr[4] = 0;
	return true;
} */

/* bool romLocationAdjust(const tNDSHeader* ndsHeader, const bool laterSdk, const bool dsiBios, u32* romLocation) {
	const bool ntrType = (ndsHeader->unitCode == 0);
	const u32 romLocationOld = *romLocation;
	if (*romLocation == 0x0C3FC000) {
		*romLocation += 0x4000;
	} else if (*romLocation == 0x0C7C0000 && ((laterSdk && !dsiBios) || !laterSdk) && ntrType) {
		*romLocation += laterSdk ? 0x8000 : 0x28000;
	} else if (*romLocation == 0x0C7C4000) {
		*romLocation += 0x4000;
	} else if (*romLocation == 0x0C7D8000 && laterSdk) {
		if (ntrType) {
			*romLocation += (valueBits & hasVramWifiBinary) ? 0x10000 : 0x28000;
		} else {
			*romLocation += 0x8000;
		}
	} else if (*romLocation == 0x0C7F8000 && (laterSdk || !dsiBios) && ntrType) {
		*romLocation += 0x8000;
	} else if (*romLocation == 0x0C7FC000) {
		*romLocation += 0x4000;
	} else if (*romLocation == 0x0CFE0000 && !ntrType) {
		*romLocation += 0x20000;
	} else if (*romLocation == 0x0CFFC000 && dsiBios) {
		*romLocation += 0x4000;
	}
	return (*romLocation != romLocationOld);
}

static void loadROMPartIntoRAM(void) {
	static bool finished = false;
	extern u32 romPartLocation;
	extern u32 romPartSrc;
	extern u32 romPartSize;
	extern u32 romPartFrame;

	if (finished || romPartFrame == 0 || *(int*)((valueBits & isSdk5) ? 0x02FFFC3C : 0x027FFC3C) < romPartFrame) {
		return;
	}

	const int oldIME = enterCriticalSection(); // This is needed to avoid crashing when reading or writing save data

	static bool inited = false;
	const u32 cacheBlockSize = 0x4000;

	static s32 preloadSizeEdit = 0;
	static u32 romLocationChange = 0;
	static u32 romOffsetChange = 0;

	if (!inited) {
		preloadSizeEdit = romPartSize;
		romLocationChange = romPartLocation;
		romOffsetChange = romPartSrc;
		inited = true;
	}

	if (preloadSizeEdit > 0) {
		const u32 romBlockSize = (preloadSizeEdit > cacheBlockSize) ? cacheBlockSize : preloadSizeEdit;
		if (lockMutex(&saveMutex)) {
			cardReadLED(true, true);
			fileRead((char*)romLocationChange, romFile, romOffsetChange, romBlockSize);
			cardReadLED(false, true);
			unlockMutex(&saveMutex);
		}
		preloadSizeEdit -= romBlockSize;
		romOffsetChange += cacheBlockSize;
		romLocationChange += cacheBlockSize;

		romLocationAdjust(ndsHeader, !(valueBits & eSdk2), (valueBits & b_dsiBios), &romLocationChange);
	} else {
		sharedAddr[5] = 0x44454C50; // 'PLED'
		finished = true;
	}

	leaveCriticalSection(oldIME);
} */
#endif

#ifdef UNUSED
static inline void sdmmcHandler(void) { // Unused
	if (sdReadOngoing) {
		if (my_sdmmc_sdcard_check_command(0x33C12)) {
			sharedAddr[4] = 0;
			cardReadLED(false, ongoingIsDma);
			sdReadOngoing = false;
		}
		return;
	}

	switch (sharedAddr[4]) {
		case 0x53445231:
		case 0x53444D31: {
		
			//#ifdef DEBUG		
			//dbg_printf("my_sdmmc_sdcard_readsector\n");
			//#endif
			// bool isDma = sharedAddr[4]==0x53444D31;
			// cardReadLED(true, isDma);
			ongoingIsDma = (sharedAddr[4] == 0x53444D31);
			cardReadLED(true, ongoingIsDma);
			sharedAddr[4] = my_sdmmc_sdcard_readsector(sharedAddr[0], (u8*)sharedAddr[1], sharedAddr[2], sharedAddr[3]);
			// cardReadLED(false, isDma);
		}	break;
		case 0x53445244:
		case 0x53444D41: {
		
			//#ifdef DEBUG		
			//dbg_printf("my_sdmmc_sdcard_readsectors\n");
			//#endif
			//bool isDma = sharedAddr[4]==0x53444D41;
			ongoingIsDma = (sharedAddr[4] == 0x53444D41);
			cardReadLED(true, ongoingIsDma);
			if ((sharedAddr[2] % 4) != 0 || (valueBits & ndmaDisabled)) {
				sharedAddr[4] = my_sdmmc_sdcard_readsectors(sharedAddr[0], sharedAddr[1], (u8*)sharedAddr[2]);
				cardReadLED(false, ongoingIsDma);
			} else {
				my_sdmmc_sdcard_readsectors_nonblocking(sharedAddr[0], sharedAddr[1], (u8*)sharedAddr[2]);
				sdReadOngoing = true;
			}
		}	break;
		/*case 0x53444348:
			sharedAddr[4] = my_sdmmc_sdcard_check_command(sharedAddr[0], sharedAddr[1]);
			//currentCmd = sharedAddr[0];
			//currentNdmaSlot = sharedAddr[1];
			break;
		case 0x53415244:
			cardReadLED(true, true);
			sharedAddr[4] = my_sdmmc_sdcard_readsectors_nonblocking(sharedAddr[0], sharedAddr[1], (u8*)sharedAddr[2]);
			//currentCmd = sharedAddr[4];
			//currentNdmaSlot = sharedAddr[3];
			timeTillDmaLedOff = 0;
			readOngoing = true;
			break;*/
		/* case 0x53445752:
			cardReadLED(true, true);
			sharedAddr[4] = my_sdmmc_sdcard_writesectors(sharedAddr[0], sharedAddr[1], (u8*)sharedAddr[2]);
			cardReadLED(false, true);
			break; */
	}
}
#endif

void runCardEngineCheck(void) {
	// if (!(valueBits & b_runCardEngineCheck)) return;

	//dbg_printf("runCardEngineCheck\n");
	#ifdef DEBUG		
	nocashMessage("runCardEngineCheck");
	#endif	

  	// if (tryLockMutex(&cardEgnineCommandMutex)) {
        //if(!readOngoing)
        //{
    
    		//nocashMessage("runCardEngineCheck mutex ok");
    
			if (!(valueBits & gameOnFlashcard)) {
				if (/* sharedAddr[3] == (vu32)0x020FF808 || sharedAddr[3] == (vu32)0x020FF80A || */ sharedAddr[3] >= (vu32)0x025FFB08 && sharedAddr[3] <= (vu32)0x025FFB0A) {	// ARM9 Card Read
					const bool isDma = (sharedAddr[3] == (vu32)0x025FFB0A);
					if (!readOngoing ? start_cardRead_arm9() : resume_cardRead_arm9()) {
						sharedAddr[3] = 0;
					}
					if (isDma) {
						sharedAddr[4] = 0x39414D44; // 'DMA9'
						IPC_SendSync(0x3);
					}
				}
			}

			/* #ifdef DEBUG
    		if (sharedAddr[3] == (vu32)0x026FF800) {
    			log_arm9();
    			sharedAddr[3] = 0;
                //IPC_SendSync(0x8);
    		} else
			#endif

			if (sharedAddr[3] == (vu32)0x025FFC01) {
				//dmaLed = (sharedAddr[3] == (vu32)0x025FFC01);
				nandRead();
    			sharedAddr[3] = 0;
			} else if (sharedAddr[3] == (vu32)0x025FFC02) {
				//dmaLed = (sharedAddr[3] == (vu32)0x025FFC02);
				nandWrite();
    			sharedAddr[3] = 0;
			} */

            /*if (sharedAddr[3] == (vu32)0x025FBC01) {
                dmaLed = false;
    			slot2Read();
    			sharedAddr[3] = 0;
    			IPC_SendSync(0x8);
    		}*/
        //}
  		// unlockMutex(&cardEgnineCommandMutex);
  	// }
}

void runCardEngineCheckHalt(void) {
	//dbg_printf("runCardEngineCheckHalt\n");
	#ifdef DEBUG		
	nocashMessage("runCardEngineCheckHalt");
	#endif	

  	// if (lockMutex(&cardEgnineCommandMutex)) {
        //if(!readOngoing)
        //{
    
    		//nocashMessage("runCardEngineCheck mutex ok");

			/* #ifndef TWLSDK
			loadROMPartIntoRAM();
			#endif */
			if (/* sharedAddr[3] == (vu32)0x020FF808 || sharedAddr[3] == (vu32)0x020FF80A || */ sharedAddr[3] >= (vu32)0x025FFB08 && sharedAddr[3] <= (vu32)0x025FFB0A) {	// ARM9 Card Read
				const bool isDma = (sharedAddr[3] == (vu32)0x025FFB0A);
				const bool dmaLed = (sharedAddr[3] == (vu32)0x025FFB09 || isDma);
				bool useApFixOverlays = false;
				u32 src = sharedAddr[2];
				u32 dst = sharedAddr[0];
				u32 len = sharedAddr[1];
				if (len >= 0x80000000) {
					len -= 0x80000000;
					useApFixOverlays = true;
				}

				// readOngoing = true;
				if (lockMutex(&saveMutex)) {
					cardReadLED(true, dmaLed);    // When a file is loading, turn on LED for card read indicator
					fileRead((char*)dst, useApFixOverlays ? apFixOverlaysFile : romFile, src, len);
					cardReadLED(false, dmaLed);    // After loading is done, turn off LED for card read indicator
					unlockMutex(&saveMutex);
				}
				// readOngoing = false;
				sharedAddr[3] = 0;
				if (isDma) {
					sharedAddr[4] = 0x39414D44; // 'DMA9'
					IPC_SendSync(0x3);
				}
			}

			#ifdef DEBUG
    		if (sharedAddr[3] == (vu32)0x026FF800) {
    			log_arm9();
    			sharedAddr[3] = 0;
                //IPC_SendSync(0x8);
    		} else
			#endif

			if (sharedAddr[3] == (vu32)0x025FFC01) {
				//dmaLed = (sharedAddr[3] == (vu32)0x025FFC01);
				nandRead();
    			sharedAddr[3] = 0;
			} else if (sharedAddr[3] == (vu32)0x025FFC02) {
				//dmaLed = (sharedAddr[3] == (vu32)0x025FFC02);
				nandWrite();
    			sharedAddr[3] = 0;
			}
			#ifdef TWLSDK
			else if (sharedAddr[3] == (vu32)0x025FFC03) {
				//dmaLed = (sharedAddr[3] == (vu32)0x025FFC03);
				sharedFontRead();
    			sharedAddr[3] = 0;
			}
			#endif

            /*if (sharedAddr[3] == (vu32)0x025FBC01) {
                dmaLed = false;
    			slot2Read();
    			sharedAddr[3] = 0;
    			IPC_SendSync(0x8);
    		}*/
        //}
  		// unlockMutex(&cardEgnineCommandMutex);
  	// }
}

//---------------------------------------------------------------------------------
void myIrqHandlerFIFO(void) {
//---------------------------------------------------------------------------------
	#ifdef DEBUG		
	nocashMessage("myIrqHandlerFIFO");
	#endif

	// calledViaIPC = true;

    if (IPC_GetSync() == 0x3) {
		/* #ifndef TWLSDK
		if (gsddFix()) {
			return;
		}
		#endif */
		swiDelay(100);
		sharedAddr[4] = 0x39414D44; // 'DMA9'
		IPC_SendSync(0x3);
		return;
	}

	runCardEngineCheck();
	if (!(valueBits & gameOnFlashcard)) {
		// sdmmcHandler();
		if (readOngoing) {
			sharedAddr[5] = 0x474E4950; // 'PING'
		}
	}
}


void myIrqHandlerVBlank(void) {
  while (1) {
	#ifdef DEBUG		
	nocashMessage("myIrqHandlerVBlank");
	#endif	

	if (valueBits & i2cBricked) {
		REG_MASTER_VOLUME = noI2CVolLevel;
	}

	#ifdef DEBUG
	nocashMessage("cheat_engine_start\n");
	#endif

	if (*(u32*)cheatEngineAddr == 0x3E4 && *(u32*)(cheatEngineAddr+0x3E8) != 0xCF000000) {
		volatile void (*cheatEngine)() = (volatile void*)cheatEngineAddr+4;
		(*cheatEngine)();
	}

	if (language >= 0 && language <= 7 && languageTimer < 60*3) {
		// Change language
		personalData->language = language;
		#ifndef TWLSDK
		if (languageAddr > 0) {
			// Extra measure for specific games
			*languageAddr = language;
		}
		#endif
		languageTimer++;
	}

	if (!funcsUnpatched && *(int*)((valueBits & isSdk5) ? 0x02FFFC3C : 0x027FFC3C) >= 60) {
		unpatchedFunctions* unpatchedFuncs = (unpatchedFunctions*)((valueBits & isSdk5) ? UNPATCHED_FUNCTION_LOCATION_SDK5 : UNPATCHED_FUNCTION_LOCATION);

		if (unpatchedFuncs->compressed_static_end) {
			*unpatchedFuncs->compressedFlagOffset = unpatchedFuncs->compressed_static_end;
		}

		#ifdef TWLSDK
		if (unpatchedFuncs->ltd_compressed_static_end) {
			*unpatchedFuncs->iCompressedFlagOffset = unpatchedFuncs->ltd_compressed_static_end;
		}

		if (unpatchedFuncs->mpuInitOffset2) {
			*unpatchedFuncs->mpuInitOffset2 = 0xEE060F12;
		}
		if (unpatchedFuncs->mpuDataOffset2) {
			*unpatchedFuncs->mpuDataOffset2 = unpatchedFuncs->mpuInitRegionOldData2;
		}
		#else
		if (!(valueBits & isSdk5)) {
			if (unpatchedFuncs->mpuDataOffset) {
				*unpatchedFuncs->mpuDataOffset = unpatchedFuncs->mpuInitRegionOldData;

				if (unpatchedFuncs->mpuAccessOffset) {
					if (unpatchedFuncs->mpuOldInstrAccess) {
						unpatchedFuncs->mpuDataOffset[unpatchedFuncs->mpuAccessOffset] = unpatchedFuncs->mpuOldInstrAccess;
					}
					if (unpatchedFuncs->mpuOldDataAccess) {
						unpatchedFuncs->mpuDataOffset[unpatchedFuncs->mpuAccessOffset + 1] = unpatchedFuncs->mpuOldDataAccess;
					}
				}
			}

			if ((u32)unpatchedFuncs->mpuDataOffsetAlt >= (u32)ndsHeader->arm9destination && (u32)unpatchedFuncs->mpuDataOffsetAlt < (u32)ndsHeader->arm9destination+0x4000) {
				*unpatchedFuncs->mpuDataOffsetAlt = unpatchedFuncs->mpuInitRegionOldDataAlt;
			}

			if (unpatchedFuncs->mpuDataOffset2) {
				*unpatchedFuncs->mpuDataOffset2 = unpatchedFuncs->mpuInitRegionOldData2;
			}
		}

		if (unpatchedFuncs->mpuInitOffset2) {
			*unpatchedFuncs->mpuInitOffset2 = 0xEE060F12;
		}
		#endif

		funcsUnpatched = true;
	}

#ifndef TWLSDK
	if (!(valueBits & gameOnFlashcard) && !(valueBits & ROMinRAM) && isSdEjected()) {
		tonccpy((u32*)0x02000300, sr_data_error, 0x020);
		rebootConsole();		// Reboot into error screen if SD card is removed
	}
#endif

/* #ifndef TWLSDK
	if (valueBits & isDlp) {
		if (!(REG_EXTKEYINPUT & KEY_A) && *(u32*)(NDS_HEADER_SDK5+0xC) != 0 && !wifiIrq) {
			IPC_SendSync(0x5);
			reset(false);
		}
	}
#endif */

	if ((0 == (REG_KEYINPUT & igmHotkey) && 0 == (REG_EXTKEYINPUT & (((igmHotkey >> 10) & 3) | ((igmHotkey >> 6) & 0xC0))) && (valueBits & igmAccessible) && !wifiIrq) /* || returnToMenu */ || sharedAddr[5] == 0x4C4D4749 /* IGML */) {
		if (tryLockMutex(&saveMutex)) {
#ifdef TWLSDK
		igmText = (struct IgmText *)INGAME_MENU_LOCATION;
		i2cWriteRegister(0x4A, 0x12, 0x00);
#endif
		inGameMenu();
#ifdef TWLSDK
		i2cWriteRegister(0x4A, 0x12, 0x01);
#endif
		unlockMutex(&saveMutex);
		}
	}

	if (afterSwapTimer > 0) {
		if (afterSwapTimer == 60*3) {
			if (lockMutex(&saveMutex)) {
				saveMainScreenSetting();
			}
			unlockMutex(&saveMutex);
			afterSwapTimer = 0;
		} else afterSwapTimer++;
	}

	u8 screenIpc = 0x6;

	if (0 == (REG_KEYINPUT & screenSwapHotkey) && 0 == (REG_EXTKEYINPUT & (((screenSwapHotkey >> 10) & 3) | ((screenSwapHotkey >> 6) & 0xC0)))) {
		if (swapTimer == 60){
			swapTimer = 0;
			screenIpc = 0x7;
			mainScreen++;
			if (mainScreen > 2) {
				mainScreen = 0;
			}
			afterSwapTimer = 1;
		}
		swapTimer++;
	} else {
		swapTimer = 0;
	}

#ifdef TWLSDK
	if (sharedAddr[3] == (vu32)0x54495845) {
		returnToLoader(false);
	}
#endif

	if (0 == (REG_KEYINPUT & (KEY_L | KEY_R | KEY_DOWN | KEY_B))) {
		if (returnTimer == 60 * 2) {
#ifdef TWLSDK
			IPC_SendSync(0x5);
#else
			returnToLoader(false);
#endif
		}
		returnTimer++;
	} else {
		returnTimer = 0;
	}

	/* if ((valueBits & b_dsiSD) && (0 == (REG_KEYINPUT & (KEY_L | KEY_R | KEY_DOWN | KEY_A)))) {
		if (tryLockMutex(&cardEgnineCommandMutex)) {
			if (ramDumpTimer == 60 * 2) {
				REG_MASTER_VOLUME = 0;
				int oldIME = enterCriticalSection();
				dumpRam();
				leaveCriticalSection(oldIME);
				REG_MASTER_VOLUME = 127;
			}
			unlockMutex(&cardEgnineCommandMutex);
		}
		ramDumpTimer++;
	} else {
		ramDumpTimer = 0;
	} */

	raUnlockService();

	if (sharedAddr[3] == (vu32)0x52534554) {
		reset(false);
	}

	if ( 0 == (REG_KEYINPUT & (KEY_L | KEY_R | KEY_START | KEY_SELECT))) {
		if (softResetTimer == 60 * 2) {
			if (saveTimer == 0) {
				if (lockMutex(&saveMutex)) {
					REG_MASTER_VOLUME = 0;
					int oldIME = enterCriticalSection();
					forceGameReboot();
					leaveCriticalSection(oldIME);
				}
				unlockMutex(&saveMutex);
			}
		} else {
			softResetTimer++;
		}
	} else {
		softResetTimer = 0;
	}

	#ifndef TWLSDK
	if (valueBits & powerCodeOnVBlank) {
		i2cIRQHandler();
	}
	#endif

	if ((valueBits & preciseVolumeControl) || (valueBits & i2cBricked)) {
		// Precise volume adjustment (for DSi)
		if (volumeAdjustActivated) {
			volumeAdjustDelay++;
			if (volumeAdjustDelay == 30) {
				volumeAdjustDelay = 0;
				volumeAdjustActivated = false;
			}
		} else if (0==(REG_KEYINPUT & KEY_SELECT)) {
			if (valueBits & i2cBricked) {
				const int oldVolLevel = noI2CVolLevel;
				if (0==(REG_KEYINPUT & KEY_UP)) {
					noI2CVolLevel = 127;
				} else if (0==(REG_KEYINPUT & KEY_DOWN)) {
					noI2CVolLevel = 0;
				}
				volumeAdjustActivated = (noI2CVolLevel != oldVolLevel);
			} else {
				const u8 i2cVolLevel = i2cReadRegister(0x4A, 0x40);
				u8 i2cNewVolLevel = i2cVolLevel;
				if (0==(REG_KEYINPUT & KEY_UP)) {
					i2cNewVolLevel++;
				} else if (0==(REG_KEYINPUT & KEY_DOWN)) {
					i2cNewVolLevel--;
				}
				if (i2cNewVolLevel == 0xFF) {
					i2cNewVolLevel = 0;
				} else if (i2cNewVolLevel > 0x1F) {
					i2cNewVolLevel = 0x1F;
				}
				if (i2cNewVolLevel != i2cVolLevel) {
					i2cWriteRegister(0x4A, 0x40, i2cNewVolLevel);
					volumeAdjustActivated = true;
				}
			}
		}
	}
	
	if (saveTimer > 0) {
		saveTimer++;
		if (saveTimer == 60) {
			//i2cWriteRegister(0x4A, 0x12, 0x00);		// If saved, power button works again.
			saveTimer = 0;
		}
	}

	if (REG_IE & IRQ_NETWORK) {
		REG_IE &= ~IRQ_NETWORK; // DSi RTC fix
	}

	bool wifiIrqCheck = (REG_WIFIIRQ != 0);
	if (wifiIrq != wifiIrqCheck) {
		// Turn off card read DMA if WiFi is used, and back on when not in use
		if (wifiIrq) {
			wifiIrqTimer++;
			if (wifiIrqTimer == 30) {
				// IPC_SendSync(0x4);
				wifiIrq = wifiIrqCheck;
			}
		} else {
			// IPC_SendSync(0x4);
			wifiIrq = wifiIrqCheck;
		}
	} else {
		wifiIrqTimer = 0;
	}

	// calledViaIPC = false;
	// runCardEngineCheck();

	// Fix ARM9 VCount IRQ settings for color LUT and/or swap screens
	if ((valueBits & useColorLut) || (mainScreen > 0) || (screenIpc == 0x7)) {
		IPC_SendSync(screenIpc);
	}

	#ifndef TWLSDK
	fpsa_run();
	#endif

	if (sharedAddr[0] == 0x524F5245) { // 'EROR'
		REG_MASTER_VOLUME = 0;
		while (REG_VCOUNT != 191) swiDelay(100);
		while (REG_VCOUNT == 191) swiDelay(100);
	} else {
		break;
	}
  }
}

#ifndef TWLSDK
void i2cIRQHandler(void) {
	int cause = (i2cReadRegister(I2C_PM, I2CREGPM_PWRIF) & 0x3) | (i2cReadRegister(I2C_GPIO, 0x02)<<2);

	switch (cause & 3) {
	case 1: {
		if (saveTimer != 0) return;

		REG_MASTER_VOLUME = 0;
		int oldIME = enterCriticalSection();
		if (consoleModel < 2) {
			//unlaunchSetFilename(true);
			sharedAddr[4] = 0x57534352;
			IPC_SendSync(0x8);
			waitFrames(5);							// Wait for DSi screens to stabilize
		}
		rebootConsole();			// Reboot console
		leaveCriticalSection(oldIME);
		break;
	}
	case 2:
		writePowerManagement(PM_CONTROL_REG,PM_SYSTEM_PWR);
		break;
	}
}
#endif

u32 myIrqEnable(u32 irq) {	
	int oldIME = enterCriticalSection();

	#ifdef DEBUG		
	nocashMessage("myIrqEnable\n");
	#endif	

	initialize();

	//if (!(valueBits & gameOnFlashcard) && !(valueBits & ROMinRAM)) {
		REG_AUXIE &= ~(1UL << 8);
	//}

	#ifdef TWLSDK
	//bool doBak = ((valueBits & gameOnFlashcard) && (valueBits & b_dsiSD));
	//if (doBak) bakSdData();
	#endif
	driveInitialize();
  	#ifdef TWLSDK
	//if (doBak) restoreSdBakData();
	#endif

	/*if (!(valueBits & gameOnFlashcard) && !(valueBits & ROMinRAM) && ndsHeader->unitCode > 0 && (valueBits & dsiMode)) {
		extern u32* dsiIrqTable;
		extern u32* dsiIrqRet;
		extern u32* extraIrqTable_offset;
		extern u32* extraIrqRet_offset;

		dsiIrqTable[8] = extraIrqTable_offset[8];
		dsiIrqRet[8] = extraIrqRet_offset[8];
	}*/

	/*const char* romTid = getRomTid(ndsHeader);

	if ((strncmp(romTid, "UOR", 3) == 0)
	|| (strncmp(romTid, "UXB", 3) == 0)
	|| (strncmp(romTid, "USK", 3) == 0)
	|| (!(valueBits & gameOnFlashcard) && !(valueBits & ROMinRAM))) {
		// Proceed below "else" code
	} else {
		u32 irq_before = REG_IE;		
		REG_IE |= irq;
		leaveCriticalSection(oldIME);
		return irq_before;
	}*/

	u32 irq_before = REG_IE | IRQ_IPC_SYNC;
	irq |= IRQ_IPC_SYNC;
	//irq |= BIT(28);
	REG_IPC_SYNC |= IPC_SYNC_IRQ_ENABLE;

	REG_IE |= irq;
	//if (!(valueBits & powerCodeOnVBlank)) {
	//	REG_AUXIE |= IRQ_I2C;
	//}
	//if (!(valueBits & gameOnFlashcard) && !(valueBits & ROMinRAM)) {	
	//	REG_AUXIE |= IRQ_SDMMC;
	//}
	leaveCriticalSection(oldIME);
	//ipcSyncHooked = true;
	return irq_before;
}

/*static void irqIPCSYNCEnable(void) {	
	if (!initializedIRQ) {
		int oldIME = enterCriticalSection();
		initialize();	
		#ifdef DEBUG		
		dbg_printf("\nirqIPCSYNCEnable\n");	
		#endif	
		REG_IE |= IRQ_IPC_SYNC;
		REG_IPC_SYNC |= IPC_SYNC_IRQ_ENABLE;
		#ifdef DEBUG		
		dbg_printf("IRQ_IPC_SYNC enabled\n");
		#endif	
		leaveCriticalSection(oldIME);
		initializedIRQ = true;
	}
}*/

static inline void applyKeyRemap(u16* keyInput, u16* extKeyInput, const u8 remappedKey) {
	if (remappedKey >= 10) {
		*extKeyInput &= ~BIT(remappedKey);
	} else {
		*keyInput &= ~BIT(remappedKey);
	}
}

void patchKeyInputs(u16* extKeyInputDst, u16 extKeyInput) {
	u16 keyInput = *(u16*)0x04000130;
	const u16 keyInputBak = keyInput;
	const u16 extKeyInputBak = extKeyInput;
	keyInput = 0x3FF;
	for (int i = 10; i <= 11; i++) {
		extKeyInput |= BIT(i);
	}

	for (int i = 0; i <= 9; i++) {
		if (!(keyInputBak & BIT(i))) {
			applyKeyRemap(&keyInput, &extKeyInput, remappedKeys[i]);
		}
	}

	for (int i = 10; i <= 11; i++) {
		if (!(extKeyInputBak & BIT(i))) {
			applyKeyRemap(&keyInput, &extKeyInput, remappedKeys[i]);
		}
	}

	u32 dst = (u32)extKeyInputDst;
	dst -= 0x30;
	*(u16*)dst = keyInput;

	*extKeyInputDst = extKeyInput;
}

//
// ARM7 Redirected functions
//

bool eepromProtect(void) {
	#ifdef DEBUG
	dbg_printf("\narm7 eepromProtect\n");
	#endif

	return true;
}

bool eepromRead(u32 src, void *dst, u32 len) {
	#ifdef DEBUG
	dbg_printf("\narm7 eepromRead\n");

	dbg_printf("\nsrc : \n");
	dbg_hexa(src);
	dbg_printf("\ndst : \n");
	dbg_hexa((u32)dst);
	dbg_printf("\nlen : \n");
	dbg_hexa(len);
	#endif

	if (!(valueBits & saveOnFlashcard) && isSdEjected()) {
		return false;
	}

	if (lockMutex(&saveMutex)) {
		// while (readOngoing) { swiDelay(100); }
		#ifdef TWLSDK
		//bool doBak = ((valueBits & gameOnFlashcard) && !(valueBits & saveOnFlashcard));
		//if (doBak) bakSdData();
		#endif
		//driveInitialize();
		/*if (saveInRam) {
			tonccpy(dst, (char*)0x02440000 + src, len);
		} else {*/
			sdmmc_set_ndma_slot(4);
			if ((u32)(src % saveSize)+len > saveSize) {
				u32 len2 = len;
				u32 len3 = 0;
				while ((u32)(src % saveSize)+len2 > saveSize) {
					len2--;
					len3++;
				}
				fileRead(dst, savFile, (src % saveSize), len2);
				fileRead(dst+len2, savFile, ((src+len2) % saveSize), len3);
			} else {
				fileRead(dst, savFile, (src % saveSize), len);
			}
			sdmmc_set_ndma_slot(0);
		//}
		#ifdef TWLSDK
		//if (doBak) restoreSdBakData();
		#endif
  		unlockMutex(&saveMutex);
	}
	return true;
}

bool eepromPageWrite(u32 dst, const void *src, u32 len) {
	#ifdef DEBUG
	dbg_printf("\narm7 eepromPageWrite\n");

	dbg_printf("\nsrc : \n");
	dbg_hexa((u32)src);
	dbg_printf("\ndst : \n");
	dbg_hexa(dst);
	dbg_printf("\nlen : \n");
	dbg_hexa(len);
	#endif

	if (!(valueBits & saveOnFlashcard) && isSdEjected()) {
		return false;
	}

	if (lockMutex(&saveMutex)) {
		// while (readOngoing) { swiDelay(100); }
		#ifdef TWLSDK
		//bool doBak = ((valueBits & gameOnFlashcard) && !(valueBits & saveOnFlashcard));
		//if (doBak) bakSdData();
		#endif
		//driveInitialize();
		saveTimer = 1;
		//i2cWriteRegister(0x4A, 0x12, 0x01);		// When we're saving, power button does nothing, in order to prevent corruption.
		/*if (saveInRam) {
			tonccpy((char*)0x02440000 + dst, src, len);
		}*/
		if (valueBits & delayWrites) {
			if (*(int*)((valueBits & isSdk5) ? 0x02FFFC3C : 0x027FFC3C) >= 60*2) {
				valueBits &= ~delayWrites;
			} else {
				waitFrames(1);
			}
		}
		sdmmc_set_ndma_slot(4);
		if ((dst % saveSize)+len > saveSize) {
			u32 len2 = len;
			u32 len3 = 0;
			while ((u32)(dst % saveSize)+len2 > saveSize) {
				len2--;
				len3++;
			}
			fileWrite(src, savFile, (dst % saveSize), len2);
			fileWrite(src+len2, savFile, ((dst+len2) % saveSize), len3);
		} else {
			fileWrite(src, savFile, (dst % saveSize), len);
		}
		sdmmc_set_ndma_slot(0);
		#ifdef TWLSDK
		//if (doBak) restoreSdBakData();
		#endif
  		unlockMutex(&saveMutex);
	}
	return true;
}

bool eepromPageProg(u32 dst, const void *src, u32 len) {
	#ifdef DEBUG
	dbg_printf("\narm7 eepromPageProg\n");
	#endif

	return eepromPageWrite(dst, src, len);
}

bool eepromPageVerify(u32 dst, const void *src, u32 len) {
	#ifdef DEBUG
	dbg_printf("\narm7 eepromPageVerify\n");

	dbg_printf("\nsrc : \n");
	dbg_hexa((u32)src);
	dbg_printf("\ndst : \n");
	dbg_hexa(dst);
	dbg_printf("\nlen : \n");
	dbg_hexa(len);
	#endif

	return eepromPageWrite(dst, src, len);
}

bool eepromPageErase (u32 dst) {
	#ifdef DEBUG	
	dbg_printf("\narm7 eepromPageErase\n");	
	#endif	

	if (!(valueBits & saveOnFlashcard) && isSdEjected()) {
		return false;
	}

	// TODO: this should be implemented?
	return true;
}

/*
TODO: return the correct ID

From gbatek 
Returns RAW unencrypted Chip ID (eg. C2h,0Fh,00h,00h), repeated every 4 bytes.
  1st byte - Manufacturer (eg. C2h=Macronix) (roughly based on JEDEC IDs)
  2nd byte - Chip size (00h..7Fh: (N+1)Mbytes, F0h..FFh: (100h-N)*256Mbytes?)
  3rd byte - Flags (see below)
  4th byte - Flags (see below)
The Flag Bits in 3th byte can be
  0   Maybe Infrared flag? (in case ROM does contain on-chip infrared stuff)
  1   Unknown (set in some 3DS carts)
  2-7 Zero
The Flag Bits in 4th byte can be
  0-2 Zero
  3   Seems to be NAND flag (0=ROM, 1=NAND) (observed in only ONE cartridge)
  4   3DS Flag (0=NDS/DSi, 1=3DS)
  5   Zero   ... set in ... DSi-exclusive games?
  6   DSi flag (0=NDS/3DS, 1=DSi)
  7   Cart Protocol Variant (0=older/smaller carts, 1=newer/bigger carts)

Existing/known ROM IDs are:
  C2h,07h,00h,00h NDS Macronix 8MB ROM  (eg. DS Vision)
  AEh,0Fh,00h,00h NDS Noname   16MB ROM (eg. Meine Tierarztpraxis)
  C2h,0Fh,00h,00h NDS Macronix 16MB ROM (eg. Metroid Demo)
  C2h,1Fh,00h,00h NDS Macronix 32MB ROM (eg. Over the Hedge)
  C2h,1Fh,00h,40h DSi Macronix 32MB ROM (eg. Art Academy, TWL-VAAV, SystemFlaw)
  80h,3Fh,01h,E0h ?            64MB ROM+Infrared (eg. Walk with Me, NTR-IMWP)
  AEh,3Fh,00h,E0h DSi Noname   64MB ROM (eg. de Blob 2, TWL-VD2V)
  C2h,3Fh,00h,00h NDS Macronix 64MB ROM (eg. Ultimate Spiderman)
  C2h,3Fh,00h,40h DSi Macronix 64MB ROM (eg. Crime Lab, NTR-VAOP)
  80h,7Fh,00h,80h NDS SanDisk  128MB ROM (DS Zelda, NTR-AZEP-0)
  80h,7Fh,01h,E0h ?            128MB ROM+Infrared? (P-letter Soul Silver, IPGE)
  C2h,7Fh,00h,80h NDS Macronix 128MB ROM (eg. Spirit Tracks, NTR-BKIP)
  C2h,7Fh,00h,C0h DSi Macronix 128MB ROM (eg. Cooking Coach/TWL-VCKE)
  ECh,7Fh,00h,88h NDS Samsung  128MB NAND (eg. Warioware D.I.Y.)
  ECh,7Fh,01h,88h NDS Samsung? 128MB NAND+What? (eg. Jam with the Band, UXBP)
  ECh,7Fh,00h,E8h DSi Samsung? 128MB NAND (eg. Face Training, USKV)
  80h,FFh,80h,E0h NDS          256MB ROM (Kingdom Hearts - Re-Coded, NTR-BK9P)
  C2h,FFh,01h,C0h DSi Macronix 256MB ROM+Infrared? (eg. P-Letter White)
  C2h,FFh,00h,80h NDS Macronix 256MB ROM (eg. Band Hero, NTR-BGHP)
  C2h,FEh,01h,C0h DSi Macronix 512MB ROM+Infrared? (eg. P-Letter White 2)
  C2h,FEh,00h,90h 3DS Macronix probably 512MB? ROM (eg. Sims 3)
  45h,FAh,00h,90h 3DS SunDisk? maybe... 1.5GB? ROM (eg. Starfox)
  C2h,F8h,00h,90h 3DS Macronix maybe... 2GB?   ROM (eg. Kid Icarus)
  C2h,7Fh,00h,90h 3DS Macronix 128MB ROM CTR-P-AENJ MMinna no Ennichi
  C2h,FFh,00h,90h 3DS Macronix 256MB ROM CTR-P-AFSJ Pro Yakyuu Famista 2011
  C2h,FEh,00h,90h 3DS Macronix 512MB ROM CTR-P-AFAJ Real 3D Bass FishingFishOn
  C2h,FAh,00h,90h 3DS Macronix 1GB ROM CTR-P-ASUJ Hana to Ikimono Rittai Zukan
  C2h,FAh,02h,90h 3DS Macronix 1GB ROM CTR-P-AGGW Luigis Mansion 2 ASiA CHT
  C2h,F8h,00h,90h 3DS Macronix 2GB ROM CTR-P-ACFJ Castlevania - Lords of Shadow
  C2h,F8h,02h,90h 3DS Macronix 2GB ROM CTR-P-AH4J Monster Hunter 4
  AEh,FAh,00h,90h 3DS          1GB ROM CTR-P-AGKJ Gyakuten Saiban 5
  AEh,FAh,00h,98h 3DS          1GB NAND CTR-P-EGDJ Tobidase Doubutsu no Mori
  45h,FAh,00h,90h 3DS          1GB ROM CTR-P-AFLJ Fantasy Life
  45h,F8h,00h,90h 3DS          2GB ROM CTR-P-AVHJ Senran Kagura Burst - Guren
  C2h,F0h,00h,90h 3DS Macronix 4GB ROM CTR-P-ABRJ Biohazard Revelations
  FFh,FFh,FFh,FFh None (no cartridge inserted)
*/
u32 cardId(void) {
	#ifdef DEBUG	
	dbg_printf("\ncardId\n");
	#endif
    
    u32 cardid = getChipId(ndsHeader, moduleParams);

    //if (!cardInitialized && strncmp(getRomTid(ndsHeader), "BO5", 3) == 0)  cardid = 0xE080FF80; // golden sun
    //if (!cardInitialized && strncmp(getRomTid(ndsHeader), "BO5", 3) == 0)  cardid = 0x80FF80E0; // golden sun
    //if (cardInitialized && strncmp(getRomTid(ndsHeader), "BO5", 3) == 0)  cardid = 0xFF000000; // golden sun
    //if (cardInitialized && strncmp(getRomTid(ndsHeader), "BO5", 3) == 0)  cardid = 0x000000FF; // golden sun

    #ifdef DEBUG
    dbg_hexa(cardid);
    #endif
    
	return cardid;
}

bool cardRead(u32 dma, u32 src, void *dst, u32 len) {
	#ifdef DEBUG	
	dbg_printf("\narm7 cardRead\n");	

	dbg_printf("\ndma : \n");
	dbg_hexa(dma);		
	dbg_printf("\nsrc : \n");
	dbg_hexa(src);		
	dbg_printf("\ndst : \n");
	dbg_hexa((u32)dst);
	dbg_printf("\nlen : \n");
	dbg_hexa(len);
	#endif	

	if (!cardReadRAM(dst, src, len)) {
		// while (readOngoing) { swiDelay(100); }
		//driveInitialize();
		cardReadLED(true, false);    // When a file is loading, turn on LED for card read indicator
		//ndmaUsed = false;
		#ifdef DEBUG	
		nocashMessage("fileRead romFile");
		#endif	
		fileRead(dst, romFile, src, len);
		//ndmaUsed = true;
		cardReadLED(false, false);    // After loading is done, turn off LED for card read indicator
	}

	return true;
}
