#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitPro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

#---------------------------------------------------------------------------------
# TARGET   - name of the output binary
# BUILD    - object/intermediate file directory
# SOURCES  - directories containing source code
# DATA     - directories containing binary data
# INCLUDES - directories containing header files
#---------------------------------------------------------------------------------
TARGET      := $(notdir $(CURDIR))
BUILD       := build
SOURCES     := source
DATA        := data
INCLUDES    := include
ROMFS       := romfs

APP_TITLE       := Activity Log++
APP_DESCRIPTION := 3DS Activity Log viewer and sync tool
APP_AUTHOR      := homebrew

#---------------------------------------------------------------------------------
# Code generation options
#---------------------------------------------------------------------------------
ARCH    := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS  := -g -Wall -O2 -mword-relocations \
            -fomit-frame-pointer -ffunction-sections \
            $(ARCH)

CFLAGS  += $(INCLUDE) -D__3DS__

CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions

ASFLAGS := -g $(ARCH)

LDFLAGS  = -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS    := -lcitro2d -lcitro3d -lctru -lm

#---------------------------------------------------------------------------------
# Library search paths
#---------------------------------------------------------------------------------
# Fall back if 3ds_rules didn't export CTRULIB
CTRULIB ?= $(DEVKITPRO)/libctru
LIBDIRS := $(CTRULIB)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT   := $(CURDIR)/$(TARGET)
export TOPDIR   := $(CURDIR)

export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                   $(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR  := $(CURDIR)/$(BUILD)

CFILES      := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES    := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES      := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES    := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

ifeq ($(strip $(CPPFILES)),)
    export LD := $(CC)
else
    export LD := $(CXX)
endif

export OFILES_BIN   := $(addsuffix .o,$(BINFILES))
export OFILES_SRC   := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES       := $(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN   := $(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE  := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                   $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                   -I$(CURDIR)/$(BUILD)

export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifeq ($(strip $(ICON)),)
    icons := $(wildcard *.png)
    ifneq (,$(findstring $(TARGET).png,$(icons)))
        export APP_ICON := $(TOPDIR)/$(TARGET).png
    else
        ifneq (,$(findstring icon.png,$(icons)))
            export APP_ICON := $(TOPDIR)/icon.png
        endif
    endif
else
    export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_SMDH)),)
    export _3DSXFLAGS += --smdh=$(CURDIR)/$(TARGET).smdh
endif

ifneq ($(ROMFS),)
    export _3DSXFLAGS += --romfs=$(CURDIR)/$(ROMFS)
endif

.PHONY: $(BUILD) clean

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(OUTPUT).smdh $(TARGET).elf

#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------

DEPENDS := $(OFILES:.o=.d)

#---------------------------------------------------------------------------------
# Main targets
#---------------------------------------------------------------------------------
ifeq ($(strip $(NO_SMDH)),)
.PHONY: all
all: $(OUTPUT).3dsx $(OUTPUT).smdh

# smdh must exist before 3dsxtool embeds it
$(OUTPUT).3dsx: $(OUTPUT).smdh
endif

$(OUTPUT).3dsx: $(OUTPUT).elf

$(OUTPUT).elf: $(OFILES)

$(OFILES_SRC): $(HFILES_BIN)

#---------------------------------------------------------------------------------
# Binary data rule
#---------------------------------------------------------------------------------
%.bin.o %_bin.h: %.bin
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
