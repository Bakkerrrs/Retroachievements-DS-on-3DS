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
SOURCES		:=	source
INCLUDES	:=	include build ../common/include
DATA		:=

#---------------------------------------------------------------------------------
# Step two of the RA network ladder. `make RA_LAUNCHER_WIFI=1` links dsiwifi's ARM7 half
# into the launcher so the Atheros chip can be brought up before a game exists to contend
# for this CPU -- see retail/arm7/source/ra_wifi7.c and docs/retroachievements.md.
#
# Off by default and it must stay off in anything shipped: a build with it on stops on a
# diagnostic summary instead of booting the game.
#
# The path is anchored with abspath rather than written relative to $(CURDIR), because this
# Makefile is read twice with $(CURDIR) meaning two different directories -- once here and
# once from build/ -- and only one of the two uses below is expanded in each pass.
#---------------------------------------------------------------------------------
DSIWIFI	:=	$(abspath $(dir $(firstword $(MAKEFILE_LIST)))/../../libs/dsiwifi)

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH	:=	-mthumb-interwork -march=armv4t

CFLAGS	:=	-g -Wall -O2\
			-mcpu=arm7tdmi -mtune=arm7tdmi -fomit-frame-pointer\
			-ffast-math \
			$(ARCH)

CFLAGS	+=	$(INCLUDE) -DARM7
# -iquote and not -I: see the same note in retail/arm9/Makefile. dsiwifi's include directory
# carries a netdb.h and a sys/socket.h of its own, and neither should ever win against
# newlib's for an angle-bracket include.
ifneq ($(filter 1 2,$(RA_LAUNCHER_WIFI)),)
CFLAGS	+=	-DRA_LAUNCHER_WIFI=$(RA_LAUNCHER_WIFI) -iquote $(DSIWIFI)/include
endif
CXXFLAGS	:=	$(CFLAGS) -fno-rtti -fno-exceptions

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=ds_arm7.specs -g $(ARCH) -Wl,--nmagic -Wl,-Map,$(notdir $*).map

#---------------------------------------------------------------------------------
# any extra libraries we wish to link with the project
#---------------------------------------------------------------------------------
LIBS	:= -lnds7

# Named by path rather than -ldsiwifi7, so a stale or missing archive is a clear error
# instead of the linker quietly finding some other copy. Before -lnds7, since it needs
# libnds and not the other way round.
ifneq ($(filter 1 2,$(RA_LAUNCHER_WIFI)),)
LIBS	:=	$(DSIWIFI)/lib/libdsiwifi7.a $(LIBS)
ARM7ELF_EXTRA_DEPS	:=	dsiwifi-check
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
export ARM7ELF	:=	$(CURDIR)/$(TARGET).elf
export DEPSDIR	:=	$(CURDIR)/$(BUILD)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
					$(foreach dir,$(DATA),$(CURDIR)/$(dir))

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

export OFILES	:=	$(addsuffix .o,$(BINFILES)) \
					$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
					$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
					-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

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

.PHONY: $(BUILD) clean

#---------------------------------------------------------------------------------
# RA_LAUNCHER_WIFI changes compiler flags and the library list, not the source list, so make
# cannot see it change: flipping it and rebuilding reuses every object and produces a .nds
# that looks built and has no probe in it, with a build log that says it worked. That is the
# failure shape this project fears most, and it happened on the first attempt to use the
# switch -- so the mode is stamped in the build directory and a change of mode wipes it.
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
	@$(MAKE) -C $(BUILD) -f $(CURDIR)/Makefile

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).elf $(TARGET).arm7


#---------------------------------------------------------------------------------
else

DEPENDS	:=	$(OFILES:.o=.d)

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
$(ARM7ELF)	:	$(OFILES) $(ARM7ELF_EXTRA_DEPS)
	@echo linking $(notdir $@)
	@$(LD)  $(LDFLAGS) $(OFILES) $(LIBPATHS) $(LIBS) -o $@

# The archive is checked rather than named as a prerequisite of the link: naming a missing
# .a sends make off to an implicit archive rule, and the build then fails as a confusing
# `ar` error instead of as the missing submodule it is. Same shape as the check in
# tools/wifiprobe/arm7/Makefile, for the same reason.
#
# The prerequisite itself is conditional, not just its recipe, so that a default build has
# no phony prerequisite on the ELF at all -- a phony one would relink the ARM7 on every
# build for everybody, which is not something a switch that is off should cost.
.PHONY: dsiwifi-check
dsiwifi-check:
	@[ -f $(DSIWIFI)/lib/libdsiwifi7.a ] || { \
		echo "libdsiwifi7.a not built -- run: make -C libs/dsiwifi release" >&2; \
		echo "(and if libs/dsiwifi is empty: git submodule update --init)" >&2; \
		exit 1; \
	}


#---------------------------------------------------------------------------------
# you need a rule like this for each extension you use as binary data 
#---------------------------------------------------------------------------------
%.bin.o	:	%.bin
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)
 
#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
