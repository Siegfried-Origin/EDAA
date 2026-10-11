"""Read the scene depth-stencil buffer at an event and save the stencil values.

EDAA_ARGS: "EID" (an event after the G-buffer pass, e.g. the bloom draw).
Writes EDAA_OUT/stencil_<eid>.png (each stencil value a distinct colour),
stencil_<eid>.txt (histogram) and depth/stencil values on a coarse grid.
"""

import os
import struct
import zlib

import renderdoc as rd
import rdlib


def write_png(path, w, h, rgb):
    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + \
        chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
    open(path, "wb").write(data)


def main(c):
    out = rdlib.env("EDAA_OUT")
    eid = int(rdlib.env("EDAA_ARGS"))
    c.SetFrameEvent(eid, True)
    # Largest D32S8 texture = scene depth
    tex = max((t for t in c.GetTextures() if "D32S8" in t.format.Name() or "R32G8X24" in t.format.Name()),
              key=lambda t: t.width * t.height)
    raw = bytes(c.GetTextureData(tex.resourceId, rd.Subresource(0, 0, 0)))
    w, h = tex.width, tex.height
    stride = len(raw) // (w * h)
    hist = {}
    palette = [(0, 0, 0), (230, 60, 60), (60, 200, 60), (60, 90, 230), (230, 200, 40), (200, 60, 200),
               (40, 200, 200), (255, 255, 255), (255, 140, 0)]
    rgb = bytearray(w * h * 3)
    for i in range(w * h):
        s = raw[i * stride + 4]
        hist[s] = hist.get(s, 0) + 1
        col = palette[(s.bit_length()) % len(palette)] if s else (0, 0, 0)
        rgb[i * 3:i * 3 + 3] = bytes(col)
    write_png(os.path.join(out, "stencil_%d.png" % eid), w, h, bytes(rgb))
    lines = ["texture #%d %s %dx%d stride %d" % (int(tex.resourceId), tex.format.Name(), w, h, stride)]
    for k in sorted(hist):
        lines.append("stencil 0x%02x: %d pixels (%.1f%%)" % (k, hist[k], 100.0 * hist[k] / (w * h)))
    lines.append("grid (depth, stencil):")
    for gy in range(1, 8):
        row = []
        for gx in range(1, 12):
            x, y = gx * w // 12, gy * h // 8
            i = (y * w + x) * stride
            row.append("%.4g/%02x" % (struct.unpack_from("<f", raw, i)[0], raw[i + 4]))
        lines.append("  " + " ".join(row))
    open(os.path.join(out, "stencil_%d.txt" % eid), "w").write("\n".join(lines))


rdlib.run(main)
