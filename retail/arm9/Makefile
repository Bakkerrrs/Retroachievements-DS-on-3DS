#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------
ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

include $(DEVKITARM)/ds_rules

# If on a tagged commit, use just the tag
ifneq ($(shell echo $(shell git tag -l --points-at HEAD) | head -c 1),)
GIT_VER := $(shell git tag -l --points-at HEAD)
else
GIT_VER := $(shell git describe --abbrev=0 --tags)-$(shell git rev-parse --short=7 HEAD)
endif

# Ensure version.h exists
ifeq (,$(wildcard include/version.h))
$(shell mkdir -p include)
$(shell touch include/version.h)
endif

# Print new version if changed
ifeq (,$(findstring $(GIT_VER), $(shell cat include/version.h)))
$(shell printf "#ifndef VERSION_HPP\n#define VERSION_HPP\n\n#define VER_NUMBER \"$(GIT_VER)\"\n\n#endif\n" > include/version.h)
endif

#---------------------------------------------------------------------------------
# BUILD is the directory where object files & intermediate files will be placed
# SOURCES is a list of directories containing source code
# INCLUDES is a list of directories containing extra header files
# DATA is a list of directories containing binary files
# all directories are relative to this makefile
#---------------------------------------------------------------------------------
BUILD		:=	build
SOURCES		:=	source source/flashcard source/polarssl source/twltool source/gm9i source/nand mbedtls libeasysave/source/ini ../common/source_general
INCLUDES	:=	include include/flashcard include/polarssl include/twltool include/gm9i include/nand libeasysave/include ../common/include

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH	:=	-mthumb-interwork

CFLAGS	:=	-g -Wall -O2\
 			-march=armv5te -mtune=arm946e-s -fomit-frame-pointer\
			-ffast-math \
			$(ARCH)

CFLAGS	+=	$(INCLUDE) -DARM9 -DNO_CARDID -DDEFAULT_CARD_READ_DMA

#---------------------------------------------------------------------------------
# Step two of the RA network ladder -- see retail/arm9/source/ra_wifi.c. This side links no
# library: dsiwifi's ARM9 half is lwip, and everything step two asks about happens on the
# ARM7. Only the IPC header is needed, which is why this adds an include path and nothing
# else. Off by default, and a build with it on stops on a summary instead of booting a game.
#---------------------------------------------------------------------------------
# -iquote and not -I, deliberately. libs/dsiwifi/include also holds netdb.h, sys/socket.h,
# arpa/inet.h and netinet/ for its lwip, and this ARM9 builds mbedtls and polarssl next
# door. On the -I path those would shadow newlib's; -iquote applies to #include "..." only,
# so <sys/socket.h> still means newlib's. Same choice tools/wifiprobe/arm7 makes.
ifneq ($(filter 1 2,$(RA_LAUNCHER_WIFI)),)
CFLAGS	+=	-DRA_LAUNCHER_WIFI=$(RA_LAUNCHER_WIFI) \
			-iquote $(abspath $(dir $(firstword $(MAKEFILE_LIST)))/../../libs/dsiwifi/include)
endif

#---------------------------------------------------------------------------------
# Step 3 needs lwip on this side, and it is *our* build of dsiwifi's ARM9 half rather than
# the submodule's: see retail/dsiwifi9/include/lwipopts_ndsbs.h, which cuts lwip's static
# pools from 833K of .bss to 76K so they fit this 736K link region at all.
#---------------------------------------------------------------------------------
DSIWIFI9	:=	$(abspath $(dir $(firstword $(MAKEFILE_LIST)))/../dsiwifi9)

#---------------------------------------------------------------------------------
# Step 3b needs one file out of rcheevos: md5.c. Not the rest of rhash -- rc_hash_nintendo_ds()
# allocates the ROM's largest code block in one piece and the launcher has no such heap, so
# retail/arm9/source/ra_hash.c streams the same ranges instead and tools/ra_hash_test.c holds
# it to rcheevos' answer on real .nds files.
#
# -iquote so nothing in rcheevos/src can win an angle-bracket include against newlib.
#---------------------------------------------------------------------------------
RCHEEVOS	:=	$(abspath $(dir $(firstword $(MAKEFILE_LIST)))/../cardenginei/arm9_ra/rcheevos)

