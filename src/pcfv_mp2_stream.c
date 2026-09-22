#include <stdint.h>
#include "pcfv_mp2_stream.h"
#include "pcfx_pcfv_player.h"

#define PCFV_MP2_PREFILL_FRAMES     24u
#define PCFV_MP2_FIELD_BUDGET       1u    /* decoded MP2 frames per field */
#define PCFV_MP2_PREFETCH_LOW_BYTES (24u * 1024u)
#define PCFV_MP2_URGENT_BYTES       8192u
#define PCFV_MP2_SYNC_LEAD_FRAMES   4u

static PcfvStreamInfo s_stream;
static PcfxMp2Async g_mp2_async;
static uint32_t g_mp2_pending_byte_offset;
static uint32_t g_mp2_pending_sectors;
static uint16_t g_next_audio_chunk;
static uint8_t g_mp2_loaded;      /* external asset preloaded into RAM */
static uint8_t g_mp2_interleaved; /* the PCFV stream carries the MP2 */
static uint8_t g_mp2_enabled;
static uint32_t g_mp2_size_bytes;
static uint8_t g_mp2_decode_budget_used;
static uint32_t g_mp2_preroll_ring_at_exit;
static uint32_t g_mp2_preroll_guard_count;
static uint32_t g_mp2_sync_pauses;
static uint32_t g_mp2_sync_resumes;

static uint32_t ring_used(void) { return pcfx_mp2_async_ring_used(); }

int pcfv_audio_attach(const PcfvStreamInfo *s)
{
    uint16_t i;
    s_stream = *s;
    g_mp2_interleaved = 0u;
    if (s->audio_codec != PCFV_AUDIO_CODEC_MP2) return g_mp2_loaded;
    for (i = 0; i < s->audio_chunk_count; ++i)
        if (s->audio_chunks[i].sectors > PCFX_MP2_STREAM_CHUNK_MAX_SECTORS) return 0;
    g_mp2_interleaved = 1u;
    return 1;
}

void pcfv_audio_reset(void)
{
    /* g_mp2_loaded/g_mp2_size_bytes survive: the preload precedes open. */
    g_mp2_enabled = 0u;
    g_mp2_interleaved = 0u;
    g_mp2_decode_budget_used = 0u;
    g_mp2_pending_byte_offset = 0u;
    g_mp2_pending_sectors = 0u;
    g_next_audio_chunk = 0u;
    g_mp2_sync_pauses = 0u;
    g_mp2_sync_resumes = 0u;
}

void pcfv_audio_seek(uint16_t frame)
{
    uint64_t target_sample;
    uint32_t target_byte, margin = KJMP2_MAX_FRAME_SIZE;
    uint16_t ci = 0, i;

    if (!g_mp2_interleaved) return; /* a preloaded asset always plays from 0 */
    if (g_mp2_enabled) pcfx_mp2_async_stop(&g_mp2_async);
    g_mp2_enabled = 0u;
    g_mp2_pending_byte_offset = 0u;
    g_mp2_pending_sectors = 0u;
    if (!s_stream.audio_bytes || !s_stream.fps_num || !s_stream.frame_count) return;

    /* Begin at the last chunk that starts at least one frame before the
       target; the decoder then drops whole frames up to the target. */
    target_sample = (uint64_t)frame * s_stream.audio_rate_hz * s_stream.fps_den / s_stream.fps_num;
    target_byte = (uint32_t)(((uint64_t)s_stream.audio_bytes * frame) / s_stream.frame_count);
    target_byte = target_byte > margin ? target_byte - margin : 0u;
    for (i = 0; i < s_stream.audio_chunk_count; ++i) {
        if (s_stream.audio_chunks[i].byte_offset <= target_byte) ci = i;
        else break;
    }
    g_next_audio_chunk = ci;
    if (pcfx_mp2_stream_begin_at(&g_mp2_async, s_stream.audio_bytes,
                                 s_stream.audio_chunk_count ? s_stream.audio_chunks[ci].byte_offset : 0u,
                                 (uint32_t)(target_sample / KJMP2_SAMPLES_PER_FRAME)))
        g_mp2_enabled = 1u;
}

/* MP2 is decoded in the background; the player never waits for a preroll. */
int pcfv_audio_preroll_ready(void) { return 1; }

