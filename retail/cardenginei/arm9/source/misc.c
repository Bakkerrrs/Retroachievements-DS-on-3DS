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
#include <nds/system.h>
#include <nds/dma.h>
#include <nds/arm9/video.h>
#include <nds/interrupts.h>
#include <nds/ipc.h>
#include <nds/timers.h>
#include <nds/memory.h> // tNDSHeader
#include "tonccpy.h"
#include "nds_header.h"
#include "cardengine.h"
#include "ra_reader.h"
#include "locations.h"
#include "cardengine_header_arm9.h"

#define extendedMemory BIT(1)
#define eSdk2 BIT(2)
#define dsiMode BIT(3)
#define enableExceptionHandler BIT(4)
#define isSdk5 BIT(5)
#define overlaysInRam BIT(6)
#define slowSoftReset BIT(10)
#define cloneboot BIT(14)
#define isDlp BIT(15)
#define useColorLut BIT(21)
#define colorLutBlockVCount BIT(22)

#include "my_fat.h"

extern cardengineArm9* volatile ce9;

extern vu32* volatile sharedAddr;

extern tNDSHeader* ndsHeader;
extern aFile* romFile;
extern aFile* savFile;
extern aFile* apFixOverlaysFile;
extern u32* cacheAddressTable;
extern u32* cacheDescriptor;
extern int* cacheCounter;

extern bool flagsSet;
extern bool igmReset;

extern u32 getDtcmBase(void);
extern void ndsCodeStart(u32* addr);
void resetSlots(void);
void initMBKARM9_dsiMode(void);

#ifndef GSDD
void SetBrightness(u8 screen, s8 bright) {
	u16 mode = 1 << 14;

	if (bright < 0) {
		mode = 2 << 14;
		bright = -bright;
	}
	if (bright > 31) {
		bright = 31;
	}
	*(u16*)(0x0400006C + (0x1000 * screen)) = bright + mode;
}

// Alternative to swiWaitForVBlank()
void waitFrames(int count) {
	for (int i = 0; i < count; i++) {
		while (REG_VCOUNT != 191);
		while (REG_VCOUNT == 191);
	}
}
#endif

/*void sleepMs(int ms) {
	if (REG_IME == 0 || REG_IF == 0) {
		return;
	}

	if(ce9->patches->sleepRef) {
		volatile void (*sleepRef)(int ms) = (volatile void*)ce9->patches->sleepRef;
		(*sleepRef)(ms);
	} else if(ce9->thumbPatches->sleepRef) {
		extern void callSleepThumb(int ms);
		callSleepThumb(ms);
	}
}

static void waitForArm7(void) {
	IPC_SendSync(0x4);
	while (sharedAddr[3] != (vu32)0);
}*/

bool IPC_SYNC_hooked = false;
void hookIPC_SYNC(void) {
	#ifndef GSDD
    if (!IPC_SYNC_hooked) {
		// The VCOUNT hook is the colour LUT's alone again. The RA reader shared it until
		// hardware showed the sharing could not work: a game that clears the Y-trigger in
		// DISPSTAT every frame leaves the reader running on a fraction of them, and Contra 4
		// does exactly that. It hooks VBlank instead -- see raRearmVBlank() below.
		//
		// A behaviour change worth having, in the direction that costs nothing: a build with
		// the reader on no longer forces IRQ_VCOUNT onto a game that never asked for one. That
		// only ever happened for the reader's sake, and the reader does not need it now.
		//
		// Only chain if there is a table to patch and a handler to install: with
		// a null irqTable this would write over the exception vectors, and with a
		// null handler the forced VCOUNT interrupt would jump to address zero.
		if ((ce9->valueBits & useColorLut)
		 && !(ce9->valueBits & colorLutBlockVCount)
		 && ce9->irqTable && ce9->patches->vcountHandlerRef) {
			u32* vcountHandler = ce9->irqTable + 2;
			ce9->intr_vcount_orig_return = *vcountHandler;
			*vcountHandler = (u32)ce9->patches->vcountHandlerRef;
		}
		u32* ipcSyncHandler = ce9->irqTable + 16;
		ce9->intr_ipc_orig_return = *ipcSyncHandler;
		*ipcSyncHandler = (u32)ce9->patches->ipcSyncHandlerRef;
		IPC_SYNC_hooked = true;
    }
	#endif
}

