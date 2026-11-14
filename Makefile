#---------------------------------------------------------------------------------
# pilis3 - Bilibili client for PlayStation 3
# PSL1GHT ppu_rules build, same toolchain flow as pkgi-ps3.
#---------------------------------------------------------------------------------
.SUFFIXES:
.NOTPARALLEL:
ifeq ($(strip $(PSL1GHT)),)
$(error "Please set PSL1GHT in your environment. export PSL1GHT=<path>")
endif

TITLE       :=	pilis3
APPID       :=	NP00WILI1
CONTENTID   :=	UP0001-$(APPID)_00-0000000000000000
ICON0       :=	pkgfiles/ICON0.PNG
SFOXML      :=	sfo.xml

include $(PSL1GHT)/ppu_rules

TARGET      :=	out/pilis3
BUILD       :=	build
SOURCES     :=	source source/platform source/net source/ui source/player
DATA        :=	
SHADERS     :=	
INCLUDES    :=	include source
PKGFILES    :=	pkgfiles

FFMPEG_PS3  ?=	/opt/ffmpeg-ps3

LIBS        :=	-L$(FFMPEG_PS3)/lib -lavformat -lavcodec -lavutil \
				-lcurl -lya2d -lfont3d -ltiny3d -lsimdmath -lgcm_sys -lio -lsysutil \
				-lrt -llv2 -lpngdec -lsysmodule -lm -lsysfs -ljpgdec -lnet -lfreetype -lz \
				-laudio -lpolarssl -ljson-c -lnetctl -lvdec

CFLAGS      =	-O2 -Wall -mcpu=cell -std=gnu99 $(MACHDEP) $(INCLUDE) -DAPP_VERSION=\"0.1\"
CXXFLAGS    =	$(CFLAGS)
LDFLAGS     =	$(MACHDEP) -Wl,-Map,$(notdir $@).map

LIBDIRS     :=	$(PORTLIBS)

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT   :=	$(CURDIR)/$(TARGET)
export VPATH    :=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
					$(foreach dir,$(DATA),$(CURDIR)/$(dir)) \
					$(foreach dir,$(SHADERS),$(CURDIR)/$(dir))
export DEPSDIR  :=	$(CURDIR)/$(BUILD)
export BUILDDIR :=	$(CURDIR)/$(BUILD)

CFILES      :=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES    :=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
sFILES      :=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
SFILES      :=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.S)))
BINFILES    :=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.bin)))
PNGFILES    :=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.png)))
JPGFILES    :=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.jpg)))

ifeq ($(strip $(CPPFILES)),)
	export LD   :=	$(CC)
else
	export LD   :=	$(CXX)
endif

export OFILES   :=	$(addsuffix .o,$(BINFILES)) \
					$(addsuffix .o,$(PNGFILES)) \
					$(addsuffix .o,$(JPGFILES)) \
					$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) \
					$(sFILES:.s=.o) $(SFILES:.S=.o)

export INCLUDE  :=	$(foreach dir,$(INCLUDES), -I$(CURDIR)/$(dir)) \
					$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
					$(LIBPSL1GHT_INC) \
					-I$(PORTLIBS)/include/freetype2 \
					-I$(PORTLIBS)/include/json-c \
					-I$(PORTLIBS)/include/curl \
					-I$(FFMPEG_PS3)/include \
					-I$(CURDIR)/$(BUILD) -I$(PORTLIBS)/include

export LIBPATHS :=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib) \
					$(LIBPSL1GHT_LIB) -L$(PORTLIBS)/lib

.PHONY: $(BUILD) clean pkg all

all: pkg

$(BUILD):
	[ -d $@ ] || mkdir -p $@
	[ -d $(dir $(OUTPUT)) ] || mkdir -p $(dir $(OUTPUT))
	$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile V=1 VERBOSE=1

clean:
	echo clean ...
	rm -fr $(BUILD) $(OUTPUT).elf $(OUTPUT).self EBOOT.BIN

pkg: clean $(BUILD) $(OUTPUT).pkg

else

DEPENDS :=	$(OFILES:.o=.d)

$(OUTPUT).self: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

endif
