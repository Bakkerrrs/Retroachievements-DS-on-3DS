#ifndef LOCATIONS_H
#define LOCATIONS_H

#define TARGETBUFFERHEADER 0x02BFF000

#define EXCEPTION_STACK_LOCATION      0x027FFC00
#define EXCEPTION_STACK_LOCATION_SDK5 0x02FFFC00

#define ROM_FILE_LOCATION          0x37EFFA0
#define ROM_FILE_LOCATION_ALT      0x302FFA0
#define ROM_FILE_LOCATION_MAINMEM  0x27FEFA0
#define ROM_FILE_LOCATION_TWLSDK   0x2FFDFA0
#define SAV_FILE_LOCATION          (ROM_FILE_LOCATION + 0x18) //+ sizeof(aFile)
#define SAV_FILE_LOCATION_ALT      (ROM_FILE_LOCATION_ALT + 0x18) //+ sizeof(aFile)
#define SAV_FILE_LOCATION_MAINMEM  (ROM_FILE_LOCATION_MAINMEM + 0x18) //+ sizeof(aFile)
#define SAV_FILE_LOCATION_TWLSDK   (ROM_FILE_LOCATION_TWLSDK + 0x18) //+ sizeof(aFile)
#define OVL_FILE_LOCATION          (ROM_FILE_LOCATION + 0x30) //+ sizeof(aFile)
#define OVL_FILE_LOCATION_ALT      (ROM_FILE_LOCATION_ALT + 0x30) //+ sizeof(aFile)
#define OVL_FILE_LOCATION_MAINMEM  (ROM_FILE_LOCATION_MAINMEM + 0x30) //+ sizeof(aFile)
#define OVL_FILE_LOCATION_TWLSDK   (ROM_FILE_LOCATION_TWLSDK + 0x30) //+ sizeof(aFile)
/* #define GBA_FILE_LOCATION         (ROM_FILE_LOCATION + 64) //+ sizeof(aFile)
#define GBA_FILE_LOCATION_ALT     (ROM_FILE_LOCATION_ALT + 64) //+ sizeof(aFile)
#define GBA_FILE_LOCATION_MAINMEM (ROM_FILE_LOCATION_MAINMEM + 64) //+ sizeof(aFile)
#define GBA_SAV_FILE_LOCATION         (ROM_FILE_LOCATION + 96) //+ sizeof(aFile)
#define GBA_SAV_FILE_LOCATION_ALT     (ROM_FILE_LOCATION_ALT + 96) //+ sizeof(aFile)
#define GBA_SAV_FILE_LOCATION_MAINMEM (ROM_FILE_LOCATION_MAINMEM + 96) //+ sizeof(aFile) */
#define FONT_FILE_LOCATION_TWLSDK  (ROM_FILE_LOCATION_TWLSDK + 0x48) //+ sizeof(aFile)

#define LOAD_CRT0_LOCATION      0x06840000 // LCDC_BANK_C
#define LOAD_CRT0_LOCATION_B4DS 0x06860000 // LCDC_BANK_D

#define IPS_LOCATION       0x02337000
#define IMAGES_LOCATION    0x02338000 // Also IPS location for B4DS mode

#define CHEAT_ENGINE_LOCATION_B4DS_BUFFERED     0x023FE400
#define CARDENGINE_ARM7_LOCATION_BUFFERED       0x023FCF80
#define CARDENGINE_ARM9_LOCATION_BUFFERED       0x023E0000
#define CARDENGINE_ARM9_SLOT2HEAP_LOCATION_BUFFERED 0x0227F800

#define CHEAT_ENGINE_LOCATION_B4DS           0x027DE000
#define CHEAT_ENGINE_LOCATION_B4DS_ALT       0x027FC000
#define CHEAT_ENGINE_LOCATION_B4DS_ALT2      0x027FD000
#define CARDENGINE_ARM7_LOCATION             0x0380E700
#define CARDENGINE_ARM9_LOCATION_DLDI_START  0x02000500
#define CARDENGINE_ARM9_LOCATION_DLDI        0x023DA400
#define CARDENGINE_ARM9_LOCATION_DLDI_32     0x023D4400
#define CARDENGINE_ARM9_LOCATION_DLDI_ALT    0x023F9C00
#define CARDENGINE_ARM9_LOCATION_DLDI_ALT2   0x023FA800
#define CARDENGINE_ARM9_LOCATION_DLDI_ALT3   0x023DB800
#define CARDENGINE_ARM9_LOCATION_DLDI_EXTMEM 0x02780000

