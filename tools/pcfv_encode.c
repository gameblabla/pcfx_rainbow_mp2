/* pcfv_encode.c -- single front-end PC-FX video encoder.

   This tool turns one ffmpeg-supported movie file (MKV/MP4/etc.) into one
   sector-aligned .pcfv file usable by the PC-FX runtime in examples/video_player.

   Pipeline:
     1. ffmpeg decodes/scales video to 256x240 PPM frames.
     2. The embedded HuC6271/RAINBOW YUV/DCT encoder compresses each frame.
     3. ffmpeg decodes/resamples audio to mono signed 16-bit PCM.
     4. The HuC6230 ADPCM encoder from HuC wav2vox is linked directly.
     5. Video frames and ADPCM are muxed into one PCFV stream.

   ffmpeg is intentionally an external executable dependency.  Everything after
   decode/resample is compiled into this single host executable. */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/wait.h>

#include "../include/pcfv_format.h"
#include "wav2vox/wav2vox.h"
#include "wav2vox/adpcmoki.h"

/* Pull the RAINBOW encoder into this translation unit.  Its original main()
   and usage() are renamed; we call its internal read_image() and encode_frame()
   helpers below. */
#define main rainbow_yuvdct_standalone_main
#define usage rainbow_yuvdct_usage
#include "rainbow_yuvdct_encode.c"
#undef usage
#undef main

typedef struct {
    uint32_t video_sector;
    uint32_t video_size;
    uint32_t video_sectors;
    uint32_t video_crc32;
    uint32_t audio_sector;
    uint32_t audio_sectors;
    uint32_t audio_byte_offset;
    uint32_t flags;
} PcfvEntry;

typedef struct {
    uint8_t *data;
    uint32_t size;
    uint32_t sectors;
    uint32_t crc32;
} EncodedFrame;

typedef struct {
    const char *ffmpeg;
    const char *tmp_base;
    uint16_t fps;
    /* 0 means encode until ffmpeg reaches EOF.  Non-zero is a hard cap. */
    uint16_t frames;
    uint16_t quality;
    double rdo;
    uint16_t max_frame_sectors;
    uint16_t min_quality;
    uint16_t retry_step;
    uint32_t audio_rate;
    double audio_gain;
    uint32_t audio_lowpass;
    uint32_t audio_highpass;
    uint32_t preroll_sectors;
    uint32_t refill_sectors;
    uint32_t ring_half_bytes;
    int no_audio;
    int keep_tmp;
    uint16_t jobs;
} Options;

static void pcfv_die(const char *msg) {
    fprintf(stderr, "pcfv_encode: %s\n", msg);
    exit(1);
}

static void pcfv_die_errno(const char *msg) {
    fprintf(stderr, "pcfv_encode: %s: %s\n", msg, strerror(errno));
    exit(1);
}

static uint32_t pcfv_ceil_div(uint32_t a, uint32_t b) { return (a + b - 1u) / b; }
static uint16_t parse_u16_range(const char *s, const char *name, int lo, int hi) {
    char *end = NULL;
    long v = strtol(s, &end, 0);
    if (!s[0] || (end && *end) || v < lo || v > hi) {
        fprintf(stderr, "bad %s: %s\n", name, s);
        exit(2);
    }
    return (uint16_t)v;
}

static uint32_t parse_u32_range(const char *s, const char *name, uint32_t lo, uint32_t hi) {
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 0);
    if (!s[0] || (end && *end) || v < lo || v > hi) {
        fprintf(stderr, "bad %s: %s\n", name, s);
        exit(2);
    }
    return (uint32_t)v;
}

static double parse_double_range(const char *s, const char *name, double lo, double hi) {
    char *end = NULL;
    double v = strtod(s, &end);
    if (!s[0] || (end && *end) || v < lo || v > hi) {
        fprintf(stderr, "bad %s: %s\n", name, s);
        exit(2);
    }
    return v;
}

static char *shell_quote(const char *s) {
    size_t len = 2;
    const char *p;
    for (p = s; *p; ++p) len += (*p == '\'') ? 4 : 1;
    char *out = (char *)malloc(len + 1);
    if (!out) pcfv_die_errno("malloc quote");
    char *q = out;
    *q++ = '\'';
    for (p = s; *p; ++p) {
        if (*p == '\'') { memcpy(q, "'\\''", 4); q += 4; }
        else *q++ = *p;
    }
    *q++ = '\'';
    *q = 0;
    return out;
}

