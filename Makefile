# Portable PCFV RAINBOW player. Asset creation is explicit; cd accepts a ready stream.
#   make cd                                  build the disc from $(STREAM)
#   make encode VIDEO_IN=movie.mkv           re-encode the stream (MPCONV-conformant)
#   make validate [VIDEO_IN=movie.mkv]       emulator gate: frames, audio, glitches
#   make test                                host tests for tools/rainbow + ADPCM
# AUDIO selects the streamed-audio backend compiled into the player:
#   mp2    MP2 decoded on the V810, PSG output   (assets/stream.pcfv)
#   adpcm  KING ADPCM ring in KRAM, no CPU decode (assets/stream_adpcm.pcfv)
#   none   silent, video paced by fields         (assets/stream_silent.pcfv)
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
AUDIO ?= mp2
OBJDIR := build
PLAYER_OBJS := main.o pcfx_pcfv_player.o
ifeq ($(AUDIO),mp2)
STREAM ?= assets/stream.pcfv
AUDIO_DEFS := -DPCFX_PCFV_AUDIO_MP2=1
AUDIO_OBJS := pcfv_mp2_stream.o pcfx_mp2_async.o kjmp2_fast.o psg_sample.o
# 10-bit PSG sample playback is quiet; the shipped asset was encoded with +8 dB.
AUDIO_GAIN_DB ?= 8
else ifeq ($(AUDIO),adpcm)
STREAM ?= assets/stream_adpcm.pcfv
AUDIO_DEFS := -DPCFX_PCFV_AUDIO_ADPCM=1
AUDIO_OBJS := pcfv_adpcm_stream.o
# 4-bit ADPCM overshoots on transients; leave headroom below full scale.
AUDIO_GAIN_DB ?= -3
# 31468 (KING rate 0), 15734, 7867 or 3934 Hz.
ADPCM_RATE ?= 31468
else ifeq ($(AUDIO),none)
STREAM ?= assets/stream_silent.pcfv
AUDIO_DEFS :=
AUDIO_OBJS :=
AUDIO_GAIN_DB ?= 0
else
$(error AUDIO must be mp2, adpcm or none)
endif
TARGET := pcfv_rainbow_$(AUDIO)_player
VIDEO_IN ?=
FPS ?= 15
FRAMES ?= 0
SCALE ?= auto
JOBS ?= 4
FIT ?= stretch
MAX_FRAME_SECTORS ?= 4
MAX_STRIP_BYTES ?= 0
LOOP ?= 0
START_FRAME ?= 0
CFLAGS := -O3 -fomit-frame-pointer -fno-builtin -ffunction-sections -fdata-sections \
          -Wall -Wextra -std=gnu99 -mv810 -mprolog-function -msda=0 \
          -Isrc -I$(OBJDIR) -I$(LIBPCFX)/include -I$(V810_GCC)/include \
          -DPCFX_PCFV_EXAMPLE_LOOP=$(LOOP) \
          -DPCFX_PCFV_EXAMPLE_START_FRAME=$(START_FRAME) $(AUDIO_DEFS) -DHAVE_GENERATED_LBAS
GCC_LIBDIR := $(firstword $(sort $(wildcard $(V810_GCC)/lib/gcc/v810/*)))
LDFLAGS := -T$(LIBPCFX)/ldscripts/v810.x -L$(LIBPCFX) -L$(V810_GCC)/v810/lib \
           -L$(GCC_LIBDIR) $(LIBPCFX)/src/crt0.o --gc-sections
LIBS := -lpcfx -lc -lsim -lnosys -lgcc
OBJS := $(addprefix $(OBJDIR)/,$(PLAYER_OBJS) $(AUDIO_OBJS))

.PHONY: all cd program encode repair test validate clean FORCE
all: cd

# Phony: changes to quality, fps or input must regenerate the asset.
RAINBOW_VIDEO = $(PYTHON) $(ROOT)/tools/rainbow/rainbow.py video "$(VIDEO_IN)" $(1) \
	  --ffmpeg "$(FFMPEG)" --fps $(FPS) --frames $(FRAMES) --scale $(SCALE) \
	  --jobs $(JOBS) --fit $(FIT) \
	  --max-frame-sectors $(MAX_FRAME_SECTORS) --max-strip-bytes $(MAX_STRIP_BYTES)

encode:
	@test -n "$(VIDEO_IN)" || { echo 'set VIDEO_IN=/path/to/movie'; exit 1; }
ifeq ($(AUDIO),adpcm)
	@mkdir -p $(OBJDIR)
	$(call RAINBOW_VIDEO,"$(OBJDIR)/video_only.pcfv") --audio none
	$(PYTHON) tools/pcfv_adpcm.py mux "$(OBJDIR)/video_only.pcfv" "$(VIDEO_IN)" "$(STREAM)" \
	  --ffmpeg "$(FFMPEG)" --rate $(ADPCM_RATE) --gain-db $(AUDIO_GAIN_DB)
else
	$(call RAINBOW_VIDEO,"$(STREAM)") --audio $(AUDIO) --audio-gain-db $(AUDIO_GAIN_DB)
endif

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
	$(PYTHON) tools/build_disc.py --stream "$(STREAM)" --cdlink "$(PCFX_CDLINK)" \
	  --make "$(MAKE) AUDIO=$(AUDIO)" --audio $(AUDIO) --target $(TARGET)

test:
	$(PYTHON) $(ROOT)/tools/rainbow/test_rainbow.py
	$(PYTHON) tools/test_pcfv_adpcm.py

# Needs PCFX_BIOS_DIR.  Pass VIDEO_IN to add A/V, pitch and picture checks.
validate:
	$(PYTHON) tools/emu_validate.py --stream "$(STREAM)" --cue $(TARGET).cue \
	  --map $(OBJDIR)/$(TARGET).map --out validation/$(AUDIO) \
	  $(if $(VIDEO_IN),--source "$(VIDEO_IN)",)

# Never delete source movies or encoded assets from clean.
clean:
	rm -f $(addprefix $(OBJDIR)/,*.o) $(OBJDIR)/$(TARGET).elf $(OBJDIR)/$(TARGET).map \
	  $(OBJDIR)/$(TARGET).program.bin $(OBJDIR)/cdlink.txt $(OBJDIR)/lbas.h \
	  $(TARGET).bin $(TARGET).cue
