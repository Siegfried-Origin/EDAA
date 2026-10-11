"""Compare the t33 instance buffer (336-byte records) of two consecutive frames.

Checks whether the second transform copy of frame N (+288..+319) equals the
current transform of frame N-1 (+0..+31), i.e. a previous-frame transform.

Usage:
    py -3 tools/compare_instances.py prev.bin cur.bin
"""

import struct
import sys

STRIDE = 336


def records(path):
    raw = open(path, "rb").read()
    return [raw[i:i + STRIDE] for i in range(0, len(raw) - STRIDE + 1, STRIDE)]


def cur_xf(r):
    """(bone, scale, quat bytes, position) of the current transform."""
    return r[0:4], r[4:8], r[8:16], r[16:28]


def alt_xf(r):
    """Same fields from the second copy: +292 pos, +304 bone, +308 scale, +312 quat."""
    return r[304:308], r[308:312], r[312:320], r[292:304]


def pos(b):
    return struct.unpack("<3f", b)


def main():
    prev, cur = records(sys.argv[1]), records(sys.argv[2])
    n = min(len(prev), len(cur))
    stats = {"used": 0, "static": 0, "moved": 0, "alt_eq_prev": 0, "alt_eq_cur": 0, "alt_other": 0}
    examples = []
    for i in range(n):
        p, c = prev[i], cur[i]
        if c == bytes(STRIDE) or p == bytes(STRIDE):
            continue
        stats["used"] += 1
        if cur_xf(c) == cur_xf(p):
            stats["static"] += 1
            continue
        stats["moved"] += 1
        if alt_xf(c) == cur_xf(p):
            stats["alt_eq_prev"] += 1
        elif alt_xf(c) == cur_xf(c):
            stats["alt_eq_cur"] += 1
        else:
            stats["alt_other"] += 1
        if len(examples) < 6:
            examples.append((i, pos(cur_xf(p)[3]), pos(cur_xf(c)[3]), pos(alt_xf(c)[3]),
                             struct.unpack("<f", p[288:292])[0], struct.unpack("<f", c[288:292])[0]))
    print("records %d (prev %d, cur %d)" % (n, len(prev), len(cur)))
    for k, v in stats.items():
        print("  %-12s %d" % (k, v))
    for i, pp, pc, pa, f288p, f288c in examples:
        print("  rec %d prev pos %s cur pos %s cur alt pos %s  +288: %g -> %g" % (
            i, [round(x, 3) for x in pp], [round(x, 3) for x in pc], [round(x, 3) for x in pa], f288p, f288c))


if __name__ == "__main__":
    main()
