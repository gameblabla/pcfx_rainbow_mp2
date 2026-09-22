#!/usr/bin/env python3
"""KING (HuC6230) ADPCM audio for PCFV0001 streams: encode, mux, check, decode.

  mux VIDEO.pcfv MOVIE OUT.pcfv   add ADPCM from MOVIE to the frames of a PCFV
                                  (rainbow.py video --audio none, or an MP2
                                  PCFV whose audio is replaced)
  check STREAM.pcfv               the player's ADPCM layout rules + frame CRCs
  decode STREAM.pcfv OUT.wav      host decode of the stored audio (listening,
                                  tests); same arithmetic as the emulator

Decoder model: vendor/pcfxemu/mednafen/pcfx/soundbox.c (non-"buggy" codec):
delta = step[index] * ((nibble & 7) + 1), sign bit 3, 49-entry step table,
index += {-1,-1,-1,-1,2,4,6,8}, predictor clamped to [-0x4000, 0x3FFF]; with
linear interpolation on (the player's setting) rate code r splits each delta
into 2**r equal (truncated) parts.  KING fetches 16-bit KRAM words and plays
the low nibble first, so byte k holds samples 2k (low) and 2k+1 (high).
Sample clock (king.c/soundbox.c): 21477272.72 Hz / 682.5 = 31468.5 Hz for
rate code 0, i.e. two samples per line; codes 1..3 halve it.  The encoder is
closed-loop: it tracks exactly what that decoder will output.

Layout (the player's pcfv_adpcm_stream.c contract): the audio is cut into
65536-byte blocks, one KRAM ring half each; blocks 0 and 1 are the preroll
before frame 0; block k >= 2 is one refill chunk placed after the frame at
which the ring half it replaces finishes playing.
"""
import argparse
import array
import math
import struct
import subprocess
import sys
import wave
import zlib
from pathlib import Path

SECTOR = 2048
HEADER = struct.Struct('<8s6H11I')
ENTRY = struct.Struct('<8I')
MAX_FRAMES = 4096
FLAG_ADPCM, FLAG_MP2 = 0x0001, 0x0002
HALF_BYTES = 65536
HALF_SECTORS = HALF_BYTES // SECTOR
PREROLL_BLOCKS = 2
RATES = {31468: 0, 15734: 1, 7867: 2, 3934: 3}
# Silence after the audio: the player stops two fields early on its
# field-granular clock, and a zero byte is NOT silence in this codec (nibble 0
# is +step), so the tail is encoded from zero samples.
TAIL_SAMPLES = 2048

STEPS = (16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50,
         55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157,
         173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449,
         494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552)
INDEX_DELTA = (-1, -1, -1, -1, 2, 4, 6, 8)


def sectors(n):
    return (n + SECTOR - 1) // SECTOR


def rate_code(rate_hz):
    for hz, code in RATES.items():
        if rate_hz == hz:
            return code
    raise ValueError(f'ADPCM rate must be one of {sorted(RATES)} Hz (KING rate codes 0-3)')


def _apply(pred, delta, parts):
    """Add delta the way the decoder does: 2**r truncated parts, clamped each."""
    if parts == 1:
        pred += delta
        return 0x3FFF if pred > 0x3FFF else -0x4000 if pred < -0x4000 else pred
    part = abs(delta) >> (parts.bit_length() - 1)
    part = -part if delta < 0 else part
    for _ in range(parts):
        pred += part
        pred = 0x3FFF if pred > 0x3FFF else -0x4000 if pred < -0x4000 else pred
    return pred


