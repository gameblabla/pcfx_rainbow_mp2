# PCFV RAINBOW video player (MP2 or KING ADPCM audio)

A PC-FX CD that streams a `PCFV0001` movie from an `append` file: RAINBOW
(HuC6271) video DMA'd by KING SCSI into KRAM, with the audio interleaved in the
same file and played in one of two ways, chosen at build time:

| `AUDIO=` | Audio path | CPU cost | Disc cost (this clip) | Stream |
|---|---|---|---|---|
| `mp2` (default) | MP2 16 kHz mono decoded on the V810, 10-bit PSG output | one MP2 frame per field | 1664 sectors (62/s) | `assets/stream.pcfv` |
| `adpcm` | HuC6230 4-bit ADPCM played by KING straight from a KRAM ring | none | 1817 sectors (68/s) at 31.47 kHz | `assets/stream_adpcm.pcfv` |
| `none` | silent, video paced by fields | none | 1611 sectors | `assets/stream_silent.pcfv` |

The player is built with the toolkit (`libpcfx`, `toolchain/`), so this
directory must sit in the toolkit root (or set `ROOT=`/`V810_GCC=`). Streams are
build products (`*.pcfv` is ignored); make them with `make encode`.

## Build, run, validate

```sh
make AUDIO=adpcm encode VIDEO_IN=~/Videos/sailor.mkv FPS=15 FIT=stretch JOBS=12
make AUDIO=adpcm cd                     # -> pcfv_rainbow_adpcm_player.cue/.bin
make test                               # host tests: tools/rainbow + ADPCM codec/layout
PCFX_BIOS_DIR=/path/to/bios make AUDIO=adpcm validate VIDEO_IN=~/Videos/sailor.mkv
```

`AUDIO=mp2` (the default) works the same way. `make cd` refuses a stream that
fails the strict RAINBOW gate, or whose audio codec does not match `AUDIO`.
`make validate` runs the disc in `pcfx-headless` at five checkpoints and
**fails** on: no frame presented / black screen, right-edge RAINBOW corruption,
CD errors, audio underflows (MP2 PSG ring; ADPCM ring half played before its
refill landed), an ADPCM clock that drifts from the hardware, audio the player
silently ignored, unfinished playback. With `VIDEO_IN` it also checks A/V
drift (≤0.12 s), pitch and each checkpoint picture against the source. Outputs
go to `validation/$(AUDIO)/`.

Other switches: `LOOP=1` (START exits, II pauses), `START_FRAME=N` (start-seek
smoke test), `ADPCM_RATE=31468|15734|7867|3934`, `AUDIO_GAIN_DB`.

## Layout