static int run_cmd(const char *cmd) {
    fprintf(stderr, "+ %s\n", cmd);
    int rc = system(cmd);
    return rc == 0;
}

static long file_size_path(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return -1; }
    long sz = ftell(fp);
    fclose(fp);
    return sz;
}

static uint8_t *read_whole_file(const char *path, uint32_t *size_out) {
    FILE *fp = fopen(path, "rb");
    if (!fp) pcfv_die_errno(path);
    if (fseek(fp, 0, SEEK_END) != 0) pcfv_die_errno("fseek");
    long sz = ftell(fp);
    if (sz < 0 || sz > 0x7fffffffL) pcfv_die("bad file size");
    rewind(fp);
    uint8_t *p = (uint8_t *)malloc(sz ? (size_t)sz : 1u);
    if (!p) pcfv_die_errno("malloc file");
    if (sz && fread(p, 1, (size_t)sz, fp) != (size_t)sz) pcfv_die_errno("read file");
    fclose(fp);
    *size_out = (uint32_t)sz;
    return p;
}

static void write_zeroes(FILE *fp, uint32_t bytes) {
    static const uint8_t zero[PCFV_SECTOR_SIZE] = {0};
    while (bytes) {
        uint32_t n = bytes > PCFV_SECTOR_SIZE ? PCFV_SECTOR_SIZE : bytes;
        if (fwrite(zero, 1, n, fp) != n) pcfv_die_errno("write zeroes");
        bytes -= n;
    }
}

static void wr16(FILE *fp, uint16_t v) {
    fputc((int)(v & 0xff), fp);
    fputc((int)((v >> 8) & 0xff), fp);
}

static void wr32(FILE *fp, uint32_t v) {
    wr16(fp, (uint16_t)(v & 0xffff));
    wr16(fp, (uint16_t)(v >> 16));
}


static uint16_t default_job_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    if (n > 32) n = 32;
    return (uint16_t)n;
}

static void warm_rainbow_encoder_once(const Options *opt) {
    /* The RAINBOW encoder has a lazily-created DCT cosine table.  Warm it before
       worker threads start so the inner encoder stays read-only/thread-safe. */
    uint8_t *rgb = (uint8_t *)calloc(256u * 240u * 3u, 1u);
    if (!rgb) pcfv_die_errno("calloc warmup rgb");
    EncoderOptions ropt;
    ropt.quality = opt->quality;
    ropt.rdo_lambda = opt->rdo;
    ropt.all_ff = 0;
    ropt.verbose = 0;
    Vec v = encode_frame(rgb, 256, 240, &ropt);
    free(v.p);
    free(rgb);
}

static EncodedFrame encode_one_rgb(uint8_t *rgb, const Options *opt, uint16_t frame_index) {
    uint16_t q = opt->quality;
    double rdo = opt->rdo;
    Vec out = {0};
    int below_preferred_floor = 0;

    for (;;) {
        EncoderOptions ropt;
        ropt.quality = q;
        ropt.rdo_lambda = rdo;
        ropt.all_ff = 0;
        ropt.verbose = 0;
        out = encode_frame(rgb, 256, 240, &ropt);
        uint32_t sectors = pcfv_ceil_div((uint32_t)out.len, PCFV_SECTOR_SIZE);

        if (!opt->max_frame_sectors || sectors <= opt->max_frame_sectors) break;

        free(out.p);
        out.p = NULL;
        out.len = 0;
        out.cap = 0;

        /* The sector budget is a runtime safety constraint, not just a rate hint:
           a frame larger than the player's KRAM slot will corrupt the next slot.
           First retry down to the user's preferred floor.  If the picture still
           does not fit, continue with an emergency fit pass rather than emitting
           an oversized frame that would stop or glitch playback. */
        if (q > opt->min_quality) {
            if (q > opt->min_quality + opt->retry_step) q = (uint16_t)(q - opt->retry_step);
            else q = opt->min_quality;
            rdo += 2.0;
        } else if (q > 1u) {
            below_preferred_floor = 1;
            q = (q > opt->retry_step) ? (uint16_t)(q - opt->retry_step) : 1u;
            rdo += 4.0;
        } else if (rdo < 1000.0) {
            below_preferred_floor = 1;
            rdo += 8.0;
        } else {
            fprintf(stderr, "frame %u cannot be forced under %u sector(s) even at minimum quality\n",
                    frame_index, opt->max_frame_sectors);
            exit(1);
        }
    }

    EncodedFrame ef;
    ef.data = out.p;
    ef.size = (uint32_t)out.len;
    ef.sectors = pcfv_ceil_div(ef.size, PCFV_SECTOR_SIZE);
    ef.crc32 = crc32_update(0, ef.data, ef.size);
    if (below_preferred_floor) {
        fprintf(stderr, "notice: frame %u needed emergency sector-fit below preferred min quality; final q=%u sectors=%u\n",
                frame_index, q, ef.sectors);
    }
    return ef;
}

