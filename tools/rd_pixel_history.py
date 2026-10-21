"""Pixel history of a few render-size pixels: every event that touched the scene
depth or the HDR scene colour there, with the pixel shader hash and depth test.

EDAA_ARGS: "x,y;x,y;..." in render-size pixels. Writes EDAA_OUT/pixel_history.txt.
"""

import os

import renderdoc as rd
import rdlib


def shader_hash(c, eid, cache):
    c.SetFrameEvent(eid, False)
    s = c.GetPipelineState()
    sid = s.GetShader(rd.ShaderStage.Pixel)

    if sid == rd.ResourceId.Null():
        return "-"

    if int(sid) not in cache:
        refl = s.GetShaderReflection(rd.ShaderStage.Pixel)
        cache[int(sid)] = rdlib.fnv1_64(bytes(refl.rawBytes)) if refl else "?"

    return cache[int(sid)]


def main(c):
    out = rdlib.env("EDAA_OUT")
    points = [tuple(int(v) for v in p.split(",")) for p in rdlib.env("EDAA_ARGS").split(";") if p]
    actions = {a.eventId: a for a in rdlib.iter_actions(c.GetRootActions())}
    sd = c.GetStructuredFile()
    textures = c.GetTextures()
    # Render size = the largest depth size that also has an R11G11B10 (HDR scene) target.
    depths = [t for t in textures if "D32S8" in t.format.Name() or "R32G8X24" in t.format.Name()]
    hdr = {(t.width, t.height) for t in textures if "R11G11B10" in t.format.Name()}
    sizes = sorted({(t.width, t.height) for t in depths if (t.width, t.height) in hdr}, key=lambda s: s[0] * s[1])
    render = sizes[-1]
    targets = [t for t in textures if (t.width, t.height) == render and
               (t in depths or "R11G11B10" in t.format.Name() or "R10G10B10A2" in t.format.Name())]
    last = max(actions)
    cache = {}
    lines = ["render size %dx%d" % render]

    for t in targets:
        lines.append("target #%d %s" % (int(t.resourceId), t.format.Name()))

    for x, y in points:
        for t in targets:
            lines.append("")
            lines.append("== pixel %d,%d  #%d %s" % (x, y, int(t.resourceId), t.format.Name()))
            history = c.PixelHistory(t.resourceId, x, y, rd.Subresource(0, 0, 0), rd.CompType.Typeless)

            for m in history:
                a = actions.get(m.eventId)
                name = a.GetName(sd) if a else "?"
                flags = []

                if m.depthTestFailed:
                    flags.append("DEPTH FAIL")
                if m.stencilTestFailed:
                    flags.append("stencil fail")
                if m.shaderDiscarded:
                    flags.append("discard")
                if m.backfaceCulled:
                    flags.append("culled")

                post = m.postMod
                frag = m.shaderOut
                pre = m.preMod
                lines.append("  %6d prim %-4d ps=%s %-40s depth %.7g -> %.7g (frag %.7g)  col (%.4g %.4g %.4g) %s" % (
                    m.eventId, m.primitiveID, shader_hash(c, m.eventId, cache), name[:40], pre.depth, post.depth, frag.depth,
                    post.col.floatValue[0], post.col.floatValue[1], post.col.floatValue[2], " ".join(flags)))

        c.SetFrameEvent(last, True)

    with open(os.path.join(out, "pixel_history.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines))


rdlib.run(main)