/*
    Install and maintain the reader's per-frame hook, and it is one function because installing and
    re-arming are the same operation: if the game's VBlank table entry is not ours, save what is there
    and put ours in.

    Called from cardRead(), and that choice is the whole point. cardRead is patched into the *game's
    code* rather than into its interrupt table, and it demonstrably runs for the entire session -- a game
    that stopped reading its own ROM would not load a level. So it survives exactly the thing that kills
    interrupt hooks.

    **VBlank, not VCOUNT, and that is the fix for the flicker.** The reader chained onto the game's
    VCOUNT handler with a Y-trigger at line 0 until Contra 4 measured what that costs: `ticks` reached
    1,132 across a session of many thousands of frames while raRearmDispstat saturated at 255, so the
    game was clearing DISP_YTRIGGER_IRQ constantly and each re-arm bought roughly one tick. The reader
    ran on about 8% of frames -- and the overlay, which has to re-assert its borrowed layer every frame
    because the game rewrites those registers every frame, was therefore visible about one frame in
    twelve. That is a fast intermittent flash, which is what the screen showed.

    VBlank has none of that exposure. A DS game keeps IRQ_VBLANK on and keeps irqTable[0] pointed at
    something of its own, because it needs the interrupt itself; there is no Y-trigger to lose. The two
    conditions left are the table entry and the enable bit, and both are still counted rather than
    blind-fixed -- raRearmDispstat now reads 0 forever, which is the clearest possible statement that the
    fragile third condition is gone.

    Taking the address of our own stub is valid because this image runs where it was linked: `ce9` itself
    is a link-time address that every line above already depends on, and the block in patch_arm9.c that
    would have relocated the patch table is commented out upstream. Nothing new is assumed here.
*/
#if RA_READER_ENABLED
extern void raVblankHandler(void);   /* card_engine_header.s */
extern u32 raIntrVblankOrigReturn;   /* ...and the word it chains through */

u8 raRearmTable;
u8 raRearmIe;
u8 raRearmDispstat;   /* kept, and now always 0; see above and raSnapshot.rearmDispstat */

/*
    cardengineArm9 is the third positional mirror in this project: card_engine_header.s declares the
    same fields as labels in order, this binary reads them through the struct, and nothing in either
    language checks the other. loadCrt0 has the same shape and got it wrong once -- `.align 4` is
    sixteen bytes in GNU as for ARM, which put a field twelve bytes out of place with a clean build.

    Pinned here rather than left to inspection because this file now reads `irqTable` to install an
    interrupt handler. A field that shifted would not fail to build; it would write a wild pointer into
    a running game's interrupt table. The numbers are what arm-none-eabi-nm reports for those labels.

    Compile-time and on the target, not in tools/ra_reader_test.sh, for the same reason cardengineArm7's
    pins are: that script compiles structs on the host, which is only valid because loadCrt0 has no
    pointers. This one does, and a 64-bit host gives them eight bytes.
*/
typedef char raCe9OffsetsPinned[
	(__builtin_offsetof(cardengineArm9, intr_vcount_orig_return) == 0x0C
	 && __builtin_offsetof(cardengineArm9, valueBits)            == 0x2C
	 && __builtin_offsetof(cardengineArm9, consoleModel)         == 0x34
	 && __builtin_offsetof(cardengineArm9, irqTable)             == 0x38
	 && __builtin_offsetof(cardengineArm9, strmLoadFlag)         == 0xA0) ? 1 : -1];

void raRearmVBlank(void) {
	u32* vblankHandler;

	/*
	    consoleModel > 0 is the 3DS family, which is what this fork supports -- see the scope note in
	    docs/retroachievements.md. A null irqTable would mean writing over the exception vectors.
	*/
	if (ce9->consoleModel == 0 || !ce9->irqTable) {
		return;
	}

	vblankHandler = ce9->irqTable;
	if (*vblankHandler != (u32)raVblankHandler) {
		/*
		    The game's own handler is saved again rather than kept from the first install: whatever it
		    put there is what our stub has to chain to now, and using the stale one would return into
		    code the game may have moved.
		*/
		raIntrVblankOrigReturn = *vblankHandler;
		*vblankHandler = (u32)raVblankHandler;
		if (raRearmTable < 255) {
			raRearmTable++;
		}
	}

	if (!(REG_IE & IRQ_VBLANK)) {
		REG_IE |= IRQ_VBLANK;
		if (raRearmIe < 255) {
			raRearmIe++;
		}
	}
}
#endif