typedef struct VideoJob {
    uint16_t frame_index;
    uint8_t *rgb;
    struct VideoJob *next;
} VideoJob;

typedef struct {
    const Options *opt;
    EncodedFrame *frames;
    uint32_t frame_cap;
    uint32_t frame_count;
    uint32_t completed;
    uint32_t queued;
    uint32_t max_queue;
    int producer_done;
    int failed;
    VideoJob *head;
    VideoJob *tail;
    pthread_mutex_t mutex;
    pthread_cond_t have_work;
    pthread_cond_t have_room;
} VideoEncodeContext;

static void ensure_frame_capacity(VideoEncodeContext *ctx, uint32_t need) {
    if (need <= ctx->frame_cap) return;
    uint32_t ncap = ctx->frame_cap ? ctx->frame_cap : 256u;
    while (ncap < need) {
        ncap *= 2u;
        if (ncap > 65535u) ncap = 65535u;
        if (ncap < need && ncap == 65535u) pcfv_die("PCFV v1 frame count limit exceeded");
    }
    EncodedFrame *nf = (EncodedFrame *)realloc(ctx->frames, ncap * sizeof(*ctx->frames));
    if (!nf) pcfv_die_errno("realloc encoded frames");
    memset(nf + ctx->frame_cap, 0, (ncap - ctx->frame_cap) * sizeof(*nf));
    ctx->frames = nf;
    ctx->frame_cap = ncap;
}

static void enqueue_video_job(VideoEncodeContext *ctx, uint16_t frame_index, uint8_t *rgb) {
    VideoJob *j = (VideoJob *)calloc(1, sizeof(*j));
    if (!j) pcfv_die_errno("calloc video job");
    j->frame_index = frame_index;
    j->rgb = rgb;

    pthread_mutex_lock(&ctx->mutex);
    while (ctx->queued >= ctx->max_queue && !ctx->failed) pthread_cond_wait(&ctx->have_room, &ctx->mutex);
    if (ctx->failed) {
        pthread_mutex_unlock(&ctx->mutex);
        free(j->rgb);
        free(j);
        pcfv_die("video worker failed");
    }
    if (ctx->tail) ctx->tail->next = j;
    else ctx->head = j;
    ctx->tail = j;
    ctx->queued++;
    pthread_cond_signal(&ctx->have_work);
    pthread_mutex_unlock(&ctx->mutex);
}

static VideoJob *dequeue_video_job(VideoEncodeContext *ctx) {
    VideoJob *j;
    pthread_mutex_lock(&ctx->mutex);
    while (!ctx->head && !ctx->producer_done) pthread_cond_wait(&ctx->have_work, &ctx->mutex);
    j = ctx->head;
    if (j) {
        ctx->head = j->next;
        if (!ctx->head) ctx->tail = NULL;
        ctx->queued--;
        pthread_cond_signal(&ctx->have_room);
    }
    pthread_mutex_unlock(&ctx->mutex);
    return j;
}

