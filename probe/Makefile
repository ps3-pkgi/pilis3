#---------------------------------------------------------------------------------
# pilis3 VDEC probe - independent package (APPID NP00VDEC0)
#---------------------------------------------------------------------------------
.SUFFIXES:
.NOTPARALLEL:
ifeq ($(strip $(PSL1GHT)),)
$(error "Please set PSL1GHT in your environment. export PSL1GHT=<path>")
endif

TITLE       :=	pilis3 vdec probe
APPID       :=	NP00VDEC0
CONTENTID   :=	UP0001-$(APPID)_00-0000000000000000
ICON0       :=	pkgfiles/ICON0.PNG
SFOXML      :=	sfo.xml

include $(PSL1GHT)/ppu_rules

TARGET      :=	out/vdec_probe
BUILD       :=	build
SOURCES     :=	source
DATA        :=	
SHADERS     :=	
INCLUDES    :=	
PKGFILES    :=	$(empty)

LIBS        :=	-ltiny3d -lgcm_sys -lsysutil -lio -lsysmodule -lrt -llv2 -lvdec -lm

CFLAGS      =	-O2 -Wall -mcpu=cell -std=gnu99 $(MACHDEP) $(INCLUDE)
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

ifeq ($(strip $(CPPFILES)),)
	export LD   :=	$(CC)
else
	export LD   :=	$(CXX)
endif

export OFILES   :=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) \
					$(sFILES:.s=.o) $(SFILES:.S=.o)

export INCLUDE  :=	$(foreach dir,$(INCLUDES), -I$(CURDIR)/$(dir)) \
					$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
					$(LIBPSL1GHT_INC) \
					-I$(CURDIR)/$(BUILD) -I$(PORTLIBS)/include

export LIBPATHS :=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib) \
					$(LIBPSL1GHT_LIB) -L$(PORTLIBS)/lib

.PHONY: $(BUILD) clean pkg all

all: pkg

$(BUILD):
	[ -d $@ ] || mkdir -p $@
	$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile V=1 VERBOSE=1

clean:
	echo clean ...
	rm -fr $(BUILD) $(OUTPUT).elf $(OUTPUT).self EBOOT.BIN

pkg: clean samples $(BUILD) $(OUTPUT).pkg

#---------------------------------------------------------------------------------
# Embedded test streams: sample_*.h264 -> source/sample_*.h
# The headers are committed; the rule only runs when a stream is newer.
#---------------------------------------------------------------------------------
SAMPLES     :=	sample_1080p sample_720p sample_1080p_red
SAMPLE_HDRS :=	$(addprefix source/,$(addsuffix .h,$(SAMPLES)))

.PHONY: samples
samples: $(SAMPLE_HDRS)

source/%.h: %.h264
	@echo gen $@ from $<
	@{ \
	  printf '/* generated from %s, %s bytes */\n#pragma once\n' '$<' "$$(wc -c < '$<')"; \
	  printf 'static const unsigned char %s[] = {\n' '$*'; \
	  od -An -v -tx1 '$<' | awk 'NF { s=""; for (i=1; i<=NF; i++) s = s (i>1 ? ", " : "") sprintf("0x%s", $$i); print "  " s "," }'; \
	  printf '};\nstatic const unsigned int %s_size = %s;\n' '$*' "$$(wc -c < '$<')"; \
	} > $@

else

DEPENDS :=	$(OFILES:.o=.d)

$(OUTPUT).self: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

endif
