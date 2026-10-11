"""Probe: print API shape on the first draw (dev helper)."""
import renderdoc as rd
import rdlib

def main(c):
    acts = [a for a in rdlib.iter_actions(c.GetRootActions()) if a.flags & rd.ActionFlags.Drawcall]
    rdlib.log("draws %d" % len(acts))
    a = acts[len(acts)//2]
    c.SetFrameEvent(a.eventId, True)
    s = c.GetPipelineState()
    rdlib.log([x for x in dir(s) if not x.startswith('_')])
    d = c.GetD3D11PipelineState()
    rdlib.log([x for x in dir(d) if not x.startswith('_')])
    rdlib.log([x for x in dir(d.vertexShader) if not x.startswith('_')])
    rdlib.log([x for x in dir(a) if not x.startswith('_')])
    try:
        cb = s.GetConstantBlock(rd.ShaderStage.Vertex, 0, 0)
        rdlib.log(type(cb)); rdlib.log([x for x in dir(cb) if not x.startswith('_')])
    except Exception as e:
        rdlib.log("GetConstantBlock: %r" % e)
    try:
        cbs = s.GetConstantBlocks(rd.ShaderStage.Vertex)
        rdlib.log(len(cbs)); rdlib.log([x for x in dir(cbs[0]) if not x.startswith('_')])
        rdlib.log([x for x in dir(cbs[0].descriptor) if not x.startswith('_')])
    except Exception as e:
        rdlib.log("GetConstantBlocks: %r" % e)
    refl = s.GetShaderReflection(rd.ShaderStage.Vertex)
    rdlib.log([x for x in dir(refl) if not x.startswith('_')])
    rdlib.log([(b.name, b.byteSize, b.fixedBindNumber if hasattr(b,'fixedBindNumber') else None) for b in refl.constantBlocks])

rdlib.run(main)