void enableIPC_SYNC(void) {
	#ifndef GSDD
	if (IPC_SYNC_hooked && !(REG_IE & IRQ_IPC_SYNC)) {
		REG_IE |= IRQ_IPC_SYNC;
	}
	#endif
}

#ifndef TWLSDK
static bool initialized = false;

void initialize(void) {
	if (initialized) {
		return;
	}

	#ifndef GSDD
	if (ce9->valueBits & isSdk5) {
		sharedAddr = (vu32*)CARDENGINE_SHARED_ADDRESS_SDK5;
		ndsHeader = (tNDSHeader*)NDS_HEADER_SDK5;
	} else {
		sharedAddr = (vu32*)CARDENGINE_SHARED_ADDRESS_SDK1;
		ndsHeader = (tNDSHeader*)NDS_HEADER;
		#ifndef DLDI
		if (ce9->valueBits & eSdk2) {
			cacheAddressTable = (u32*)CACHE_ADDRESS_TABLE_LOCATION2;
			cacheDescriptor = (u32*)CACHE_DESCRIPTOR_TABLE_LOCATION2;
			cacheCounter = (int*)CACHE_COUNTER_TABLE_LOCATION2;
		}
		#endif
	}
	#endif
	initialized = true;
}
#endif


//static void clearIcache (void) {
      // Seems to have no effect
      // disable interrupt
      /*int oldIME = enterCriticalSection();
      IC_InvalidateAll();
      // restore interrupt
      leaveCriticalSection(oldIME);*/
//}

extern void resetMpu(void);