static void *video_worker_main(void *opaque) {
    VideoEncodeContext *ctx = (VideoEncodeContext *)opaque;
    for (;;) {
        VideoJob *j = dequeue_video_job(ctx);
        if (!j) {
            pthread_mutex_lock(&ctx->mutex);
            if (ctx->producer_done) { pthread_mutex_unlock(&ctx->mutex); break; }
            pthread_mutex_unlock(&ctx->mutex);
            continue;
        }
        EncodedFrame ef = encode_one_rgb(j->rgb, ctx->opt, j->frame_index);
        free(j->rgb);
        pthread_mutex_lock(&ctx->mutex);
        ctx->frames[j->frame_index] = ef;
        ctx->completed++;
        if ((ctx->completed & 15u) == 0u || ctx->completed == ctx->frame_count) {
            fprintf(stderr, "encoded %u/%u frames\n", (unsigned)ctx->completed, (unsigned)ctx->frame_count);
        }
        pthread_mutex_unlock(&ctx->mutex);
        free(j);
    }
    return NULL;
}

static FILE *open_ffmpeg_rawvideo_pipe(const Options *opt, const char *input) {
    char *q_ffmpeg = shell_quote(opt->ffmpeg);
    char *q_input = shell_quote(input);
    char cmd[16384];
    if (opt->frames) {
        snprintf(cmd, sizeof(cmd),
                 "%s -hide_banner -loglevel error -y -i %s "
                 "-vf fps=%u,scale=256:240:force_original_aspect_ratio=increase:flags=lanczos,crop=256:240 "
                 "-frames:v %u -an -sn -dn -pix_fmt rgb24 -f rawvideo -",
                 q_ffmpeg, q_input, opt->fps, opt->frames);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "%s -hide_banner -loglevel error -y -i %s "
                 "-vf fps=%u,scale=256:240:force_original_aspect_ratio=increase:flags=lanczos,crop=256:240 "
                 "-an -sn -dn -pix_fmt rgb24 -f rawvideo -",
                 q_ffmpeg, q_input, opt->fps);
    }
    free(q_ffmpeg);
    free(q_input);
    fprintf(stderr, "+ %s\n", cmd);
    FILE *fp = popen(cmd, "r");
    if (!fp) pcfv_die_errno("popen ffmpeg rawvideo");
    return fp;
}

static uint16_t encode_video_from_ffmpeg(const Options *opt, const char *input, EncodedFrame **frames_out) {
    enum { FRAME_BYTES = 256 * 240 * 3 };
    VideoEncodeContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.opt = opt;
    ctx.max_queue = (uint32_t)opt->jobs * 3u + 8u;
    pthread_mutex_init(&ctx.mutex, NULL);
    pthread_cond_init(&ctx.have_work, NULL);
    pthread_cond_init(&ctx.have_room, NULL);

    warm_rainbow_encoder_once(opt);

    pthread_t *threads = (pthread_t *)calloc(opt->jobs, sizeof(*threads));
    if (!threads) pcfv_die_errno("calloc worker threads");
    for (uint16_t i = 0; i < opt->jobs; ++i) {
        if (pthread_create(&threads[i], NULL, video_worker_main, &ctx) != 0) pcfv_die_errno("pthread_create");
    }

    FILE *pipe = open_ffmpeg_rawvideo_pipe(opt, input);
    uint32_t idx = 0;
    for (;;) {
        if (idx >= 65535u) pcfv_die("PCFV v1 frame count limit exceeded (65535 frames)");
        uint8_t *rgb = (uint8_t *)malloc(FRAME_BYTES);
        if (!rgb) pcfv_die_errno("malloc raw frame");
        size_t got = fread(rgb, 1, FRAME_BYTES, pipe);
        if (got == 0) { free(rgb); break; }
        if (got != FRAME_BYTES) {
            free(rgb);
            pcfv_die("short rawvideo frame from ffmpeg");
        }
        pthread_mutex_lock(&ctx.mutex);
        ensure_frame_capacity(&ctx, idx + 1u);
        ctx.frame_count = idx + 1u;
        pthread_mutex_unlock(&ctx.mutex);
        enqueue_video_job(&ctx, (uint16_t)idx, rgb);
        idx++;
    }
    int rc = pclose(pipe);
    if (rc == -1) pcfv_die_errno("pclose ffmpeg rawvideo");
    if (rc != 0) fprintf(stderr, "warning: ffmpeg rawvideo exited with status %d\n", rc);

    pthread_mutex_lock(&ctx.mutex);
    ctx.producer_done = 1;
    pthread_cond_broadcast(&ctx.have_work);
    pthread_mutex_unlock(&ctx.mutex);
    for (uint16_t i = 0; i < opt->jobs; ++i) pthread_join(threads[i], NULL);
    free(threads);

    if (!ctx.frame_count) pcfv_die("ffmpeg produced no video frames");
    *frames_out = ctx.frames;
    fprintf(stderr, "Encoded %u video frames%s using %u worker thread(s).\n",
            (unsigned)ctx.frame_count, opt->frames ? "" : " (EOF)", opt->jobs);

    pthread_mutex_destroy(&ctx.mutex);
    pthread_cond_destroy(&ctx.have_work);
    pthread_cond_destroy(&ctx.have_room);
    return (uint16_t)ctx.frame_count;
}