#define DONOR_ROM_ARM7_LOCATION                      0x02680000
#define DONOR_ROM_ARM7_SIZE_LOCATION                 0x026B0000
#define DONOR_ROM_ARM7I_SIZE_LOCATION                0x026B0004
#define DONOR_ROM_MBK6_LOCATION                      0x026B0008
#define DONOR_ROM_DEVICE_LIST_LOCATION               0x026B000C
#define ARM9_DEC_SIZE_LOCATION                       0x026B0010
#define INGAME_MENU_EXT_LOCATION                     0x026B8000
#define INGAME_MENU_EXT_LOCATION_B4DS                0x02380000
#define CHEAT_ENGINE_BUFFERED_LOCATION	             0x026E0000
#define INGAME_MENU_LOCATION                         0x02F88000
#define INGAME_MENU_LOCATION_B4DS                    0x023E6000
#define INGAME_MENU_LOCATION_B4DS_EXTMEM             (INGAME_MENU_LOCATION_B4DS+0x400000)
#define CACHE_ADDRESS_TABLE_LOCATION                 0x027D8000
#define CACHE_ADDRESS_TABLE_LOCATION2                0x027E0000
#define CACHE_DESCRIPTOR_TABLE_LOCATION              0x027DA000
#define CACHE_DESCRIPTOR_TABLE_LOCATION2             0x027E2000
#define CACHE_COUNTER_TABLE_LOCATION                 0x027DC000
#define CACHE_COUNTER_TABLE_LOCATION2                0x027E4000
#define UNPATCHED_FUNCTION_LOCATION                  0x027FFA40
#define UNPATCHED_FUNCTION_LOCATION_SDK5             0x02FFFA40

#define BLOWFISH_LOCATION_B4DS                       0x023ECE00
#define CARDENGINEI_ARM9_BUFFERED_LOCATION           0x026F0000
#define BLOWFISH_LOCATION                            0x027B0C00
#define ARM7_FIX_BUFFERED_LOCATION                   0x027B1E00
#define CARDENGINEI_ARM7_BUFFERED_LOCATION           0x027B2000
/*
    Where the launcher stages cardenginei_arm9_ra for the bootloader to copy into DSi
    WRAM. It has to be in a range the bootloader does *not* clear on startup -- see the
    main-RAM staging map in docs/retroachievements.md, which is the thing that decides
    this. 0x02400000-0x02680000 is 2.5MB of preserved, unclaimed EWRAM; this sits in the
    middle of it, 512K clear of the donor ROM above.

    The cap is a cap, not a reservation: only the loadable image passes through here, not
    the 256K window, and rcheevos' runtime measures 68K linked. Grow it if that changes, but
    re-read the staging map first -- the neighbouring ranges are preserved for reasons
    that are not obvious from the addresses.
*/
#define CARDENGINEI_ARM9_RA_BUFFERED_LOCATION        0x02600000
#define CARDENGINEI_ARM9_RA_BUFFERED_MAX             0x40000

/*
    Staging layout at that address. The magic is how the bootloader knows the launcher
    actually put something there: the region is uninitialised otherwise, and jumping into
    whatever the last occupant left would be a crash. It is the same device the colour LUT
    uses with its 'cLUT' word, for the same reason.

      +0x00  CARDENGINEI_ARM9_RA_STAGE_MAGIC, written last and cleared after the copy
      +0x10  the binary image, starting with the branch the loader checks for

    Reads as "SRA1" in a byte-wise hex dump. IMAGE_MAX is how much gets copied -- a fixed
    amount rather than a carried length, again like the colour LUT, since the destination
    window is 256K and copying past the image only touches .bss, which this binary does
    not assume is initialised anyway.

    Copying a fixed amount is only safe in one direction, and that is worth being explicit
    about: too much is harmless, too little is a truncated binary that starts correctly and
    fails somewhere in the middle of code that is simply not there. Nothing in the loader
    can detect that -- the branch at +0 still looks valid. So the Makefile for
    cardenginei_arm9_ra fails the build if the image exceeds this, which is the only place
    the two numbers can actually be compared.

    128K, raised from 64K when rcheevos went in: the runtime links to 68K, of which about
    20K is newlib's printf reached through rc_update_richpresence(). See
    docs/retroachievements.md for why that is being carried rather than cut.
*/
#define CARDENGINEI_ARM9_RA_STAGE_MAGIC              0x31415253
#define CARDENGINEI_ARM9_RA_IMAGE_OFFSET             0x10
#define CARDENGINEI_ARM9_RA_IMAGE_MAX                0x20000

