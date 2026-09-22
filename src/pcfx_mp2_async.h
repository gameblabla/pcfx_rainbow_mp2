#ifndef PCFX_MP2_ASYNC_H
#define PCFX_MP2_ASYNC_H

#include <stdint.h>
#include "kjmp2_fast.h"

#define PCFX_MP2_PRELOAD_MAX_BYTES (192u * 1024u)
#define PCFX_MP2_STREAM_BUF_BYTES  (64u * 1024u)
#define PCFX_MP2_STREAM_CHUNK_MAX_SECTORS 8u
/* KRAM bounce window (page 0) for MP2 CD reads: libpcfx's eris_cd_read_dma()
   for the blocking loaders, the player's non-blocking DMA while streaming.
   It sits just above the player's 28-slot RAINBOW ring. */
#define PCFX_MP2_KRAM_WORD_ADDR    0x0001D000u
#define PCFX_MP2_KRAM_WORDS        (PCFX_MP2_STREAM_CHUNK_MAX_SECTORS * 1024u)

typedef struct PcfxMp2Async {
    kjmp2v_context_t ctx;
    const uint8_t *src;
    const uint8_t *pos;
    const uint8_t *end;
    uint32_t size;
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t bytes_loaded;
    uint32_t bytes_consumed;
    uint32_t frames_decoded;
    uint32_t samples_written;
    uint32_t last_frame_bytes;
    uint32_t max_ring_used;
    uint32_t error_code;
    uint32_t error_offset;
    uint8_t opened;
    uint8_t started;
    uint8_t done;
    uint8_t stream_mode;
    uint8_t input_eof;
    uint8_t timer_inited;
    /* Appended after the fields tools/extract_pcfv_stats.py reads by offset. */
    uint8_t need_sync;          /* stream began mid-frame: hunt for a header */
    uint32_t first_frame;       /* stream frame index of the first ring frame */
    uint32_t skip_to_frame;     /* drop whole frames before this index */
} PcfxMp2Async;

extern uint8_t g_pcfx_mp2_preload_buf[PCFX_MP2_PRELOAD_MAX_BYTES];

uint32_t pcfx_mp2_frame_size(const uint8_t *frame);
uint32_t pcfx_mp2_channels(const uint8_t *src);
uint32_t pcfx_mp2_timer_period_for_rate(uint32_t rate);
int pcfx_mp2_load_cd(uint32_t lba, uint32_t size_bytes);
int pcfx_mp2_async_open(PcfxMp2Async *s, const uint8_t *src, uint32_t size_bytes);
int pcfx_mp2_stream_begin(PcfxMp2Async *s, uint32_t total_size_bytes);
/* Begin at byte_offset of the stream (a chunk boundary, usually mid-frame).
   Bytes up to the next frame header are discarded and whole frames before
   skip_to_frame are dropped undecoded; MP2 Layer II frames carry no bit
   reservoir, so decoding can resume at any frame. */
int pcfx_mp2_stream_begin_at(PcfxMp2Async *s, uint32_t total_size_bytes,
                             uint32_t byte_offset, uint32_t skip_to_frame);
int pcfx_mp2_stream_read_cd(PcfxMp2Async *s, uint32_t lba, uint32_t byte_offset, uint32_t sectors);
int pcfx_mp2_stream_append_bytes(PcfxMp2Async *s, uint32_t byte_offset, const uint8_t *data, uint32_t bytes);
/* Zero-copy append: reserve room at the tail, fill it, then commit. */
uint8_t *pcfx_mp2_stream_tail(PcfxMp2Async *s, uint32_t room_bytes);
int pcfx_mp2_stream_commit(PcfxMp2Async *s, uint32_t byte_offset, uint32_t bytes);
uint32_t pcfx_mp2_stream_buffered_bytes(const PcfxMp2Async *s);
uint32_t pcfx_mp2_stream_free_bytes(PcfxMp2Async *s);
uint32_t pcfx_mp2_async_update(PcfxMp2Async *s, uint32_t frame_budget);
void pcfx_mp2_async_start(PcfxMp2Async *s);
void pcfx_mp2_async_stop(PcfxMp2Async *s);
int pcfx_mp2_async_started(const PcfxMp2Async *s);
int pcfx_mp2_async_done(const PcfxMp2Async *s);
uint32_t pcfx_mp2_async_underflows(void);
uint32_t pcfx_mp2_async_ring_used(void);
uint32_t pcfx_mp2_async_samples_emitted(void);
/* Stream position of the PSG read pointer, in samples from stream start. */
uint32_t pcfx_mp2_async_clock(const PcfxMp2Async *s);
int pcfx_mp2_async_playing(void);

#endif
