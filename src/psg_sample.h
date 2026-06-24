#ifndef PSG_SAMPLE_H
#define PSG_SAMPLE_H

#include <stdint.h>
#include "kjmp2_fast.h"

#define PSG_AUDIO_FORMAT_PSG10_MONO   10u
#define PSG_AUDIO_FORMAT_PSG10_STEREO 11u

#define PSG10_DEFAULT_HIGH_VOL 31u
#define PSG10_DEFAULT_LOW_VOL  12u
#define PSG10_MONO_HIGH_CHANNEL 0u
#define PSG10_MONO_LOW_CHANNEL  1u
#define PSG10_ST_L_HIGH_CHANNEL 0u
#define PSG10_ST_L_LOW_CHANNEL  1u
#define PSG10_ST_R_HIGH_CHANNEL 2u
#define PSG10_ST_R_LOW_CHANNEL  3u

#define PSG10MP2_RING_BITS     15u
#define PSG10MP2_RING_SIZE     (1u << PSG10MP2_RING_BITS)
#define PSG10MP2_RING_MASK     (PSG10MP2_RING_SIZE - 1u)
#define PSG10MP2_FRAME_SAMPLES KJMP2_SAMPLES_PER_FRAME

extern kjmp2v_psg10_sample_t g_mp2psg10_ring[PSG10MP2_RING_SIZE];
extern volatile uint32_t g_mp2psg10_read_pos;
extern volatile uint32_t g_mp2psg10_write_pos;
extern volatile uint32_t g_mp2psg10_playing;
extern volatile uint32_t g_mp2psg10_stereo;
extern volatile uint32_t g_mp2psg10_decode_done;
extern volatile uint32_t g_mp2psg10_irq_samples_emitted;
extern volatile uint32_t g_mp2psg10_underflows;

void PSG10MP2_SetPairVolumes(uint8_t high_volume, uint8_t low_volume);
void PSG10MP2_InitTimer(int period);
void PSG10MP2_InitTimerStereo(int period);
void PSG10MP2_InitTimerFractional(int base_period, uint32_t frac_16_16);
void PSG10MP2_InitTimerFractionalStereo(int base_period, uint32_t frac_16_16);
void PSG10MP2_StopTimer(void);
void PSG10MP2_StartMono(void);
void PSG10MP2_StartStereo(void);
void PSG10MP2_Stop(void);
void PSG10MP2_RingReset(void);
uint32_t PSG10MP2_RingUsed(void);
uint32_t PSG10MP2_RingFree(void);
void PSG10MP2_RingPushFrame(const kjmp2v_psg10_sample_t *src);
void PSG10MP2_SetDecodeDone(void);

static inline int PSG10MP2_RingGetContiguousFrame(kjmp2v_psg10_sample_t **dst)
{
    uint32_t idx = g_mp2psg10_write_pos & PSG10MP2_RING_MASK;
    if (idx <= (PSG10MP2_RING_SIZE - PSG10MP2_FRAME_SAMPLES)) {
        *dst = &g_mp2psg10_ring[idx];
        return 1;
    }
    return 0;
}

static inline void PSG10MP2_RingCommitFrame(void)
{
    g_mp2psg10_write_pos += PSG10MP2_FRAME_SAMPLES;
}
static inline int PSG10MP2_IsPlaying(void) { return g_mp2psg10_playing != 0; }
static inline uint32_t PSG10MP2_GetIrqSamplesEmitted(void) { return g_mp2psg10_read_pos; }
static inline uint32_t PSG10MP2_GetUnderflows(void) { return g_mp2psg10_underflows; }
void samplepsg_timer_irq(void);
void samplepsg_timer_irq_stereo(void);
uint32_t PSG10MP2_ServiceTimerOnce(void);

#endif
