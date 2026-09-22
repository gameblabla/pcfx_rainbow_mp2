#!/usr/bin/env python3
"""Validate PCFV, build and converge its generated append LBA before handoff."""
import argparse
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/rainbow'))
import pcfv
from rainbow_decode import decode_stream


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--stream', required=True)
    p.add_argument('--cdlink', required=True)
    p.add_argument('--make', default='make')
    a = p.parse_args()
    stream = Path(a.stream).resolve()
    parsed = pcfv.read(stream)
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
    config.write_text('binary build/pcfv_rainbow_mp2_player.program.bin\n'
                      'lbaheader build/lbas.h\nname RAINBOW MP2\nmaker homebrew\n'
                      'makerid HBR\ncountry 1\nversion 256\ndate 20260922\n'
                      'append build/stream.pcfv\n')
    for attempt in range(4):
        before = header.read_bytes()
        subprocess.run([a.make, 'program'], check=True)
        subprocess.run([a.cdlink, str(config), 'pcfv_rainbow_mp2_player'], check=True)
        if header.read_bytes() == before:
            print(f'PCFV disc validated: {len(parsed["frames"])} frames; append LBA converged in {attempt + 1} passes')
            return
    raise ValueError('append LBA failed to converge')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as exc:
        sys.exit(f'build_disc: {exc}')
