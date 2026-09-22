#ifndef PCFV_FORMAT_H
#define PCFV_FORMAT_H

#include <stdint.h>

/* PCFV: PC-FX full-motion stream for HuC6271 RAINBOW + HuC6230 ADPCM or MP2.

   All multi-byte fields are little-endian on disc.  Units named "sector" are
   2048-byte CD sectors relative to the beginning of the PCFV file, not absolute
   disc LBAs.  The runtime adds the asset's absolute LBA from lbas.h.

   The layout is:
     64-byte PCFV header
     frame_count * 32-byte PCFV frame entries
     zero padding to data_start_sector
     ADPCM preroll sectors (ADPCM streams only)
     for each frame:
       one padded RAINBOW YUV/DCT frame
       optional padded audio chunk (ADPCM refill block or MP2 bytes)

   ADPCM streams (flags bit0) are cut in 65536-byte blocks, one KRAM ring half
   each: blocks 0-1 are the preroll, block k >= 2 is one refill chunk.  MP2
   streams (flags bit1) carry 1-4 sector chunks and no preroll.  Writers:
   tools/rainbow/pcfv.py (silent, MP2), tools/pcfv_adpcm.py (ADPCM).
*/

#define PCFV_MAGIC        "PCFV0001"
#define PCFV_SECTOR_SIZE  2048u
#define PCFV_HEADER_BYTES 64u
#define PCFV_ENTRY_BYTES  32u
#define PCFV_WIDTH        256u
#define PCFV_HEIGHT       240u

/* Header offsets, for small V810 runtime parsers. */
#define PCFV_HDR_MAGIC              0u   /* char[8] */
#define PCFV_HDR_WIDTH              8u   /* u16 */
#define PCFV_HDR_HEIGHT             10u  /* u16 */
#define PCFV_HDR_FPS_NUM            12u  /* u16 */
#define PCFV_HDR_FPS_DEN            14u  /* u16 */
#define PCFV_HDR_FRAME_COUNT        16u  /* u16 */
#define PCFV_HDR_FLAGS              18u  /* u16, bit0: ADPCM, bit1: MP2 */
#define PCFV_HDR_INDEX_BYTES        20u  /* u32 */
#define PCFV_HDR_DATA_START_SECTOR  24u  /* u32 */
#define PCFV_HDR_MAX_VIDEO_SECTORS  28u  /* u32 */
#define PCFV_HDR_AUDIO_BYTES        32u  /* u32 */
#define PCFV_HDR_AUDIO_SECTORS      36u  /* u32 */
#define PCFV_HDR_AUDIO_PREROLL      40u  /* u32 sectors */
#define PCFV_HDR_AUDIO_REFILL       44u  /* u32 sectors */
#define PCFV_HDR_RING_HALF_BYTES    48u  /* u32 */
#define PCFV_HDR_AUDIO_RATE_HZ      52u  /* u32 */
#define PCFV_HDR_RESERVED0          56u  /* u32 */
#define PCFV_HDR_RESERVED1          60u  /* u32 */

#define PCFV_FRAME_VIDEO_SECTOR      0u  /* u32 */
#define PCFV_FRAME_VIDEO_SIZE        4u  /* u32 */
#define PCFV_FRAME_VIDEO_SECTORS     8u  /* u32 */
#define PCFV_FRAME_VIDEO_CRC32       12u /* u32 */
#define PCFV_FRAME_AUDIO_SECTOR      16u /* u32 */
#define PCFV_FRAME_AUDIO_SECTORS     20u /* u32 */
#define PCFV_FRAME_AUDIO_BYTE_OFFSET 24u /* u32 */
#define PCFV_FRAME_FLAGS             28u /* u32 */

#endif /* PCFV_FORMAT_H */
