"""Checks an EDAA recording (docs/recording.md) and prints its frames.

    python record_check.py <recording folder>

Verifies the file sizes against record.json and lists per frame the time,
jitter, flags and the exposure inputs (Elite's luminance L and the exposure
computed from cb2 with the tonemap formula, reverse-engineering.md section 10).
"""

import json
import math
import os
import struct
import sys

# recorder::frame_params, in order (recorder.hpp).
PARAMS = struct.Struct(
    '<II Q d II IIII 2f 16f 16f 3f 3f 4f 12f 64f 4i 4f 4f 16f')
FLAGS = {1: 'cut', 2: 'gap', 4: 'reactive', 8: 'tonemap', 16: 'foot', 32: 'vehicle'}


def elite_exposure(cb2, lum):
    """The tonemap's exposure (cb2 as 16 float4) times cb2[2].x and cb2[6].y."""
    c = lambda r, i: cb2[r * 4 + i]
    if lum <= 0:
        return 0.0
    bias = c(8, 0) + (1.4 if c(8, 2) != 0 else 0.0)
    x = math.log2(8 * lum) - bias - 3
    w2 = 2 * c(7, 3)
    curve = (w2 / (math.log10(1 + 2 ** (x * c(8, 1))) + w2)) ** c(7, 3)
    e = (1.03 * c(7, 2) - curve) / lum * 2 ** -c(7, 1)
    return c(2, 0) * e * c(6, 1)


def main(folder):
    meta = json.load(open(os.path.join(folder, 'record.json')))
    n = meta['frames']
    cw, ch = meta['crop'][2], meta['crop'][3]
    ok = True

    if meta['params_bytes'] != PARAMS.size:
        print(f"params_bytes {meta['params_bytes']} != {PARAMS.size}: reader out of date")
        return 1

    expected = {'params.bin': n * PARAMS.size, 'tonemap.bin': n * meta['tonemap_bytes']}
    for p in meta['planes']:
        expected[p['name'] + '.bin'] = n * cw * ch * p['bytes_per_pixel']

    for name, size in expected.items():
        have = os.path.getsize(os.path.join(folder, name))
        state = 'ok' if have == size else f'WRONG (expected {size})'
        ok &= have == size
        print(f'{name:14} {have:>14} bytes  {state}')

    print(f"{n} frames, {meta['dropped']} dropped, {meta['duration_s']:.2f} s, render {meta['render_size']}, crop {meta['crop']}")
    params = open(os.path.join(folder, 'params.bin'), 'rb').read()
    tonemap = open(os.path.join(folder, 'tonemap.bin'), 'rb').read()
    tb = meta['tonemap_bytes']

    for i in range(n):
        v = PARAMS.unpack_from(params, i * PARAMS.size)
        flags = v[1]
        time, jx, jy = v[3], v[10], v[11]
        names = ','.join(t for b, t in FLAGS.items() if flags & b)
        line = f'{i:4} t={time:7.3f} jitter=({jx:+.3f},{jy:+.3f}) [{names}]'
        if flags & 8:
            cb2 = struct.unpack_from('<64f', tonemap, i * tb)
            lum = struct.unpack_from('<f', tonemap, i * tb + 256)[0]
            line += f' L={lum:.5f} exposure={elite_exposure(cb2, lum):.3f}'
        print(line)

    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1]))
