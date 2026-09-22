#!/usr/bin/env python3
"""Run the built disc in pcfx-headless and gate it on runtime evidence.

A build that links is not a build that plays.  This drives the headless
emulator at several checkpoints, dumps RAM, reads the player's own counters
through the linker map (extract_pcfv_stats.py), and fails on the regressions
this package has actually shipped:

  * black screen / hung boot  -> no frames presented, screenshot all black
  * RAINBOW starvation        -> right-edge band far worse than the body
  * CD or audio underflows    -> non-zero underflow/error counters
  * clip never finishes       -> g_done != 1 at the final checkpoint

With --source it also runs regression_check.py (A/V drift, pitch, start-up
audio preroll, per-checkpoint picture vs the source movie).

The emulator is evidence for what pcfxemu implements, not proof of retail
hardware behaviour.  Rebuild it (scripts/build-headless.sh) after updating
vendor/pcfxemu, or you are validating against old decoder bugs.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = Path(os.environ.get('PCFX_TOOLKIT_ROOT') or HERE.parents[1])


def run_emu(emu, bios, cue, frames, extra):
    cmd = [str(emu), '--bios-dir', str(bios), '--pcfx', '--frames', str(frames)] + extra + [str(cue)]
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if proc.returncode:
        raise SystemExit(f'emulator failed at {frames} frames:\n{proc.stdout[-2000:]}')


def stats_from_ram(map_path, ram_path):
    out = subprocess.check_output([sys.executable, str(HERE / 'extract_pcfv_stats.py'), str(map_path), str(ram_path)])
    return json.loads(out)


def load_rgb(png):
    from PIL import Image
    import numpy as np
    return np.asarray(Image.open(png).convert('RGB'), dtype=np.float64)


def nonblack_fraction(img):
    return float((img.sum(axis=2) > 24).mean())


def right_edge_starvation(img):
    """Source-independent RAINBOW starvation signature.

    A strip whose byte budget runs out before its entropy does decodes its
    last macroblock column(s) from garbage: a hard seam at x=240 and a noisy
    band to its right.  Measured on this package's legacy stream vs the
    MPCONV re-encode: seam 4.3-7.2 vs <=0.62, band HF 2.8-8.7 vs <=0.73.
    Returns (seam, hf); both must be high to count, so one real vertical edge
    at x=240 does not trip it."""
    import numpy as np
    d = np.abs(np.diff(img, axis=1)).mean(axis=(0, 2))
    seam = d[239] / (np.mean([d[x - 1] for x in range(16, 240, 16)]) + 1e-6)
    hf = lambda a: np.abs(np.diff(a, axis=1)).mean() + np.abs(np.diff(a, axis=0)).mean()
    return float(seam), float(hf(img[:, 240:]) / (hf(img[:, 16:240]) + 1e-6))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--cue', default='pcfv_rainbow_mp2_player.cue')
    ap.add_argument('--map', default='build/pcfv_rainbow_mp2_player.map')
    ap.add_argument('--stream', default='assets/stream.pcfv')
    ap.add_argument('--source', help='source movie for A/V, pitch and picture checks')
    ap.add_argument('--bios-dir', default=os.environ.get('PCFX_BIOS_DIR'))
    ap.add_argument('--emu', default=os.environ.get('PCFX_HEADLESS') or str(ROOT / 'toolchain/bin/pcfx-headless'))
    ap.add_argument('--checkpoints', default='1500,1800,2100,2400',
                    help='emulated frame counts to screenshot + dump (must fall inside playback)')
    ap.add_argument('--final', type=int, default=3000, help='frame count by which playback must be done')
    ap.add_argument('--out', default='validation/current')
    a = ap.parse_args()
    if not a.bios_dir:
        raise SystemExit('set PCFX_BIOS_DIR or pass --bios-dir (a BIOS is never bundled)')
    for p in (a.cue, a.map, a.stream, a.emu):
        if not Path(p).exists():
            raise SystemExit(f'missing {p}; run make cd first')

    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    failures = []
    points = [int(x) for x in a.checkpoints.split(',') if x] + [a.final]
    for n in points:
        final = n == a.final
        png, ram = out / f'frame_{n}.png', out / f'ram_{n}.bin'
        extra = ['--screenshot', str(png), '--dump', 'ram', str(ram)]
        if final:
            extra += ['--wav', str(out / f'out_{n}.wav')]
        run_emu(a.emu, a.bios_dir, a.cue, n, extra)
        st = stats_from_ram(a.map, ram)
        ram.unlink()
        (out / f'stats_{n}.json').write_text(json.dumps(st, indent=2))
        shown = st.get('g_video_frames_presented', 0)
        img = load_rgb(png)
        lit = nonblack_fraction(img)
        seam, hf = right_edge_starvation(img)
        print(f'{n:5d} frames: presented={shown} done={st.get("g_done")} nonblack={lit:.2f} '
              f'edge_seam={seam:.2f} edge_hf={hf:.2f} '
              f'video_underflows={st.get("g_scsi_dma_video_underflows")} dma_errors={st.get("g_scsi_dma_errors")} '
              f'psg_underflows={st.get("g_mp2psg10_underflows")}')
        for key in ('g_scsi_dma_video_underflows', 'g_scsi_dma_errors', 'g_mp2psg10_underflows', 'g_abort'):
            if st.get(key):
                failures.append(f'{n}: {key}={st[key]}')
        if final:
            if st.get('g_done') != 1:
                failures.append(f'{n}: playback not finished (g_done={st.get("g_done")}, presented={shown})')
            if st.get('g_video_frames_skipped'):
                failures.append(f'{n}: {st["g_video_frames_skipped"]} frames skipped')
        else:
            if not shown:
                failures.append(f'{n}: no RAINBOW frame presented (hung boot or dead vblank wait?)')
            if not st.get('g_done') and lit < 0.05:
                failures.append(f'{n}: black screen while playing (nonblack {lit:.2f})')
            if lit >= 0.05 and seam > 2.0 and hf > 2.0:
                failures.append(f'{n}: right-edge RAINBOW corruption (seam {seam:.2f}, band HF {hf:.2f}); '
                                'check strip sizes count stuffed FF 00 bytes: rainbow.py inspect')

    if a.source:
        wav = out / f'out_{a.final}.wav'
        # A clip that is done has blanked the display, so regression_check skips it
        # for picture checks but still uses it for A/V and pitch.
        proc = subprocess.run([sys.executable, str(HERE / 'regression_check.py'),
                               '--stream', a.stream, '--source', a.source, '--wav', str(wav),
                               '--stats-dir', str(out), '--frames-dir', str(out),
                               '--report', str(out / 'regression_report.json'),
                               '--max-av-drift-sec', '0.12'],
                              stdout=subprocess.PIPE, text=True)
        report = json.loads(proc.stdout)
        for check in (report.get('visual') or {}).get('checks', []):
            print(f'  {check["name"]}: source frame {check["source_frame"]} corr={check["correlation"]:.3f} '
                  f'mae={check["mae"]:.1f} right_edge_mae={check.get("edge_mae", 0):.1f}')
        print(f'  max A/V drift {report["av_sync"]["max_abs_av_delta_sec"]:.3f}s; '
              f'pitch error {report["pitch"]["duration_error_pct"]:.2f}%' if report.get('pitch') else '')
        failures += report['failures']

    (out / 'summary.json').write_text(json.dumps(dict(pass_=not failures, failures=failures), indent=2))
    if failures:
        print('FAIL\n  ' + '\n  '.join(failures))
        raise SystemExit(1)
    print(f'PASS ({out})')


if __name__ == '__main__':
    main()
