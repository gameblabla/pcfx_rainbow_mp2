#ifndef PCFV_MP2_STREAM_H
#define PCFV_MP2_STREAM_H

/* PCFV audio backend: MPEG-1/2 Layer II interleaved in the PCFV file (or a
   separate MP2 asset preloaded with pcfx_pcfv_mp2_load_from_cd()), decoded on
   the V810 by kjmp2_fast and played as 10-bit samples through the PSG timer
   IRQ (psg_sample.c).  Implements pcfx_pcfv_internal.h.

   CD path: the player's non-blocking SCSI DMA lands each chunk in the KRAM
   bounce window, which is copied straight into the decoder's stream buffer
   (pcfx_mp2_stream_tail/commit), so no intermediate RAM copy exists.

   The PSG sample clock is the playback timeline: video frames are latched
   when the samples played reach their presentation time. */

#include "pcfx_pcfv_internal.h"
#include "pcfx_mp2_async.h"

#define PCFV_MP2_KRAM_WORD_ADDR PCFX_MP2_KRAM_WORD_ADDR

/* Ring thresholds, in PSG samples (1152 per MP2 frame). */
#define PCFV_MP2_START_RING_SAMPLES    (24u * 1152u)  /* first start */
#define PCFV_MP2_DECODE_LOW_SAMPLES    (16u * 1152u)  /* decode below this */
#define PCFV_MP2_URGENT_RING_SAMPLES   (8u * 1152u)   /* restart after underflow */
#define PCFV_MP2_CRITICAL_RING_SAMPLES (4u * 1152u)

#endif
