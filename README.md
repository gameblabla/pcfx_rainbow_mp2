# PCFV video player example

This example builds a PC-FX CD that appends `assets/stream.pcfv` with the vendored `pcfx-cdlink`, generates `lbas.h`, and plays the stream with non-blocking KING SCSI DMA.

Build single-shot playback:

```sh
make LOOP=0
```

Build looping playback; START exits:

```sh
make LOOP=1
```

Change the source movie:

```sh
make clean
make VIDEO_IN=/path/to/movie.mkv FPS=15 JOBS=8
```

The default `src/main.c` uses the generic blocking API:

```c
PcfxPcfvOptions opt;
opt.stop_buttons = PCFX_PCFV_BTN_START;
opt.loop = PCFX_PCFV_EXAMPLE_LOOP ? PCFX_PCFV_PLAY_LOOP : PCFX_PCFV_PLAY_ONCE;
return pcfx_pcfv_play(BINARY_LBA_ASSETS_STREAM_PCFV, &opt) ? 1 : 0;
```

Simple APIs are also available:

```c
pcfx_pcfv_play_once(lba, PCFX_PCFV_BTN_START);
pcfx_pcfv_play_looping(lba, PCFX_PCFV_BTN_START);
```

Use `pcfx_pcfv_open()`, `pcfx_pcfv_update()`, and `pcfx_pcfv_stop()` when the game needs to branch based on input or clip end.

`FRAMES=0` is the default and means encode the entire decoded source. Use `FRAMES=N` only for short validation clips.


## Pause and sector seek

The simple player now exposes pause and sector-start helpers.  The sample binds
START to exit and II to pause/resume.

```c
int ok = pcfx_pcfv_play_once_paused(BINARY_LBA_ASSETS_STREAM_PCFV,
                                    PCFX_PCFV_BTN_START, PCFX_PCFV_BTN_II);

/* Start from a sector inside the PCFV asset. */
ok = pcfx_pcfv_play_once_from_sector(BINARY_LBA_ASSETS_STREAM_PCFV,
                                     1234, PCFX_PCFV_BTN_START, PCFX_PCFV_BTN_II);

/* Start from an absolute disc LBA. */
ok = pcfx_pcfv_play_once_from_disc_lba(BINARY_LBA_ASSETS_STREAM_PCFV,
                                       BINARY_LBA_ASSETS_STREAM_PCFV + 1234,
                                       PCFX_PCFV_BTN_START, PCFX_PCFV_BTN_II);
```

Sector seeking maps to the first frame whose RAINBOW data sector is at or after
the requested point.  For perfect audio seeking, make branch clips or keyframes
start at ADPCM reset boundaries; otherwise the player re-primes from the closest
known ADPCM refill chunk and the predictor may settle briefly.

## Runtime seek/query helpers

The flexible API can now seek after the clip is already open.  The current video
frame remains on screen while the requested target frame is read and prebuffered.
Audio is stopped during the seek and restarted only after the new target frame is
latched, so a branch does not produce audio over stale video.

```c
if (pressed & LEFT_BRANCH) {
    pcfx_pcfv_seek_disc_lba(left_branch_lba);
}

if (pressed & PAUSE) {
    pcfx_pcfv_toggle_paused();
}
```

Branch table helpers:

```c
uint16_t n      = pcfx_pcfv_frame_count();
uint16_t cur    = pcfx_pcfv_current_frame();
uint32_t sector = pcfx_pcfv_stream_sector_for_frame(cur);
uint32_t lba    = pcfx_pcfv_disc_lba_for_frame(cur);
uint16_t frame  = pcfx_pcfv_frame_for_stream_sector(sector);
```

Additional blocking convenience starts:

```c
pcfx_pcfv_play_once_from_data_sector(lba, data_sector, PCFX_PCFV_BTN_START, PCFX_PCFV_BTN_II);
pcfx_pcfv_play_once_from_frame(lba, frame_index, PCFX_PCFV_BTN_START, PCFX_PCFV_BTN_II);
```
