#!/usr/bin/env python3
"""Host tests for tools/pcfv_adpcm.py.  Run: python3 tools/test_pcfv_adpcm.py

They pin what the ADPCM player depends on:
  * the encoder is closed-loop against the emulator's decoder arithmetic
    (vendor/pcfxemu soundbox.c) at every KING rate code, low nibble first;
  * padding is the codec's silence (0x80), never zero bytes (a ramp);
  * the PCFV layout is exactly what pcfv_adpcm_stream.c accepts: whole
    ring-half blocks, blocks 0-1 as preroll, one refill per later block,
    stored no later than the moment its ring half frees;
  * the Python constants match the C player's.
Emulator evidence is `make AUDIO=adpcm validate`; neither proves hardware.
"""
import array
import math
from pathlib import Path
import re
import struct
import sys
import tempfile
import unittest
import zlib

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import pcfv_adpcm as ad  # noqa: E402

SRC = HERE.parent / 'src'


def tone(n, rate, hz=440.0, amp=12000):
    return array.array('h', (int(amp * math.sin(2 * math.pi * hz * i / rate)) for i in range(n)))


def snr_db(ref, rec):
    noise = sum((a - b) ** 2 for a, b in zip(ref, rec)) or 1
    return 10 * math.log10(sum(a * a for a in ref) / noise)


def fake_frames(count):
    # Only CRC/extent matter to the layout checker (RAINBOW framing is
    # tools/rainbow's job); vary the sizes within the 4-sector slot.
    return [bytes([i & 255]) * (5000 + 37 * (i % 50)) for i in range(count)]


class Codec(unittest.TestCase):
    def test_closed_loop_every_rate(self):
        for rate, code in ad.RATES.items():
            pcm = tone(rate // 4, rate, hz=rate / 40)
            rec = ad.decode(ad.encode(pcm, code), code, len(pcm))
            self.assertGreater(snr_db(pcm, rec), 18.0, f'rate code {code}')

    def test_low_nibble_first(self):
        # A rising first sample needs a positive code (sign bit clear) in the
        # LOW nibble, a falling second sample a negative one in the high nibble.
        data = ad.encode(array.array('h', [4000, -4000]), 0)
        self.assertEqual(data[0] & 0x08, 0)
        self.assertEqual(data[0] & 0x80, 0x80)

    def test_zero_bytes_are_not_silence_but_0x80_is(self):
        ramp = ad.decode(bytes(512), 0)
        self.assertGreater(max(ramp), 30000)          # nibble 0 = +step: rails
        quiet = ad.decode(b'\x80' * 512, 0)
        self.assertLess(max(abs(x) for x in quiet), 64)

    def test_encoded_silence_tail_stays_quiet(self):
        pcm = tone(4000, 31468) + array.array('h', [0] * ad.TAIL_SAMPLES)
        rec = ad.decode(ad.encode(pcm, 0), 0, len(pcm))
        self.assertLess(max(abs(x) for x in rec[-ad.TAIL_SAMPLES // 2:]), 200)


class Layout(unittest.TestCase):
    def build(self, seconds=21.0, fps=15, rate=31468):
        frames = fake_frames(int(seconds * fps))
        samples = len(frames) * rate // fps
        adpcm = ad.encode(tone(samples, rate), ad.rate_code(rate))
        tmp = Path(tempfile.mkdtemp()) / 'a.pcfv'
        info = ad.write(tmp, frames, 4, fps, 1, adpcm, rate)
        return tmp, frames, adpcm, info

    def test_round_trip_and_player_rules(self):
        path, frames, adpcm, info = self.build()
        s = ad.read(path)
        self.assertEqual(s['frames'], frames)
        self.assertEqual(s['audio'][:len(adpcm)], adpcm)
        self.assertEqual(len(s['audio']) % ad.SECTOR, 0)
        self.assertEqual(set(s['audio'][len(adpcm):]), {0x80} if len(s['audio']) > len(adpcm) else set())
        self.assertEqual(info['audio_blocks'], s['blocks'])
        self.assertEqual(ad.stream_codec(path), 'adpcm')

    def test_refills_stored_before_their_half_frees(self):
        path, frames, _, _ = self.build(seconds=40.0)
        data = path.read_bytes()
        block_sec = ad.HALF_BYTES * 2 / 31468
        seen = []
        for i in range(len(frames)):
            e = ad.ENTRY.unpack_from(data, ad.HEADER.size + i * ad.ENTRY.size)
            if e[5]:
                k = e[6] // ad.HALF_BYTES
                seen.append(k)
                # due when block k-2 finishes, i.e. (k-1) block times in
                self.assertLessEqual(i / 15.0, (k - 1) * block_sec + 1e-9)
        self.assertEqual(seen, list(range(2, 2 + len(seen))))

    def test_short_clip_is_preroll_only(self):
        path, _, _, info = self.build(seconds=3.0)
        self.assertEqual(info['audio_blocks'], 1)
        self.assertEqual(ad.read(path)['blocks'], 1)

    def test_reader_rejects_what_the_player_rejects(self):
        path, _, _, _ = self.build()
        good = bytearray(path.read_bytes())

        def mutate(fn):
            bad = bytearray(good)
            fn(bad)
            p = path.with_name('bad.pcfv')
            p.write_bytes(bad)
            with self.assertRaises(ValueError):
                ad.read(p)

        def ring(b):
            struct.pack_into('<I', b, 48, 32768)
        def rate(b):
            struct.pack_into('<I', b, 52, 32000)
        def preroll(b):
            struct.pack_into('<I', b, 40, 33)
        def chunk_offset(b):
            for i in range(struct.unpack_from('<H', b, 16)[0]):
                o = ad.HEADER.size + i * ad.ENTRY.size
                if struct.unpack_from('<I', b, o + 20)[0]:
                    struct.pack_into('<I', b, o + 24, struct.unpack_from('<I', b, o + 24)[0] + 2048)
                    return
        def mp2_flag(b):
            struct.pack_into('<H', b, 18, 2)
        def crc(b):
            struct.pack_into('<I', b, ad.HEADER.size + 12, zlib.crc32(b'x'))
        for fn in (ring, rate, preroll, chunk_offset, mp2_flag, crc):
            mutate(fn)


class PlayerConstants(unittest.TestCase):
    def test_python_matches_c(self):
        h = (SRC / 'pcfv_adpcm_stream.h').read_text()
        num = lambda name: int(re.search(rf'#define {name}\s+(\w+)u', h).group(1), 0)
        self.assertEqual(num('PCFV_ADPCM_HALF_BYTES'), ad.HALF_BYTES)
        self.assertEqual(num('PCFV_ADPCM_START_ALIGN_BYTES'), 512)
        rates = {num(f'PCFV_ADPCM_RATE{c}_HZ'): c for c in range(4)}
        self.assertEqual(rates, ad.RATES)
        # 524 samples per 262-line field = 2 per line at rate code 0.
        self.assertEqual(num('PCFV_ADPCM_RATE0_SAMPLES_PER_FIELD'), 524)
        self.assertEqual(round(21477272.72 / 682.5 * 262 * 1365 / 21477272.72), 524)


if __name__ == '__main__':
    unittest.main(verbosity=2)
