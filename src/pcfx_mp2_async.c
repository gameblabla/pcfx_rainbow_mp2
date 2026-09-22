#include <stdint.h>
#include <string.h>
#include <pcfx/v810.h>
#include <eris/cd.h>
#include <eris/scsi.h>
#include "pcfx_mp2_async.h"
#include "psg_sample.h"

uint8_t g_pcfx_mp2_preload_buf[PCFX_MP2_PRELOAD_MAX_BYTES] __attribute__((aligned(2048)));
static uint8_t g_mp2_stream_buf[PCFX_MP2_STREAM_BUF_BYTES] __attribute__((aligned(4)));
static uint8_t g_mp2_chunk_tmp[PCFX_MP2_STREAM_CHUNK_MAX_SECTORS * 2048u] __attribute__((aligned(2048)));
static kjmp2v_psg10_sample_t g_mp2_frame_tmp[KJMP2_SAMPLES_PER_FRAME] __attribute__((aligned(4)));

static uint32_t mp2_ring_used_fast(void) { return g_mp2psg10_write_pos - g_mp2psg10_read_pos; }
static uint32_t mp2_ring_free_fast(void) { return (PSG10MP2_RING_SIZE - 1u) - mp2_ring_used_fast(); }

uint32_t pcfx_mp2_timer_period_for_rate(uint32_t rate)
{
    if (rate == 22050u) return 65u;
    if (rate == 24000u) return 60u;
    return 89u; /* 16 kHz base for 89.5-tick fractional timer */
}

uint32_t pcfx_mp2_channels(const uint8_t *src)
{
    if (!src || src[0] != 0xff || ((src[1] & 0xf6u) != 0xf4u)) return 0;
    return (((src[3] >> 6) & 3u) == 3u) ? 1u : 2u;
}

uint32_t pcfx_mp2_frame_size(const uint8_t *frame)
{
    unsigned bit_rate_index_minus1;
    unsigned sampling_frequency;
    unsigned padding_bit;
    if (!frame || frame[0] != 0xff || ((frame[1] & 0xf6u) != 0xf4u)) return 0;
    bit_rate_index_minus1 = ((unsigned)((frame[2] >> 4) & 15u)) - 1u;
    if (bit_rate_index_minus1 > 13u) return 0;
    sampling_frequency = (unsigned)((frame[2] >> 2) & 3u);
    if (sampling_frequency == 3u) return 0;
    if ((frame[1] & 0x08u) == 0u) {
        static const uint16_t bitrates_lsf[14] = {8,16,24,32,40,48,56,64,80,96,112,128,144,160};
        static const uint16_t rates_lsf[3] = {22050,24000,16000};
        padding_bit = (unsigned)((frame[2] >> 1) & 1u);
        return (uint32_t)((144000ul * bitrates_lsf[bit_rate_index_minus1] / rates_lsf[sampling_frequency]) + padding_bit);
    } else {
        static const uint16_t bitrates_mpeg1[14] = {32,48,56,64,80,96,112,128,160,192,224,256,320,384};
        static const uint16_t rates_mpeg1[3] = {44100,48000,32000};
        padding_bit = (unsigned)((frame[2] >> 1) & 1u);
        return (uint32_t)((144000ul * bitrates_mpeg1[bit_rate_index_minus1] / rates_mpeg1[sampling_frequency]) + padding_bit);
    }
}

/* liberis' eris_cd_read path can clobber callee-saved registers with this
   toolchain/liberis combination. Save the live register set around it. */
static uint32_t pcfx_cd_read_safe(uint32_t lba, uint8_t *buf, uint32_t size)
{
    uint32_t ret;
    __asm__ volatile (
        "addi -20, sp, sp\n"
        "st.w r16, 0[sp]\n"
        "st.w r17, 4[sp]\n"
        "st.w r18, 8[sp]\n"
        "st.w r19, 12[sp]\n"
        "st.w lp, 16[sp]\n"
        "mov %1, r6\n"
        "mov %2, r7\n"
        "mov %3, r8\n"
        "jal _eris_cd_read\n"
        "mov r10, %0\n"
        "ld.w 0[sp], r16\n"
        "ld.w 4[sp], r17\n"
        "ld.w 8[sp], r18\n"
        "ld.w 12[sp], r19\n"
        "ld.w 16[sp], lp\n"
        "addi 20, sp, sp\n"
        : "=r"(ret)
        : "r"(lba), "r"(buf), "r"(size)
        : "r6", "r7", "r8", "r10", "r11", "r12", "r13", "r14", "r15", "memory"
    );
    return ret;
}

int pcfx_mp2_load_cd(uint32_t lba, uint32_t size_bytes)
{
    uint32_t rounded;
    if (!lba || !size_bytes || size_bytes > PCFX_MP2_PRELOAD_MAX_BYTES) return 0;
    rounded = (size_bytes + 2047u) & ~2047u;
    scsi_reset();
    return pcfx_cd_read_safe(lba, g_pcfx_mp2_preload_buf, rounded) ? 1 : 0;
}

