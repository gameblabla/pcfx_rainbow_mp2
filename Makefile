# Portable PCFV/MP2 player. Asset creation is explicit; cd accepts a ready stream.
#   make cd                                  build the disc from assets/stream.pcfv
#   make encode VIDEO_IN=movie.mkv           re-encode the stream (MPCONV-conformant)
#   make validate [VIDEO_IN=movie.mkv]       emulator gate: frames, audio, glitches
#   make test                                host tests for tools/rainbow
ROOT := $(abspath ..)
V810_GCC ?= $(if $(V810GCC),$(V810GCC),$(ROOT)/toolchain/v810-gcc)
V810GCC ?= $(V810_GCC)
LIBPCFX ?= $(ROOT)/vendor/libpcfx
CC := $(V810_GCC)/bin/v810-gcc
LD := $(V810_GCC)/bin/v810-ld
OBJCOPY := $(V810_GCC)/bin/v810-objcopy
PCFX_CDLINK ?= $(ROOT)/toolchain/bin/pcfx-cdlink-large
PYTHON ?= python3
FFMPEG ?= ffmpeg
TARGET := pcfv_rainbow_mp2_player
OBJDIR := build
STREAM ?= assets/stream.pcfv
VIDEO_IN ?=
FPS ?= 15
FRAMES ?= 0
SCALE ?= auto
JOBS ?= 4
AUDIO ?= mp2
# 10-bit PSG sample playback is quiet; the shipped asset was encoded with +8 dB.
AUDIO_GAIN_DB ?= 8
FIT ?= stretch
MAX_FRAME_SECTORS ?= 4
MAX_STRIP_BYTES ?= 0
LOOP ?= 0
CFLAGS := -O3 -fomit-frame-pointer -fno-builtin -ffunction-sections -fdata-sections \
          -Wall -Wextra -std=gnu99 -mv810 -mprolog-function -msda=0 \
          -Isrc -I$(OBJDIR) -I$(LIBPCFX)/include -I$(V810_GCC)/include \
          -DPCFX_PCFV_EXAMPLE_LOOP=$(LOOP) -DPCFX_PCFV_USE_MP2=1 -DHAVE_GENERATED_LBAS
GCC_LIBDIR := $(firstword $(sort $(wildcard $(V810_GCC)/lib/gcc/v810/*)))
LDFLAGS := -T$(LIBPCFX)/ldscripts/v810.x -L$(LIBPCFX) -L$(V810_GCC)/v810/lib \
           -L$(GCC_LIBDIR) $(LIBPCFX)/src/crt0.o --gc-sections
LIBS := -lpcfx -lc -lsim -lnosys -lgcc
OBJS := $(addprefix $(OBJDIR)/,main.o pcfx_pcfv_player.o pcfx_mp2_async.o kjmp2_fast.o psg_sample.o)

.PHONY: all cd program encode repair test validate clean FORCE
all: cd

# Phony: changes to quality, fps or input must regenerate the asset.
encode:
	@test -n "$(VIDEO_IN)" || { echo 'set VIDEO_IN=/path/to/movie'; exit 1; }
	$(PYTHON) $(ROOT)/tools/rainbow/rainbow.py video "$(VIDEO_IN)" "$(STREAM)" \
	  --ffmpeg "$(FFMPEG)" --fps $(FPS) --frames $(FRAMES) --scale $(SCALE) \
	  --jobs $(JOBS) --audio $(AUDIO) --audio-gain-db $(AUDIO_GAIN_DB) --fit $(FIT) \
	  --max-frame-sectors $(MAX_FRAME_SECTORS) --max-strip-bytes $(MAX_STRIP_BYTES)

repair:
	@test -n "$(LEGACY_STREAM)" || { echo 'set LEGACY_STREAM=/path/to/old.pcfv'; exit 1; }
	$(PYTHON) $(ROOT)/tools/rainbow/rainbow.py repair-legacy "$(LEGACY_STREAM)" "$(STREAM)"

$(OBJDIR):
	mkdir -p $@

$(OBJDIR)/%.o: src/%.c $(wildcard src/*.h) $(OBJDIR)/lbas.h FORCE | $(OBJDIR)
	$(CC) $(CFLAGS) $(if $(filter kjmp2_fast,$*),-fno-unroll-loops,) -c $< -o $@

$(OBJDIR)/$(TARGET).elf: $(OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@ -Map $(OBJDIR)/$(TARGET).map

$(OBJDIR)/$(TARGET).program.bin: $(OBJDIR)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@

program: $(OBJDIR)/$(TARGET).program.bin

cd:
	$(PYTHON) tools/build_disc.py --stream "$(STREAM)" --cdlink "$(PCFX_CDLINK)" --make "$(MAKE)"

test:
	$(PYTHON) $(ROOT)/tools/rainbow/test_rainbow.py

# Needs PCFX_BIOS_DIR.  Pass VIDEO_IN to add A/V, pitch and picture checks.
validate:
	$(PYTHON) tools/emu_validate.py --stream "$(STREAM)" $(if $(VIDEO_IN),--source "$(VIDEO_IN)",)

# Never delete source movies or encoded assets from clean.
clean:
	rm -f $(OBJS) $(OBJDIR)/$(TARGET).elf $(OBJDIR)/$(TARGET).map \
	  $(OBJDIR)/$(TARGET).program.bin $(OBJDIR)/cdlink.txt $(OBJDIR)/lbas.h \
	  $(TARGET).bin $(TARGET).cue