ifneq ($(filter 1 2,$(RA_LAUNCHER_WIFI)),)
CFLAGS	+=	-iquote $(RCHEEVOS)/src
endif

CXXFLAGS	:=	$(CFLAGS) -fno-rtti -fno-exceptions

ASFLAGS	:=	-g $(ARCH) $(INCLUDE)

LDFLAGS	=	-specs=../ds_arm9_ndsbs.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

#---------------------------------------------------------------------------------
# any extra libraries we wish to link with the project (order is important)
#---------------------------------------------------------------------------------
LIBS	:=	-lfat -lnds9

# Named by path rather than -l, so a stale or missing archive is a clear error instead of the
# linker quietly finding another copy. Before -lfat/-lnds9, since it needs them.
ifneq ($(filter 1 2,$(RA_LAUNCHER_WIFI)),)
LIBS	:=	$(DSIWIFI9)/libdsiwifi9_ndsbs.a $(LIBS)
endif

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level containing
# include and lib
#---------------------------------------------------------------------------------
LIBDIRS	:=	$(LIBNDS)

#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

TARGET			:=	nds-bootstrap
export ARM9ELF	:=	$(CURDIR)/$(TARGET).elf
export DEPSDIR	:= $(CURDIR)/$(BUILD)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
					$(foreach dir,$(DATA),$(CURDIR)/$(dir))

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES	:=	

#---------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
#---------------------------------------------------------------------------------
	export LD	:=	$(CC)
#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------
	export LD	:=	$(CXX)
#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------

export OFILES	:=	$(addsuffix .o,$(BINFILES)) \
					$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)

# md5.c by name rather than by adding its directory to SOURCES: that directory is all of
# rhash, and the rest of it is the server-side and disc code this launcher must not carry.
ifneq ($(filter 1 2,$(RA_LAUNCHER_WIFI)),)
export VPATH	:=	$(VPATH) $(RCHEEVOS)/src/rhash
export OFILES	:=	$(OFILES) md5.o
endif

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
					$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
					-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: $(BUILD) clean

#---------------------------------------------------------------------------------
# See the same stamp in retail/arm7/Makefile. RA_LAUNCHER_WIFI changes flags and not the
# source list, so make cannot see it change; without this, flipping it reuses stale objects
# and produces a .nds that looks built and has no probe in it.
#---------------------------------------------------------------------------------
# The raw value, not on/off. There are three modes now -- 0 not built, 1 the diagnostic that
# halts, 2 fetch-then-boot -- and a stamp that could not tell 1 from 2 would hand back objects
# built for the wrong one, which is the exact failure this stamp exists to prevent.
RA_WIFI_MODE	:=	$(if $(RA_LAUNCHER_WIFI),$(RA_LAUNCHER_WIFI),0)
RA_WIFI_STAMP	:=	$(BUILD)/.ra_launcher_wifi-$(RA_WIFI_MODE)

$(BUILD):
	@[ -f $(RA_WIFI_STAMP) ] || rm -fr $@ $(TARGET).elf
	@[ -d $@ ] || mkdir -p $@
	@touch $(RA_WIFI_STAMP)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).elf $(TARGET).nds *.bin 

#---------------------------------------------------------------------------------
else

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
$(ARM9ELF)	:	$(OFILES)
	@echo linking $(notdir $@)
	@$(LD)  $(LDFLAGS) $(OFILES) $(LIBPATHS) $(LIBS) -o $@

#---------------------------------------------------------------------------------
# you need a rule like this for each extension you use as binary data 
#---------------------------------------------------------------------------------
%.bin.o	:	%.bin
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPSDIR)/*.d

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