static int pcfx_mp2_init_decoder_from_header(PcfxMp2Async *s, const uint8_t *src)
{
    s->sample_rate = (uint32_t)kjmp2v_get_sample_rate(src);
    s->channels = pcfx_mp2_channels(src);
    if (s->sample_rate != 16000u && s->sample_rate != 22050u && s->sample_rate != 24000u) {
        s->error_code = 1u;
        return 0;
    }
    if (s->channels != 1u && s->channels != 2u) {
        s->error_code = 3u;
        return 0;
    }
    kjmp2v_init(&s->ctx);
    PSG10MP2_RingReset();
    PSG10MP2_SetPairVolumes(31u, 12u);
    s->timer_inited = 0u;
    s->opened = 1u;
    return 1;
}

int pcfx_mp2_async_open(PcfxMp2Async *s, const uint8_t *src, uint32_t size_bytes)
{
    if (!s || !src || size_bytes < 4u) return 0;
    memset(s, 0, sizeof(*s));
    s->src = src;
    s->pos = src;
    s->end = src + size_bytes;
    s->size = size_bytes;
    s->bytes_loaded = size_bytes;
    s->input_eof = 1u;
    return pcfx_mp2_init_decoder_from_header(s, src);
}

static void pcfx_mp2_stream_compact(PcfxMp2Async *s)
{
    uint32_t remain;
    if (!s || !s->stream_mode || s->pos == g_mp2_stream_buf) return;
    remain = (uint32_t)(s->end - s->pos);
    if (remain) memmove(g_mp2_stream_buf, s->pos, remain);
    s->src = g_mp2_stream_buf;
    s->pos = g_mp2_stream_buf;
    s->end = g_mp2_stream_buf + remain;
}

int pcfx_mp2_stream_begin(PcfxMp2Async *s, uint32_t total_size_bytes)
{
    if (!s || total_size_bytes < 4u) return 0;
    memset(s, 0, sizeof(*s));
    s->src = g_mp2_stream_buf;
    s->pos = g_mp2_stream_buf;
    s->end = g_mp2_stream_buf;
    s->size = total_size_bytes;
    s->stream_mode = 1u;
    return 1;
}

uint32_t pcfx_mp2_stream_buffered_bytes(const PcfxMp2Async *s)
{
    if (!s || !s->stream_mode) return 0;
    return (uint32_t)(s->end - s->pos);
}

uint32_t pcfx_mp2_stream_free_bytes(PcfxMp2Async *s)
{
    uint32_t used;
    if (!s || !s->stream_mode) return 0;
    pcfx_mp2_stream_compact(s);
    used = (uint32_t)(s->end - g_mp2_stream_buf);
    return (used < PCFX_MP2_STREAM_BUF_BYTES) ? (PCFX_MP2_STREAM_BUF_BYTES - used) : 0u;
}

int pcfx_mp2_stream_append_bytes(PcfxMp2Async *s, uint32_t byte_offset, const uint8_t *data, uint32_t bytes)
{
    uint32_t free_bytes;
    if (!s || !s->stream_mode || !data) return 0;
    if (byte_offset != s->bytes_loaded) return 0;
    if (byte_offset + bytes > s->size) bytes = s->size - byte_offset;
    free_bytes = pcfx_mp2_stream_free_bytes(s);
    if (free_bytes < bytes) return 0;
    if (bytes) {
        memcpy((void *)s->end, data, bytes);
        s->end += bytes;
        s->bytes_loaded += bytes;
    }
    if (s->bytes_loaded >= s->size) s->input_eof = 1u;
    if (!s->opened && (uint32_t)(s->end - s->pos) >= 4u) {
        if (!pcfx_mp2_init_decoder_from_header(s, s->pos)) {
            s->done = 1u;
            return 0;
        }
    }
    return 1;
}

int pcfx_mp2_stream_read_cd(PcfxMp2Async *s, uint32_t lba, uint32_t byte_offset, uint32_t sectors)
{
    uint32_t read_bytes;
    uint32_t real_bytes;
    uint32_t free_bytes;
    if (!s || !s->stream_mode || !lba || !sectors || sectors > PCFX_MP2_STREAM_CHUNK_MAX_SECTORS) return 0;
    if (byte_offset != s->bytes_loaded) return 0;
    read_bytes = sectors * 2048u;
    real_bytes = read_bytes;
    if (byte_offset + real_bytes > s->size) real_bytes = s->size - byte_offset;
    free_bytes = pcfx_mp2_stream_free_bytes(s);
    if (free_bytes < real_bytes) return 0;
    scsi_reset();
    if (!pcfx_cd_read_safe(lba, g_mp2_chunk_tmp, read_bytes)) {
        scsi_reset();
        return 0;
    }
    scsi_reset();
    return pcfx_mp2_stream_append_bytes(s, byte_offset, g_mp2_chunk_tmp, real_bytes);
}