/*
    Achievement definitions, in the server's own memaddr syntax, staged as text alongside
    the binary and copied into the top of its window.

    Read from a file on the SD card rather than compiled in, and that is the point. Testing
    a definition against a running game is the slowest loop in this project -- build, flash,
    play, photograph -- and a definition is exactly the kind of thing that needs trying
    several times before it is right. Through a file it is an edit, not a flash cycle.

    It is also phase 3's mechanism in miniature. When the launcher eventually logs in and
    fetches a real set before the game boots, this is the path the definitions will travel:
    launcher -> staging -> DSi WRAM. Building it now for a hand-written file means the part
    that has to work under a network later is already the part that has been exercised.

    Layout at the staged and copied addresses alike:

      +0x00  CARDENGINEI_ARM9_RA_DEFS_MAGIC, written last
      +0x04  length of the text, excluding the terminator
      +0x08  the text, NUL-terminated

    Reads as "RDA1" in a byte-wise hex dump. The block lives at the *top* of the window and
    the heap is shortened to stop below it, because the alternative -- putting it inside the
    image the loader copies -- would land it in memory the allocator hands out.
*/
#define CARDENGINEI_ARM9_RA_DEFS_MAGIC               0x31414452
#define CARDENGINEI_ARM9_RA_DEFS_MAX                 0x8000
#define CARDENGINEI_ARM9_RA_DEFS_HEADER              0x8
#define CARDENGINEI_ARM9_RA_DEFS_BUFFERED_LOCATION   (CARDENGINEI_ARM9_RA_BUFFERED_LOCATION + 0x30000)
#define CARDENGINEI_ARM9_RA_DEFS_LOCATION            (CARDENGINEI_ARM9_RA_LOCATION + CARDENGINEI_ARM9_RA_SIZE - CARDENGINEI_ARM9_RA_DEFS_MAX)

/*
    What is earned and not yet sent, grouped by game, for the in-game menu's Sync Pending page.

    Staged and copied exactly like the definitions block above and for the same reason: it has to
    survive into the game, and the only memory that does is this window. It sits directly *below* the
    definitions, and the heap is shortened to stop below both -- see the ra_startup() call in
    cardenginei_arm9_ra, which is where that reservation is made rather than in the linker script.

    Half a kilobyte for a structure that is about 180 bytes, because the whole point of the round number
    is that the next thing to want a reservation here does not have to move two other constants to get
    one. Against a 156K arena it is not a trade.

    The magic is the same value as RA_PENDING_MAGIC in ra_wifi.h and must stay that way: the bootloader
    checks it from here without knowing what a raPendingBlock is, and the launcher writes it from there.
    The host suite pins the two together, because nothing else would notice them drifting apart.
*/
#define CARDENGINEI_ARM9_RA_PENDING_MAGIC            0x31504152   /* 'RAP1' */
#define CARDENGINEI_ARM9_RA_PENDING_MAX              0x200
#define CARDENGINEI_ARM9_RA_PENDING_BUFFERED_LOCATION (CARDENGINEI_ARM9_RA_BUFFERED_LOCATION + 0x38000)
/*
    Inside the definitions' own reservation, at the top of it, and *not* carved out of the heap.

    This is where it belongs and it took a detour to get back here. An earlier build put it below the
    definitions with the arena shortened to stop under both; that build failed on hardware and so did
    the one that moved it here, so both placements were blamed in turn. Neither was the cause: the
    failure is intermittent, and it has since been seen and not seen with the same binary. Every
    conclusion drawn by comparing those builds -- including "it is the arena" and "it is this address"
    -- was reading noise.

    What settles the placement is not that evidence but the design: the definitions reserve 32 KB, the
    heap already stops below them, and the largest real set measured uses 8,306 of it. Putting the
    tally in the top of that costs nothing and takes nothing from rcheevos. The launcher's cap comes
    down by the same amount so a set can never grow into it.

    The intermittent fault is a separate open question, and raSnapshot.defsMagic exists to catch it.
*/
#define CARDENGINEI_ARM9_RA_PENDING_LOCATION         (CARDENGINEI_ARM9_RA_DEFS_LOCATION \
                                                     + CARDENGINEI_ARM9_RA_DEFS_MAX \
                                                     - CARDENGINEI_ARM9_RA_PENDING_MAX)