| File | Role |
|---|---|
| `src/pcfx_pcfv_player.c` | Video core: non-blocking SCSI/KING DMA scheduler, RAINBOW ring and latch, A/V sync. `#if PCFX_PCFV_AUDIO_MP2 / _ADPCM` selects the backend. |
| `src/pcfx_pcfv_internal.h` | The contract between the core and an audio backend (hooks + the core's CD/KRAM services). |
| `src/pcfv_mp2_stream.c/.h` | MP2 backend: chunk fetch policy, decode budget, PSG start/pause, seek. |
| `src/pcfv_adpcm_stream.c/.h` | ADPCM backend: KRAM ring halves, refill scheduling, KING channel 0, field-exact clock, pause, seek. |
| `src/pcfx_mp2_async.c`, `kjmp2_fast.c`, `psg_sample.c` | MP2 decoder, stream buffer and PSG sample output (MP2 builds only). |
| `tools/pcfv_adpcm.py` | ADPCM encoder (closed-loop against the emulator's decoder), muxer, layout checker, host decoder. |
| `tools/build_disc.py`, `emu_validate.py`, `regression_check.py` | Disc build and emulator gates for all three builds. |

Both backends use libpcfx for the parts it does well: KRAM → RAM copies go
through `king_set_kram_read()`/`king_kram_read()`, and the blocking MP2 loaders
(`pcfx_pcfv_mp2_load_from_cd()`, `pcfx_mp2_stream_read_cd()`) use
`eris_cd_read_dma()` (CD → KRAM → RAM) in place of the old CPU-PIO read.
Streaming stays on the player's own non-blocking DMA state machine, because
every libpcfx CD read blocks.

## ADPCM path

- **Ring:** 2 × 64 KiB halves at KRAM word `0x20000` (page 0), KING channel 0
  in repeat mode. Stream block *j* (64 KiB) always lives in half *j & 1*;
  REG.53 reports "half 0 done" (bit 1) / "half 1 done" (bit 0), and the player
  refills that half with block *j + 2* through the same CD channel as video,
  ahead of speculative video reads. The deadline is one half (4.2 s at
  31.47 kHz).
- **Clock:** KING's ADPCM clock is the 21.477 MHz master / 682.5 =
  31468.5 Hz, exactly 2 samples per 1365-clock line, i.e. 524 per 262-line field
  (`vendor/pcfxemu` soundbox.c/king.c). The player counts fields for a
  sample-exact clock and re-anchors it on every half event. Video frames are
  latched against that clock, as MP2 uses the PSG read pointer.
- **Encoder:** `tools/pcfv_adpcm.py` models the emulator's decoder exactly
  (step table, `step × (n + 1)` deltas, index table, 15-bit clamp, rate-code
  interpolation split) and stores the low nibble first. A zero byte is *not*
  silence in this codec (nibble 0 = +step, a ramp to the rail); padding is
  `0x80` / encoded zeros, and ≥2048 silent samples end every stream.
- **Pause:** disabling channel 0 freezes playback, but re-enabling restarts at
  START (REG.58). Resume points START at the paused position (512-byte grain),
  enables, then points START back at the ring base for the wrap.
- **Seek:** loads the target block and the next one, and starts inside the
  block at the 512-byte point at or before the target, with a decoder reset.
  Exact at frame 0; elsewhere the predictor settles briefly.

## RAINBOW quality: ADPCM vs MP2

Neither codec forces different RAINBOW settings. The PC-FX drive gives
150 sectors/s; this clip uses 62/s with MP2 and 68/s with 31.47 kHz ADPCM
(15.7 kHz ADPCM: ~64/s). What bounds picture quality is the **per-frame KRAM
slot**, currently 4 sectors (`tools/rainbow/pcfv.py MAX_FRAME_SECTORS`, the
player's `VIDEO_BUFFER_STRIDE_MAX`). This clip's frames are budget-bound
(MPCONV scales 3–8, 0 finest). Measured on 40 frames of the clip:

| Slot | Video sectors/s @15 fps | Mean scale | Largest strip |
|---|---|---|---|
| 4 sectors | 60 | 6.4 | 972 B |
| 5 sectors | 75 | 4.1 | 1202 B |
| 6 sectors | 90 | 3.2 | 1202 B |
| 8 sectors | 120 | 1.7 | 1840 B |

A 6-sector slot roughly halves the scale and still leaves the disc at ~65 % with
either codec. It needs the shared `pcfv.py` limit raised, a KRAM re-plan (the
ADPCM build frees the MP2 bounce window; 20 × 6-sector slots fit below the
ADPCM ring), and a hardware check that the larger strips meet the 16-raster
decode deadline. ADPCM does free the V810 (no MP2 decode) and 16 KiB of KRAM,
which matters if a game runs logic during playback.

## Validation (2026-09-22, `pcfx-headless`, emulator evidence only)

| Build | Frames | Underflows | Max A/V drift | Pitch | Picture corr |
|---|---|---|---|---|---|
| `mp2` | 401/401, 0 skipped | 0 | 0.076 s | +0.01 % | 0.986–0.994 |
| `adpcm` 31.47 kHz | 401/401, 0 skipped | 0 (7/7 blocks refilled) | 0.080 s | −0.002 % | 0.986–0.994 |
| `none` | 401/401 | — | — | — | — |

ADPCM field clock vs hardware half events: ≤452 samples (<1 field). The
emulator's ADPCM output matches the host decode of the stream (waveform
correlation 0.97–0.98) at exactly 31468.5 Hz. Pause (II at frames 1500/1650)
resumes within 0.065 s on both codecs. `START_FRAME=180` plays the remaining 221
frames, with the captured audio 12 ms (ADPCM) / 49 ms (MP2) from the source
position. `LOOP=1` ADPCM restarts audio on pass 2. Confirm on real hardware
before making release claims.

## What was wrong, and how it was found

| Symptom | Cause | Fix |
|---|---|---|
| Glitched tiles on the right edge | Old C encoder wrote strip sizes that counted **unstuffed** bytes, no word alignment, no dummy/guard words: the decoder ran out of budget before the last macroblock columns | Stream re-encoded with `tools/rainbow`; `repair-legacy` can migrate old streams losslessly |
| Soft/washed picture, cropped framing | JPEG-style tables, float colour/FDCT, no null runs, crop fit | MPCONV base tables + rescale, integer pipeline, per-frame finest scale |
| Black screen after the liberis → libpcfx port | `wait_vblank()` polled the VDC status VD bit, which is only raised while VDC CR bit 3 (vblank IRQ enable) is set; the port wrote `CR = BB` and the wait never returned | Frame timing uses the Tetsu raster counter, double-read (`pcfx-frame-timing`) |
| `LOOP=1`: second pass black for seconds, then 2× speed and silent | reopen kept per-pass state (`g_mp2_enabled`, `g_video_frames_presented`) | `pcfx_pcfv_reset_state()` resets per-pass state |
| "Validated" but still broken | `toolchain/bin/pcfx-headless` predated the vendored RAINBOW decoder fix | Rebuild the emulator after updating `vendor/pcfxemu`; `make validate` gates on runtime counters and pixels |
| ADPCM build: refills stop after the first, then stale audio | libpcfx `irq_disable()` returns the raw PSW.ID bit (0x1000) and writes `PSW = 0x1000`; `irq_restore()` keeps only bit 0, so `irq_restore(irq_disable())` **enables** interrupts. The first RAINBOW latch switched IRQs on with no handler installed, and a stray interrupt overwrote the stream index in RAM (found by RAM diffs + a canary between update steps) | `start_rainbow_frame()` saves/restores PSW.ID itself (since fixed in `vendor/libpcfx`, checked by its `examples/012_irq_save_restore`; liberis still has the bug) |
| MP2 build black once IRQs stayed off until audio start | The PSG timer setup unmasked VDC-A too (`irq_set_mask(0x37)`); its pending IRQ ran a BIOS handler. The libpcfx bug had hidden this by enabling IRQs during the hidden warm-up | Unmask the timer only (`0x3F`) |
| II "paused" the picture but MP2 kept playing | The pause toggled inside the poll loop, then the same field restarted the PSG | No latch or audio start in a field that paused; the MP2 backend really stops the PSG |
| MP2 seek restarted the audio at 0 (the audio-clocked picture then waited for it); the silent build did not compile | Audio seek was ADPCM-only and returned early for streams without a preroll; MP2 globals were referenced outside `#if` | The MP2 backend starts at the chunk before the target, resyncs to the next frame header and drops frames up to the target; audio lives behind the backend contract |
| Pitch gate failed a correct ADPCM capture (−17.7 %) | Level-threshold "active spans" split differently for 4-bit ADPCM than for the source | Pitch = spacing of two waveform-correlated anchors (0.001 % resolution); spans kept as a diagnostic |

The June build this was imported from (and its legacy assets) lives in the
`pcfx_rainbow_mp2_startup_sync_package` directory next to this one.

## Open items (not verified on hardware)

- During the hidden warm-up the player sets every Tetsu priority to 0 and relies on
  priority 0 to hide the RAINBOW. C6261 (R08–R09) requires unique priorities and
  treats 0 as the bottom plane, not hidden, so on hardware the first decoded frame
  may be visible for the ~8 warm-up fields. The emulator shows black. Left unchanged
  pending a hardware check; the documented hide is the plane-enable bit.
- `LOOP=1` reopens the stream, so each pass has the same ~3.5 s black prebuffer as the
  first boot.
- ADPCM facts taken from `vendor/pcfxemu` rather than hardware: the 2 samples/line
  clock, low-nibble-first order, REG.53 half/END bits, and START being read only at
  the enable edge and at the END wrap (the pause/seek start trick). The 128 KiB
  preroll and 64 KiB refill SCSI-DMA arms are larger than the one hardware-confirmed
  16 KiB `eris_cd_read_kram` arm.

## Player API

```c
PcfxPcfvOptions opt;
opt.stop_buttons = PCFX_PCFV_BTN_START;
opt.pause_buttons = PCFX_PCFV_BTN_II;
opt.loop = PCFX_PCFV_PLAY_ONCE;
opt.seek_mode = PCFX_PCFV_SEEK_NONE;
opt.start_sector = 0;
return pcfx_pcfv_play(BINARY_LBA_BUILD_STREAM_PCFV, &opt) ? 1 : 0;
```

Also `pcfx_pcfv_play_once()`, `pcfx_pcfv_play_looping()`, the `*_from_frame`/
`*_from_sector` variants, and the non-blocking `pcfx_pcfv_open()` /
`pcfx_pcfv_update()` / `pcfx_pcfv_seek_frame()` / `pcfx_pcfv_stop()` for games that
branch on input or clip end. The API is identical in every `AUDIO` build.

BIOS ROMs are never included; supply one through `PCFX_BIOS_DIR`.
