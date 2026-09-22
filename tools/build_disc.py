#!/usr/bin/env python3
"""Validate PCFV, build and converge its generated append LBA before handoff."""
import argparse
from pathlib import Path
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/rainbow'))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import pcfv
import pcfv_adpcm
from rainbow_decode import decode_stream


def read_stream(path, audio):
    """Parse with the reader for the build's audio backend; refuse a mismatch
    instead of shipping a disc that plays silently."""
    codec = pcfv_adpcm.stream_codec(path)
    if codec != audio:
        raise ValueError(f'{path} carries {codec} audio but AUDIO={audio}; '
                         f'build with AUDIO={codec} or re-encode')
    return pcfv_adpcm.read(path) if codec == 'adpcm' else pcfv.read(path)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--stream', required=True)
    p.add_argument('--cdlink', required=True)
    p.add_argument('--make', default='make')
    p.add_argument('--audio', choices=['mp2', 'adpcm', 'none'], default='mp2')
    p.add_argument('--target', default='pcfv_rainbow_mp2_player')
    a = p.parse_args()
    stream = Path(a.stream).resolve()
    parsed = read_stream(stream, a.audio)
    for i, frame in enumerate(parsed['frames']):
        try:
            decode_stream(frame)
        except ValueError as exc:
            raise ValueError(f'frame {i}: {exc}; regenerate or use repair-legacy for the old encoder') from exc
    build = Path('build')
    build.mkdir(exist_ok=True)
    # Fix the append path so its generated symbol is independent of user input.
    staged = build / 'stream.pcfv'
    if staged.is_symlink() or staged.exists():
        staged.unlink()
    staged.symlink_to(stream)
    header = build / 'lbas.h'
    header.write_text('#define BINARY_LBA_BUILD_STREAM_PCFV 0u\n')
    config = build / 'cdlink.txt'
    config.write_text(f'binary build/{a.target}.program.bin\n'
                      f'lbaheader build/lbas.h\nname RAINBOW {a.audio.upper()}\nmaker homebrew\n'
                      'makerid HBR\ncountry 1\nversion 256\ndate 20260922\n'
                      'append build/stream.pcfv\n')
    for attempt in range(4):
        before = header.read_bytes()
        subprocess.run(shlex.split(a.make) + ['program'], check=True)
        subprocess.run([a.cdlink, str(config), a.target], check=True)
        if header.read_bytes() == before:
            print(f'PCFV disc validated: {len(parsed["frames"])} frames, {a.audio} audio; '
                  f'append LBA converged in {attempt + 1} passes')
            return
    raise ValueError('append LBA failed to converge')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as exc:
        sys.exit(f'build_disc: {exc}')