/*
    The in-game viewer's index: one entry per achievement, pointing into the definitions block.

    **The menu cannot parse that block, and the reason is that the block is mutated in place.**
    ra_split_definitions() turns a record's newline into a NUL so rcheevos is handed C strings, and
    turns the tab after the memaddr and the tab after the title into NULs for the same reason. By the
    time a player can open the menu, the NUL that ends a record is indistinguishable from the two
    that came from tabs -- and an earned `#!` record, which that function skips entirely, still has
    its tabs. Two shapes in one buffer, and no rule that separates them without guessing.

    Every scheme for recovering the boundaries from the bytes alone is a heuristic: "a record starts
    with `#!`, or with digits-then-colon, or with something memaddr-shaped". This project has a
    section about what heuristics cost.

    So the offsets are recorded **while the block is still pristine**, by the same pass that is about
    to mutate it, and the menu reads a table instead of a text. Nothing has to agree about
    delimiters, and a future field costs the index a member rather than costing the menu a rule.

    Placed below the pending tally, in the same reservation and for the same reason -- the
    definitions reserve 32K, the heap already stops below them, and the largest real set measured
    uses 9,662 of it. The launcher's cap comes down by both, so a set can never grow into either.

    2.5K holds 128 entries of 16 bytes with room over. 128 is the ceiling rcheevos is given elsewhere
    in this fork, and a set larger than that is one the block would refuse before the index did.

    It was 2K for a 12-byte entry, and grew when the entry gained the date an achievement was earned:
    128 * 16 + the header is 2,060, which is twelve bytes past 0x800. The extra 512 comes out of the
    rcheevos arena, which the fit test measures at about 11K of margin -- so the test that would
    notice this going too far already exists and already runs.
*/
#define CARDENGINEI_ARM9_RA_VIEWER_MAGIC             0x31564152   /* 'RAV1' */
#define CARDENGINEI_ARM9_RA_VIEWER_MAX               0xA00
#define CARDENGINEI_ARM9_RA_VIEWER_LOCATION          (CARDENGINEI_ARM9_RA_DEFS_LOCATION \
                                                      + CARDENGINEI_ARM9_RA_DEFS_MAX \
                                                      - CARDENGINEI_ARM9_RA_PENDING_MAX \
                                                      - CARDENGINEI_ARM9_RA_VIEWER_MAX)

/*
    What kind of session this boot is running, for the in-game menu. Today that is one flag:
    whether the player asked for hardcore and got it.

    Staged and copied exactly like the tally and the index above, and kept *separate* from the
    tally for one reason, which is timing. raWifiStagePending() runs at stage 13, so every
    ordinary early exit -- `sync=0`, no access point, a refused login -- leaves no pending block
    at all, while the ladder still falls through to `done:`, loads this ROM's cached set and boots
    the game. A session that earns achievements with the radio off is the normal case here, not an
    edge. A hardcore flag folded into a block that is absent on exactly those boots would read as
    "not hardcore" precisely when the answer matters most, and the menu would unlock its RAM
    editor for a session that is about to claim h=1.

    So this one is written the moment ra.cfg has been parsed and the gate has run, before a single
    register of the radio is touched, on every path out of raWifiProbe().

    Its absence is meaningful and is the safe default: no magic means no launcher told this boot
    anything, which happens on a plain nds-bootstrap build, on a RA_LAUNCHER_WIFI=0 build, and on
    any console where the RA window is not staged. None of those can submit a hardcore unlock, and
    all of them should leave the RAM viewer exactly as it has always been.

    256 bytes for one flag, for the reason the tally's half-kilobyte gives: the next thing the menu
    needs to know about the session should cost a field rather than three constants and a copy.
*/
#define CARDENGINEI_ARM9_RA_SESSION_MAGIC            0x31534152   /* 'RAS1' */
#define CARDENGINEI_ARM9_RA_SESSION_MAX              0x100
/*
    Where the hardcore flag sits inside that block, and the floor below which cheats are not on.

    Both are restated here for the bootloader, which reaches into this block and does not include
    ra_wifi.h -- the same arrangement as the magic above, and for the same reason: it must be able to
    act on these without knowing what a raSessionBlock is. The host suite pins each against its
    original, because nothing else would notice them drifting.

    The bootloader needs them because it has the last word on cheats. The launcher sizes the three
    cheat files and stages a mode; only the bootloader knows whether the engine it is about to
    install actually fits, and if it installs one it clears this byte. See RA_CHEATS_MIN_BYTES in
    ra_wifi.h for why the floor is 4 rather than 0.
*/
#define CARDENGINEI_ARM9_RA_SESSION_HARDCORE_OFFSET  0x4
#define CARDENGINEI_ARM9_RA_SESSION_REFUSAL_OFFSET   0x5
#define CARDENGINEI_ARM9_RA_SESSION_REFUSED_CHEATS   1
#define CARDENGINEI_ARM9_RA_CHEATS_MIN_BYTES         4
#define CARDENGINEI_ARM9_RA_SESSION_BUFFERED_LOCATION (CARDENGINEI_ARM9_RA_BUFFERED_LOCATION + 0x38200)
#define CARDENGINEI_ARM9_RA_SESSION_LOCATION         (CARDENGINEI_ARM9_RA_VIEWER_LOCATION \
                                                      - CARDENGINEI_ARM9_RA_SESSION_MAX)