def encode(pcm, code=0):
    """int16 samples -> ADPCM bytes (low nibble first).  Greedy per sample,
    choosing the magnitude whose reconstruction lands closest to the target."""
    parts = 1 << code
    pred = index = 0
    out = bytearray((len(pcm) + 1) // 2)
    for i, sample in enumerate(pcm):
        target = sample >> 1                      # 16-bit PCM -> 15-bit range
        step = STEPS[index]
        diff = target - pred
        sign = 8 if diff < 0 else 0
        mag = (2 * abs(diff) // step + 1) // 2 - 1  # round(|diff| / step) - 1
        mag = 0 if mag < 0 else 7 if mag > 7 else mag
        best = None
        for m in (mag, mag - 1, mag + 1):
            if 0 <= m <= 7:
                d = step * (m + 1)
                p = _apply(pred, -d if sign else d, parts)
                if best is None or abs(target - p) < best[0]:
                    best = (abs(target - p), m, p)
        _, mag, pred = best
        index += INDEX_DELTA[mag]
        index = 0 if index < 0 else 48 if index > 48 else index
        out[i >> 1] |= (sign | mag) << (4 * (i & 1))
    return bytes(out)


def decode(adpcm, code=0, samples=None):
    """ADPCM bytes -> int16 samples, the emulator's arithmetic."""
    parts = 1 << code
    pred = index = 0
    total = len(adpcm) * 2 if samples is None else samples
    out = array.array('h', bytes(2 * total))
    for i in range(total):
        nib = (adpcm[i >> 1] >> (4 * (i & 1))) & 15
        d = STEPS[index] * ((nib & 7) + 1)
        pred = _apply(pred, -d if nib & 8 else d, parts)
        index += INDEX_DELTA[nib & 7]
        index = 0 if index < 0 else 48 if index > 48 else index
        out[i] = max(-32768, min(32767, pred * 2))
    return out


def extract_pcm(ffmpeg, movie, rate, seconds, gain_db):
    """Mono s16 at the ADPCM rate, band-limited for a 4-bit codec."""
    lowpass = min(12000, int(rate * 0.4))
    cmd = [ffmpeg, '-nostdin', '-v', 'error', '-i', str(Path(movie).resolve()),
           '-map', '0:a:0', '-vn', '-ac', '1',
           '-af', f'highpass=f=40,lowpass=f={lowpass},aresample={rate}:filter_size=64:phase_shift=10,'
                  f'volume={gain_db}dB,apad',
           '-t', f'{seconds:.6f}', '-ar', str(rate), '-f', 's16le', 'pipe:1']
    raw = subprocess.run(cmd, check=True, stdout=subprocess.PIPE).stdout
    pcm = array.array('h')
    pcm.frombytes(raw[:len(raw) & ~1])
    if sys.byteorder != 'little':
        pcm.byteswap()
    return pcm


def stream_codec(path):
    h = HEADER.unpack_from(Path(path).read_bytes()[:HEADER.size])
    flags, audio_bytes = h[6], h[10]
    if not audio_bytes:
        return 'none'
    return 'mp2' if flags & FLAG_MP2 else 'adpcm'


def chunk_frames(count, fps_num, fps_den, rate, blocks):
    """Frame after which each refill block k >= 2 is stored: when the half it
    replaces (block k - 2) has finished playing, i.e. at (k - 1) block times."""
    at, last = {}, -1
    for k in range(PREROLL_BLOCKS, blocks):
        f = (k - 1) * HALF_BYTES * 2 * fps_num // (rate * fps_den)
        f = max(f, last + 1)
        if f >= count:
            raise ValueError('too few video frames to carry the ADPCM refill chunks')
        at[f], last = k, f
    return at


def write(path, frames, stride, fps_num, fps_den, adpcm, rate):
    count = len(frames)
    if not 1 <= count <= MAX_FRAMES:
        raise ValueError('video must have 1..4096 frames')
    # 0x80 = +step then -step: the codec's silence (zero bytes would ramp).
    adpcm = adpcm + b'\x80' * (-len(adpcm) % SECTOR)
    blocks = (len(adpcm) + HALF_BYTES - 1) // HALF_BYTES
    preroll = min(len(adpcm), PREROLL_BLOCKS * HALF_BYTES) // SECTOR
    start = sectors(HEADER.size + count * ENTRY.size)
    refill_at = chunk_frames(count, fps_num, fps_den, rate, blocks)
    cursor = start + preroll
    payload = bytearray(adpcm[:preroll * SECTOR])
    entries = []
    for i, frame in enumerate(frames):
        vs = cursor
        payload += frame + bytes(stride * SECTOR - len(frame))
        cursor += stride
        aus = ans = abo = 0
        if i in refill_at:
            abo = refill_at[i] * HALF_BYTES
            part = adpcm[abo:abo + HALF_BYTES]
            aus, ans = cursor, sectors(len(part))
            payload += part
            cursor += ans
        entries.append(ENTRY.pack(vs, len(frame), stride, zlib.crc32(frame), aus, ans, abo, 0))
    header = HEADER.pack(b'PCFV0001', 256, 240, fps_num, fps_den, count, FLAG_ADPCM,
                         count * ENTRY.size, start, stride, len(adpcm), sectors(len(adpcm)),
                         preroll, HALF_SECTORS, HALF_BYTES, rate, 0, 0)
    with Path(path).open('wb') as out:
        out.write(header + b''.join(entries))
        out.write(bytes(start * SECTOR - HEADER.size - count * ENTRY.size))
        out.write(payload)
    return dict(frames=count, slot_sectors=stride, total_sectors=cursor, audio_bytes=len(adpcm),
                audio_blocks=blocks, rate=rate)


def read(path):
    """Parse an ADPCM PCFV and apply the player's rules (pcfv_audio_attach and
    parse_pcfv_header).  Returns frames, audio bytes and timing."""
    data = Path(path).read_bytes()
    if len(data) < HEADER.size:
        raise ValueError('truncated PCFV header')
    (magic, w, hgt, fpsn, fpsd, count, flags, index_bytes, start, stride, audio_bytes,
     audio_sectors, preroll, refill, ring, rate, _, _) = HEADER.unpack_from(data)
    if magic != b'PCFV0001' or (w, hgt) != (256, 240):
        raise ValueError('expected PCFV0001, 256x240')
    if not 1 <= count <= MAX_FRAMES or not fpsn or not fpsd or fpsn > 60 * fpsd:
        raise ValueError('invalid frame count or frame rate')
    if index_bytes != count * ENTRY.size or start < sectors(HEADER.size + index_bytes):
        raise ValueError('invalid PCFV index/data start')
    if not 1 <= stride <= 4 or len(data) % SECTOR:
        raise ValueError('invalid video slot stride or file sector alignment')
    if flags & FLAG_MP2 or not audio_bytes:
        raise ValueError('not an ADPCM PCFV')
    if ring != HALF_BYTES:
        raise ValueError(f'ring half {ring} bytes; the player uses {HALF_BYTES}')
    rate_code(rate)
    if audio_sectors != sectors(audio_bytes):
        raise ValueError('inconsistent audio length')
    blocks = (audio_bytes + HALF_BYTES - 1) // HALF_BYTES
    pre_bytes = preroll * SECTOR
    if not preroll or (pre_bytes < audio_bytes and pre_bytes % HALF_BYTES):
        raise ValueError('ADPCM preroll must hold whole ring halves')
    audio = bytearray(data[start * SECTOR:start * SECTOR + min(pre_bytes, audio_bytes)])
    frames, intervals = [], [(start, start + preroll)]
    for i in range(count):
        vs, size, ns, crc, aus, ans, abo, _ = ENTRY.unpack_from(data, HEADER.size + i * ENTRY.size)
        if not 1 <= ns <= stride or not 0 < size <= ns * SECTOR or vs < start or (vs + ns) * SECTOR > len(data):
            raise ValueError(f'frame {i}: invalid video extent')
        frame = data[vs * SECTOR:vs * SECTOR + size]
        if zlib.crc32(frame) != crc:
            raise ValueError(f'frame {i}: video CRC mismatch')
        frames.append(frame)
        intervals.append((vs, vs + ns))
        if ans:
            # Each chunk is the next whole block: block j lives in ring half j & 1.
            want = min(HALF_BYTES, audio_bytes - abo) if abo < audio_bytes else 0
            if (abo != len(audio) or abo % HALF_BYTES or not want or
                    ans > HALF_SECTORS or ans * SECTOR < want):
                raise ValueError(f'frame {i}: ADPCM chunk is not the next ring-half block')
            if (aus + ans) * SECTOR > len(data):
                raise ValueError(f'frame {i}: truncated ADPCM chunk')
            audio += data[aus * SECTOR:aus * SECTOR + want]
            intervals.append((aus, aus + ans))
    intervals.sort()
    if any(a[1] > b[0] for a, b in zip(intervals, intervals[1:])):
        raise ValueError('overlapping PCFV extents')
    if len(audio) != audio_bytes:
        raise ValueError(f'ADPCM blocks cover {len(audio)} of {audio_bytes} bytes')
    return dict(frames=frames, fps_num=fpsn, fps_den=fpsd, audio=bytes(audio), rate=rate,
                stride=stride, blocks=blocks)


def mux_command(a):
    import pcfv  # tools/rainbow: the silent video container
    code = rate_code(a.rate)
    video = pcfv.read(a.video)
    if video['audio']:
        print(f'{a.video}: replacing its MP2 audio (video frames are reused as-is)')
    frames, fpsn, fpsd = video['frames'], video['fps_num'], video['fps_den']
    samples = len(frames) * a.rate * fpsd // fpsn
    pcm = extract_pcm(a.ffmpeg, a.movie, a.rate, samples / a.rate, a.gain_db)
    pcm = pcm[:samples]
    pad = samples - len(pcm) + TAIL_SAMPLES
    pcm.extend([0] * (pad + (-(samples + pad) % (2 * SECTOR))))
    adpcm = encode(pcm, code)
    target = Path(a.output)
    target.parent.mkdir(parents=True, exist_ok=True)
    tmp = target.with_suffix(target.suffix + '.tmp')
    info = write(tmp, frames, video['stride'], fpsn, fpsd, adpcm, a.rate)
    read(tmp)
    tmp.replace(target)
    ref = pcm[:samples]
    rec = decode(adpcm, code, samples)
    noise = sum((x - y) ** 2 for x, y in zip(ref, rec)) or 1
    signal = sum(x * x for x in ref) or 1
    info['snr_db'] = round(10 * math.log10(signal / noise), 2)
    print(f'{target}: {info}')


def check_command(a):
    s = read(a.stream)
    print(f'{a.stream}: {len(s["frames"])} frames @ {s["fps_num"]}/{s["fps_den"]} fps, '
          f'{len(s["audio"])} ADPCM bytes in {s["blocks"]} blocks @ {s["rate"]} Hz: OK')


def decode_command(a):
    s = read(a.stream)
    pcm = decode(s['audio'], rate_code(s['rate']))
    with wave.open(a.output, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(s['rate'])
        w.writeframes(pcm.tobytes())


def main():
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/rainbow'))
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='command', required=True)
    q = sub.add_parser('mux')
    q.add_argument('video')
    q.add_argument('movie')
    q.add_argument('output')
    q.add_argument('--ffmpeg', default='ffmpeg')
    q.add_argument('--rate', type=int, default=31468, help='31468, 15734, 7867 or 3934 Hz')
    q.add_argument('--gain-db', type=float, default=-3.0)
    q = sub.add_parser('check')
    q.add_argument('stream')
    q = sub.add_parser('decode')
    q.add_argument('stream')
    q.add_argument('output')
    a = p.parse_args()
    try:
        {'mux': mux_command, 'check': check_command, 'decode': decode_command}[a.command](a)
    except (ValueError, OSError, subprocess.CalledProcessError) as exc:
        p.exit(1, f'pcfv_adpcm: {exc}\n')


if __name__ == '__main__':
    main()
