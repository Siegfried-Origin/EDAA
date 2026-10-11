"""Dump raw buffer contents at an event, as float / uint rows of 16 bytes.

EDAA_ARGS: "EID;RES:OFFSET:SIZE[;RES:OFFSET:SIZE...]", e.g. "5001;63773:0:5376".
Writes EDAA_OUT/buffer_<res>_<eid>.txt (readable) and .bin (raw).
"""

import os
import struct

import rdlib


def main(c):
    out = rdlib.env("EDAA_OUT")
    parts = rdlib.env("EDAA_ARGS").split(";")
    eid = int(parts[0])
    c.SetFrameEvent(eid, True)
    ids = {int(r.resourceId): r.resourceId for r in c.GetResources()}
    for spec in parts[1:]:
        res, off, size = (int(x) for x in spec.split(":"))
        raw = bytes(c.GetBufferData(ids[res], off, size))
        with open(os.path.join(out, "buffer_%d_%d.bin" % (res, eid)), "wb") as fh:
            fh.write(raw)
        lines = []
        for i in range(0, len(raw) - 15, 16):
            f = struct.unpack("<4f", raw[i:i + 16])
            u = struct.unpack("<4I", raw[i:i + 16])
            lines.append("+%-6d %14.6g %14.6g %14.6g %14.6g   | %08x %08x %08x %08x" % ((off + i,) + f + u))
        with open(os.path.join(out, "buffer_%d_%d.txt" % (res, eid)), "w") as fh:
            fh.write("\n".join(lines))


rdlib.run(main)
