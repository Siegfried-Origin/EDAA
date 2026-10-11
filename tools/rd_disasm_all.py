"""Disassemble every shader used in the frame.

Writes EDAA_OUT/shaders/<hash>-<stage>.txt (hash = 3DMigoto FNV hash, so the
files match ShaderFixes/ naming) and EDAA_OUT/shaders/index.txt listing the
first event where each shader is used.
"""

import os

import renderdoc as rd
import rdlib

STAGES = [("vs", rd.ShaderStage.Vertex), ("hs", rd.ShaderStage.Hull),
          ("ds", rd.ShaderStage.Domain), ("gs", rd.ShaderStage.Geometry),
          ("ps", rd.ShaderStage.Pixel), ("cs", rd.ShaderStage.Compute)]


def main(c):
    out = os.path.join(rdlib.env("EDAA_OUT"), "shaders")
    os.makedirs(out, exist_ok=True)
    target = c.GetDisassemblyTargets(True)[0]
    done = {}
    for a in rdlib.iter_actions(c.GetRootActions()):
        if not a.flags & (rd.ActionFlags.Drawcall | rd.ActionFlags.Dispatch):
            continue
        c.SetFrameEvent(a.eventId, False)
        s = c.GetPipelineState()
        for key, stage in STAGES:
            sid = s.GetShader(stage)
            if sid == rd.ResourceId.Null() or int(sid) in done:
                continue
            refl = s.GetShaderReflection(stage)
            h = rdlib.fnv1_64(bytes(refl.rawBytes))
            pipe = s.GetComputePipelineObject() if key == "cs" else s.GetGraphicsPipelineObject()
            text = c.DisassembleShader(pipe, refl, target)
            with open(os.path.join(out, "%s-%s.txt" % (h, key)), "w", encoding="utf-8") as f:
                f.write(text)
            done[int(sid)] = (a.eventId, key, h)
    with open(os.path.join(out, "index.txt"), "w") as f:
        for eid, key, h in sorted(done.values()):
            f.write("%d %s %s\n" % (eid, key, h))


rdlib.run(main)
