#ifndef PCFX_PCFV_PLAYER_H
#define PCFX_PCFV_PLAYER_H

#include <stdint.h>

/* PC-FX pad bits as returned by libpcfx contrlr_pad_read(0). */
#define PCFX_PCFV_BTN_I      (1u << 0)
#define PCFX_PCFV_BTN_II     (1u << 1)
#define PCFX_PCFV_BTN_III    (1u << 2)
#define PCFX_PCFV_BTN_START  (1u << 7)
#define PCFX_PCFV_BTN_UP     (1u << 8)
#define PCFX_PCFV_BTN_RIGHT  (1u << 9)
#define PCFX_PCFV_BTN_DOWN   (1u << 10)
#define PCFX_PCFV_BTN_LEFT   (1u << 11)

#define PCFX_PCFV_PLAY_ONCE 0u
#define PCFX_PCFV_PLAY_LOOP 1u

/* Optional start-position modes for the simple/blocking player.

   NONE             Start at the first PCFV frame.
   STREAM_SECTOR    start_sector is relative to the beginning of the PCFV file.
                    This is the usual mode when you looked up a sector in the
                    PCFV frame table.
   DISC_LBA         start_sector is an absolute CD LBA.  The player subtracts
                    stream_lba and then seeks inside that PCFV file.
   DATA_SECTOR      start_sector is relative to PCFV data_start_sector, i.e.
                    after the header/index area.

   The seek lands on the first RAINBOW frame whose on-disc sector is >= the
   requested sector.  ADPCM is re-primed from the closest available ADPCM refill
   chunk; exact random-access audio requires authoring clip/keyframe boundaries
   at valid ADPCM reset points. */
#define PCFX_PCFV_SEEK_NONE          0u
#define PCFX_PCFV_SEEK_STREAM_SECTOR 1u
#define PCFX_PCFV_SEEK_DISC_LBA      2u
#define PCFX_PCFV_SEEK_DATA_SECTOR   3u
#define PCFX_PCFV_SEEK_FRAME         4u  /* start_sector is a frame index */

typedef struct PcfxPcfvOptions {
    /* Button mask that exits playback on a rising edge.  Use START by default. */
    uint16_t stop_buttons;

    /* Button mask that toggles pause on a rising edge.  0 disables pause. */
    uint16_t pause_buttons;

    /* PCFX_PCFV_PLAY_ONCE: return after the final frame.
       PCFX_PCFV_PLAY_LOOP: restart the clip and only return after stop_buttons. */
    uint8_t loop;

    /* Optional starting point.  Use PCFX_PCFV_SEEK_NONE to start at frame 0. */
    uint8_t seek_mode;
    uint32_t start_sector;
} PcfxPcfvOptions;

/* Simple blocking API.  In single-shot mode it returns 1 after natural end or
   0 if stop_buttons aborts.  In looping mode it keeps restarting the clip and
   returns only when stop_buttons is pressed.  If pause_buttons is nonzero,
   pressing it toggles video/audio pause while staying inside the blocking call. */
int pcfx_pcfv_play(uint32_t stream_lba, const PcfxPcfvOptions *opt);
int pcfx_pcfv_play_once(uint32_t stream_lba, uint16_t stop_buttons);
int pcfx_pcfv_play_looping(uint32_t stream_lba, uint16_t stop_buttons);

/* Convenience variants for the non-general/blocking player. */
int pcfx_pcfv_play_once_paused(uint32_t stream_lba, uint16_t stop_buttons, uint16_t pause_buttons);
int pcfx_pcfv_play_looping_paused(uint32_t stream_lba, uint16_t stop_buttons, uint16_t pause_buttons);
int pcfx_pcfv_play_once_from_sector(uint32_t stream_lba, uint32_t stream_relative_sector,
                                    uint16_t stop_buttons, uint16_t pause_buttons);
int pcfx_pcfv_play_looping_from_sector(uint32_t stream_lba, uint32_t stream_relative_sector,
                                       uint16_t stop_buttons, uint16_t pause_buttons);
