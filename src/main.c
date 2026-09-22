#include <stdint.h>
#include "pcfx_pcfv_player.h"

#if defined(HAVE_GENERATED_LBAS)
#include "lbas.h"
#endif
#if defined(HAVE_GENERATED_AUDIO_INFO)
#include "generated_audio_info.h"
#endif

#ifndef BINARY_LBA_ASSETS_STREAM_PCFV
#define BINARY_LBA_ASSETS_STREAM_PCFV BINARY_LBA_BUILD_STREAM_PCFV
#endif
#ifndef BINARY_LBA_ASSETS_AUDIO_MP2
#define BINARY_LBA_ASSETS_AUDIO_MP2 0u
#endif
#ifndef PCFX_MP2_AUDIO_SIZE
#define PCFX_MP2_AUDIO_SIZE 0u
#endif
#ifndef PCFX_PCFV_EXAMPLE_LOOP
#define PCFX_PCFV_EXAMPLE_LOOP 0
#endif
#ifndef PCFX_PCFV_EXAMPLE_START_FRAME
#define PCFX_PCFV_EXAMPLE_START_FRAME 0
#endif

int main(void) {
    PcfxPcfvOptions opt;

    /* The audio (MP2 or ADPCM, per build) is interleaved inside the PCFV asset
       and streamed by the player. */
    opt.stop_buttons = PCFX_PCFV_BTN_START;
    opt.pause_buttons = PCFX_PCFV_BTN_II;
    opt.loop = PCFX_PCFV_EXAMPLE_LOOP ? PCFX_PCFV_PLAY_LOOP : PCFX_PCFV_PLAY_ONCE;
    /* make START_FRAME=N: start-seek smoke test (audio primed for frame N). */
    opt.seek_mode = PCFX_PCFV_EXAMPLE_START_FRAME ? PCFX_PCFV_SEEK_FRAME : PCFX_PCFV_SEEK_NONE;
    opt.start_sector = PCFX_PCFV_EXAMPLE_START_FRAME;
    return pcfx_pcfv_play(BINARY_LBA_ASSETS_STREAM_PCFV, &opt) ? 1 : 0;
}
