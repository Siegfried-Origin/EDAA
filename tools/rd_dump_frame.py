"""Dump a whole frame to JSON for offline analysis.

Output (in EDAA_OUT):
  frame.json   - textures, resource names, and one record per action
                 (draw/dispatch/clear/copy/resolve) with targets, shaders
                 (incl. 3DMigoto hash), viewports and bound constant buffers.
  cbuffers.json - unique constant-buffer contents, keyed by content hash,
                  as float lists (raw bytes reinterpreted as float32).
"""

import hashlib
import os

import renderdoc as rd
import rdlib

STAGES = [
    ("vs", rd.ShaderStage.Vertex),
    ("hs", rd.ShaderStage.Hull),
    ("ds", rd.ShaderStage.Domain),
    ("gs", rd.ShaderStage.Geometry),
    ("ps", rd.ShaderStage.Pixel),
    ("cs", rd.ShaderStage.Compute),
]

INTERESTING = (rd.ActionFlags.Drawcall | rd.ActionFlags.Dispatch | rd.ActionFlags.Clear
               | rd.ActionFlags.Copy | rd.ActionFlags.Resolve | rd.ActionFlags.Present)


def rid(r):
    return int(r) if r != rd.ResourceId.Null() else None


def main(c):
    out = rdlib.env("EDAA_OUT")
    sfile = c.GetStructuredFile()

    names = {int(r.resourceId): r.name for r in c.GetResources()}
    textures = []
    for t in c.GetTextures():
        textures.append({
            "id": int(t.resourceId), "name": names.get(int(t.resourceId), ""),
            "w": t.width, "h": t.height, "d": t.depth, "array": t.arraysize,
            "mips": t.mips, "msaa": t.msSamp, "format": t.format.Name(),
            "type": str(t.type), "flags": str(t.creationFlags),
        })
    buffers = [{"id": int(b.resourceId), "name": names.get(int(b.resourceId), ""),
                "size": b.length, "flags": str(b.creationFlags)} for b in c.GetBuffers()]

    shader_hash = {}
    blobs = {}
    actions = []

    for a in rdlib.iter_actions(c.GetRootActions()):
        if not (a.flags & INTERESTING):
            continue
        rec = {
            "eid": a.eventId, "name": a.GetName(sfile), "flags": int(a.flags),
            "outputs": [rid(o) for o in a.outputs if rid(o) is not None],
            "depth": rid(a.depthOut),
            "indices": a.numIndices, "instances": a.numInstances,
        }
        if a.flags & rd.ActionFlags.Copy or a.flags & rd.ActionFlags.Resolve:
            rec["copy"] = [rid(a.copySource), rid(a.copyDestination)]

        if a.flags & (rd.ActionFlags.Drawcall | rd.ActionFlags.Dispatch):
            c.SetFrameEvent(a.eventId, False)
            s = c.GetPipelineState()
            vp = s.GetViewport(0)
            rec["viewport"] = [vp.x, vp.y, vp.width, vp.height, vp.minDepth, vp.maxDepth]
            ds = c.GetD3D11PipelineState().outputMerger.depthStencilState
            rec["depthfunc"] = str(ds.depthFunction) if ds.depthEnable else None
            rec["depthwrite"] = bool(ds.depthWrites)
            rec["shaders"] = {}
            for key, stage in STAGES:
                sid = s.GetShader(stage)
                if sid == rd.ResourceId.Null():
                    continue
                refl = s.GetShaderReflection(stage)
                if int(sid) not in shader_hash:
                    shader_hash[int(sid)] = rdlib.fnv1_64(bytes(refl.rawBytes)) if refl else None
                cbs = []
                for i, block in enumerate(refl.constantBlocks if refl else []):
                    d = s.GetConstantBlock(stage, i, 0).descriptor
                    res = rid(d.resource)
                    entry = {"slot": block.fixedBindNumber, "res": res,
                             "off": d.byteOffset, "size": d.byteSize, "decl": block.byteSize}
                    if res is not None:
                        size = d.byteSize or block.byteSize
                        raw = bytes(c.GetBufferData(d.resource, d.byteOffset, min(size, 65536)))
                        h = hashlib.md5(raw).hexdigest()[:16]
                        if h not in blobs:
                            blobs[h] = rdlib.floats(raw)
                        entry["data"] = h
                    cbs.append(entry)
                rec["shaders"][key] = {"id": int(sid), "hash": shader_hash[int(sid)], "cb": cbs}
        actions.append(rec)

    rdlib.write_json(os.path.join(out, "frame.json"),
                     {"textures": textures, "buffers": buffers, "actions": actions})
    rdlib.write_json(os.path.join(out, "cbuffers.json"), blobs)
    rdlib.log("actions=%d unique_cbuffers=%d" % (len(actions), len(blobs)))


rdlib.run(main)
