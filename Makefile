ROOT ?= .
PREFIX ?= v810
V810GCC ?= /opt/v810-gcc
PATH_PREFIX := $(V810GCC)/bin
CC := $(PATH_PREFIX)/$(PREFIX)-gcc
LD := $(PATH_PREFIX)/$(PREFIX)-ld
OBJCOPY := $(PATH_PREFIX)/$(PREFIX)-objcopy
HOSTCC ?= gcc
FFMPEG ?= ffmpeg
PCFX_CDLINK ?= $(ROOT)/tools/pcfxtools/pcfx-cdlink
PCFV_ENCODE ?= $(ROOT)/build/pcfv_encode

TARGET := pcfv_rainbow_mp2_player
OBJDIR := build
ELF := $(OBJDIR)/$(TARGET).elf
BIN := $(OBJDIR)/$(TARGET).program.bin
MAP := $(OBJDIR)/$(TARGET).map
CDLINK := $(OBJDIR)/cdlink.txt
AUDIO_INFO := $(OBJDIR)/generated_audio_info.h
VIDEO_IN ?= data/sailor.mkv
VIDEO_ONLY_STREAM := assets/video_only.pcfv
STREAM := assets/stream.pcfv
AUDIO_MP2 := assets/audio.mp2
MP2_LEAD_SECTORS ?= 4
MP2_CHUNK_SECTORS ?= 4
FRAMES ?= 0
FPS ?= 15
QUALITY ?= 82
RDO ?= 8
MAX_FRAME_SECTORS ?= 4
MIN_QUALITY ?= 58
JOBS ?= 0
LOOP ?= 0