static int extract_audio_pcm(const Options *opt, const char *input, const char *tmpdir, uint16_t frame_count, char *raw_path, size_t raw_path_size) {
    double duration = opt->fps ? ((double)frame_count / (double)opt->fps) : 0.0;
    snprintf(raw_path, raw_path_size, "%s/audio.s16", tmpdir);
    char *q_ffmpeg = shell_quote(opt->ffmpeg);
    char *q_input = shell_quote(input);
    char *q_raw = shell_quote(raw_path);
    char cmd[16384];
    snprintf(cmd, sizeof(cmd),
             "%s -hide_banner -loglevel error -y -i %s -t %.6f -vn -sn -dn -ac 1 "
             "-af \"highpass=f=%u,lowpass=f=%u,aresample=%u:filter_size=64:phase_shift=10,volume=%.6f\" "
             "-ar %u -sample_fmt s16 -f s16le %s",
             q_ffmpeg, q_input, duration, opt->audio_highpass, opt->audio_lowpass,
             opt->audio_rate, opt->audio_gain, opt->audio_rate, q_raw);
    free(q_ffmpeg); free(q_input); free(q_raw);
    return run_cmd(cmd);
}

static uint8_t *encode_audio_adpcm(const Options *opt, const char *pcm_path, uint32_t *bytes_out) {
    uint32_t pcm_bytes = 0;
    uint8_t *pcm_u8 = read_whole_file(pcm_path, &pcm_bytes);
    uint32_t samples = pcm_bytes / 2u;
    uint32_t adpcm_bytes = (samples + 1u) >> 1;
    uint8_t *adpcm = (uint8_t *)calloc(adpcm_bytes ? adpcm_bytes : 1u, 1u);
    if (!adpcm) pcfv_die_errno("calloc adpcm");

    OKI_ADPCM state;
    iBiasValue = 0;
    state.format = WAV_TAG_HUC6230;
    state.index = 0;
    state.value = 2048 << 3;
    state.minvalue = state.value;
    state.maxvalue = state.value;
    EncodeAdpcmPcfx((int16_t *)pcm_u8, adpcm, (int)samples, (int)(adpcm_bytes * 2u), &state);

    /* HuC wav2vox emits high nibble first.  KING consumes low nibble first from
       KRAM, so swap each byte for direct CD->KRAM playback. */
    for (uint32_t i = 0; i < adpcm_bytes; ++i) adpcm[i] = (uint8_t)((adpcm[i] >> 4) | (adpcm[i] << 4));

    free(pcm_u8);
    *bytes_out = adpcm_bytes;
    fprintf(stderr, "audio: %u PCM samples -> %u HuC6230 ADPCM bytes\n", samples, adpcm_bytes);
    (void)opt;
    return adpcm;
}