uint32_t pcfx_mp2_async_update(PcfxMp2Async *s, uint32_t frame_budget)
{
    uint32_t decoded = 0;
    if (!s || s->done || frame_budget == 0u) return 0;
    if (!s->opened) {
        if ((uint32_t)(s->end - s->pos) < 4u) return 0;
        if (!pcfx_mp2_init_decoder_from_header(s, s->pos)) {
            s->done = 1u;
            return 0;
        }
    }
    while (frame_budget-- && ((uint32_t)(s->end - s->pos) >= 4u)) {
        uint32_t used;
        uint32_t fsz;
        unsigned long got;
        kjmp2v_psg10_sample_t *frame_out = g_mp2_frame_tmp;
        int direct_ring = 0;
        used = mp2_ring_used_fast();
        if (used > s->max_ring_used) s->max_ring_used = used;
        if (mp2_ring_free_fast() < KJMP2_SAMPLES_PER_FRAME) break;
        fsz = pcfx_mp2_frame_size(s->pos);
        if (!fsz || fsz > KJMP2_MAX_FRAME_SIZE) {
            if (s->stream_mode && !s->input_eof) break;
            s->error_code = 2u;
            s->error_offset = s->bytes_consumed;
            s->done = 1u;
            PSG10MP2_SetDecodeDone();
            break;
        }
        if (s->pos + fsz > s->end) {
            if (s->stream_mode && !s->input_eof) break;
            s->done = 1u;
            PSG10MP2_SetDecodeDone();
            break;
        }
        direct_ring = PSG10MP2_RingGetContiguousFrame(&frame_out);
        got = kjmp2v_decode_frame_psg10(&s->ctx, s->pos, frame_out);
        if (got != fsz) {
            s->error_code = 4u;
            s->error_offset = s->bytes_consumed;
            s->done = 1u;
            PSG10MP2_SetDecodeDone();
            break;
        }
        if (direct_ring) PSG10MP2_RingCommitFrame();
        else PSG10MP2_RingPushFrame(g_mp2_frame_tmp);
        s->pos += fsz;
        s->bytes_consumed += fsz;
        s->frames_decoded++;
        s->samples_written += KJMP2_SAMPLES_PER_FRAME;
        s->last_frame_bytes = fsz;
        decoded++;
    }
    if (!s->done && s->input_eof && ((uint32_t)(s->end - s->pos) < 4u)) {
        s->done = 1u;
        PSG10MP2_SetDecodeDone();
    }
    return decoded;
}

void pcfx_mp2_async_start(PcfxMp2Async *s)
{
    if (!s || !s->opened) return;
    if (s->started && PSG10MP2_IsPlaying()) return;
    if (mp2_ring_used_fast() == 0u) return;
    if (!s->timer_inited) {
        if (s->sample_rate == 16000u) {
            /* 16 kHz lands between integer timer periods: period 90 is slow,
               period 89 is fast.  Alternate 89/90 with 0.5 fractional duty. */
            if (s->channels == 2u) PSG10MP2_InitTimerFractionalStereo(89, 32768u);
            else PSG10MP2_InitTimerFractional(89, 32768u);
        } else {
            if (s->channels == 2u) PSG10MP2_InitTimerStereo((int)pcfx_mp2_timer_period_for_rate(s->sample_rate));
            else PSG10MP2_InitTimer((int)pcfx_mp2_timer_period_for_rate(s->sample_rate));
        }
        s->timer_inited = 1u;
    }
    if (s->channels == 2u) PSG10MP2_StartStereo();
    else PSG10MP2_StartMono();
    s->started = 1u;
}

void pcfx_mp2_async_stop(PcfxMp2Async *s)
{
    if (!s) return;
    PSG10MP2_Stop();
    if (s->timer_inited) PSG10MP2_StopTimer();
    /* Keep started latched across sync pauses; pause/resume should not
       re-enter the large first-start threshold or strand tail samples. */
}

int pcfx_mp2_async_started(const PcfxMp2Async *s) { return (s && s->started) ? 1 : 0; }
int pcfx_mp2_async_done(const PcfxMp2Async *s) { return (s && s->done && !PSG10MP2_IsPlaying() && mp2_ring_used_fast() == 0u) ? 1 : 0; }
uint32_t pcfx_mp2_async_underflows(void) { return PSG10MP2_GetUnderflows(); }
uint32_t pcfx_mp2_async_ring_used(void) { return mp2_ring_used_fast(); }

uint32_t pcfx_mp2_async_samples_emitted(void) { return g_mp2psg10_read_pos; }
int pcfx_mp2_async_playing(void) { return PSG10MP2_IsPlaying() ? 1 : 0; }