INCLUDES := -I. -Isrc -I$(OBJDIR) -I$(ROOT)/include -I$(V810GCC)/include -I$(V810GCC)/$(PREFIX)/include
GCC_LIBDIR := $(firstword $(sort $(wildcard $(V810GCC)/lib/gcc/$(PREFIX)/*)))
CFLAGS_BASE := -O3 -fomit-frame-pointer -fno-builtin -ffunction-sections -fdata-sections -Wall -Wextra -std=gnu99 -mv810 -mprolog-function -msda=0 $(INCLUDES)
CFLAGS := $(CFLAGS_BASE) -DPCFX_PCFV_EXAMPLE_LOOP=$(LOOP) -DPCFX_PCFV_USE_MP2=1
ifneq ($(wildcard lbas.h),)
CFLAGS += -DHAVE_GENERATED_LBAS
endif
ifneq ($(wildcard $(AUDIO_INFO)),)
CFLAGS += -DHAVE_GENERATED_AUDIO_INFO
endif
LDFLAGS := -L$(V810GCC)/lib -L$(V810GCC)/$(PREFIX)/lib -L$(GCC_LIBDIR) $(V810GCC)/$(PREFIX)/lib/crt0.o --gc-sections
LIBS := -leris -lc -lsim -lnosys -lgcc

OBJS := $(OBJDIR)/main.o $(OBJDIR)/pcfx_pcfv_player.o $(OBJDIR)/pcfx_mp2_async.o $(OBJDIR)/kjmp2_fast.o $(OBJDIR)/psg_sample.o

.PHONY: all cd encode audio clean clean-build clean-output
all: cd

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(PCFV_ENCODE): $(ROOT)/tools/pcfv_encode.c
	$(MAKE) -C $(ROOT) tools

$(VIDEO_ONLY_STREAM): $(PCFV_ENCODE) $(VIDEO_IN) | $(OBJDIR)
	mkdir -p assets
	$(PCFV_ENCODE) --ffmpeg $(FFMPEG) --frames $(FRAMES) --fps $(FPS) \
		--quality $(QUALITY) --rdo $(RDO) --max-frame-sectors $(MAX_FRAME_SECTORS) \
		--min-quality $(MIN_QUALITY) --no-audio --tmp $(OBJDIR) \
		$(if $(filter-out 0,$(JOBS)),--jobs $(JOBS),) \
		$(VIDEO_IN) $@

$(STREAM): $(VIDEO_ONLY_STREAM) $(AUDIO_MP2) tools/mux_pcfv_mp2.py | $(OBJDIR)
	mkdir -p assets
	python3 tools/mux_pcfv_mp2.py --lead-sectors $(MP2_LEAD_SECTORS) --chunk-sectors $(MP2_CHUNK_SECTORS) $(VIDEO_ONLY_STREAM) $(AUDIO_MP2) $@

$(AUDIO_MP2): $(VIDEO_IN) | $(OBJDIR)
	mkdir -p assets
	$(FFMPEG) -y -hide_banner -i $(VIDEO_IN) -map 0:a:0 -vn -af volume=8dB -ac 1 -ar 16000 -b:a 32k -c:a mp2 $@

$(AUDIO_INFO): $(AUDIO_MP2) | $(OBJDIR)
	@sz=$$(stat -c%s $(AUDIO_MP2)); \
	{ echo '#ifndef GENERATED_AUDIO_INFO_H'; \
	  echo '#define GENERATED_AUDIO_INFO_H'; \
	  echo "#define PCFX_MP2_AUDIO_SIZE $${sz}u"; \
	  echo '#endif'; } > $@

encode: $(STREAM) $(AUDIO_MP2) $(AUDIO_INFO)
audio: $(AUDIO_MP2) $(AUDIO_INFO)

$(OBJDIR)/main.o: src/main.c $(wildcard lbas.h) $(AUDIO_INFO) | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/pcfx_pcfv_player.o: src/pcfx_pcfv_player.c src/pcfx_pcfv_player.h src/pcfx_mp2_async.h $(ROOT)/include/pcfv_format.h | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/pcfx_mp2_async.o: src/pcfx_mp2_async.c src/pcfx_mp2_async.h src/kjmp2_fast.h src/psg_sample.h | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/kjmp2_fast.o: src/kjmp2_fast.c src/kjmp2_fast.h | $(OBJDIR)
	$(CC) $(CFLAGS) -fno-unroll-loops -c $< -o $@

$(OBJDIR)/psg_sample.o: src/psg_sample.c src/psg_sample.h src/kjmp2_fast.h | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(ELF): $(OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@ -Map $(MAP)

$(BIN): $(ELF)
	$(OBJCOPY) -O binary $< $@

$(CDLINK): $(BIN) $(STREAM) | $(OBJDIR)
	@{ \
		echo "binary $(BIN)"; \
		echo "lbaheader lbas.h"; \
		echo "name RBOW MP2I"; \
		echo "maker ChatGPT"; \
		echo "makerid CGT"; \
		echo "country JP"; \
		echo "version 02"; \
		echo "date 20260622"; \
		echo "append $(STREAM)"; \
	} > $@

cd: clean-output $(STREAM) $(AUDIO_MP2) $(AUDIO_INFO) $(PCFX_CDLINK)
	$(MAKE) clean-build
	$(MAKE) $(BIN)
	$(MAKE) $(CDLINK)
	$(PCFX_CDLINK) $(CDLINK) $(TARGET)
	$(MAKE) clean-build
	$(MAKE) $(BIN)
	$(MAKE) $(CDLINK)
	$(PCFX_CDLINK) $(CDLINK) $(TARGET)
	$(MAKE) clean-build
	$(MAKE) $(BIN)
	$(MAKE) $(CDLINK)
	$(PCFX_CDLINK) $(CDLINK) $(TARGET)

clean-build:
	rm -f $(OBJDIR)/*.o $(ELF) $(BIN) $(MAP) $(CDLINK)

clean-output:
	rm -f $(TARGET).cue $(TARGET).bin lbas.h

clean:
	rm -rf $(OBJDIR) $(TARGET).cue $(TARGET).bin lbas.h $(STREAM) $(VIDEO_ONLY_STREAM) $(AUDIO_MP2)