void reset(u32 param, u32 tid2) {
	sysSetCardOwner(false);	// Give Slot-1 access to arm7
#ifndef TWLSDK
	const u32 resetParams = ((ce9->valueBits & isSdk5) ? RESET_PARAM_SDK5 : RESET_PARAM);
	*(u32*)resetParams = param;
	#ifndef GSDD
	if (ce9->valueBits & slowSoftReset) {
		if (ce9->consoleModel < 2) {
			// Make screens white
			SetBrightness(0, 31);
			SetBrightness(1, 31);
			waitFrames(5);	// Wait for DSi screens to stabilize
		}
		enterCriticalSection();
		cacheFlush();
		sharedAddr[3] = 0x52534554;
		while (1);
	} else
	#endif
	{
		sharedAddr[3] = 0x52534554;
	}
#else
	const bool isDSiWare = (*(u32*)0x02FFE234 == 0x00030004 || *(u32*)0x02FFE234 == 0x00030005 || *(u32*)0x02FFE234 == 0x00030015 || *(u32*)0x02FFE234 == 0x00030017);
	if (param == 0xFFFFFFFF || isDSiWare) { // If DSiWare...
		if (param == 0xFFFFFFFF || (param != *(u32*)0x02FFE230 && tid2 != *(u32*)0x02FFE234)) {
			/*if (ce9->consoleModel < 2) {
				// Make screens white
				SetBrightness(0, 31);
				SetBrightness(1, 31);
				waitFrames(5);	// Wait for DSi screens to stabilize
			}
			enterCriticalSection();
			cacheFlush();*/
			sharedAddr[3] = 0x54495845;
			//while (1);
		} else {
			sharedAddr[3] = 0x52534554;
		}
	} else {
		*(u32*)RESET_PARAM_SDK5 = param;
		sharedAddr[3] = 0x52534554;
	}
#endif

 	register int i, reg;

	REG_IME = 0;
	REG_IE = 0;
	REG_IF = ~0;

	cacheFlush();
	resetMpu();

	if (igmReset) {
		igmReset = false;
#ifdef TWLSDK
		if (ce9->nandTmpJumpFuncOffset && isDSiWare) {
			*(u32*)0x02FFD230 = *(u32*)0x02FFE230;
			*(u32*)0x02FFD234 = *(u32*)0x02FFE234;
		}
#endif
	} else {
		toncset((u8*)getDtcmBase()+0x3E00, 0, 0x200);
#ifdef TWLSDK
		if (ce9->nandTmpJumpFuncOffset && isDSiWare) {
			*(u32*)0x02FFD230 = 0;
			*(u32*)0x02FFD234 = 0;
		}
#endif
	}

	// Clear out ARM9 DMA channels
	for (i = 0; i < 4; i++) {
		DMA_CR(i) = 0;
		DMA_SRC(i) = 0;
		DMA_DEST(i) = 0;
		TIMER_CR(i) = 0;
		TIMER_DATA(i) = 0;
	}

	for (i = 0; i < 4; i++) {
		for(reg=0; reg<0x1c; reg+=4)*((vu32*)(0x04004104 + ((i*0x1c)+reg))) = 0;//Reset NDMA.
	}

	// Clear out FIFO
	REG_IPC_SYNC = 0;
	REG_IPC_FIFO_CR = IPC_FIFO_ENABLE | IPC_FIFO_SEND_CLEAR;
	REG_IPC_FIFO_CR = 0;

	flagsSet = false;
	IPC_SYNC_hooked = false;

#ifdef TWLSDK
	if (param == 0xFFFFFFFF || isDSiWare) { // If DSiWare...
		REG_DISPSTAT = 0;
		REG_DISPCNT = 0;
		REG_DISPCNT_SUB = 0;
		GFX_STATUS = 0;

		toncset((u16*)0x04000000, 0, 0x56);
		toncset((u16*)0x04001000, 0, 0x56);

		VRAM_A_CR = 0x80;
		VRAM_B_CR = 0x80;
		VRAM_C_CR = 0x80;
		VRAM_D_CR = 0x80;
		VRAM_E_CR = 0x80;
		VRAM_F_CR = 0x80;
		VRAM_G_CR = 0x80;
		VRAM_H_CR = 0x80;
		VRAM_I_CR = 0x80;

		toncset16(BG_PALETTE, 0, 256); // Clear palettes
		toncset16(BG_PALETTE_SUB, 0, 256);
		toncset(VRAM, 0, 0xC0000); // Clear VRAM

		VRAM_A_CR = 0;
		VRAM_B_CR = 0;
		VRAM_C_CR = 0;
		VRAM_D_CR = 0;
		VRAM_E_CR = 0;
		VRAM_F_CR = 0;
		VRAM_G_CR = 0;
		VRAM_H_CR = 0;
		VRAM_I_CR = 0;
	}

	#ifndef DLDI
	if (ce9->cacheAddress < 0x02F00000) {
		resetSlots();
	}
	#endif

	while (sharedAddr[0] != 0x44414F4C) { // 'LOAD'
		while (REG_VCOUNT != 191);
		while (REG_VCOUNT == 191);
	}

	if (ndsHeader->unitCode > 0 && sharedAddr[3] == 0x54495845) {
		initMBKARM9_dsiMode();
		REG_SCFG_EXT = 0x8307F100;
		REG_SCFG_CLK = 0x87;
		REG_SCFG_RST = 1;
	}

	sysSetCardOwner(true);	// Give Slot-1 access back to arm9
#else
	while (sharedAddr[0] != 0x44414F4C) { // 'LOAD'
		while (REG_VCOUNT != 191);
		while (REG_VCOUNT == 191);
	}

	sysSetCardOwner(true);	// Give Slot-1 access back to arm9

	#ifndef GSDD
	if ((ce9->valueBits & isDlp) || *(u32*)(resetParams+0xC) > 0) {
		sharedAddr[4] = 0;
		initialized = false;
	}
	#endif
#endif

	sharedAddr[0] = 0x544F4F42; // 'BOOT'
	sharedAddr[3] = 0;
	while (REG_VCOUNT != 191);
	while (REG_VCOUNT == 191);

	// Start ARM9
	ndsCodeStart(ndsHeader->arm9executeAddress);
}