static int fetch_next_chunk(void)
{
    const PcfvAudioChunk *c;
    if (!g_mp2_interleaved || !g_mp2_enabled || pcfv_cd_busy()) return 0;
    if (g_next_audio_chunk >= s_stream.audio_chunk_count) return 0;
    c = &s_stream.audio_chunks[g_next_audio_chunk];
    if (!c->sectors) { g_next_audio_chunk++; return 1; }
    if (pcfx_mp2_stream_free_bytes(&g_mp2_async) < c->sectors * PCFV_SECTOR_SIZE) return 0;
    g_mp2_pending_byte_offset = c->byte_offset;
    g_mp2_pending_sectors = c->sectors;
    if (!pcfv_cd_read_audio(s_stream.lba + c->sector, PCFV_MP2_KRAM_WORD_ADDR, c->sectors)) {
        g_mp2_pending_byte_offset = 0u;
        g_mp2_pending_sectors = 0u;
        return 0;
    }
    return 1;
}

static int should_fetch_urgent(void)
{
    if (!g_mp2_interleaved || !g_mp2_enabled) return 0;
    if (g_next_audio_chunk >= s_stream.audio_chunk_count) return 0;
    if (!pcfx_mp2_async_started(&g_mp2_async)) {
        /* Hidden boot: video fills its read-ahead first, so startup cannot
           deadlock on audio reads sharing the one CD channel. */
        if (!pcfv_video_frames_presented() && pcfv_video_ready_count() < pcfv_video_prebuffer_target()) return 0;
        return ring_used() < PCFV_MP2_START_RING_SAMPLES;
    }
    if (ring_used() < PCFV_MP2_CRITICAL_RING_SAMPLES) return 1;
    if (ring_used() < PCFV_MP2_URGENT_RING_SAMPLES &&
        pcfx_mp2_stream_buffered_bytes(&g_mp2_async) < PCFV_MP2_URGENT_BYTES) return 1;
    return 0;
}

int pcfv_audio_fetch_urgent(void) { return should_fetch_urgent() ? fetch_next_chunk() : 0; }

int pcfv_audio_fetch_background(void)
{
    if (!g_mp2_interleaved || !g_mp2_enabled) return 0;
    if (g_next_audio_chunk >= s_stream.audio_chunk_count) return 0;
    if (pcfv_video_ready_count() < 10u && ring_used() >= PCFV_MP2_URGENT_RING_SAMPLES) return 0;
    if (pcfx_mp2_stream_buffered_bytes(&g_mp2_async) >= PCFV_MP2_PREFETCH_LOW_BYTES) return 0;
    return fetch_next_chunk();
}

void pcfv_audio_cd_done(void)
{
    uint32_t bytes = g_mp2_pending_sectors * PCFV_SECTOR_SIZE;
    uint8_t *dst;
    if (g_mp2_pending_byte_offset + bytes > s_stream.audio_bytes)
        bytes = s_stream.audio_bytes - g_mp2_pending_byte_offset;
    dst = pcfx_mp2_stream_tail(&g_mp2_async, bytes);
    if (dst) {
        pcfv_kram_read(PCFV_MP2_KRAM_WORD_ADDR, dst, bytes);
        if (pcfx_mp2_stream_commit(&g_mp2_async, g_mp2_pending_byte_offset, bytes) &&
            g_next_audio_chunk < s_stream.audio_chunk_count)
            g_next_audio_chunk++;
    }
    g_mp2_pending_byte_offset = 0u;
    g_mp2_pending_sectors = 0u;
}

void pcfv_audio_cd_failed(void)
{
    g_mp2_pending_byte_offset = 0u;
    g_mp2_pending_sectors = 0u;
}

void pcfv_audio_new_field(void) { g_mp2_decode_budget_used = 0u; }

static void decode_budget(uint32_t frames)
{
    uint32_t used;
    if (!g_mp2_enabled || g_mp2_decode_budget_used) return;
    g_mp2_decode_budget_used = 1u;
    used = ring_used();
    if (!pcfx_mp2_async_started(&g_mp2_async)) {
        if (used < PCFV_MP2_START_RING_SAMPLES) (void)pcfx_mp2_async_update(&g_mp2_async, frames);
        return;
    }
    if (used < PCFV_MP2_DECODE_LOW_SAMPLES) (void)pcfx_mp2_async_update(&g_mp2_async, frames);
}

/* Diagnostic lead meter only.  Never stop the PSG timer for A/V sync: the
   stop/resume is audible even when the ring never underflows.  Video follows
   the audio clock instead and may skip a stale frame if it falls behind. */
