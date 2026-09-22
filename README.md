# PCFV RAINBOW + MP2 video player

A PC-FX CD that streams a `PCFV0001` movie from an `append` file: RAINBOW
(HuC6271) video DMA'd by KING SCSI into KRAM, MP2 audio decoded on the V810
and played through the PSG, locked to the audio clock. The shipped asset is a
401-frame, 15 fps Sailor Moon clip (`assets/stream.pcfv`).

The player is built with the toolkit (`libpcfx`, `toolchain/`), so this
directory must sit in the toolkit root (or set `ROOT=`/`V810_GCC=`).

## Build, run, validate

```sh
make cd                                   # -> pcfv_rainbow_mp2_player.cue/.bin
make test                                 # host tests for tools/rainbow
PCFX_BIOS_DIR=/path/to/bios make validate VIDEO_IN=~/Videos/sailor.mkv
```

`make cd` refuses a stream that fails the strict RAINBOW gate (a legacy
stream, for example). `make validate` runs the disc in `pcfx-headless` at five
checkpoints and **fails** on: no frame presented / black screen, right-edge
RAINBOW corruption, CD/PSG underflows, unfinished playback; with `VIDEO_IN` it
also checks A/V drift (≤0.12 s), pitch and each checkpoint against the source.
Outputs land in `validation/current/`.

Current result (2026-09-22, rebuilt `pcfx-headless`): 401/401 frames, 0 skipped,
0 underflows, audio starts on the first visible field with a 27,648-sample
ring, max A/V drift 0.053 s, pitch error 0.00 %, picture correlation vs source
0.985–0.994. Emulator evidence only: confirm on real hardware before release
claims.

## Re-encode

```sh
make encode VIDEO_IN=/path/to/movie.mkv FPS=15 FIT=stretch JOBS=12
make cd
```

`tools/rainbow/rainbow.py video` is byte-identical to Doom PC-FX's
MPCONV-conformant encoder. Each frame gets the finest MPCONV scale (0 finest …
15 coarsest) that fits the player's fixed 4-sector KRAM slot, so quality rises
while the disc bandwidth stays the same. Audio is MP2 mono 16 kHz 32 kbit/s with
`AUDIO_GAIN_DB=8` (10-bit PSG playback is quiet). 256×240 pixels are shown as
a 4:3 picture, so `FIT=stretch` is right for 4:3 sources; `contain`/`crop` fit
against a 4:3 frame for other aspect ratios.

## What was wrong, and how it was found

| Symptom | Cause | Fix |
|---|---|---|
| Glitched tiles on the right edge | Old C encoder wrote strip sizes that counted **unstuffed** bytes, no word alignment, no dummy/guard words: the decoder ran out of budget before the last macroblock columns | Stream re-encoded with `tools/rainbow`; `repair-legacy` can migrate old streams losslessly |
| Soft/washed picture, cropped framing | JPEG-style tables, float colour/FDCT, no null runs, crop fit | MPCONV base tables + rescale, integer pipeline, per-frame finest scale |
| Black screen after the liberis → libpcfx port | `wait_vblank()` polled the VDC status VD bit, which is only raised while VDC CR bit 3 (vblank IRQ enable) is set; the port wrote `CR = BB` and the wait never returned | Frame timing uses the Tetsu raster counter, double-read (`pcfx-frame-timing`) |
| `LOOP=1`: second pass black for seconds, then 2× speed and silent (the June build never restarted at all) | reopen kept per-pass state (`g_mp2_enabled`, `g_video_frames_presented`), so MP2 never restarted and video ran unclocked | `pcfx_pcfv_reset_state()` resets per-pass state; pass 2 now plays in sync with sound |
| "Validated" but still broken | `toolchain/bin/pcfx-headless` predated the vendored RAINBOW decoder fix, so neither the glitch nor its fix was visible | Rebuild the emulator after updating `vendor/pcfxemu`; `make validate` gates on runtime counters and pixels |

The original June build is kept as `sailormoon_pcfx_example.zip`; its validation
is in `validation/2026-06-22-original/`, and earlier READMEs/logs in `history/`.
Old assets are in `assets/legacy/` (`stream.legacy.pcfv` original,
`stream.repaired.pcfv` = `repair-legacy` output).

## Open items (not verified on hardware)

- During the hidden warm-up the player sets every Tetsu priority to 0 and relies on
  priority 0 to hide the RAINBOW. C6261 (R08–R09) requires unique priorities and
  treats 0 as the bottom plane, not hidden, so on hardware the first decoded frame
  may be visible for the ~8 warm-up fields. The emulator shows black. Left unchanged
  pending a hardware check; the documented hide is the plane-enable bit.
- `LOOP=1` reopens the stream, so each pass has the same ~5 s black prebuffer as the
  first boot.

## Player API

```c
PcfxPcfvOptions opt;
opt.stop_buttons = PCFX_PCFV_BTN_START;
opt.loop = PCFX_PCFV_EXAMPLE_LOOP ? PCFX_PCFV_PLAY_LOOP : PCFX_PCFV_PLAY_ONCE;
return pcfx_pcfv_play(BINARY_LBA_BUILD_STREAM_PCFV, &opt) ? 1 : 0;
```

Also `pcfx_pcfv_play_once()`, `pcfx_pcfv_play_looping()`, and the
non-blocking `pcfx_pcfv_open()` / `pcfx_pcfv_update()` / `pcfx_pcfv_stop()` for
games that branch on input or clip end. `make LOOP=1` builds a looping disc
(START exits, II pauses).

BIOS ROMs are never included; supply one through `PCFX_BIOS_DIR`.