static void write_pcfv(const char *out_path, const Options *opt,
                       EncodedFrame *frames, uint16_t frame_count,
                       const uint8_t *audio, uint32_t audio_bytes) {
    uint32_t audio_sectors = audio_bytes ? pcfv_ceil_div(audio_bytes, PCFV_SECTOR_SIZE) : 0;
    uint32_t preroll = opt->no_audio ? 0 : opt->preroll_sectors;
    uint32_t refill = opt->no_audio ? 0 : opt->refill_sectors;
    if (preroll > audio_sectors) preroll = audio_sectors;
    if (refill > audio_sectors) refill = audio_sectors ? audio_sectors : 1u;

    uint32_t index_bytes = (uint32_t)frame_count * PCFV_ENTRY_BYTES;
    uint32_t data_start_sector = pcfv_ceil_div(PCFV_HEADER_BYTES + index_bytes, PCFV_SECTOR_SIZE);
    uint32_t cur_sector = data_start_sector + preroll;
    uint32_t next_audio_sector_index = preroll;
    uint32_t remaining_audio_sectors = audio_sectors > preroll ? audio_sectors - preroll : 0;

    PcfvEntry *entries = (PcfvEntry *)calloc(frame_count, sizeof(*entries));
    if (!entries) pcfv_die_errno("calloc entries");

    uint32_t max_video_sectors = 0;
    uint32_t frames_per_refill = 1;
    if (audio_bytes) {
        uint64_t num = (uint64_t)frame_count * (uint64_t)opt->ring_half_bytes + (audio_bytes / 2u);
        frames_per_refill = (uint32_t)(num / audio_bytes);
        if (frames_per_refill < 1u) frames_per_refill = 1u;
    }
    uint32_t next_refill_frame = frames_per_refill;

    for (uint16_t i = 0; i < frame_count; ++i) {
        entries[i].video_sector = cur_sector;
        entries[i].video_size = frames[i].size;
        entries[i].video_sectors = frames[i].sectors;
        entries[i].video_crc32 = frames[i].crc32;
        if (frames[i].sectors > max_video_sectors) max_video_sectors = frames[i].sectors;
        cur_sector += frames[i].sectors;

        if (remaining_audio_sectors && i >= next_refill_frame) {
            uint32_t n = refill;
            if (n > remaining_audio_sectors) n = remaining_audio_sectors;
            entries[i].audio_sector = cur_sector;
            entries[i].audio_sectors = n;
            entries[i].audio_byte_offset = next_audio_sector_index * PCFV_SECTOR_SIZE;
            cur_sector += n;
            next_audio_sector_index += n;
            remaining_audio_sectors -= n;
            next_refill_frame += frames_per_refill;
        }
    }
    if (remaining_audio_sectors) {
        entries[frame_count - 1u].audio_sector = cur_sector;
        entries[frame_count - 1u].audio_sectors = remaining_audio_sectors;
        entries[frame_count - 1u].audio_byte_offset = next_audio_sector_index * PCFV_SECTOR_SIZE;
        cur_sector += remaining_audio_sectors;
    }

    FILE *out = fopen(out_path, "wb");
    if (!out) pcfv_die_errno(out_path);
    if (fwrite(PCFV_MAGIC, 1, 8, out) != 8) pcfv_die_errno("write magic");
    wr16(out, PCFV_WIDTH); wr16(out, PCFV_HEIGHT);
    wr16(out, opt->fps); wr16(out, 1u);
    wr16(out, frame_count); wr16(out, audio_bytes ? 1u : 0u);
    wr32(out, index_bytes);
    wr32(out, data_start_sector);
    wr32(out, max_video_sectors);
    wr32(out, audio_bytes);
    wr32(out, audio_sectors);
    wr32(out, preroll);
    wr32(out, refill);
    wr32(out, opt->ring_half_bytes);
    wr32(out, opt->audio_rate);
    wr32(out, 0); wr32(out, 0);

    for (uint16_t i = 0; i < frame_count; ++i) {
        wr32(out, entries[i].video_sector);
        wr32(out, entries[i].video_size);
        wr32(out, entries[i].video_sectors);
        wr32(out, entries[i].video_crc32);
        wr32(out, entries[i].audio_sector);
        wr32(out, entries[i].audio_sectors);
        wr32(out, entries[i].audio_byte_offset);
        wr32(out, entries[i].flags);
    }
    uint32_t pos = PCFV_HEADER_BYTES + index_bytes;
    if (pos < data_start_sector * PCFV_SECTOR_SIZE) write_zeroes(out, data_start_sector * PCFV_SECTOR_SIZE - pos);

    if (preroll) {
        uint32_t copy = preroll * PCFV_SECTOR_SIZE;
        uint32_t real = copy > audio_bytes ? audio_bytes : copy;
        if (fwrite(audio, 1, real, out) != real) pcfv_die_errno("write audio preroll");
        if (copy > real) write_zeroes(out, copy - real);
    }

    for (uint16_t i = 0; i < frame_count; ++i) {
        if (fwrite(frames[i].data, 1, frames[i].size, out) != frames[i].size) pcfv_die_errno("write video frame");
        uint32_t vpadded = frames[i].sectors * PCFV_SECTOR_SIZE;
        if (vpadded > frames[i].size) write_zeroes(out, vpadded - frames[i].size);
        if (entries[i].audio_sectors) {
            uint32_t off = entries[i].audio_byte_offset;
            uint32_t chunk = entries[i].audio_sectors * PCFV_SECTOR_SIZE;
            uint32_t real = chunk;
            if (off + real > audio_bytes) real = audio_bytes - off;
            if (fwrite(audio + off, 1, real, out) != real) pcfv_die_errno("write audio refill");
            if (chunk > real) write_zeroes(out, chunk - real);
        }
    }
    fclose(out);

    fprintf(stderr,
            "Wrote %s: %u frames @ %u fps, max video sectors=%u, audio=%u bytes/%u sectors, total=%u sectors\n",
            out_path, frame_count, opt->fps, max_video_sectors, audio_bytes, audio_sectors, cur_sector);
    free(entries);
}