#define CARDENGINEI_ARM9_CLUT_BUFFERED_LOCATION      0x027CE800
#define COLOR_LUT_BUFFERED_LOCATION                  0x027D0000
#define CARDENGINEI_ARM9_SDK5_BUFFERED_LOCATION      0x027E0000

#define CHEAT_ENGINE_LOCATION                      0x037DC000
#define CHEAT_ENGINE_LOCATION_ALT                  0x0301C000
#define CHEAT_ENGINE_TWLSDK_LOCATION               0x02F7BC00
#define CHEAT_ENGINE_TWLSDK_LOCATION_3DS           0x0DFFBC00
#define CHEAT_ENGINE_DSIWARE_LOCATION              0x02FD9400
#define CHEAT_ENGINE_DSIWARE_LOCATION3             0x02F80C00
#define CARDENGINEI_ARM7_LOCATION                  0x037E0400
#define CARDENGINEI_ARM7_LOCATION_DLDI             0x037F0000
#define CARDENGINEI_ARM7_LOCATION_ALT              0x03020400
#define CARDENGINEI_ARM7_LOCATION_ALT_DLDI         0x03030000
#define CARDENGINEI_ARM7_TWLSDK_LOCATION           0x03037A00
#define CARDENGINEI_ARM7_TWLSDK_LOCATION3          0x03807200
#define CARDENGINEI_ARM7_DSIWARE_LOCATION          0x03037C00
#define CARDENGINEI_ARM7_DSIWARE_LOCATION3         0x03807400
#define CARDENGINEI_ARM9_LOCATION                  0x027FC000
#define CARDENGINEI_ARM9_LOCATION_DLDI_DRIVER      CACHE_ADDRESS_TABLE_LOCATION
#define CARDENGINEI_ARM9_LOCATION2_DLDI_DRIVER     CACHE_ADDRESS_TABLE_LOCATION2
#define CARDENGINEI_ARM9_TWLSDK_LOCATION           0x02FD8800 // Used for DSi-Enhanced games in DSi mode
#define CARDENGINEI_ARM9_TWLSDK_LOCATION3          0x02F80000 // Used for DSi-Exclusive games
#define CARDENGINEI_ARM9_DSIWARE_LOCATION          CARDENGINEI_ARM9_TWLSDK_LOCATION
#define CARDENGINEI_ARM9_DSIWARE_LOCATION3         CARDENGINEI_ARM9_TWLSDK_LOCATION3
#define CARDENGINEI_ARM9_CLUT_LOCATION             0x03732800

/*
    cardenginei_arm9_ra -- the RetroAchievements code that does not fit in the ARM9
    cardengine's 12K window. DSi WRAM is the only region that can host it: MPU region 3
    denies instruction fetches from 0x0C/0x0D, main RAM belongs to the running game, and
    the colour LUT already proves code executes from here.

    It takes the top 256K of the 512K window, leaving 0x03700000-0x03740000 for the
    nitro file info preload and the ROM-in-RAM headroom -- so RA_WRAMSIZE below is what
    wramSize becomes when this is loaded, exactly as 0x32800 is what it becomes for the
    colour LUT. The two are mutually exclusive: the LUT's stored palettes live inside
    this range. See the space budget in docs/retroachievements.md; 256K covers the
    measured 100-270K that rcheevos plus a real achievement set needs.
*/
#define CARDENGINEI_ARM9_RA_LOCATION               0x03740000
#define CARDENGINEI_ARM9_RA_SIZE                   0x40000
#define CARDENGINEI_ARM9_RA_WRAMSIZE               0x40000

