"""How a buffer is created and written during the frame.

EDAA_ARGS: resource id, e.g. "56917".
Writes EDAA_OUT/usage_<res>.txt: buffer description (size, creation flags),
every event that uses it (RenderDoc usage: CPU write, copy, UAV, SRV...), and
the API calls whose parameters reference it (Map/Unmap, UpdateSubresource,
Copy*), so you can tell whether the CPU or a compute shader fills it.
"""

import os

import renderdoc as rd
import rdlib


def references(obj, res_id):
    """True if a structured chunk parameter tree references the resource."""
    if obj.type.basetype == rd.SDBasic.Resource and obj.AsResourceId() == res_id:
        return True
    return any(references(obj.GetChild(i), res_id) for i in range(obj.NumChildren()))


def main(c):
    out = rdlib.env("EDAA_OUT")
    res_num = int(rdlib.env("EDAA_ARGS"))
    res_id = next(r.resourceId for r in c.GetResources() if int(r.resourceId) == res_num)
    lines = []

    for b in c.GetBuffers():
        if b.resourceId == res_id:
            lines.append("buffer %d: %d bytes, creation flags %s" % (res_num, b.length, str(b.creationFlags)))

    lines.append("")
    lines.append("Usage by event:")
    counts = {}
    for u in c.GetUsage(res_id):
        name = str(u.usage).split(".")[-1]
        counts[name] = counts.get(name, 0) + 1
        lines.append("  EID %6d  %s" % (u.eventId, name))
    lines.insert(1, "usage counts: " + ", ".join("%s %d" % kv for kv in sorted(counts.items())))

    lines.append("")
    lines.append("API calls referencing it (excluding draws/binds):")
    sf = c.GetStructuredFile()
    calls = {}
    for chunk in sf.chunks:
        name = chunk.name
        if not any(k in name for k in ("Map", "Unmap", "Update", "Copy", "Create", "Discard")):
            continue
        if references(chunk, res_id):
            calls[name] = calls.get(name, 0) + 1
    for name, n in sorted(calls.items()):
        lines.append("  %-60s %d" % (name, n))

    with open(os.path.join(out, "usage_%d.txt" % res_num), "w") as fh:
        fh.write("\n".join(lines) + "\n")


rdlib.run(main)