static void usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s [options] input.mp4|mkv|... output.pcfv\n"
        "\n"
        "Video options:\n"
        "  --ffmpeg PATH             ffmpeg executable, default ffmpeg\n"
        "  --fps N                   output FPS, default 15\n"
        "  --frames N                max frames to encode; 0=whole input, default 0\n"
        "  --quality N               RAINBOW quality 1..100, default 80\n"
        "  --rdo N                   entropy-aware lambda, default 8\n"
        "  --max-frame-sectors N     per-frame CD sector budget, default 4\n"
        "  --min-quality N           preferred retry quality floor, default 58\n"
        "  --retry-step N            quality decrement on retry, default 6\n"
        "  --jobs N                  video encoder worker threads, default CPU count\n"
        "\n"
        "Audio options:\n"
        "  --audio-rate N            ADPCM source/rate class, default 31468\n"
        "  --audio-gain X            pre-ADPCM gain, default 0.70\n"
        "  --audio-lowpass N         lowpass Hz, default 12000\n"
        "  --audio-highpass N        highpass Hz, default 80\n"
        "  --no-audio                video-only PCFV\n"
        "\n"
        "Mux options:\n"
        "  --preroll-sectors N       ADPCM sectors before video, default 64\n"
        "  --refill-sectors N        ADPCM sectors per refill, default 32\n"
        "  --ring-half-bytes N       PC-FX ADPCM ring half, default 65536\n"
        "  --tmp DIR                 temp dir base, default build\n"
        "  --keep-tmp                leave decoded temp files\n",
        argv0);
}