#define CARDENGINE_SHARED_ADDRESS_SDK1 0x027FFA0C
#define CARDENGINE_SHARED_ADDRESS_SDK5 0x02FFFA0C

/*
    How many u32 slots that block actually has, which is not a round number and is not obvious from
    anything near it: the next thing in memory is UNPATCHED_FUNCTION_LOCATION at 0x027FFA40, so
    sharedAddr[13] *is* the unpatched-function table. Slots 0-8 are taken (card reads, the DMA9 and
    PING handshakes, the in-game menu's battery and clock), which leaves exactly four.

    Written down because the failure mode is silent. Adding a slot past the end would corrupt a table
    the loader uses to restore the game's own functions, and would show up as a game misbehaving with
    nothing pointing back here. RA_SHARED_* below claims two of the four and static-asserts the bound.
*/
#define CARDENGINE_SHARED_SLOTS        13

#define LOADER_RETURN_LOCATION                     (u32)CARDENGINEI_ARM7_BUFFERED_LOCATION+0xF400
#define LOADER_RETURN_SDK5_LOCATION                (u32)CARDENGINEI_ARM7_BUFFERED_LOCATION+0x8400
#define LOADER_RETURN_DSIWARE_LOCATION             (u32)CARDENGINEI_ARM7_BUFFERED_LOCATION+0x8000

#define RESET_PARAM      0x27FFC20
#define RESET_PARAM_SDK5 0x2FFFC20

//#define TEMP_MEM 0x02FFE000 // __DSiHeader

#define NDS_HEADER         0x027FFE00
#define NDS_HEADER_SDK5    0x02FFFE00 // __NDSHeader
#define NDS_HEADER_POKEMON 0x027FF000

#define DSI_HEADER         0x027FE000
#define DSI_HEADER_SDK5    0x02FFE000 // __DSiHeader

#define ROM_LOCATION               0x0C3EC000
#define ROM_LOCATION_ALT           0x0C400000
#define ROM_LOCATION_DSIMODE       0x0C800000
#define ROM_LOCATION_TWLSDK        0x0D000000

#define CACHE_ADRESS_START                     ROM_LOCATION
#define CACHE_ADRESS_START_ALT                 ROM_LOCATION_ALT
#define CACHE_ADRESS_START_DSIMODE             ROM_LOCATION_DSIMODE
#define retail_CACHE_ADRESS_START_SMALL        0x0CFDC000
#define retail_CACHE_ADRESS_START_TWLSDK_SMALL 0x02F60000
#define retail_CACHE_ADRESS_START_TWLSDK       0x02F00000
#define retail_CACHE_ADRESS_START_TWLSDK_LARGE 0x02D00000

#define retail_CACHE_ADRESS_SIZE              0xBE0000
#define retail_CACHE_ADRESS_SIZE_DSIMODE      0x7FF000
#define retail_CACHE_ADRESS_SIZE_BROWSER      0x3E0000
#define retail_CACHE_ADRESS_SIZE_TWLSDK_SMALL       0x20000
#define retail_CACHE_ADRESS_SIZE_TWLSDK_SMALL_CHEAT 0x1BC00
#define retail_CACHE_ADRESS_SIZE_TWLSDK       0x80000
#define retail_CACHE_ADRESS_SIZE_TWLSDK_CHEAT 0x7BC00
#define retail_CACHE_ADRESS_SIZE_TWLSDK_LARGE       0x280000
#define retail_CACHE_ADRESS_SIZE_TWLSDK_LARGE_CHEAT 0x27BC00

#define dev_CACHE_ADRESS_START_TWLSDK       0x0D000000
#define dev_CACHE_ADRESS_START_TWLSDK_SMALL 0x0DFE0000

#define dev_CACHE_ADRESS_SIZE                 0x1BE0000
#define dev_CACHE_ADRESS_SIZE_DSIMODE         0x17FF000
#define dev_CACHE_ADRESS_SIZE_TWLSDK          0x1000000
#define dev_CACHE_ADRESS_SIZE_TWLSDK_CHEAT    0xFFBC00
#define dev_CACHE_ADRESS_SIZE_TWLSDK_ROMinRAM 0xFE0000

#define dev_CACHE_SLOTS_16KB        dev_CACHE_ADRESS_SIZE/0x4000
#define dev_CACHE_SLOTS_16KB_TWLSDK dev_CACHE_ADRESS_SIZE_TWLSDK/0x4000

#endif // LOCATIONS_H
