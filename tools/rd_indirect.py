"""List the indirect draws and dispatches of a capture, with their targets.

Writes EDAA_OUT/indirect.txt: event, call, pixel/compute shader hash, render
targets and depth (indirect draws never get the jitter viewport, see
frame_tracking.cpp on_indirect).
"""

import os

import renderdoc as rd
import rdlib


def main(c):
    out = rdlib.env("EDAA_OUT")
    textures = {int(t.resourceId): t for t in c.GetTextures()}
    sd = c.GetStructuredFile()
    cache = {}
    lines = []

    def describe(rid):
        t = textures.get(int(rid))
        return "#%d %dx%d %s" % (int(rid), t.width, t.height, t.format.Name()) if t else "#%d" % int(rid)

    def stage_hash(s, stage):
        sid = s.GetShader(stage)

        if sid == rd.ResourceId.Null():
            return "-"

        if int(sid) not in cache:
            refl = s.GetShaderReflection(stage)
            cache[int(sid)] = rdlib.fnv1_64(bytes(refl.rawBytes)) if refl else "?"

        return cache[int(sid)]

    for a in rdlib.iter_actions(c.GetRootActions()):
        if not a.flags & rd.ActionFlags.Indirect:
            continue

        c.SetFrameEvent(a.eventId, False)
        s = c.GetPipelineState()
        outputs = [o.resource for o in s.GetOutputTargets() if o.resource != rd.ResourceId.Null()]
        depth = s.GetDepthTarget().resource
        lines.append("%6d %-40s ps=%s cs=%s -> %s depth %s" % (
            a.eventId, a.GetName(sd)[:40], stage_hash(s, rd.ShaderStage.Pixel), stage_hash(s, rd.ShaderStage.Compute),
            ", ".join(describe(o) for o in outputs) or "-", describe(depth) if depth != rd.ResourceId.Null() else "-"))

    with open(os.path.join(out, "indirect.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) if lines else "no indirect call")


rdlib.run(main)
