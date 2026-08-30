#---------------------------------------------------------------------------------
# dsiwifi's ARM9 half -- lwip and the host side of the driver -- built in this tree rather
# than taken from libs/dsiwifi/lib/libdsiwifi9.a.
#
# The reason is one number. dsiwifi sizes lwip for a homebrew app that owns all 16 MB:
# PBUF_POOL_SIZE 512 with MEMP_MEM_MALLOC 0 makes memp_memory_PBUF_POOL_base 784,387 bytes of
# .bss, and the launcher's entire ARM9 link region is 753,664. The pool is larger than the
# region. See include/lwipopts_ndsbs.h for the sizing, and for why the launcher cannot simply
# be given more room instead.
#
# The submodule's own Makefile offers no way in: its CFLAGS and INCLUDES are `:=`
# assignments, so a command-line override replaces rather than extends them, and `release`
# builds both CPUs at once so any override would hit the ARM7 half too. Building the sources
# here is what buys control of the flags -- the same reason retail/cardenginei/arm9_ra
# compiles rcheevos' sources instead of linking a library.
#
# Only reached when RA_LAUNCHER_WIFI=1. Nothing here is part of a normal build.
#
# ppp/ is left out of SOURCES: PPP_SUPPORT is off, so none of it is referenced, and its
# polarssl/ directory carries md5.c, sha1.c, des.c and arc4.c whose basenames would collide
# with the launcher's own source/polarssl if these ever moved into that build.
#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------
ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

include $(DEVKITARM)/ds_rules

DSIWIFI	:=	$(abspath $(dir $(firstword $(MAKEFILE_LIST)))/../../libs/dsiwifi)
HOST	:=	$(DSIWIFI)/arm_host

BUILD		:=	build
SOURCES		:=	$(HOST)/source $(DSIWIFI)/common/source $(HOST)/source/nds \
				$(HOST)/source/lwip $(HOST)/source/lwip/ipv4 $(HOST)/source/lwip/ipv6 \
				$(HOST)/source/lwip/netif $(HOST)/source/lwip/api
INCLUDES	:=	$(HOST)/include $(DSIWIFI)/common/source $(DSIWIFI)/include \
				$(DSIWIFI)/include/lwip $(HOST)/source/nds

#---------------------------------------------------------------------------------
# Deliberately the same code-generation flags the submodule uses for this library, so the
# only difference between what we build and what it builds is the sizing header below.
#---------------------------------------------------------------------------------
ARCH	:=	-mthumb -mthumb-interwork

CFLAGS	:=	-g -Wall -Os\
			-ffunction-sections -fdata-sections \
			-march=armv5te -mtune=arm946e-s -fomit-frame-pointer -mno-unaligned-access\
			-ffast-math \
			$(ARCH)

CFLAGS	+=	$(INCLUDE) -DARM9 -DARM_HOST

# The whole point of building this here. See include/lwipopts_ndsbs.h -- no -I path can
# override lwip's quoted include of its own sibling lwipopts.h, so this arrives ahead of it
# and wins on the header guard.
CFLAGS	+=	-include lwipopts_ndsbs.h

CXXFLAGS	:=	$(CFLAGS) -fno-rtti -fno-exceptions
ASFLAGS		:=	-g $(ARCH) $(INCLUDE) -DARM9 -DARM_HOST

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export TARGET	:=	$(CURDIR)/libdsiwifi9_ndsbs.a
export DEPSDIR	:=	$(CURDIR)/$(BUILD)
export VPATH	:=	$(foreach dir,$(SOURCES),$(dir))

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

export OFILES	:=	$(CFILES:.c=.o) $(SFILES:.s=.o)

# LIBDIRS before INCLUDE: both are `:=`, so INCLUDE would otherwise expand an empty LIBDIRS
# and the build would fail on nds/ndstypes.h -- which is exactly how this was found.
export LIBDIRS	:=	$(LIBNDS)

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(dir)) \
					-I$(CURDIR)/include \
					$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
					-I$(CURDIR)/$(BUILD)

.PHONY: $(BUILD) clean

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) libdsiwifi9_ndsbs.a

#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------

DEPENDS	:=	$(OFILES:.o=.d)

$(TARGET): $(OFILES)
	@rm -f $@
	@$(AR) rcs $@ $(OFILES)
	@echo "built ... $(notdir $@) ($$(stat -c%s $@) bytes, $(words $(OFILES)) objects)"

-include $(DEPENDS)

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
