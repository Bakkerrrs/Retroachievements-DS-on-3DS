#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------
ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

include $(DEVKITARM)/ds_rules

#---------------------------------------------------------------------------------
# BUILD is the directory where object files & intermediate files will be placed
# SOURCES is a list of directories containing source code
# INCLUDES is a list of directories containing extra header files
# DATA is a list of directories containing binary files
# all directories are relative to this makefile
#---------------------------------------------------------------------------------
BUILD		:=	build
#---------------------------------------------------------------------------------
# rcheevos comes in as a submodule, pinned to a tag. Only its *runtime* is built here:
# parsing achievement definitions and evaluating them against memory. rapi (HTTP and
# JSON), rc_client, and all of rhash except md5 are for talking to the server and
# identifying the game, which phase 2 does not do. See docs/retroachievements.md for the
# measured sizes -- the whole library is 121K, the runtime is 35K, and the loader only
# copies 64K into this window.
#---------------------------------------------------------------------------------
SOURCES		:=	source rcheevos/src rcheevos/src/rcheevos rcheevos/src/rhash
INCLUDES	:=	include ../../common/include rcheevos/include rcheevos/src
DATA		:=	data


#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH	:=	-mthumb-interwork -march=armv5te -mtune=arm946e-s

CFLAGS	:=	-g -Wall -Os -ffunction-sections -fdata-sections\
 			-march=armv5te -mtune=arm946e-s -fomit-frame-pointer\
			-ffast-math \
			$(ARCH)

# ADD -DDEBUG here for logging/debug
CFLAGS	+=	$(INCLUDE) -DARM9

# A diagnostic, off by default: RA_OVERLAY_DEMO=60 raises the achievement notification once a
# second regardless of whether anything unlocked. That answers "can anything be seen on that screen
# at all" in ten seconds instead of a three-minute session that ends with an achievement permanently
# earned -- and it is what identified the layer ordering as the reason a real unlock was invisible.
#
# Needs the object removed to take effect. make tracks file timestamps and not CFLAGS, so setting
# this on an already-built tree relinks the old ra_overlay.o and the ROM is silently not a
# diagnostic at all -- which is exactly what happened the first time, and the snapshot said so with
# shows and denied both at zero after 2,863 ticks.
# Use: rm -f cardenginei/arm9_ra/build/ra_overlay.o (or make clean).
#
# It lives in *this* Makefile because the overlay does. It was in cardenginei/arm9's until the
# overlay moved here, and leaving it there would have been a flag that quietly reached nothing.
ifneq ($(RA_OVERLAY_DEMO),)
CFLAGS	+=	-DOVERLAY_DEMO_INTERVAL=$(RA_OVERLAY_DEMO)
endif

CXXFLAGS	:=	$(CFLAGS) -fno-rtti -fno-exceptions

ASFLAGS	:=	-g $(ARCH) $(INCLUDE)

#---------------------------------------------------------------------------------
# -Os and --gc-sections because the loader copies a fixed CARDENGINEI_ARM9_RA_IMAGE_MAX
# into the window: without dead-code stripping every rcheevos object links whole and the
# image pushes against that limit for code no achievement will ever reach. The entry
# point survives because the linker script KEEPs .init.
#---------------------------------------------------------------------------------
LDFLAGS	=	-nostartfiles -T cardengine.ld -g $(ARCH) -Wl,--gc-sections -Wl,-Map,$(notdir $*.map)

#---------------------------------------------------------------------------------
# any extra libraries we wish to link with the project (order is important)
#
# -lm is for one symbol: fmod(), which rcheevos' float modulus operator reaches. No DS
# achievement is likely to use it, but the reference is not conditional, so the library
# does not link without it. Not libnds -- nothing here touches libnds, and linking it
# would pull an interrupt dispatcher and a console into a window that has neither.
#---------------------------------------------------------------------------------
LIBS	:=	-lm

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

export TARGET	:=	cardenginei_arm9_ra
export OUTPUT	:=	$(CURDIR)/../../nitrofiles/$(TARGET)
export DEPSDIR	:=	$(CURDIR)/$(BUILD)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
					$(foreach dir,$(DATA),$(CURDIR)/$(dir))

#---------------------------------------------------------------------------------
# A whitelist rather than a blacklist, because the submodule's file set grows with
# upstream and a blacklist would silently start pulling things in. Everything in
# src/rcheevos is the runtime except rc_validate.c, which is a development-time checker
# for achievement definitions and does not build for this target.
#---------------------------------------------------------------------------------
RCHEEVOS_RUNTIME	:=	$(filter-out rc_validate.c, \
				$(notdir $(wildcard rcheevos/src/rcheevos/*.c))) \
				rc_util.c rc_compat.c rc_version.c \
				md5.c

CFILES		:=	$(notdir $(wildcard source/*.c)) $(RCHEEVOS_RUNTIME)
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

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

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
					$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
					-I$(CURDIR)/$(BUILD)
 
export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)
 
.PHONY: $(BUILD) clean
 
#---------------------------------------------------------------------------------
$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).bin $(TARGET).elf
 
 
#---------------------------------------------------------------------------------
else

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
$(OUTPUT).bin:	$(DEPSDIR)/$(TARGET).elf
$(DEPSDIR)/$(TARGET).elf:	cardengine.ld $(OFILES)
#	@echo linking $(notdir $@)
#	@$(LD)  $(LDFLAGS) $(OFILES) $(LIBPATHS) $(LIBS) -o $@

cardengine.ld:	$(CURDIR)/../cardengine.ld.in
	$(CPP) -P $(INCLUDE) $< $@


#---------------------------------------------------------------------------------
# The loader copies a fixed CARDENGINEI_ARM9_RA_IMAGE_MAX bytes out of staging, so an
# image larger than that is copied incomplete: the branch at its entry point is still
# valid, so it boots, and then fails inside code that is simply not there. Nothing at run
# time can detect it and the snapshot would show a stage that never advances.
#
# This is the only place the built size and the copy length can be compared, so it is
# compared here. The limit is read back out of locations.h rather than repeated, so the
# check cannot quietly disagree with the loader it is protecting.
#---------------------------------------------------------------------------------
RA_IMAGE_MAX	:=	$(shell echo CARDENGINEI_ARM9_RA_IMAGE_MAX | \
					$(CPP) -P $(INCLUDE) -include locations.h - | grep -v '^$$' | tail -1)

#---------------------------------------------------------------------------------
%.bin	:	%.elf
#---------------------------------------------------------------------------------
	@$(OBJCOPY) -O binary $< $@
	@size=`wc -c < $@`; max=$$(($(RA_IMAGE_MAX))); \
	if [ "$$size" -gt "$$max" ]; then \
		echo "$(notdir $@): $$size bytes, over the $$max the loader copies." >&2; \
		echo "It would be truncated at boot rather than fail here. Raise" >&2; \
		echo "CARDENGINEI_ARM9_RA_IMAGE_MAX in retail/common/include/locations.h," >&2; \
		echo "or take something out -- see docs/retroachievements.md." >&2; \
		rm -f $@; exit 1; \
	fi; \
	echo "built ... $(notdir $@)  ($$size of $$max bytes, `expr $$max - $$size` spare)"

-include $(DEPSDIR)/*.d
 
#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
