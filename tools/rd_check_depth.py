"""Cross-check the projection against the depth buffer at one draw.

EDAA_ARGS: event id of a main-camera draw, e.g. "5001".
For a few of its post-VS vertices, prints clip position, the predicted NDC
depth (z/w) and pixel, and the depth-buffer value at that pixel after the
draw. Also prints min/max of the depth buffer. Output: EDAA_OUT/depth_<eid>.txt
"""

import os
import struct

import renderdoc as rd
import rdlib


def main(c):
    eid = int(rdlib.env("EDAA_ARGS"))
    c.SetFrameEvent(eid, True)
    s = c.GetPipelineState()
    vp = s.GetViewport(0)
    depth_id = s.GetDepthTarget().resource
    tex = next(t for t in c.GetTextures() if t.resourceId == depth_id)
    lines = ["eid %d depth #%d %s %dx%d" % (eid, int(depth_id), tex.format.Name(), tex.width, tex.height)]

    # Whole depth surface (D32_FLOAT_S8X24: 8 bytes per texel, float first).
    raw = bytes(c.GetTextureData(depth_id, rd.Subresource(0, 0, 0)))
    stride = len(raw) // (tex.width * tex.height)
    depth = lambda x, y: struct.unpack_from("<f", raw, (y * tex.width + x) * stride)[0]
    vals = [struct.unpack_from("<f", raw, i)[0] for i in range(0, len(raw), stride * 97)]
    lines.append("depth min %g max %g (sampled), bytes/texel %d" % (min(vals), max(vals), stride))

    mesh = c.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
    data = bytes(c.GetBufferData(mesh.vertexResourceId, mesh.vertexByteOffset, mesh.vertexByteStride * 4000))
    n = len(data) // mesh.vertexByteStride
    checked = 0
    for i in range(0, n, max(1, n // 400)):
        x, y, z, w = struct.unpack_from("<4f", data, i * mesh.vertexByteStride)
        if w <= 0:
            continue
        px = int((x / w * 0.5 + 0.5) * vp.width + vp.x)
        py = int((0.5 - y / w * 0.5) * vp.height + vp.y)
        if not (0 <= px < tex.width and 0 <= py < tex.height):
            continue
        d = depth(px, py)
        lines.append("v%-5d clip=(%.4g %.4g %.4g %.4g) ndcZ=%.6g pixel=(%d,%d) depthbuf=%.6g viewZ=%.4g" % (
            i, x, y, z, w, z / w, px, py, d, w))
        checked += 1
        if checked >= 25:
            break
    with open(os.path.join(rdlib.env("EDAA_OUT"), "depth_%d.txt" % eid), "w") as f:
        f.write("\n".join(lines))


rdlib.run(main)
