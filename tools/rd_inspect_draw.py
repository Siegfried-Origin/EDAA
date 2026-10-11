"""Inspect specific draws in detail.

EDAA_ARGS: comma-separated event ids, optionally suffixed with "+ps" to also
disassemble the pixel shader, e.g. "5001,7901+ps".

For each event writes EDAA_OUT/draw_<eid>.txt with:
  - vertex input layout and vertex buffer bindings (+ the first per-instance
    elements decoded as floats)
  - VS resources (SRVs) and constant buffers
  - VS (and optionally PS) DXBC disassembly
"""

import os
import struct

import renderdoc as rd
import rdlib


def disasm(c, s, stage):
    refl = s.GetShaderReflection(stage)
    if refl is None:
        return "(none)"
    pipe = s.GetGraphicsPipelineObject()
    target = c.GetDisassemblyTargets(True)[0]
    return c.DisassembleShader(pipe, refl, target)


def main(c):
    out = rdlib.env("EDAA_OUT")
    for item in rdlib.env("EDAA_ARGS").split(","):
        item = item.strip()
        if not item:
            continue
        eid = int(item.split("+")[0])
        c.SetFrameEvent(eid, True)
        s = c.GetPipelineState()
        lines = ["eid %d" % eid]

        vbs = s.GetVBuffers()
        lines.append("== vertex buffers ==")
        for i, vb in enumerate(vbs):
            if vb.resourceId != rd.ResourceId.Null():
                lines.append("  slot %d: res %d stride %d offset %d" % (
                    i, int(vb.resourceId), vb.byteStride, vb.byteOffset))

        lines.append("== input layout ==")
        inst_slots = set()
        for a in s.GetVertexInputs():
            lines.append("  %-16s fmt=%-20s slot=%d off=%d perInstance=%s rate=%d" % (
                a.name, a.format.Name(), a.vertexBuffer, a.byteOffset,
                a.perInstance, a.instanceRate))
            if a.perInstance:
                inst_slots.add(a.vertexBuffer)

        for slot in sorted(inst_slots):
            vb = vbs[slot]
            raw = bytes(c.GetBufferData(vb.resourceId, vb.byteOffset, vb.byteStride * 4))
            lines.append("== per-instance data, slot %d (first 4 instances, floats) ==" % slot)
            for i in range(4):
                chunk = raw[i * vb.byteStride:(i + 1) * vb.byteStride]
                f = rdlib.floats(chunk)
                lines.append("  [%d] " % i + " ".join("%.6g" % x for x in f))

        lines.append("== VS read-only resources ==")
        for used in s.GetReadOnlyResources(rd.ShaderStage.Vertex):
            d = used.descriptor
            lines.append("  bind %d res %d fmt %s elemsize %s" % (
                used.access.index, int(d.resource), d.format.Name(), d.elementByteSize))

        lines.append("== PS read-only resources ==")
        for used in s.GetReadOnlyResources(rd.ShaderStage.Pixel):
            d = used.descriptor
            lines.append("  bind %d res %d fmt %s" % (used.access.index, int(d.resource), d.format.Name()))
        lines.append("== output targets ==")
        for o in s.GetOutputTargets():
            if o.resource != rd.ResourceId.Null():
                lines.append("  rt res %d fmt %s" % (int(o.resource), o.format.Name()))

        lines.append("== VS disassembly ==")
        lines.append(disasm(c, s, rd.ShaderStage.Vertex))
        if "+ps" in item:
            lines.append("== PS disassembly ==")
            lines.append(disasm(c, s, rd.ShaderStage.Pixel))

        with open(os.path.join(out, "draw_%d.txt" % eid), "w", encoding="utf-8") as f:
            f.write("\n".join(lines))


rdlib.run(main)
