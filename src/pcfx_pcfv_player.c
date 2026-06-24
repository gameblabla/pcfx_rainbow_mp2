#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <eris/king.h>
#include <eris/tetsu.h>
#include <eris/7up.h>
#include <eris/low/7up.h>
#include <eris/low/soundbox.h>
#include <eris/low/scsi.h>
#include <eris/pad.h>
#include "pcfx_pcfv_player.h"
#if defined(PCFX_PCFV_USE_MP2) && PCFX_PCFV_USE_MP2
#include "pcfx_mp2_async.h"
#endif

#if defined(HAVE_GENERATED_LBAS)
#include "lbas.h"
#endif

#if defined(HAVE_GENERATED_AUDIO_INFO)
#include "generated_audio_info.h"
#endif

#ifndef BINARY_LBA_ASSETS_STREAM_PCFV
#define BINARY_LBA_ASSETS_STREAM_PCFV 0u
#endif

#define PCFV_MAX_FRAMES        4096u
#define PCFV_HEADER_READ_BYTES (((64u + (PCFV_MAX_FRAMES * 32u)) + 2047u) & ~2047u)
#define PCFV_MAX_AUDIO_CHUNKS  1024u
#define PCFV_SECTOR_SIZE       2048u
#define RAINBOW_WIDTH           256u
#define RAINBOW_HEIGHT          240u
#define RAINBOW_START_SCANLINE  6u
#define RAINBOW_RESTART_RASTER  248u

#define VIDEO_BUFFER_COUNT      28u
#define VIDEO_PREBUFFER_TARGET   20u
#define VIDEO_SKIP_SEARCH        2u
#define VIDEO_SYNC_SKIP_MAX      27u
#define VIDEO_BUFFER_FIRST_WORD 0x00001000u
#define VIDEO_BUFFER_STRIDE_MAX 0x00001000u /* 8 KiB byte spacing; max four 2048-byte sectors */
#define VIDEO_BUFFER_STRIDE_DEFAULT 0x00001000u
#define VIDEO_BATCH_MAX          8u
#define KRAM_ADPCM_WORD_ADDR    0x00020000u
#define KRAM_MP2_WORD_ADDR      0x0001D000u
#define MP2_CHUNK_TMP_BYTES     (PCFX_MP2_STREAM_CHUNK_MAX_SECTORS * PCFV_SECTOR_SIZE)
/* Header/index is read before video buffering starts, so it may reuse the
   low KRAM area that later becomes the RAINBOW read-ahead ring. */
#define KRAM_HEADER_WORD_ADDR   0x00001000u
#define ADPCM_HALF_BYTES        65536u
#define ADPCM_RING_BYTES        (ADPCM_HALF_BYTES * 2u)
#define ADPCM_HALF_WORDS        (ADPCM_HALF_BYTES / 2u)
#define ADPCM_RING_WORDS        (ADPCM_RING_BYTES / 2u)
#define SCSI_TIMEOUT_TICKS      0x00300000u
#define SCHEDULER_POLL_BUDGET   96u
#define AUDIO_START_AFTER_VISIBLE_FIELDS 1u

#define PCFX_KING_ADPCM_CH0_ENABLE 0x0001u
#define PCFV_FLAG_MP2_AUDIO 0x0002u
#define PCFV_AUDIO_CODEC_NONE  0u
#define PCFV_AUDIO_CODEC_ADPCM 1u
#define PCFV_AUDIO_CODEC_MP2   2u

static uint16_t pcfx_adpcm_rate_bits(uint32_t rate_hz) {
    if (rate_hz >= 24000u) return 0u; /* PC-FX ~31.47kHz */
    if (rate_hz >= 12000u) return 1u; /* PC-FX ~15.73kHz */
    if (rate_hz >= 6000u)  return 2u;
    return 3u;
}

static int pcfx_adpcm_rate_enum(uint32_t rate_hz) {
    if (rate_hz >= 24000u) return ADPCM_RATE_32000;
    if (rate_hz >= 12000u) return ADPCM_RATE_16000;
    if (rate_hz >= 6000u)  return ADPCM_RATE_8000;
    return ADPCM_RATE_4000;
}

static uint8_t g_pcfv_head[PCFV_HEADER_READ_BYTES] __attribute__((aligned(4)));

typedef struct {
    uint32_t video_sector;
    uint32_t video_size;
    uint32_t video_sectors;
    uint32_t video_crc32;
    uint32_t audio_sector;
    uint32_t audio_sectors;
    uint32_t audio_byte_offset;
    uint32_t flags;
} FrameEntry;

typedef struct {
    uint32_t sector;
    uint32_t sectors;
    uint32_t byte_offset;
    uint16_t frame_index;
} AudioChunk;

static int cd_dma_is_busy(void);
static uint16_t count_ready_video_buffers(void);
static void kram_read_bytes(uint32_t word_addr, uint8_t *dst, uint32_t bytes);
static FrameEntry g_entries[PCFV_MAX_FRAMES];
static AudioChunk g_audio_chunks[PCFV_MAX_AUDIO_CHUNKS];
static uint16_t g_frame_count;
static uint16_t g_fps_num;
static uint16_t g_fps_den;
static uint16_t g_audio_chunk_count;
static uint16_t g_next_audio_chunk;
static uint32_t g_data_start_sector;
static uint32_t g_audio_preroll_sectors;
static uint32_t g_audio_refill_sectors;
static uint32_t g_ring_half_bytes;
static uint32_t g_audio_bytes;
static uint32_t g_audio_rate_hz;
static uint16_t g_pcfv_flags;
static uint8_t g_audio_codec;

static inline void port_out_h(uint32_t port, uint16_t value) {
    __asm__ volatile ("out.h %0,0[%1]" :: "r"(value), "r"(port) : "memory");
}

static inline void port_out_w(uint32_t port, uint32_t value) {
    __asm__ volatile ("out.w %0,0[%1]" :: "r"(value), "r"(port) : "memory");
}

static inline uint16_t port_in_h(uint32_t port) {
    uint16_t value;
    __asm__ volatile ("in.h 0[%1],%0" : "=r"(value) : "r"(port) : "memory");
    return value;
}

static inline uint32_t port_in_w(uint32_t port) {
    uint32_t value;
    __asm__ volatile ("in.w 0[%1],%0" : "=r"(value) : "r"(port) : "memory");
    return value;
}

static inline void king_select(uint16_t reg) { port_out_h(0x600, reg); }
static inline void king_data_h(uint16_t value) { port_out_h(0x604, value); }

static void king_write_reg16(uint16_t reg, uint16_t value) {
    king_select(reg);
    king_data_h(value);
}

static void king_write_reg32(uint16_t reg, uint32_t value) {
    king_select(reg);
    port_out_w(0x604, value);
}