int pcfx_pcfv_play_once_from_disc_lba(uint32_t stream_lba, uint32_t absolute_disc_lba,
                                      uint16_t stop_buttons, uint16_t pause_buttons);
int pcfx_pcfv_play_looping_from_disc_lba(uint32_t stream_lba, uint32_t absolute_disc_lba,
                                         uint16_t stop_buttons, uint16_t pause_buttons);
int pcfx_pcfv_play_once_from_data_sector(uint32_t stream_lba, uint32_t data_relative_sector,
                                         uint16_t stop_buttons, uint16_t pause_buttons);
int pcfx_pcfv_play_looping_from_data_sector(uint32_t stream_lba, uint32_t data_relative_sector,
                                            uint16_t stop_buttons, uint16_t pause_buttons);
int pcfx_pcfv_play_once_from_frame(uint32_t stream_lba, uint16_t frame_index,
                                   uint16_t stop_buttons, uint16_t pause_buttons);
int pcfx_pcfv_play_looping_from_frame(uint32_t stream_lba, uint16_t frame_index,
                                      uint16_t stop_buttons, uint16_t pause_buttons);

/* Backward-compatible name: this is the looping blocking player. */
int pcfx_pcfv_play_loop(uint32_t stream_lba, uint16_t stop_buttons);

/* Flexible API for Dragon's Lair-style games: open a clip, call update once per
   main-loop iteration, poll your own game state between calls, and stop/switch
   when your branch condition is met.  This low-level API is single-shot; call
   pcfx_pcfv_open() again when you intentionally want to restart or branch. */
int  pcfx_pcfv_open(uint32_t stream_lba, const PcfxPcfvOptions *opt);
int  pcfx_pcfv_update(void);
void pcfx_pcfv_stop(void);
int  pcfx_pcfv_aborted(void);
void pcfx_pcfv_set_paused(int paused);
void pcfx_pcfv_toggle_paused(void);
int  pcfx_pcfv_paused(void);

/* Runtime seek helpers for the flexible API.  They keep the current frame on
   screen while the new target is asynchronously prebuffered.  Audio is stopped,
   re-primed from the nearest PCFV ADPCM chunk, then restarted after the first
   new video frame is latched.  Return 1 if the seek request was accepted. */
int  pcfx_pcfv_seek_frame(uint16_t frame_index);
int  pcfx_pcfv_seek_stream_sector(uint32_t stream_relative_sector);
int  pcfx_pcfv_seek_disc_lba(uint32_t absolute_disc_lba);
int  pcfx_pcfv_seek_data_sector(uint32_t data_relative_sector);

/* Query helpers useful for Dragon's Lair-style branching tables. */
uint16_t pcfx_pcfv_frame_count(void);
uint16_t pcfx_pcfv_current_frame(void);
uint32_t pcfx_pcfv_stream_sector_for_frame(uint16_t frame_index);
uint32_t pcfx_pcfv_disc_lba_for_frame(uint16_t frame_index);
uint32_t pcfx_pcfv_data_sector_for_frame(uint16_t frame_index);
uint16_t pcfx_pcfv_frame_for_stream_sector(uint32_t stream_relative_sector);
uint16_t pcfx_pcfv_frame_for_disc_lba(uint32_t absolute_disc_lba);
uint16_t pcfx_pcfv_frame_for_data_sector(uint32_t data_relative_sector);

/* Optional RAINBOW+MP2 path. Load the MP2 asset from CD into CPU RAM before
   opening the PCFV stream, then the player decodes it cooperatively while the
   RAINBOW stream continues to use KING DMA from CD. */
int pcfx_pcfv_mp2_load_from_cd(uint32_t mp2_lba, uint32_t mp2_size_bytes);
uint32_t pcfx_pcfv_mp2_frames_decoded(void);
uint32_t pcfx_pcfv_mp2_underflows(void);
uint32_t pcfx_pcfv_mp2_ring_used(void);
uint32_t pcfx_pcfv_mp2_error_code(void);
uint32_t pcfx_pcfv_mp2_error_offset(void);

#endif