static void sync_meter(void)
{
    uint64_t limit;
    uint32_t presented = pcfv_video_frames_presented();
    if (!g_mp2_enabled || !pcfx_mp2_async_playing() || !presented || !s_stream.fps_num) return;
    limit = (uint64_t)(presented + PCFV_MP2_SYNC_LEAD_FRAMES) * s_stream.audio_rate_hz * s_stream.fps_den;
    limit /= s_stream.fps_num;
    if ((uint64_t)pcfx_mp2_async_samples_emitted() >= limit) g_mp2_sync_pauses++;
}

void pcfv_audio_service(void)
{
    decode_budget(PCFV_MP2_FIELD_BUDGET);
    sync_meter();
}

int pcfv_audio_boot_prefill_pending(void)
{
    return g_mp2_interleaved && g_mp2_enabled && ring_used() < PCFV_MP2_START_RING_SAMPLES;
}

void pcfv_audio_boot_prefill_done(uint32_t fields)
{
    if (!g_mp2_interleaved || !g_mp2_enabled) return;
    g_mp2_preroll_ring_at_exit = ring_used();
    g_mp2_preroll_guard_count = fields;
}

/* A preloaded asset opens only after the RAINBOW boot: arming the PSG timer
   before PCFV's prebuffer left headless validation on a black screen. */
void pcfv_audio_after_boot(void)
{
    if (g_mp2_interleaved || !g_mp2_loaded || g_mp2_enabled || !g_mp2_size_bytes) return;
    if (pcfx_mp2_async_open(&g_mp2_async, g_pcfx_mp2_preload_buf, g_mp2_size_bytes)) {
        g_mp2_enabled = 1u;
        (void)pcfx_mp2_async_update(&g_mp2_async, PCFV_MP2_PREFILL_FRAMES);
    } else {
        g_mp2_loaded = 0u;
    }
}

void pcfv_audio_visible_field(void)
{
    uint32_t threshold = pcfx_mp2_async_started(&g_mp2_async) ?
        PCFV_MP2_URGENT_RING_SAMPLES : PCFV_MP2_START_RING_SAMPLES;
    if (!g_mp2_enabled) return;
    if (ring_used() >= threshold) {
        uint8_t was_playing = (uint8_t)pcfx_mp2_async_playing();
        pcfx_mp2_async_start(&g_mp2_async);
        if (!was_playing && pcfx_mp2_async_playing()) g_mp2_sync_resumes++;
    }
}

int pcfv_audio_playing(void) { return pcfx_mp2_async_playing(); }
int pcfv_audio_started(void) { return g_mp2_enabled && pcfx_mp2_async_started(&g_mp2_async); }
int pcfv_audio_is_clock(void) { return g_mp2_interleaved && g_mp2_enabled; }
uint32_t pcfv_audio_clock(void) { return pcfx_mp2_async_clock(&g_mp2_async); }
uint32_t pcfv_audio_buffered(void) { return ring_used(); }

int pcfv_audio_complete(void)
{
    if (g_mp2_interleaved && g_mp2_enabled) return pcfx_mp2_async_done(&g_mp2_async);
    return 1;
}

/* The PSG ring holds its position across a stop/start, so pause is exact. */
void pcfv_audio_set_paused(int paused)
{
    if (!g_mp2_enabled || !pcfx_mp2_async_started(&g_mp2_async)) return;
    if (paused) pcfx_mp2_async_stop(&g_mp2_async);
    else pcfx_mp2_async_start(&g_mp2_async);
}

void pcfv_audio_stop(void)
{
    if (g_mp2_enabled) pcfx_mp2_async_stop(&g_mp2_async);
}

/* ---- Public MP2 helpers declared in pcfx_pcfv_player.h ------------------ */

int pcfx_pcfv_mp2_load_from_cd(uint32_t mp2_lba, uint32_t mp2_size_bytes)
{
    g_mp2_loaded = 0u;
    g_mp2_enabled = 0u;
    g_mp2_size_bytes = 0u;
    if (!pcfx_mp2_load_cd(mp2_lba, mp2_size_bytes)) return 0;
    /* Timer/IRQ setup and the decode preroll wait for pcfv_audio_after_boot(). */
    g_mp2_loaded = 1u;
    g_mp2_size_bytes = mp2_size_bytes;
    return 1;
}

uint32_t pcfx_pcfv_mp2_frames_decoded(void) { return g_mp2_async.frames_decoded; }
uint32_t pcfx_pcfv_mp2_underflows(void) { return pcfx_mp2_async_underflows(); }
uint32_t pcfx_pcfv_mp2_ring_used(void) { return ring_used(); }
uint32_t pcfx_pcfv_mp2_error_code(void) { return g_mp2_async.error_code; }
uint32_t pcfx_pcfv_mp2_error_offset(void) { return g_mp2_async.error_offset; }
