"""Which later events read the texture written by a given pixel shader.

EDAA_ARGS: "<pixel shader hash>". Finds the draws using that shader, takes their
render target, then lists every later draw or dispatch binding it as an input,
with its shaders, targets, viewport and depth state. Writes EDAA_OUT/readers.txt.
"""

import os

import renderdoc as rd
import rdlib


def stage_hash(state, stage, cache):
    sid = state.GetShader(stage)

    if sid == rd.ResourceId.Null():
        return "-"

    if int(sid) not in cache:
        refl = state.GetShaderReflection(stage)
        cache[int(sid)] = rdlib.fnv1_64(bytes(refl.rawBytes)) if refl else "?"

    return cache[int(sid)]


def main(c):
    out = rdlib.env("EDAA_OUT")
    wanted = rdlib.env("EDAA_ARGS").strip().lower()
    textures = {int(t.resourceId): t for t in c.GetTextures()}
    sd = c.GetStructuredFile()
    actions = [a for a in rdlib.iter_actions(c.GetRootActions())
               if a.flags & (rd.ActionFlags.Drawcall | rd.ActionFlags.Dispatch)]
    cache = {}
    lines = []
    written = set()

    def describe(rid):
        t = textures.get(int(rid))
        return "#%d %dx%d %s" % (int(rid), t.width, t.height, t.format.Name()) if t else "#%d" % int(rid)

    for a in actions:
        c.SetFrameEvent(a.eventId, False)
        s = c.GetPipelineState()
        ps = stage_hash(s, rd.ShaderStage.Pixel, cache)
        outputs = [o.resource for o in s.GetOutputTargets() if o.resource != rd.ResourceId.Null()]
        depth = s.GetDepthTarget().resource

        if ps == wanted:
            written.update(int(o) for o in outputs)
            lines.append("%6d WRITER ps=%s -> %s" % (a.eventId, ps, ", ".join(describe(o) for o in outputs)))
            continue

        if not written:
            continue

        reads = []

        for stage in (rd.ShaderStage.Pixel, rd.ShaderStage.Compute, rd.ShaderStage.Vertex):
            for used in s.GetReadOnlyResources(stage):
                res = used.descriptor.resource
                if int(res) in written:
                    reads.append("%s t%d" % (str(stage).split(".")[-1], used.access.index))

        if reads:
            vp = s.GetViewport(0)
            lines.append("%6d %-34s ps=%s cs=%s reads %s -> %s depth %s vp %.0fx%.0f" % (
                a.eventId, a.GetName(sd)[:34], ps, stage_hash(s, rd.ShaderStage.Compute, cache), ", ".join(reads),
                ", ".join(describe(o) for o in outputs) or "-", describe(depth) if depth != rd.ResourceId.Null() else "-",
                vp.width, vp.height))

    with open(os.path.join(out, "readers.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines))


rdlib.run(main)