int main(int argc, char **argv) {
    Options opt;
    memset(&opt, 0, sizeof(opt));
    opt.ffmpeg = "ffmpeg";
    opt.tmp_base = "build";
    opt.fps = 15;
    opt.frames = 0;
    opt.quality = 80;
    opt.rdo = 8.0;
    opt.max_frame_sectors = 4;
    opt.min_quality = 58;
    opt.retry_step = 6;
    opt.audio_rate = 31468;
    opt.audio_gain = 0.70;
    opt.audio_lowpass = 12000;
    opt.audio_highpass = 80;
    opt.preroll_sectors = 64;
    opt.refill_sectors = 32;
    opt.ring_half_bytes = 65536;
    opt.jobs = default_job_count();

    int ai = 1;
    while (ai < argc && strncmp(argv[ai], "--", 2) == 0) {
        const char *o = argv[ai++];
        if (!strcmp(o, "--help") || !strcmp(o, "-h")) { usage(argv[0]); return 0; }
        else if (!strcmp(o, "--ffmpeg") && ai < argc) opt.ffmpeg = argv[ai++];
        else if (!strcmp(o, "--tmp") && ai < argc) opt.tmp_base = argv[ai++];
        else if (!strcmp(o, "--fps") && ai < argc) opt.fps = parse_u16_range(argv[ai++], "fps", 1, 60);
        else if (!strcmp(o, "--frames") && ai < argc) opt.frames = parse_u16_range(argv[ai++], "frames", 0, 65535);
        else if (!strcmp(o, "--quality") && ai < argc) opt.quality = parse_u16_range(argv[ai++], "quality", 1, 100);
        else if (!strcmp(o, "--rdo") && ai < argc) opt.rdo = parse_double_range(argv[ai++], "rdo", 0.0, 1000.0);
        else if (!strcmp(o, "--max-frame-sectors") && ai < argc) opt.max_frame_sectors = parse_u16_range(argv[ai++], "max-frame-sectors", 0, 64);
        else if (!strcmp(o, "--min-quality") && ai < argc) opt.min_quality = parse_u16_range(argv[ai++], "min-quality", 1, 100);
        else if (!strcmp(o, "--retry-step") && ai < argc) opt.retry_step = parse_u16_range(argv[ai++], "retry-step", 1, 50);
        else if (!strcmp(o, "--jobs") && ai < argc) opt.jobs = parse_u16_range(argv[ai++], "jobs", 1, 64);
        else if (!strcmp(o, "--audio-rate") && ai < argc) opt.audio_rate = parse_u32_range(argv[ai++], "audio-rate", 3000, 32000);
        else if (!strcmp(o, "--audio-gain") && ai < argc) opt.audio_gain = parse_double_range(argv[ai++], "audio-gain", 0.01, 2.0);
        else if (!strcmp(o, "--audio-lowpass") && ai < argc) opt.audio_lowpass = parse_u32_range(argv[ai++], "audio-lowpass", 1000, 20000);
        else if (!strcmp(o, "--audio-highpass") && ai < argc) opt.audio_highpass = parse_u32_range(argv[ai++], "audio-highpass", 0, 1000);
        else if (!strcmp(o, "--no-audio")) opt.no_audio = 1;
        else if (!strcmp(o, "--preroll-sectors") && ai < argc) opt.preroll_sectors = parse_u32_range(argv[ai++], "preroll-sectors", 0, 256);
        else if (!strcmp(o, "--refill-sectors") && ai < argc) opt.refill_sectors = parse_u32_range(argv[ai++], "refill-sectors", 1, 256);
        else if (!strcmp(o, "--ring-half-bytes") && ai < argc) opt.ring_half_bytes = parse_u32_range(argv[ai++], "ring-half-bytes", 2048, 131072);
        else if (!strcmp(o, "--keep-tmp")) opt.keep_tmp = 1;
        else { usage(argv[0]); return 2; }
    }
    if (argc - ai != 2) { usage(argv[0]); return 2; }
    const char *input = argv[ai++];
    const char *output = argv[ai++];

    if (mkdir(opt.tmp_base, 0777) != 0 && errno != EEXIST) pcfv_die_errno("mkdir tmp base");

    char tmpdir[4096];
    snprintf(tmpdir, sizeof(tmpdir), "%s/pcfv_%ld_XXXXXX", opt.tmp_base, (long)getpid());
    if (!mkdtemp(tmpdir)) pcfv_die_errno("mkdtemp");

    EncodedFrame *frames = NULL;
    uint16_t frame_count = encode_video_from_ffmpeg(&opt, input, &frames);

    uint8_t *audio = NULL;
    uint32_t audio_bytes = 0;
    if (!opt.no_audio) {
        char pcm_path[4096];
        if (extract_audio_pcm(&opt, input, tmpdir, frame_count, pcm_path, sizeof(pcm_path))) {
            long pcm_size = file_size_path(pcm_path);
            if (pcm_size > 0) audio = encode_audio_adpcm(&opt, pcm_path, &audio_bytes);
        } else {
            fprintf(stderr, "warning: ffmpeg audio extraction failed; writing video-only PCFV\n");
        }
    }

    write_pcfv(output, &opt, frames, frame_count, audio, audio_bytes);

    for (uint16_t i = 0; i < frame_count; ++i) free(frames[i].data);
    free(frames);
    free(audio);
    if (!opt.keep_tmp) {
        char cmd[8192];
        char *q = shell_quote(tmpdir);
        snprintf(cmd, sizeof(cmd), "rm -rf %s", q);
        free(q);
        (void)system(cmd);
    }
    return 0;
}