static uint16_t king_read_reg16(uint16_t reg) {
    king_select(reg);
    return port_in_h(0x604);
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline int vblank_active(void) {
    volatile uint16_t * const sr = (volatile uint16_t *)0x80000400u;
    return ((*sr & 0x0020u) != 0);
}

static void wait_vblank(void) {
    while (!vblank_active()) { }
    while (vblank_active()) { }
}

/* -------------------------------------------------------------------------
   Small non-blocking SCSI READ(10) + KING DMA state machine.

   This replaces liberis' eris_low_scsi_command(), eris_cd_read(),
   eris_cd_read_kram(), and eris_low_scsi_finish_dma() on the playback path.
   The only long phase is data transfer, and it is KING DMA-polled.  Command,
   status, and message phases are advanced one handshake at a time by
   cd_dma_poll_budget().
   ------------------------------------------------------------------------- */

typedef enum {
    SCSI_PHASE_BUS_FREE    = 0,
    SCSI_PHASE_SELECT      = 1,
    SCSI_PHASE_DATA_OUT    = 2,
    SCSI_PHASE_DATA_IN     = 3,
    SCSI_PHASE_COMMAND     = 4,
    SCSI_PHASE_STATUS      = 5,
    SCSI_PHASE_MESSAGE_OUT = 6,
    SCSI_PHASE_MESSAGE_IN  = 7,
    SCSI_PHASE_ILLEGAL     = 8
} ScsiPhase;

typedef enum {
    CD_DMA_IDLE = 0,
    CD_DMA_SELECT_INIT,
    CD_DMA_SELECT_DELAY0,
    CD_DMA_SELECT_DELAY1,
    CD_DMA_SELECT_DELAY2,
    CD_DMA_WAIT_SELECTED,
    CD_DMA_RELEASE_DELAY,
    CD_DMA_WAIT_COMMAND,
    CD_DMA_COMMAND_WAIT_REQ,
    CD_DMA_COMMAND_WAIT_DROP,
    CD_DMA_COMMAND_WAIT_RAISE,
    CD_DMA_WAIT_DATA_REQ,
    CD_DMA_ACTIVE,
    CD_DMA_FINISH_WAIT_REQ,
    CD_DMA_FINISH_WAIT_DROP,
    CD_DMA_FINISH_WAIT_NEXT,
    CD_DMA_DONE,
    CD_DMA_ERROR
} CdDmaState;

typedef enum {
    CD_REQ_NONE = 0,
    CD_REQ_HEADER,
    CD_REQ_AUDIO_PREROLL,
    CD_REQ_AUDIO_HALF0,
    CD_REQ_AUDIO_HALF1,
    CD_REQ_VIDEO,
    CD_REQ_MP2
} CdReqKind;

typedef struct {
    CdDmaState state;
    CdReqKind kind;
    uint32_t lba;
    uint32_t kram_word_addr;
    uint32_t bytes;
    uint32_t timeout;
    uint16_t delay;
    uint16_t video_buf;
    uint16_t video_count;
    uint8_t cdb[10];
    uint8_t cdb_len;
    uint8_t cdb_pos;
    uint8_t status_byte;
    uint8_t message_byte;
} CdDmaRequest;

static CdDmaRequest g_cd_dma;
static uint8_t g_header_ready;
static uint8_t g_audio_preroll_ready;
static uint8_t g_adpcm_started;
static uint8_t g_rainbow_visible;
static uint8_t g_pending_audio_half0;
static uint8_t g_pending_audio_half1;
static uint32_t g_scsi_dma_started;
static uint32_t g_scsi_dma_completed;
static uint32_t g_scsi_dma_errors;
static uint32_t g_scsi_dma_late_frames;
static uint32_t g_scsi_dma_video_underflows;
static uint32_t g_video_frames_presented;
static uint32_t g_video_frames_skipped;
static uint32_t g_video_frames_held;
static uint32_t g_video_frames_dropped_stale;
static uint32_t g_video_ready_highwater;
static uint32_t g_vblank_latched_frames;
static uint32_t g_midfield_latched_frames;
static uint32_t g_video_buffer_stride_words;
static uint32_t g_audio_refills_completed;
static uint32_t g_audio_start_latch_frame;
static uint32_t g_audio_start_visible_fields;
static uint32_t g_audio_start_ring_used;
static uint32_t g_audio_start_samples_emitted;
static uint32_t g_mp2_preroll_ring_at_exit;
static uint32_t g_mp2_preroll_guard_count;
static uint32_t g_stream_lba;
static uint16_t g_stop_buttons;
static uint16_t g_pause_buttons;
static uint8_t g_loop_playback;
static uint8_t g_done;
static uint8_t g_abort;
static uint8_t g_paused;
static uint8_t g_seek_mode;
static uint8_t g_seek_in_progress;
static uint16_t g_start_frame;
static uint32_t g_seek_sector;
static uint32_t g_audio_preroll_src_sector;
static uint32_t g_audio_preroll_run_sectors;
static uint16_t g_adpcm_control_word;
static uint32_t g_prev_pad;
static uint16_t g_fields_per_frame;
static uint16_t g_field_counter;
static uint32_t g_rainbow_visible_fields;
static uint32_t g_mp2_sync_pauses;
static uint32_t g_mp2_sync_resumes;

#if defined(PCFX_PCFV_USE_MP2) && PCFX_PCFV_USE_MP2
static PcfxMp2Async g_mp2_async;
static uint8_t g_mp2_chunk_tmp[MP2_CHUNK_TMP_BYTES] __attribute__((aligned(4)));
static uint32_t g_mp2_pending_byte_offset;
static uint32_t g_mp2_pending_sectors;
static uint8_t g_mp2_loaded;
static uint8_t g_mp2_enabled;
static uint32_t g_mp2_size_bytes;
static uint8_t g_mp2_decode_budget_used;
#define PCFV_MP2_PREFILL_FRAMES 24u
#define PCFV_MP2_FIELD_BUDGET   1u
#define PCFV_MP2_PREFETCH_LOW_BYTES (24u * 1024u)
#define PCFV_MP2_START_RING_SAMPLES (24u * 1152u)
#define PCFV_MP2_DECODE_LOW_SAMPLES (16u * 1152u)
#define PCFV_MP2_DECODE_HIGH_SAMPLES (24u * 1152u)
#define PCFV_MP2_URGENT_RING_SAMPLES (8u * 1152u)
#define PCFV_MP2_CRITICAL_RING_SAMPLES (4u * 1152u)
#define PCFV_MP2_URGENT_BYTES 8192u
#define PCFV_MP2_SYNC_LEAD_FRAMES 4u
#define PCFV_MP2_SYNC_RESUME_FRAMES 2u
static int pcfv_mp2_fetch_next_chunk(void);
static void pcfv_mp2_decode_budget(uint32_t frames) {
    uint32_t used;
    if (!g_mp2_enabled) return;
    if (g_mp2_decode_budget_used) return;
    g_mp2_decode_budget_used = 1u;
    used = pcfx_mp2_async_ring_used();
    if (!pcfx_mp2_async_started(&g_mp2_async)) {
        if (used < PCFV_MP2_START_RING_SAMPLES)
            (void)pcfx_mp2_async_update(&g_mp2_async, frames);
        return;
    }
    if (used < PCFV_MP2_DECODE_LOW_SAMPLES) {
        (void)pcfx_mp2_async_update(&g_mp2_async, frames);
    }
}

static int pcfv_mp2_should_fetch_urgent(void) {
    if (g_audio_codec != PCFV_AUDIO_CODEC_MP2 || !g_mp2_enabled) return 0;
    if (g_next_audio_chunk >= g_audio_chunk_count) return 0;
    if (!pcfx_mp2_async_started(&g_mp2_async)) {
        uint16_t target = VIDEO_PREBUFFER_TARGET;
        if (target > VIDEO_BUFFER_COUNT - 2u) target = VIDEO_BUFFER_COUNT - 2u;
        if (!g_video_frames_presented && count_ready_video_buffers() < target) return 0;
        return pcfx_mp2_async_ring_used() < PCFV_MP2_START_RING_SAMPLES;
    }
    if (pcfx_mp2_async_ring_used() < PCFV_MP2_CRITICAL_RING_SAMPLES) return 1;
    if (pcfx_mp2_async_ring_used() < PCFV_MP2_URGENT_RING_SAMPLES &&
        pcfx_mp2_stream_buffered_bytes(&g_mp2_async) < PCFV_MP2_URGENT_BYTES) return 1;
    return 0;
}
static void pcfv_mp2_prepare_loaded(void) {
    if (g_audio_codec == PCFV_AUDIO_CODEC_MP2) {
        if (!g_mp2_enabled && g_audio_bytes) {
            if (pcfx_mp2_stream_begin(&g_mp2_async, g_audio_bytes)) g_mp2_enabled = 1u;
        }
        return;
    }
    if (!g_mp2_loaded || g_mp2_enabled || !g_mp2_size_bytes) return;
    if (pcfx_mp2_async_open(&g_mp2_async, g_pcfx_mp2_preload_buf, g_mp2_size_bytes)) {
        g_mp2_enabled = 1u;
        (void)pcfx_mp2_async_update(&g_mp2_async, PCFV_MP2_PREFILL_FRAMES);
    } else {
        g_mp2_loaded = 0u;
        g_mp2_enabled = 0u;
    }
}
static uint32_t pcfv_mp2_sync_limit_samples(uint32_t lead_frames) {
    uint64_t n;
    uint32_t frames;
    if (!g_fps_num || !g_audio_rate_hz) return 0xffffffffu;
    frames = g_video_frames_presented + lead_frames;
    n = (uint64_t)frames * (uint64_t)g_audio_rate_hz * (uint64_t)g_fps_den;
    n /= (uint64_t)g_fps_num;
    if (n > 0xffffffffull) return 0xffffffffu;
    return (uint32_t)n;
}

static int pcfv_mp2_audio_within_video_window(uint32_t lead_frames) {
    if (!g_video_frames_presented) return 0;
    return pcfx_mp2_async_samples_emitted() < pcfv_mp2_sync_limit_samples(lead_frames);
}

static void pcfv_mp2_sync_to_video(void) {
    /* Do not stop the PSG timer for A/V sync: timer stop/resume creates audible
       discontinuities even when the ring never underflows.  Keep audio
       continuous; video presentation is buffered and may skip a stale frame if
       it ever falls behind.  Retain this counter as a diagnostic lead meter. */
    if (!g_mp2_enabled || !pcfx_mp2_async_started(&g_mp2_async)) return;
    if (pcfx_mp2_async_playing() && !pcfv_mp2_audio_within_video_window(PCFV_MP2_SYNC_LEAD_FRAMES)) {
        g_mp2_sync_pauses++;
    }
}

static void pcfv_mp2_start_if_ready(void) {
    uint32_t threshold = pcfx_mp2_async_started(&g_mp2_async) ?
        PCFV_MP2_URGENT_RING_SAMPLES : PCFV_MP2_START_RING_SAMPLES;
    if (!g_mp2_enabled) return;
    if (pcfx_mp2_async_ring_used() >= threshold) {
        uint8_t was_playing = (uint8_t)pcfx_mp2_async_playing();
        pcfx_mp2_async_start(&g_mp2_async);
        if (!was_playing && pcfx_mp2_async_playing()) g_mp2_sync_resumes++;
    }
}
#else
#define pcfv_mp2_decode_budget(frames) ((void)0)
#define pcfv_mp2_start_if_ready() ((void)0)
#define pcfv_mp2_sync_to_video() ((void)0)
#endif

static uint8_t g_video_state[VIDEO_BUFFER_COUNT]; /* 0 free, 1 loading, 2 ready, 3 displaying */
static uint8_t g_video_ready_delay[VIDEO_BUFFER_COUNT];
static uint16_t g_video_frame[VIDEO_BUFFER_COUNT];
static uint32_t g_video_kram[VIDEO_BUFFER_COUNT];
static uint16_t g_next_load_frame;
static uint16_t g_next_display_frame;
static uint16_t g_current_display_buf;
static uint8_t g_have_display_buf;

static inline void scsi_reg_w(uint16_t reg, uint16_t value) {
    port_out_h(0x600, reg);
    port_out_h(0x604, value);
}

static inline uint16_t scsi_reg_r(uint16_t reg) {
    port_out_h(0x600, reg);
    return port_in_h(0x604);
}

static ScsiPhase scsi_phase_now(void) {
    uint16_t sr = port_in_h(0x602);
    uint16_t bits;

    if ((sr & 0x0040u) == 0) return SCSI_PHASE_BUS_FREE;
    if (sr & 0x0002u) return SCSI_PHASE_SELECT;

    bits = sr & 0x001Cu;
    if (bits == 0x0000u) return SCSI_PHASE_DATA_OUT;
    if (bits == 0x0004u) return SCSI_PHASE_DATA_IN;
    if (bits == 0x0008u) return SCSI_PHASE_COMMAND;
    if (bits == 0x000Cu) return SCSI_PHASE_STATUS;
    if (bits == 0x0018u) return SCSI_PHASE_MESSAGE_OUT;
    if (bits == 0x001Cu) return SCSI_PHASE_MESSAGE_IN;
    return SCSI_PHASE_ILLEGAL;
}

static inline int scsi_req_now(void) {
    return (port_in_h(0x602) & 0x0020u) != 0;
}

static void scsi_make_read10(uint8_t cdb[10], uint32_t lba, uint32_t bytes) {
    uint32_t sectors = (bytes + (PCFV_SECTOR_SIZE - 1u)) / PCFV_SECTOR_SIZE;
    cdb[0] = 0x28;
    cdb[1] = 0x00;
    cdb[2] = (uint8_t)(lba >> 24);
    cdb[3] = (uint8_t)(lba >> 16);
    cdb[4] = (uint8_t)(lba >> 8);
    cdb[5] = (uint8_t)(lba);
    cdb[6] = 0x00;
    cdb[7] = (uint8_t)(sectors >> 8);
    cdb[8] = (uint8_t)(sectors);
    cdb[9] = 0x00;
}

static void scsi_begin_king_dma(uint32_t kram_word_addr, uint32_t bytes) {
    scsi_reg_w(0x03, 0x0001);
    king_write_reg32(0x09, kram_word_addr);
    king_write_reg32(0x0A, bytes);
    scsi_reg_w(0x02, 0x0002);
    king_write_reg16(0x0B, 0x0001);
    scsi_reg_w(0x07, 0x0001);
}

static int scsi_check_king_dma(void) {
    if (scsi_reg_r(0x0B) & 0x0001u) return 1;
    king_select(0x0A);
    return port_in_w(0x604) != 0;
}

static void scsi_stop_king_dma_no_eat(void) {
    scsi_reg_w(0x02, 0x0000);
    scsi_reg_w(0x03, 0x0000);
}

static void scsi_ack_assert(uint16_t ack_value) {
    scsi_reg_w(0x01, ack_value);
}

static void scsi_bus_abort_reset(void) {
    eris_low_scsi_abort();
    eris_low_scsi_reset();
}

static int cd_dma_is_busy(void) {
    return g_cd_dma.state != CD_DMA_IDLE &&
           g_cd_dma.state != CD_DMA_DONE &&
           g_cd_dma.state != CD_DMA_ERROR;
}

static void cd_dma_complete_current(void) {
    if (g_cd_dma.kind == CD_REQ_HEADER) {
        g_header_ready = 1;
    } else if (g_cd_dma.kind == CD_REQ_AUDIO_PREROLL) {
        g_audio_preroll_ready = 1;
    } else if (g_cd_dma.kind == CD_REQ_AUDIO_HALF0 || g_cd_dma.kind == CD_REQ_AUDIO_HALF1) {
        g_audio_refills_completed++;
    } else if (g_cd_dma.kind == CD_REQ_VIDEO && g_cd_dma.video_buf < VIDEO_BUFFER_COUNT) {
        uint16_t n = g_cd_dma.video_count ? g_cd_dma.video_count : 1u;
        uint16_t k;
        for (k = 0; k < n && (uint16_t)(g_cd_dma.video_buf + k) < VIDEO_BUFFER_COUNT; ++k) {
            g_video_state[g_cd_dma.video_buf + k] = 2;
            g_video_ready_delay[g_cd_dma.video_buf + k] = 2;
        }
    } else if (g_cd_dma.kind == CD_REQ_MP2) {
#if defined(PCFX_PCFV_USE_MP2) && PCFX_PCFV_USE_MP2
        uint32_t bytes = g_mp2_pending_sectors * PCFV_SECTOR_SIZE;
        if (g_mp2_pending_byte_offset + bytes > g_audio_bytes) bytes = g_audio_bytes - g_mp2_pending_byte_offset;
        kram_read_bytes(KRAM_MP2_WORD_ADDR, g_mp2_chunk_tmp, g_mp2_pending_sectors * PCFV_SECTOR_SIZE);
        (void)pcfx_mp2_stream_append_bytes(&g_mp2_async, g_mp2_pending_byte_offset, g_mp2_chunk_tmp, bytes);
        if (g_next_audio_chunk < g_audio_chunk_count) g_next_audio_chunk++;
        g_mp2_pending_byte_offset = 0;
        g_mp2_pending_sectors = 0;
#endif
    }
    g_cd_dma.kind = CD_REQ_NONE;
    g_cd_dma.state = CD_DMA_DONE;
    g_scsi_dma_completed++;
}

static void cd_dma_fail_current(void) {
    if (g_cd_dma.kind == CD_REQ_VIDEO && g_cd_dma.video_buf < VIDEO_BUFFER_COUNT) {
        uint16_t n = g_cd_dma.video_count ? g_cd_dma.video_count : 1u;
        uint16_t k;
        for (k = 0; k < n && (uint16_t)(g_cd_dma.video_buf + k) < VIDEO_BUFFER_COUNT; ++k) {
            g_video_state[g_cd_dma.video_buf + k] = 0;
            g_video_ready_delay[g_cd_dma.video_buf + k] = 0;
            g_video_frame[g_cd_dma.video_buf + k] = 0xffffu;
        }
    } else if (g_cd_dma.kind == CD_REQ_MP2) {
        g_mp2_pending_byte_offset = 0;
        g_mp2_pending_sectors = 0;
    } else if (g_cd_dma.kind == CD_REQ_AUDIO_HALF0) {
        g_pending_audio_half0 = 1;
    } else if (g_cd_dma.kind == CD_REQ_AUDIO_HALF1) {
        g_pending_audio_half1 = 1;
    }
    g_cd_dma.kind = CD_REQ_NONE;
    g_cd_dma.state = CD_DMA_ERROR;
    g_scsi_dma_errors++;
    scsi_bus_abort_reset();
}

static int cd_dma_start(CdReqKind kind, uint32_t lba, uint32_t kram_word_addr, uint32_t bytes, uint16_t video_buf) {
    if (cd_dma_is_busy()) return 0;
    if (!bytes) return 0;

    bytes = (bytes + (PCFV_SECTOR_SIZE - 1u)) & ~(PCFV_SECTOR_SIZE - 1u);
    memset(&g_cd_dma, 0, sizeof(g_cd_dma));
    g_cd_dma.kind = kind;
    g_cd_dma.lba = lba;
    g_cd_dma.kram_word_addr = kram_word_addr;
    g_cd_dma.bytes = bytes;
    g_cd_dma.video_buf = video_buf;
    g_cd_dma.video_count = 1u;
    g_cd_dma.cdb_len = 10;
    scsi_make_read10(g_cd_dma.cdb, lba, bytes);

    g_cd_dma.state = CD_DMA_SELECT_INIT;
    g_scsi_dma_started++;
    return 1;
}

static void cd_dma_poll_one(void) {
    ScsiPhase ph;

    if (!cd_dma_is_busy()) return;
    if (++g_cd_dma.timeout > SCSI_TIMEOUT_TICKS) {
        cd_dma_fail_current();
        return;
    }

    switch (g_cd_dma.state) {
    case CD_DMA_SELECT_INIT:
        scsi_reg_w(0x03, 0x0000);
        scsi_reg_w(0x00, 0x0084);
        g_cd_dma.delay = 2;
        g_cd_dma.state = CD_DMA_SELECT_DELAY0;
        break;

    case CD_DMA_SELECT_DELAY0:
        if (g_cd_dma.delay) { g_cd_dma.delay--; break; }
        scsi_reg_w(0x01, 0x0001);        /* assert BSY */
        g_cd_dma.delay = 2;
        g_cd_dma.state = CD_DMA_SELECT_DELAY1;
        break;

    case CD_DMA_SELECT_DELAY1:
        if (g_cd_dma.delay) { g_cd_dma.delay--; break; }
        scsi_reg_w(0x01, 0x0005);        /* assert BSY + SEL */
        g_cd_dma.delay = 2;
        g_cd_dma.state = CD_DMA_SELECT_DELAY2;
        break;

    case CD_DMA_SELECT_DELAY2:
        if (g_cd_dma.delay) { g_cd_dma.delay--; break; }
        g_cd_dma.state = CD_DMA_WAIT_SELECTED;
        break;

    case CD_DMA_WAIT_SELECTED:
        if (scsi_phase_now() != SCSI_PHASE_BUS_FREE) {
            g_cd_dma.delay = 1;
            g_cd_dma.state = CD_DMA_RELEASE_DELAY;
        }
        break;

    case CD_DMA_RELEASE_DELAY:
        if (g_cd_dma.delay) { g_cd_dma.delay--; break; }
        scsi_reg_w(0x01, 0x0000);        /* release select */
        g_cd_dma.delay = 1;
        g_cd_dma.state = CD_DMA_WAIT_COMMAND;
        break;

    case CD_DMA_WAIT_COMMAND:
        if (g_cd_dma.delay) { g_cd_dma.delay--; break; }
        if (scsi_phase_now() == SCSI_PHASE_COMMAND) {
            scsi_reg_w(0x03, 0x0002);
            g_cd_dma.state = CD_DMA_COMMAND_WAIT_REQ;
        }
        break;

    case CD_DMA_COMMAND_WAIT_REQ:
        ph = scsi_phase_now();
        if (ph != SCSI_PHASE_COMMAND) {
            scsi_reg_w(0x01, 0x0000);
            scsi_reg_w(0x03, 0x0000);
            g_cd_dma.state = CD_DMA_WAIT_DATA_REQ;
            break;
        }
        if (scsi_req_now()) {
            uint8_t b = 0;
            if (g_cd_dma.cdb_pos < g_cd_dma.cdb_len) b = g_cd_dma.cdb[g_cd_dma.cdb_pos++];
            scsi_reg_w(0x00, b);
            scsi_reg_w(0x01, 0x0001);
            scsi_reg_w(0x01, 0x0011);
            g_cd_dma.state = CD_DMA_COMMAND_WAIT_DROP;
        }
        break;

    case CD_DMA_COMMAND_WAIT_DROP:
        ph = scsi_phase_now();
        if (ph != SCSI_PHASE_COMMAND) {
            scsi_reg_w(0x01, 0x0000);
            scsi_reg_w(0x03, 0x0000);
            g_cd_dma.state = CD_DMA_WAIT_DATA_REQ;
            break;
        }
        if (!scsi_req_now()) {
            scsi_reg_w(0x01, 0x0001);
            g_cd_dma.state = CD_DMA_COMMAND_WAIT_RAISE;
        }
        break;

    case CD_DMA_COMMAND_WAIT_RAISE:
        ph = scsi_phase_now();
        if (ph != SCSI_PHASE_COMMAND) {
            scsi_reg_w(0x01, 0x0000);
            scsi_reg_w(0x03, 0x0000);
            g_cd_dma.state = CD_DMA_WAIT_DATA_REQ;
            break;
        }
        if (scsi_req_now()) g_cd_dma.state = CD_DMA_COMMAND_WAIT_REQ;
        break;

    case CD_DMA_WAIT_DATA_REQ:
        ph = scsi_phase_now();
        if (ph == SCSI_PHASE_DATA_IN && scsi_req_now()) {
            scsi_begin_king_dma(g_cd_dma.kram_word_addr, g_cd_dma.bytes);
            g_cd_dma.timeout = 0;
            g_cd_dma.state = CD_DMA_ACTIVE;
        } else if (ph == SCSI_PHASE_STATUS || ph == SCSI_PHASE_MESSAGE_IN || ph == SCSI_PHASE_BUS_FREE) {
            g_cd_dma.state = CD_DMA_FINISH_WAIT_REQ;
        }
        break;

    case CD_DMA_ACTIVE:
        if (!scsi_check_king_dma()) {
            scsi_stop_king_dma_no_eat();
            g_cd_dma.timeout = 0;
            g_cd_dma.state = CD_DMA_FINISH_WAIT_REQ;
        }
        break;

    case CD_DMA_FINISH_WAIT_REQ:
        ph = scsi_phase_now();
        if (ph == SCSI_PHASE_BUS_FREE) {
            cd_dma_complete_current();
            break;
        }
        if (!scsi_req_now()) break;

        if (ph == SCSI_PHASE_STATUS) {
            g_cd_dma.status_byte = (uint8_t)scsi_reg_r(0x00);
            scsi_ack_assert(0x0010);
            g_cd_dma.state = CD_DMA_FINISH_WAIT_DROP;
        } else if (ph == SCSI_PHASE_MESSAGE_IN) {
            g_cd_dma.message_byte = (uint8_t)scsi_reg_r(0x00);
            scsi_ack_assert(0x0010);
            g_cd_dma.state = CD_DMA_FINISH_WAIT_DROP;
        } else if (ph == SCSI_PHASE_DATA_IN) {
            (void)scsi_reg_r(0x00);      /* eat stray input byte if the target supplies one */
            scsi_ack_assert(0x0010);
            g_cd_dma.state = CD_DMA_FINISH_WAIT_DROP;
        } else {
            cd_dma_fail_current();
        }
        break;

    case CD_DMA_FINISH_WAIT_DROP:
        if (!scsi_req_now()) {
            scsi_ack_assert(0x0000);
            g_cd_dma.state = CD_DMA_FINISH_WAIT_NEXT;
        }
        break;

    case CD_DMA_FINISH_WAIT_NEXT:
        ph = scsi_phase_now();
        if (ph == SCSI_PHASE_BUS_FREE) {
            if (g_cd_dma.status_byte == 0x00 || g_cd_dma.status_byte == 0x10) cd_dma_complete_current();
            else cd_dma_fail_current();
        } else if (scsi_req_now()) {
            g_cd_dma.state = CD_DMA_FINISH_WAIT_REQ;
        }
        break;

    default:
        break;
    }
}

static void cd_dma_poll_budget(uint16_t budget) {
    while (budget--) {
        CdDmaState old_state = g_cd_dma.state;
        uint8_t old_pos = g_cd_dma.cdb_pos;
        cd_dma_poll_one();
        if (!cd_dma_is_busy()) break;
        if (g_cd_dma.state == old_state && g_cd_dma.cdb_pos == old_pos) {
            /* Avoid monopolizing a whole field while waiting for REQ/phase. */
            break;
        }
    }
}

/* ------------------------------------------------------------------------- */

static void setup_rainbow_regs(void) {
    port_out_h(0x200, 0x0000);
    port_out_h(0x202, 0x0000);
    port_out_h(0x208, 0xff80);
    port_out_h(0x20c, 0x0000);
    port_out_h(0x210, 0x0000);
    port_out_h(0x214, 0x0000);
    port_out_h(0x204, 0x0001);
}

static void setup_video(void) {
    eris_king_init();
    eris_tetsu_init();

    eris_king_set_kram_pages(0, 0, 0, 0);
    eris_king_set_bg_prio(KING_BGPRIO_HIDE, KING_BGPRIO_HIDE, KING_BGPRIO_HIDE, KING_BGPRIO_HIDE, 0);
    eris_king_set_bg_mode(KING_BGMODE_NONE, KING_BGMODE_NONE, KING_BGMODE_NONE, KING_BGMODE_NONE);

    eris_tetsu_set_7up_palette(0, 0);
    eris_tetsu_set_king_palette(0, 0, 0, 0);
    eris_tetsu_set_rainbow_palette(0);
    eris_tetsu_set_priorities(0, 0, 0, 0, 0, 0, 0);
    g_rainbow_visible = 0;
    /* Keep RAINBOW fetch enabled from boot, but with output priority zero.
       That lets the HuC6271 pipeline warm without showing the layer. */
    eris_tetsu_set_video_mode(TETSU_LINES_262, 0, TETSU_DOTCLOCK_5MHz,
                              TETSU_COLORS_256, TETSU_COLORS_16,
                              0, 0, 0, 0, 0, 0, 1);

    eris_low_sup_set_control(0, 0, 1, 0);
    eris_low_sup_set_control(1, 0, 1, 0);
}

static void set_rainbow_visible(uint8_t visible) {
    eris_tetsu_set_priorities(0, 0, 0, 0, 0, 0, visible ? 7 : 0);
    /* Keep the RAINBOW video-mode bit enabled even when hidden.  Visibility is
       controlled by priority only, so hidden warm-up fields still exercise the
       RAINBOW scanout path instead of creating a first-visible pipeline fill. */
    eris_tetsu_set_video_mode(TETSU_LINES_262, 0, TETSU_DOTCLOCK_5MHz,
                              TETSU_COLORS_256, TETSU_COLORS_16,
                              0, 0, 0, 0, 0, 0, 1);
    g_rainbow_visible = visible ? 1 : 0;
}

static void start_rainbow_frame(uint32_t kram_word_addr, uint16_t block_count) {
    int irq_state = irq_disable();
    king_write_reg16(0x40, 0x0000);
    king_write_reg32(0x41, kram_word_addr);
    king_write_reg16(0x42, RAINBOW_START_SCANLINE);
    king_write_reg16(0x43, block_count);
    king_write_reg16(0x44, 0x0000);
    king_write_reg16(0x40, 0x0001);
    irq_restore(irq_state);
}

static void kram_read_bytes(uint32_t word_addr, uint8_t *dst, uint32_t bytes) {
    uint32_t words = (bytes + 1u) >> 1;
    uint32_t i;
    king_write_reg32(0x0C, (word_addr & 0x0003FFFFu) | (1u << 18));
    for (i = 0; i < words; ++i) {
        uint16_t v = king_read_reg16(0x0E);
        dst[i * 2u + 0u] = (uint8_t)v;
        if ((i * 2u + 1u) < bytes) dst[i * 2u + 1u] = (uint8_t)(v >> 8);
    }
}

static int parse_pcfv_header(void) {
    uint32_t index_bytes;
    kram_read_bytes(KRAM_HEADER_WORD_ADDR, g_pcfv_head, PCFV_HEADER_READ_BYTES);

    if (memcmp(g_pcfv_head, "PCFV0001", 8) != 0) return 0;
    if (rd16(g_pcfv_head + 8) != RAINBOW_WIDTH || rd16(g_pcfv_head + 10) != RAINBOW_HEIGHT) return 0;

    g_fps_num = rd16(g_pcfv_head + 12);
    g_fps_den = rd16(g_pcfv_head + 14);
    g_frame_count = rd16(g_pcfv_head + 16);
    g_pcfv_flags = rd16(g_pcfv_head + 18);
    if (g_frame_count == 0 || g_frame_count > PCFV_MAX_FRAMES) return 0;

    index_bytes = rd32(g_pcfv_head + 20);
    g_data_start_sector = rd32(g_pcfv_head + 24);
    g_audio_bytes = rd32(g_pcfv_head + 32);
    g_audio_preroll_sectors = rd32(g_pcfv_head + 40);
    g_audio_refill_sectors = rd32(g_pcfv_head + 44);
    g_ring_half_bytes = rd32(g_pcfv_head + 48);
    g_audio_rate_hz = rd32(g_pcfv_head + 52);
    if (!g_audio_rate_hz) g_audio_rate_hz = 31468u;
    if ((g_pcfv_flags & PCFV_FLAG_MP2_AUDIO) && g_audio_bytes) {
        g_audio_codec = PCFV_AUDIO_CODEC_MP2;
    } else if (g_audio_bytes) {
        g_audio_codec = PCFV_AUDIO_CODEC_ADPCM;
    } else {
        g_audio_codec = PCFV_AUDIO_CODEC_NONE;
    }
    if (g_audio_codec == PCFV_AUDIO_CODEC_ADPCM && g_ring_half_bytes != ADPCM_HALF_BYTES) return 0;
    g_video_buffer_stride_words = (rd32(g_pcfv_head + 28) * PCFV_SECTOR_SIZE + 1u) >> 1;
    if (!g_video_buffer_stride_words || g_video_buffer_stride_words > VIDEO_BUFFER_STRIDE_MAX) return 0;
    if (index_bytes != (uint32_t)g_frame_count * 32u) return 0;
    if ((64u + index_bytes) > PCFV_HEADER_READ_BYTES) return 0;

    g_audio_chunk_count = 0;
    {
        uint16_t i;
        for (i = 0; i < g_frame_count; ++i) {
            const uint8_t *e = g_pcfv_head + 64u + ((uint32_t)i * 32u);
            g_entries[i].video_sector = rd32(e + 0);
            g_entries[i].video_size = rd32(e + 4);
            g_entries[i].video_sectors = rd32(e + 8);
            g_entries[i].video_crc32 = rd32(e + 12);
            g_entries[i].audio_sector = rd32(e + 16);
            g_entries[i].audio_sectors = rd32(e + 20);
            g_entries[i].audio_byte_offset = rd32(e + 24);
            g_entries[i].flags = rd32(e + 28);
            if (!g_entries[i].video_sectors) return 0;
            if (g_entries[i].audio_sectors && g_audio_chunk_count < PCFV_MAX_AUDIO_CHUNKS) {
                g_audio_chunks[g_audio_chunk_count].sector = g_entries[i].audio_sector;
                g_audio_chunks[g_audio_chunk_count].sectors = g_entries[i].audio_sectors;
                g_audio_chunks[g_audio_chunk_count].byte_offset = g_entries[i].audio_byte_offset;
                g_audio_chunks[g_audio_chunk_count].frame_index = i;
                g_audio_chunk_count++;
            }
        }
    }
    return 1;
}


static uint16_t pcfv_find_frame_for_asset_sector(uint32_t asset_sector) {
    uint16_t i;
    if (!g_frame_count) return 0;

    /* A sector before the first video frame maps to frame 0. */
    for (i = 0; i < g_frame_count; ++i) {
        if (g_entries[i].video_sector >= asset_sector) return i;
        if (asset_sector >= g_entries[i].video_sector &&
            asset_sector < (g_entries[i].video_sector + g_entries[i].video_sectors)) return i;
    }
    return (uint16_t)(g_frame_count - 1u);
}

static uint16_t pcfv_resolve_start_frame(void) {
    uint32_t asset_sector = 0;

    switch (g_seek_mode) {
    case PCFX_PCFV_SEEK_FRAME:
        return (g_seek_sector < g_frame_count) ? (uint16_t)g_seek_sector : (uint16_t)(g_frame_count - 1u);
    case PCFX_PCFV_SEEK_STREAM_SECTOR:
        asset_sector = g_seek_sector;
        break;
    case PCFX_PCFV_SEEK_DISC_LBA:
        asset_sector = (g_seek_sector > g_stream_lba) ? (g_seek_sector - g_stream_lba) : 0u;
        break;
    case PCFX_PCFV_SEEK_DATA_SECTOR:
        asset_sector = g_data_start_sector + g_seek_sector;
        break;
    case PCFX_PCFV_SEEK_NONE:
    default:
        asset_sector = 0;
        break;
    }

    return pcfv_find_frame_for_asset_sector(asset_sector);
}

static void pcfv_prepare_audio_seek(uint16_t start_frame) {
    uint32_t target_audio_byte;
    uint16_t ci;
    uint16_t chosen = 0xffffu;

    g_audio_preroll_src_sector = g_data_start_sector;
    g_audio_preroll_run_sectors = g_audio_preroll_sectors;
    g_next_audio_chunk = 0;

    if (!g_audio_bytes || !g_audio_preroll_sectors || start_frame == 0 || !g_frame_count) return;

    /* Approximate the desired ADPCM point from video time.  PCFV v1 stores
       refill chunk byte offsets but not independent ADPCM predictor states, so
       arbitrary audio random access can have a short predictor-settling transient.
       Branch-heavy games should encode branch clips at ADPCM reset boundaries. */
    target_audio_byte = (uint32_t)(((uint64_t)g_audio_bytes * (uint64_t)start_frame) / (uint64_t)g_frame_count);

    if (target_audio_byte < (g_audio_preroll_sectors * PCFV_SECTOR_SIZE)) {
        return;
    }

    for (ci = 0; ci < g_audio_chunk_count; ++ci) {
        if (g_audio_chunks[ci].byte_offset <= target_audio_byte) chosen = ci;
        else break;
    }

    if (chosen != 0xffffu) {
        uint32_t fill = g_audio_chunks[chosen].sectors;
        if (fill > (ADPCM_RING_BYTES / PCFV_SECTOR_SIZE)) fill = ADPCM_RING_BYTES / PCFV_SECTOR_SIZE;
        g_audio_preroll_src_sector = g_audio_chunks[chosen].sector;
        g_audio_preroll_run_sectors = fill;
        g_next_audio_chunk = (uint16_t)(chosen + 1u);
    }
}

static void setup_video_buffers(void) {
    uint16_t i;
    for (i = 0; i < VIDEO_BUFFER_COUNT; ++i) {
        g_video_state[i] = 0;
        g_video_ready_delay[i] = 0;
        g_video_frame[i] = 0xffffu;
        g_video_kram[i] = VIDEO_BUFFER_FIRST_WORD + ((uint32_t)i * g_video_buffer_stride_words);
    }
    g_next_load_frame = g_start_frame;
    g_next_display_frame = g_start_frame;
    g_have_display_buf = 0;
    g_current_display_buf = 0xffffu;
}


/* Reset the read-ahead ring for a new target while optionally keeping the
   current visible buffer locked.  This is the key distinction between startup
   and in-game seeks: startup shows black while it prebuffers, while runtime
   seeks keep the last good frame on screen until the target frame is ready. */
static void setup_video_buffers_for_seek(uint16_t frame, uint8_t keep_current_display) {
    uint16_t i;
    uint16_t keep = (keep_current_display && g_have_display_buf && g_current_display_buf < VIDEO_BUFFER_COUNT)
                    ? g_current_display_buf : 0xffffu;

    for (i = 0; i < VIDEO_BUFFER_COUNT; ++i) {
        g_video_kram[i] = VIDEO_BUFFER_FIRST_WORD + ((uint32_t)i * g_video_buffer_stride_words);
        if (i == keep) {
            g_video_state[i] = 3;
            /* Preserve g_video_frame[i] as an informational current-frame value. */
            g_video_ready_delay[i] = 0;
        } else {
            g_video_state[i] = 0;
            g_video_ready_delay[i] = 0;
            g_video_frame[i] = 0xffffu;
        }
    }

    g_next_load_frame = frame;
    g_next_display_frame = frame;
    g_field_counter = 0;
    g_done = 0;
}

static void cd_dma_cancel_for_seek(void) {
    if (cd_dma_is_busy()) {
        if (g_cd_dma.kind == CD_REQ_VIDEO && g_cd_dma.video_buf < VIDEO_BUFFER_COUNT) {
            g_video_state[g_cd_dma.video_buf] = 0;
            g_video_ready_delay[g_cd_dma.video_buf] = 0;
            g_video_frame[g_cd_dma.video_buf] = 0xffffu;
        }
        scsi_bus_abort_reset();
    }
    memset(&g_cd_dma, 0, sizeof(g_cd_dma));
}


static int find_video_buf_for_frame(uint16_t frame) {
    uint16_t i;
    for (i = 0; i < VIDEO_BUFFER_COUNT; ++i) {
        if (g_video_state[i] == 2 && g_video_ready_delay[i] == 0 && g_video_frame[i] == frame) return (int)i;
    }
    return -1;
}

static uint16_t frame_distance(uint16_t from, uint16_t to) {
    if (!g_frame_count) return 0xffffu;
    return (to >= from) ? (uint16_t)(to - from) : (uint16_t)(g_frame_count - from + to);
}

static uint16_t count_ready_video_buffers(void) {
    uint16_t i, n = 0;
    for (i = 0; i < VIDEO_BUFFER_COUNT; ++i) if (g_video_state[i] == 2 && g_video_ready_delay[i] == 0) n++;
    if (n > g_video_ready_highwater) g_video_ready_highwater = n;
    return n;
}

static void age_ready_video_buffers(void) {
    uint16_t i;
    for (i = 0; i < VIDEO_BUFFER_COUNT; ++i) {
        if (g_video_state[i] == 2 && g_video_ready_delay[i]) g_video_ready_delay[i]--;
    }
}

static void drop_stale_video_buffers(uint16_t target_frame) {
    uint16_t i;
    if (!g_frame_count) return;
    for (i = 0; i < VIDEO_BUFFER_COUNT; ++i) {
        if (g_video_state[i] == 2 && g_video_ready_delay[i] == 0) {
            uint16_t d = frame_distance(target_frame, g_video_frame[i]);
            if (d > (g_frame_count / 2u)) {
                g_video_state[i] = 0;
                g_video_ready_delay[i] = 0;
                g_video_frame[i] = 0xffffu;
                g_video_frames_dropped_stale++;
            }
        }
    }
}

static int find_best_ready_video_buf(uint16_t target_frame, uint16_t max_skip, uint16_t *skip_out) {
    uint16_t i;
    uint16_t best_d = 0xffffu;
    int best_i = -1;
    for (i = 0; i < VIDEO_BUFFER_COUNT; ++i) {
        if (g_video_state[i] == 2 && g_video_ready_delay[i] == 0) {
            uint16_t d = frame_distance(target_frame, g_video_frame[i]);
            if (d <= max_skip && d < best_d) {
                best_d = d;
                best_i = (int)i;
            }
        }
    }
    if (best_i >= 0 && skip_out) *skip_out = best_d;
    return best_i;
}

static int frame_already_loading_or_ready(uint16_t frame) {
    uint16_t i;
    for (i = 0; i < VIDEO_BUFFER_COUNT; ++i) {
        if (g_video_state[i] != 0 && g_video_frame[i] == frame) return 1;
    }
    return 0;
}

static int alloc_video_buf(void) {
    uint16_t i;
    for (i = 0; i < VIDEO_BUFFER_COUNT; ++i) {
        if (g_video_state[i] == 0) return (int)i;
    }
    return -1;
}
static int alloc_video_buf_run(uint16_t wanted, uint16_t *count_out) {
    uint16_t i, n;
    if (wanted < 1u) wanted = 1u;
    if (wanted > VIDEO_BATCH_MAX) wanted = VIDEO_BATCH_MAX;
    for (i = 0; i < VIDEO_BUFFER_COUNT; ++i) {
        if (g_video_state[i] != 0) continue;
        n = 0;
        while ((uint16_t)(i + n) < VIDEO_BUFFER_COUNT && n < wanted && g_video_state[i + n] == 0) n++;
        if (n) {
            *count_out = n;
            return (int)i;
        }
    }
    *count_out = 0;
    return -1;
}

static uint16_t video_contiguous_batch_count(uint16_t frame) {
    uint16_t n = 1u;
    uint32_t fixed = g_entries[frame].video_sectors;
    if (!fixed || fixed > 4u) return 1u;
    while (n < VIDEO_BATCH_MAX && (uint16_t)(frame + n) < g_frame_count) {
        uint16_t prev = (uint16_t)(frame + n - 1u);
        uint16_t curf = (uint16_t)(frame + n);
        if (frame_already_loading_or_ready(curf)) break;
        if (g_entries[curf].video_sectors != fixed) break;
        if (g_entries[prev].video_sector + g_entries[prev].video_sectors != g_entries[curf].video_sector) break;
        n++;
    }
    return n;
}


static void queue_more_video_if_idle(void) {
    uint16_t probes;
    const uint32_t base = g_stream_lba;

    if (cd_dma_is_busy()) return;
    if (!g_frame_count) return;
    if (!g_loop_playback && g_next_load_frame >= g_frame_count) return;

    /* Do not keep the video ring completely full.  Leaving free KRAM slots lets
       the loader issue multi-frame contiguous READ(10) batches instead of
       degenerating back to one command per frame. */
    if (g_video_frames_presented && count_ready_video_buffers() >= VIDEO_PREBUFFER_TARGET) return;

    for (probes = 0; probes < g_frame_count; ++probes) {
        int bi;
        uint16_t frame = g_next_load_frame;
        if (!frame_already_loading_or_ready(frame)) {
            uint16_t want = video_contiguous_batch_count(frame);
            uint16_t got = 0, k;
            bi = alloc_video_buf_run(want, &got);
            if (bi < 0 || got == 0u) return;
            if (got > want) got = want;
            for (k = 0; k < got; ++k) {
                g_video_state[bi + k] = 1;
                g_video_ready_delay[bi + k] = 0;
                g_video_frame[bi + k] = (uint16_t)(frame + k);
            }
            if (cd_dma_start(CD_REQ_VIDEO, base + g_entries[frame].video_sector, g_video_kram[bi],
                             got * g_entries[frame].video_sectors * PCFV_SECTOR_SIZE, (uint16_t)bi)) {
                g_cd_dma.video_count = got;
                g_next_load_frame = (uint16_t)(g_next_load_frame + got);
                if (g_next_load_frame >= g_frame_count) {
                    if (g_loop_playback) g_next_load_frame = 0;
                    else g_next_load_frame = g_frame_count;
                }
                return;
            }
            for (k = 0; k < got; ++k) {
                g_video_state[bi + k] = 0;
                g_video_ready_delay[bi + k] = 0;
                g_video_frame[bi + k] = 0xffffu;
            }
            return;
        }
        g_next_load_frame++;
        if (g_next_load_frame >= g_frame_count) {
            if (g_loop_playback) g_next_load_frame = 0;
            else g_next_load_frame = g_frame_count;
        }
    }
}

#if defined(PCFX_PCFV_USE_MP2) && PCFX_PCFV_USE_MP2
static int pcfv_mp2_fetch_next_chunk(void) {
    const uint32_t base = g_stream_lba;
    const AudioChunk *c;
    if (g_audio_codec != PCFV_AUDIO_CODEC_MP2 || !g_mp2_enabled) return 0;
    if (cd_dma_is_busy()) return 0;
    if (g_next_audio_chunk >= g_audio_chunk_count) return 0;
    c = &g_audio_chunks[g_next_audio_chunk];
    if (!c->sectors) { g_next_audio_chunk++; return 1; }
    if (pcfx_mp2_stream_free_bytes(&g_mp2_async) < (c->sectors * PCFV_SECTOR_SIZE)) return 0;
    g_mp2_pending_byte_offset = c->byte_offset;
    g_mp2_pending_sectors = c->sectors;
    if (!cd_dma_start(CD_REQ_MP2, base + c->sector, KRAM_MP2_WORD_ADDR,
                      c->sectors * PCFV_SECTOR_SIZE, 0xffffu)) {
        g_mp2_pending_byte_offset = 0;
        g_mp2_pending_sectors = 0;
        return 0;
    }
    return 1;
}
#endif

static void scheduler_start_if_idle(void) {
    const uint32_t base = g_stream_lba;
    if (cd_dma_is_busy()) return;

    if (!g_header_ready) {
        (void)cd_dma_start(CD_REQ_HEADER, base, KRAM_HEADER_WORD_ADDR, PCFV_HEADER_READ_BYTES, 0xffffu);
        return;
    }

    if (g_audio_codec == PCFV_AUDIO_CODEC_ADPCM && !g_audio_preroll_ready && g_audio_preroll_run_sectors) {
        (void)cd_dma_start(CD_REQ_AUDIO_PREROLL, base + g_audio_preroll_src_sector, KRAM_ADPCM_WORD_ADDR,
                           g_audio_preroll_run_sectors * PCFV_SECTOR_SIZE, 0xffffu);
        return;
    }

#if defined(PCFX_PCFV_USE_MP2) && PCFX_PCFV_USE_MP2
    if (pcfv_mp2_should_fetch_urgent()) {
        if (pcfv_mp2_fetch_next_chunk()) return;
    }
#endif

    /* ADPCM is prioritized over speculative video because the video ring has
       several frames of elasticity while the ADPCM ring has hard half-buffer
       deadlines. */
    if (g_audio_codec == PCFV_AUDIO_CODEC_ADPCM && g_pending_audio_half0 && g_next_audio_chunk < g_audio_chunk_count) {
        const AudioChunk *c = &g_audio_chunks[g_next_audio_chunk++];
        if (cd_dma_start(CD_REQ_AUDIO_HALF0, base + c->sector, KRAM_ADPCM_WORD_ADDR,
                         c->sectors * PCFV_SECTOR_SIZE, 0xffffu)) {
            g_pending_audio_half0 = 0;
        }
        return;
    }

    if (g_audio_codec == PCFV_AUDIO_CODEC_ADPCM && g_pending_audio_half1 && g_next_audio_chunk < g_audio_chunk_count) {
        const AudioChunk *c = &g_audio_chunks[g_next_audio_chunk++];
        if (cd_dma_start(CD_REQ_AUDIO_HALF1, base + c->sector, KRAM_ADPCM_WORD_ADDR + ADPCM_HALF_WORDS,
                         c->sectors * PCFV_SECTOR_SIZE, 0xffffu)) {
            g_pending_audio_half1 = 0;
        }
        return;
    }

    queue_more_video_if_idle();

#if defined(PCFX_PCFV_USE_MP2) && PCFX_PCFV_USE_MP2
    if (!cd_dma_is_busy() &&
        g_audio_codec == PCFV_AUDIO_CODEC_MP2 && g_mp2_enabled &&
        g_next_audio_chunk < g_audio_chunk_count &&
        (count_ready_video_buffers() >= 10u || pcfx_mp2_async_ring_used() < PCFV_MP2_URGENT_RING_SAMPLES) &&
        pcfx_mp2_stream_buffered_bytes(&g_mp2_async) < PCFV_MP2_PREFETCH_LOW_BYTES) {
        (void)pcfv_mp2_fetch_next_chunk();
    }
#endif
}

static void scheduler_poll(void) {
    cd_dma_poll_budget(SCHEDULER_POLL_BUDGET);
    if (!cd_dma_is_busy()) scheduler_start_if_idle();
}

static void poll_adpcm_refill(void) {
    if (g_audio_codec != PCFV_AUDIO_CODEC_ADPCM) return;
    if (!g_adpcm_started) return;
    {
        uint16_t st = king_read_reg16(0x53);
        if (st & 0x0002u) g_pending_audio_half0 = 1;
        if (st & 0x0001u) g_pending_audio_half1 = 1;
    }
    scheduler_start_if_idle();
}

static void setup_adpcm_audio(void) {
    eris_king_set_kram_pages(0, 0, 0, 0);
    eris_low_adpcm_set_control(pcfx_adpcm_rate_enum(g_audio_rate_hz), 1, 1, 1, 1);
    eris_low_adpcm_set_volume(0, 63, 63);
    eris_low_adpcm_set_volume(1, 0, 0);
    eris_low_cdda_set_volume(0, 0);

    king_write_reg16(0x50, 0x0000);
    king_write_reg16(0x51, 0x0001);
    king_write_reg16(0x52, 0x0000);
    king_write_reg16(0x58, (uint16_t)(KRAM_ADPCM_WORD_ADDR >> 8));
    king_write_reg32(0x59, KRAM_ADPCM_WORD_ADDR + ADPCM_RING_WORDS - 1u);
    king_write_reg16(0x5A, (uint16_t)((KRAM_ADPCM_WORD_ADDR + ADPCM_HALF_WORDS) >> 6));

    eris_low_adpcm_set_control(pcfx_adpcm_rate_enum(g_audio_rate_hz), 1, 1, 0, 0);
    (void)king_read_reg16(0x53);
    g_adpcm_control_word = (uint16_t)(PCFX_KING_ADPCM_CH0_ENABLE | (pcfx_adpcm_rate_bits(g_audio_rate_hz) << 2));
    king_write_reg16(0x50, g_adpcm_control_word);
    g_adpcm_started = 1;
}

static void fail_black_loop(void) {
    setup_rainbow_regs();
    for (;;) wait_vblank();
}

static void pcfv_boot_stream_async(void) {
    uint16_t ready_count;

    scheduler_start_if_idle();
    while (!g_header_ready) {
#if defined(PCFX_PCFV_USE_MP2) && PCFX_PCFV_USE_MP2
        g_mp2_decode_budget_used = 0u;
#endif
        scheduler_poll();
        pcfv_mp2_decode_budget(PCFV_MP2_FIELD_BUDGET);
        wait_vblank();
        if (g_cd_dma.state == CD_DMA_ERROR) fail_black_loop();
    }
    if (!parse_pcfv_header()) fail_black_loop();
    g_start_frame = pcfv_resolve_start_frame();
    pcfv_prepare_audio_seek(g_start_frame);
#if defined(PCFX_PCFV_USE_MP2) && PCFX_PCFV_USE_MP2
    if (g_audio_codec == PCFV_AUDIO_CODEC_MP2) {
        g_audio_preroll_ready = 1;
        pcfv_mp2_prepare_loaded();
        /* Do not read MP2 during the hidden RAINBOW boot/prebuffer phase.
           MP2 chunks use the same SCSI/KING DMA scheduler as video; fetching
           begins after the first RAINBOW frame is latched so startup cannot
           deadlock on audio. */
    } else
#endif
    if (!g_audio_bytes || !g_audio_preroll_run_sectors) g_audio_preroll_ready = 1;
    setup_video_buffers();

    scheduler_start_if_idle();
    while (!g_audio_preroll_ready) {
#if defined(PCFX_PCFV_USE_MP2) && PCFX_PCFV_USE_MP2
        g_mp2_decode_budget_used = 0u;
#endif
        scheduler_poll();
        pcfv_mp2_decode_budget(PCFV_MP2_FIELD_BUDGET);
        wait_vblank();
        if (g_cd_dma.state == CD_DMA_ERROR) fail_black_loop();
    }
    /* Do not start ADPCM during the hidden video prebuffer.  The audio engine
       is started at the same late-raster latch that makes RAINBOW visible,
       so audio cannot lead the black warm-up screen. */

    /* Prebuffer several video frames before first display.  This removes the
       cadence dependency between command latency and visible frame pacing. */
    ready_count = 0;
    {
        uint16_t target = VIDEO_PREBUFFER_TARGET;
        if (target > VIDEO_BUFFER_COUNT - 2u) target = VIDEO_BUFFER_COUNT - 2u;
        if (target > g_frame_count) target = g_frame_count;
        while (ready_count < target) {
#if defined(PCFX_PCFV_USE_MP2) && PCFX_PCFV_USE_MP2
            g_mp2_decode_budget_used = 0u;
#endif
            ready_count = count_ready_video_buffers();
            scheduler_poll();
            pcfv_mp2_decode_budget(PCFV_MP2_FIELD_BUDGET);
            wait_vblank();
            age_ready_video_buffers();
            if (g_cd_dma.state == CD_DMA_ERROR) fail_black_loop();
        }
    }

#if defined(PCFX_PCFV_USE_MP2) && PCFX_PCFV_USE_MP2
    if (g_audio_codec == PCFV_AUDIO_CODEC_MP2 && g_mp2_enabled) {
        /* Hidden audio prefill after the RAINBOW ring is already full.  This
           prevents the first visible frames from racing ahead while the first
           MP2 CD chunk is read and decoded, but it cannot starve startup video. */
        uint32_t guard = 0;
        while (pcfx_mp2_async_ring_used() < PCFV_MP2_START_RING_SAMPLES && guard++ < 240u) {
            /* Boot does not run through pcfx_pcfv_update(), so the live-loop
               "one MP2 decode attempt per visible field" guard must be reset
               here.  Without this, hidden preroll decodes only one MP2 frame
               and the first visible frames can be silent until runtime decode
               catches up. */
            g_mp2_decode_budget_used = 0u;
            scheduler_poll();
            pcfv_mp2_decode_budget(PCFV_MP2_FIELD_BUDGET);
            scheduler_start_if_idle();
            wait_vblank();
            age_ready_video_buffers();
            if (g_cd_dma.state == CD_DMA_ERROR) fail_black_loop();
        }
        g_mp2_preroll_ring_at_exit = pcfx_mp2_async_ring_used();
        g_mp2_preroll_guard_count = guard;
    }
#endif

    /* Align the playback scheduler to a clean field boundary.  Without this,
       boot/prebuffer completion can fall in the middle of vblank after the
       RAINBOW transfer-start scanline has already passed, producing a few
       top-only startup fields. */
    wait_vblank();
}

static void pcfx_pcfv_reset_state(void) {
    memset(&g_cd_dma, 0, sizeof(g_cd_dma));
    g_header_ready = 0;
    g_audio_preroll_ready = 0;
    g_adpcm_started = 0;
    g_adpcm_control_word = 0;
    g_rainbow_visible = 0;
    g_paused = 0;
    g_pending_audio_half0 = 0;
    g_pending_audio_half1 = 0;
    g_next_audio_chunk = 0;
    g_audio_chunk_count = 0;
    g_mp2_pending_byte_offset = 0;
    g_mp2_pending_sectors = 0;
    g_audio_preroll_src_sector = 0;
    g_audio_preroll_run_sectors = 0;
    g_frame_count = 0;
    g_start_frame = 0;
    g_seek_in_progress = 0;
    g_next_load_frame = g_start_frame;
    g_next_display_frame = g_start_frame;
    g_have_display_buf = 0;
    g_current_display_buf = 0xffffu;
    g_done = 0;
    g_abort = 0;
    g_fields_per_frame = 0;
    g_field_counter = 0;
    g_vblank_latched_frames = 0;
    g_rainbow_visible_fields = 0;
    g_mp2_sync_pauses = 0;
    g_mp2_sync_resumes = 0;
    g_video_buffer_stride_words = VIDEO_BUFFER_STRIDE_DEFAULT;
}

int pcfx_pcfv_open(uint32_t stream_lba, const PcfxPcfvOptions *opt) {
    pcfx_pcfv_reset_state();
    setup_video();
    setup_rainbow_regs();
    eris_pad_init(0);
    eris_low_scsi_reset();

    g_stream_lba = stream_lba;
    g_stop_buttons = (opt && opt->stop_buttons) ? opt->stop_buttons : PCFX_PCFV_BTN_START;
    g_pause_buttons = opt ? opt->pause_buttons : 0u;
    g_seek_mode = opt ? opt->seek_mode : PCFX_PCFV_SEEK_NONE;
    g_seek_sector = opt ? opt->start_sector : 0u;
    /* The flexible open/update API is intentionally single-shot.  The simple
       blocking pcfx_pcfv_play() handles looping by reopening the stream so
       audio, video, and preread state restart together. */
    g_loop_playback = 0;
    g_done = 0;
    g_abort = 0;
    g_prev_pad = eris_pad_read(0);

    if (!g_stream_lba) return 0;
    pcfv_boot_stream_async();
#if defined(PCFX_PCFV_USE_MP2) && PCFX_PCFV_USE_MP2
    /* Do not initialize the PSG timer/IRQ before the RAINBOW player has
       configured video and completed its CD prebuffer.  The MP2 data itself
       can be read early, but timer setup before PCFV boot leaves this build on
       a black screen in headless PC-FX validation. */
    if (g_audio_codec != PCFV_AUDIO_CODEC_MP2) pcfv_mp2_prepare_loaded();
#endif

    return 1;
}

static uint16_t pcfx_pcfv_fields_per_frame(void) {
    uint32_t rounded;
    if (!g_fps_num) return 1;
    rounded = ((60u * (uint32_t)g_fps_den) + ((uint32_t)g_fps_num / 2u)) / (uint32_t)g_fps_num;
#if defined(PCFX_PCFV_USE_MP2) && PCFX_PCFV_USE_MP2
    /* MP2 playback is the pitch-correct continuous clock.  Do not pace RAINBOW
       by a fixed field count under MP2; the latch path gates to audio PTS and
       may catch up/skip stale frames if CD loading was late. */
    if (g_audio_codec == PCFV_AUDIO_CODEC_MP2) return 1u;
#endif
    if (rounded < 1u) rounded = 1u;
    if (rounded > 10u) rounded = 10u;
    return (uint16_t)rounded;
}

static uint32_t pcfx_pcfv_pad_pressed(void) {
    uint32_t raw = eris_pad_read(0);
    uint32_t pressed = raw & ~g_prev_pad;
    g_prev_pad = raw;
    return pressed;
}

void pcfx_pcfv_set_paused(int paused) {
    uint8_t want = paused ? 1u : 0u;
    if (g_paused == want) return;
    g_paused = want;

    /* Keep the current RAINBOW buffer latched.  For audio, disabling KING ch0
       pauses the ADPCM address engine; reenabling with the same rate/control
       word resumes from the current ADPCM pointer on the target model used by
       this player. */
    if (g_adpcm_started) {
        if (g_paused) king_write_reg16(0x50, 0x0000);
        else king_write_reg16(0x50, g_adpcm_control_word);
    }
}

void pcfx_pcfv_toggle_paused(void) { pcfx_pcfv_set_paused(!g_paused); }

int pcfx_pcfv_paused(void) { return g_paused ? 1 : 0; }

static int pcfx_pcfv_handle_buttons(void) {
    uint32_t pressed = pcfx_pcfv_pad_pressed();
    if (g_pause_buttons && (pressed & g_pause_buttons)) pcfx_pcfv_set_paused(!g_paused);
    if (pressed & g_stop_buttons) return 1;
    return 0;
}

static int pcfv_audio_playback_complete(void) {
#if defined(PCFX_PCFV_USE_MP2) && PCFX_PCFV_USE_MP2
    if (g_audio_codec == PCFV_AUDIO_CODEC_MP2 && g_mp2_enabled) {
        return pcfx_mp2_async_done(&g_mp2_async) ? 1 : 0;
    }
#endif
    return 1;
}

static uint16_t pcfv_audio_target_frame_for_sync(void) {
#if defined(PCFX_PCFV_USE_MP2) && PCFX_PCFV_USE_MP2
    uint64_t n;
    if (g_audio_codec != PCFV_AUDIO_CODEC_MP2 || !g_audio_rate_hz || !g_fps_den) return g_next_display_frame;
    if (!pcfx_mp2_async_started(&g_mp2_async)) return g_next_display_frame;
    n = (uint64_t)pcfx_mp2_async_samples_emitted() * (uint64_t)g_fps_num;
    n /= ((uint64_t)g_audio_rate_hz * (uint64_t)g_fps_den);
    if (n >= g_frame_count) return (g_frame_count ? (uint16_t)(g_frame_count - 1u) : 0u);
    return (uint16_t)n;
#else
    return g_next_display_frame;
#endif
}

static int pcfv_video_due_for_audio_clock(void) {
#if defined(PCFX_PCFV_USE_MP2) && PCFX_PCFV_USE_MP2
    uint16_t target;
    if (g_audio_codec != PCFV_AUDIO_CODEC_MP2) return 1;
    if (!pcfx_mp2_async_started(&g_mp2_async)) return 1;
    target = pcfv_audio_target_frame_for_sync();
    if (!g_loop_playback && g_next_display_frame > target) return 0;
    if (!g_loop_playback) return 1;
    if (frame_distance(g_next_display_frame, target) < (g_frame_count / 2u)) {
        return g_next_display_frame <= target;
    }
    return 0;
#else
    return 1;
#endif
}

static int pcfv_latch_ready_display_frame(void) {
    uint16_t skipped = 0;
    int bi;

    if (g_next_display_frame >= g_frame_count) {
        if (pcfv_audio_playback_complete()) g_done = 1;
        return 0;
    }

    drop_stale_video_buffers(g_next_display_frame);
    bi = (g_next_display_frame < g_frame_count) ? find_video_buf_for_frame(g_next_display_frame) : -1;
    if (bi < 0 && g_next_display_frame < g_frame_count) {
        uint16_t max_skip = VIDEO_SKIP_SEARCH;
#if defined(PCFX_PCFV_USE_MP2) && PCFX_PCFV_USE_MP2
        if (g_audio_codec == PCFV_AUDIO_CODEC_MP2) {
            uint16_t audio_target = pcfv_audio_target_frame_for_sync();
            if (!g_loop_playback) {
                if (audio_target > g_next_display_frame) {
                    uint16_t ahead = (uint16_t)(audio_target - g_next_display_frame);
                    if (ahead > max_skip) max_skip = (ahead > VIDEO_SYNC_SKIP_MAX) ? VIDEO_SYNC_SKIP_MAX : ahead;
                }
            } else if (frame_distance(g_next_display_frame, audio_target) < (g_frame_count / 2u)) {
                uint16_t ahead = frame_distance(g_next_display_frame, audio_target);
                if (ahead > max_skip) max_skip = (ahead > VIDEO_SYNC_SKIP_MAX) ? VIDEO_SYNC_SKIP_MAX : ahead;
            }
        }
#endif
        bi = find_best_ready_video_buf(g_next_display_frame, max_skip, &skipped);
    }

    if (bi >= 0) {
        uint16_t old_buf = g_current_display_buf;
        g_video_state[bi] = 3;
        g_current_display_buf = (uint16_t)bi;
        g_have_display_buf = 1;
        if (old_buf < VIDEO_BUFFER_COUNT && old_buf != (uint16_t)bi) {
            g_video_state[old_buf] = 0;
            g_video_ready_delay[old_buf] = 0;
            g_video_frame[old_buf] = 0xffffu;
        }
        g_video_frames_presented++;
        if (skipped) g_video_frames_skipped += skipped;
        g_next_display_frame = g_video_frame[bi] + 1u;
        if (g_next_display_frame >= g_frame_count) {
            if (g_loop_playback) g_next_display_frame = 0;
            else { g_next_display_frame = g_frame_count; if (pcfv_audio_playback_complete()) g_done = 1; }
        }
        g_seek_in_progress = 0;
        return 1;
    }

    g_scsi_dma_video_underflows++;
    g_video_frames_held++;
    return 0;
}


int pcfx_pcfv_update(void) {
    uint16_t i;

#if defined(PCFX_PCFV_USE_MP2) && PCFX_PCFV_USE_MP2
    g_mp2_decode_budget_used = 0u;
#endif

    if (!g_fields_per_frame) g_fields_per_frame = pcfx_pcfv_fields_per_frame();
    if (g_abort || g_done) return 0;
    if (pcfx_pcfv_handle_buttons()) { g_abort = 1; return 0; }

    if (g_paused) {
        while ((uint16_t)eris_tetsu_get_raster() < RAINBOW_RESTART_RASTER) {
            if (g_seek_in_progress) {
                poll_adpcm_refill();
                scheduler_poll();
                pcfv_mp2_decode_budget(PCFV_MP2_FIELD_BUDGET);
                scheduler_start_if_idle();
            } else {
                cd_dma_poll_budget(SCHEDULER_POLL_BUDGET);
            }
            if (pcfx_pcfv_handle_buttons()) { g_abort = 1; return 0; }
        }
        if (g_seek_in_progress) {
            age_ready_video_buffers();
            (void)pcfv_latch_ready_display_frame();
        }
        if (g_have_display_buf && g_current_display_buf < VIDEO_BUFFER_COUNT) {
            start_rainbow_frame(g_video_kram[g_current_display_buf], 15);
        }
        while ((uint16_t)eris_tetsu_get_raster() >= RAINBOW_RESTART_RASTER) {
            if (g_seek_in_progress) {
                poll_adpcm_refill();
                scheduler_poll();
                pcfv_mp2_decode_budget(PCFV_MP2_FIELD_BUDGET);
                scheduler_start_if_idle();
            } else {
                cd_dma_poll_budget(SCHEDULER_POLL_BUDGET);
            }
            if (pcfx_pcfv_handle_buttons()) { g_abort = 1; return 0; }
        }
        return 1;
    }

    while ((uint16_t)eris_tetsu_get_raster() < RAINBOW_RESTART_RASTER) {
        poll_adpcm_refill();
        scheduler_poll();
        pcfv_mp2_decode_budget(PCFV_MP2_FIELD_BUDGET);
        pcfv_mp2_sync_to_video();
        scheduler_start_if_idle();
        if (g_cd_dma.state == CD_DMA_ERROR) {
            g_scsi_dma_late_frames++;
            scheduler_start_if_idle();
        }
        if (pcfx_pcfv_handle_buttons()) { g_abort = 1; return 0; }
    }

    age_ready_video_buffers();

    if (g_field_counter == 0) {
        /* During hidden HuC6271 warm-up, keep restarting the first decoded
           RAINBOW frame instead of consuming the timeline.  Advancing hidden
           frames made video start several frames ahead of MP2. */
        if ((g_rainbow_visible || !g_have_display_buf) && pcfv_video_due_for_audio_clock()) {
            (void)pcfv_latch_ready_display_frame();
        }
    }

    if (g_have_display_buf && g_current_display_buf < VIDEO_BUFFER_COUNT) {
        start_rainbow_frame(g_video_kram[g_current_display_buf], 15);
        g_vblank_latched_frames++;
        if (!vblank_active()) g_midfield_latched_frames++;
        if (!g_rainbow_visible && g_vblank_latched_frames >= 8u) {
            set_rainbow_visible(1);
            g_rainbow_visible_fields = 0;
            g_field_counter = 0;
        }
        if (g_rainbow_visible) {
            g_rainbow_visible_fields++;
#if defined(PCFX_PCFV_USE_MP2) && PCFX_PCFV_USE_MP2
            if (g_mp2_enabled && g_rainbow_visible_fields >= AUDIO_START_AFTER_VISIBLE_FIELDS) {
                pcfv_mp2_start_if_ready();
                if (!g_audio_start_latch_frame && pcfx_mp2_async_playing()) {
                    g_audio_start_latch_frame = g_vblank_latched_frames;
                    g_audio_start_visible_fields = g_rainbow_visible_fields;
                    g_audio_start_ring_used = pcfx_mp2_async_ring_used();
                    g_audio_start_samples_emitted = pcfx_mp2_async_samples_emitted();
                }
            } else
#endif
            if (g_audio_codec == PCFV_AUDIO_CODEC_ADPCM && g_audio_bytes && !g_adpcm_started && g_audio_preroll_ready &&
                g_rainbow_visible_fields >= AUDIO_START_AFTER_VISIBLE_FIELDS) {
                setup_adpcm_audio();
                g_audio_start_latch_frame = g_vblank_latched_frames;
            }
        }
    }

    poll_adpcm_refill();
    scheduler_poll();
    scheduler_start_if_idle();

    while ((uint16_t)eris_tetsu_get_raster() >= RAINBOW_RESTART_RASTER) {
        poll_adpcm_refill();
        for (i = 0; i < 2u; ++i) scheduler_poll();
        pcfv_mp2_decode_budget(PCFV_MP2_FIELD_BUDGET);
        pcfv_mp2_sync_to_video();
        scheduler_start_if_idle();
        if (pcfx_pcfv_handle_buttons()) { g_abort = 1; return 0; }
    }

    g_field_counter++;
    if (g_field_counter >= g_fields_per_frame) g_field_counter = 0;
    if (g_next_display_frame >= g_frame_count && pcfv_audio_playback_complete()) g_done = 1;
    return !g_done && !g_abort;
}


uint16_t pcfx_pcfv_frame_count(void) { return g_frame_count; }

uint16_t pcfx_pcfv_current_frame(void) {
    if (g_have_display_buf && g_current_display_buf < VIDEO_BUFFER_COUNT) return g_video_frame[g_current_display_buf];
    return g_start_frame;
}

uint32_t pcfx_pcfv_stream_sector_for_frame(uint16_t frame_index) {
    if (!g_frame_count) return 0;
    if (frame_index >= g_frame_count) frame_index = (uint16_t)(g_frame_count - 1u);
    return g_entries[frame_index].video_sector;
}

uint32_t pcfx_pcfv_disc_lba_for_frame(uint16_t frame_index) {
    return g_stream_lba + pcfx_pcfv_stream_sector_for_frame(frame_index);
}

uint32_t pcfx_pcfv_data_sector_for_frame(uint16_t frame_index) {
    uint32_t s = pcfx_pcfv_stream_sector_for_frame(frame_index);
    return (s > g_data_start_sector) ? (s - g_data_start_sector) : 0u;
}

uint16_t pcfx_pcfv_frame_for_stream_sector(uint32_t stream_relative_sector) {
    return pcfv_find_frame_for_asset_sector(stream_relative_sector);
}

uint16_t pcfx_pcfv_frame_for_disc_lba(uint32_t absolute_disc_lba) {
    uint32_t rel = (absolute_disc_lba > g_stream_lba) ? (absolute_disc_lba - g_stream_lba) : 0u;
    return pcfv_find_frame_for_asset_sector(rel);
}

uint16_t pcfx_pcfv_frame_for_data_sector(uint32_t data_relative_sector) {
    return pcfv_find_frame_for_asset_sector(g_data_start_sector + data_relative_sector);
}

int pcfx_pcfv_seek_frame(uint16_t frame_index) {
    uint8_t was_paused;

    if (!g_header_ready || !g_frame_count) return 0;
    if (frame_index >= g_frame_count) return 0;

    was_paused = g_paused;
    cd_dma_cancel_for_seek();

    /* Stop audio immediately.  It will be re-primed from the closest authored
       ADPCM chunk and restarted only after the requested video frame has been
       latched.  That keeps sector seeks from producing audio-over-old-video. */
    king_write_reg16(0x50, 0x0000);
    g_adpcm_started = 0;
    g_adpcm_control_word = 0;
    g_audio_preroll_ready = 0;
    g_pending_audio_half0 = 0;
    g_pending_audio_half1 = 0;
    g_audio_refills_completed = 0;

    g_start_frame = frame_index;
    if (g_audio_codec == PCFV_AUDIO_CODEC_ADPCM) {
        pcfv_prepare_audio_seek(frame_index);
        if (!g_audio_bytes || !g_audio_preroll_run_sectors) g_audio_preroll_ready = 1;
    } else {
        g_audio_preroll_ready = 1;
    }

    setup_video_buffers_for_seek(frame_index, 1);
    g_seek_in_progress = 1;
    g_done = 0;
    g_abort = 0;
    g_rainbow_visible_fields = 0;

    g_paused = was_paused;
    scheduler_start_if_idle();
    return 1;
}

int pcfx_pcfv_seek_stream_sector(uint32_t stream_relative_sector) {
    return pcfx_pcfv_seek_frame(pcfv_find_frame_for_asset_sector(stream_relative_sector));
}

int pcfx_pcfv_seek_disc_lba(uint32_t absolute_disc_lba) {
    uint32_t rel = (absolute_disc_lba > g_stream_lba) ? (absolute_disc_lba - g_stream_lba) : 0u;
    return pcfx_pcfv_seek_stream_sector(rel);
}

int pcfx_pcfv_seek_data_sector(uint32_t data_relative_sector) {
    return pcfx_pcfv_seek_stream_sector(g_data_start_sector + data_relative_sector);
}

void pcfx_pcfv_stop(void) {
    king_write_reg16(0x40, 0x0000);
    king_write_reg16(0x50, 0x0000);
#if defined(PCFX_PCFV_USE_MP2) && PCFX_PCFV_USE_MP2
    if (g_mp2_enabled) pcfx_mp2_async_stop(&g_mp2_async);
#endif
    g_adpcm_started = 0;
    g_adpcm_control_word = 0;
    g_paused = 0;
    set_rainbow_visible(0);
}

int pcfx_pcfv_aborted(void) { return g_abort ? 1 : 0; }

int pcfx_pcfv_play(uint32_t stream_lba, const PcfxPcfvOptions *opt) {
    PcfxPcfvOptions one;
    uint8_t loop;

    one.stop_buttons = (opt && opt->stop_buttons) ? opt->stop_buttons : PCFX_PCFV_BTN_START;
    one.pause_buttons = opt ? opt->pause_buttons : 0u;
    one.seek_mode = opt ? opt->seek_mode : PCFX_PCFV_SEEK_NONE;
    one.start_sector = opt ? opt->start_sector : 0u;
    loop = (opt && opt->loop) ? 1u : 0u;

    for (;;) {
        /* Always open the runtime in single-shot mode.  For looping playback we
           restart from the top after a natural end; this also restarts ADPCM
           cleanly instead of letting the video wrap over stale audio state. */
        one.loop = PCFX_PCFV_PLAY_ONCE;
        if (!pcfx_pcfv_open(stream_lba, &one)) return 0;
        while (pcfx_pcfv_update()) { }
        pcfx_pcfv_stop();
        if (pcfx_pcfv_aborted()) return 0;
        if (!loop) return 1;
    }
}

static void pcfv_default_options(PcfxPcfvOptions *opt, uint8_t loop, uint16_t stop_buttons, uint16_t pause_buttons) {
    opt->stop_buttons = stop_buttons ? stop_buttons : PCFX_PCFV_BTN_START;
    opt->pause_buttons = pause_buttons;
    opt->loop = loop;
    opt->seek_mode = PCFX_PCFV_SEEK_NONE;
    opt->start_sector = 0;
}

int pcfx_pcfv_play_once(uint32_t stream_lba, uint16_t stop_buttons) {
    PcfxPcfvOptions opt;
    pcfv_default_options(&opt, PCFX_PCFV_PLAY_ONCE, stop_buttons, 0);
    return pcfx_pcfv_play(stream_lba, &opt);
}

int pcfx_pcfv_play_looping(uint32_t stream_lba, uint16_t stop_buttons) {
    PcfxPcfvOptions opt;
    pcfv_default_options(&opt, PCFX_PCFV_PLAY_LOOP, stop_buttons, 0);
    return pcfx_pcfv_play(stream_lba, &opt);
}

int pcfx_pcfv_play_once_paused(uint32_t stream_lba, uint16_t stop_buttons, uint16_t pause_buttons) {
    PcfxPcfvOptions opt;
    pcfv_default_options(&opt, PCFX_PCFV_PLAY_ONCE, stop_buttons, pause_buttons);
    return pcfx_pcfv_play(stream_lba, &opt);
}

int pcfx_pcfv_play_looping_paused(uint32_t stream_lba, uint16_t stop_buttons, uint16_t pause_buttons) {
    PcfxPcfvOptions opt;
    pcfv_default_options(&opt, PCFX_PCFV_PLAY_LOOP, stop_buttons, pause_buttons);
    return pcfx_pcfv_play(stream_lba, &opt);
}

int pcfx_pcfv_play_once_from_sector(uint32_t stream_lba, uint32_t stream_relative_sector,
                                    uint16_t stop_buttons, uint16_t pause_buttons) {
    PcfxPcfvOptions opt;
    pcfv_default_options(&opt, PCFX_PCFV_PLAY_ONCE, stop_buttons, pause_buttons);
    opt.seek_mode = PCFX_PCFV_SEEK_STREAM_SECTOR;
    opt.start_sector = stream_relative_sector;
    return pcfx_pcfv_play(stream_lba, &opt);
}

int pcfx_pcfv_play_looping_from_sector(uint32_t stream_lba, uint32_t stream_relative_sector,
                                       uint16_t stop_buttons, uint16_t pause_buttons) {
    PcfxPcfvOptions opt;
    pcfv_default_options(&opt, PCFX_PCFV_PLAY_LOOP, stop_buttons, pause_buttons);
    opt.seek_mode = PCFX_PCFV_SEEK_STREAM_SECTOR;
    opt.start_sector = stream_relative_sector;
    return pcfx_pcfv_play(stream_lba, &opt);
}

int pcfx_pcfv_play_once_from_disc_lba(uint32_t stream_lba, uint32_t absolute_disc_lba,
                                      uint16_t stop_buttons, uint16_t pause_buttons) {
    PcfxPcfvOptions opt;
    pcfv_default_options(&opt, PCFX_PCFV_PLAY_ONCE, stop_buttons, pause_buttons);
    opt.seek_mode = PCFX_PCFV_SEEK_DISC_LBA;
    opt.start_sector = absolute_disc_lba;
    return pcfx_pcfv_play(stream_lba, &opt);
}

int pcfx_pcfv_play_looping_from_disc_lba(uint32_t stream_lba, uint32_t absolute_disc_lba,
                                         uint16_t stop_buttons, uint16_t pause_buttons) {
    PcfxPcfvOptions opt;
    pcfv_default_options(&opt, PCFX_PCFV_PLAY_LOOP, stop_buttons, pause_buttons);
    opt.seek_mode = PCFX_PCFV_SEEK_DISC_LBA;
    opt.start_sector = absolute_disc_lba;
    return pcfx_pcfv_play(stream_lba, &opt);
}



int pcfx_pcfv_play_once_from_data_sector(uint32_t stream_lba, uint32_t data_relative_sector,
                                         uint16_t stop_buttons, uint16_t pause_buttons) {
    PcfxPcfvOptions opt;
    pcfv_default_options(&opt, PCFX_PCFV_PLAY_ONCE, stop_buttons, pause_buttons);
    opt.seek_mode = PCFX_PCFV_SEEK_DATA_SECTOR;
    opt.start_sector = data_relative_sector;
    return pcfx_pcfv_play(stream_lba, &opt);
}

int pcfx_pcfv_play_looping_from_data_sector(uint32_t stream_lba, uint32_t data_relative_sector,
                                            uint16_t stop_buttons, uint16_t pause_buttons) {
    PcfxPcfvOptions opt;
    pcfv_default_options(&opt, PCFX_PCFV_PLAY_LOOP, stop_buttons, pause_buttons);
    opt.seek_mode = PCFX_PCFV_SEEK_DATA_SECTOR;
    opt.start_sector = data_relative_sector;
    return pcfx_pcfv_play(stream_lba, &opt);
}

int pcfx_pcfv_play_once_from_frame(uint32_t stream_lba, uint16_t frame_index,
                                   uint16_t stop_buttons, uint16_t pause_buttons) {
    PcfxPcfvOptions opt;
    pcfv_default_options(&opt, PCFX_PCFV_PLAY_ONCE, stop_buttons, pause_buttons);
    opt.seek_mode = PCFX_PCFV_SEEK_FRAME;
    opt.start_sector = frame_index;
    return pcfx_pcfv_play(stream_lba, &opt);
}

int pcfx_pcfv_play_looping_from_frame(uint32_t stream_lba, uint16_t frame_index,
                                      uint16_t stop_buttons, uint16_t pause_buttons) {
    PcfxPcfvOptions opt;
    pcfv_default_options(&opt, PCFX_PCFV_PLAY_LOOP, stop_buttons, pause_buttons);
    opt.seek_mode = PCFX_PCFV_SEEK_FRAME;
    opt.start_sector = frame_index;
    return pcfx_pcfv_play(stream_lba, &opt);
}

int pcfx_pcfv_play_loop(uint32_t stream_lba, uint16_t stop_buttons) {
    return pcfx_pcfv_play_looping(stream_lba, stop_buttons);
}


#if defined(PCFX_PCFV_USE_MP2) && PCFX_PCFV_USE_MP2
int pcfx_pcfv_mp2_load_from_cd(uint32_t mp2_lba, uint32_t mp2_size_bytes) {
    g_mp2_loaded = 0u;
    g_mp2_enabled = 0u;
    g_mp2_size_bytes = 0u;
    if (!pcfx_mp2_load_cd(mp2_lba, mp2_size_bytes)) {
        return 0;
    }
    /* Only preload the MP2 bytes here.  Timer/IRQ setup and the initial decode
       preroll are deferred until after PCFV's RAINBOW boot/prebuffer phase. */
    g_mp2_loaded = 1u;
    g_mp2_size_bytes = mp2_size_bytes;
    return 1;
}
uint32_t pcfx_pcfv_mp2_frames_decoded(void) { return g_mp2_async.frames_decoded; }
uint32_t pcfx_pcfv_mp2_underflows(void) { return pcfx_mp2_async_underflows(); }
uint32_t pcfx_pcfv_mp2_ring_used(void) { return pcfx_mp2_async_ring_used(); }
uint32_t pcfx_pcfv_mp2_error_code(void) { return g_mp2_async.error_code; }
uint32_t pcfx_pcfv_mp2_error_offset(void) { return g_mp2_async.error_offset; }
#else
int pcfx_pcfv_mp2_load_from_cd(uint32_t mp2_lba, uint32_t mp2_size_bytes) { (void)mp2_lba; (void)mp2_size_bytes; return 0; }
uint32_t pcfx_pcfv_mp2_frames_decoded(void) { return 0; }
uint32_t pcfx_pcfv_mp2_underflows(void) { return 0; }
uint32_t pcfx_pcfv_mp2_ring_used(void) { return 0; }
uint32_t pcfx_pcfv_mp2_error_code(void) { return 0; }
uint32_t pcfx_pcfv_mp2_error_offset(void) { return 0; }
#endif
