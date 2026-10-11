# Analysis tools

Scripts used to reverse-engineer the renderer from RenderDoc captures.
Method and examples: [docs/finding-camera-matrices.md](../docs/finding-camera-matrices.md).
Output goes to `analysis/` (ignored by git).

## Replay scripts (run inside qrenderdoc)

Run with `tools\run_rd.ps1 -Script <name> -Capture <file.rdc> -Out <folder> [-ScriptArgs "..."]`.
Errors are written to `<Out>\log.txt`.

| Script | Purpose | `-ScriptArgs` |
|---|---|---|
| `rd_dump_frame.py` | Dump every action: targets, viewport, depth state, shaders (3DMigoto hash), cbuffer contents -> `frame.json`, `cbuffers.json` | - |
| `rd_inspect_draw.py` | Input layout, vertex buffers, VS SRVs, VS/PS disassembly of chosen draws -> `draw_<eid>.txt` | `5001+ps,9002` |
| `rd_disasm_all.py` | Disassemble every shader of the frame -> `shaders/<hash>-<stage>.txt` | - |
| `rd_buffer_dump.py` | Raw buffer contents at an event -> `.txt` and `.bin` | `EID;RES:OFFSET:SIZE` |
| `rd_check_depth.py` | Compare post-VS clip depth of a draw with the depth buffer -> `depth_<eid>.txt` | `5001` |
| `rd_stencil.py` | Stencil values of the scene depth at an event (histogram, PNG, grid) | `6723` |
| `rd_probe.py` | Print the RenderDoc API surface (dev helper) | - |
| `rdlib.py` | Shared helpers (open capture, 3DMigoto FNV hash, ...) | |

## Offline scripts (Python 3 + numpy)

| Script | Purpose |
|---|---|
| `frame_passes.py <dump> [--draws]` | Pass list grouped by render targets |
| `find_matrices.py <dump>` | Scan cbuffers for projection, viewProj and inverses |
| `cb_timeline.py <dump> <stage> <slot> <regs>` | How a cbuffer changes through the frame |
| `cb_dump.py <dump> <eid> <stage> <slot> [first [last]]` | Print a cbuffer as float4 registers |
| `check_layout.py <dump>` | Validate the documented ED camera layout, list all views |
| `compare_instances.py <prev.bin> <cur.bin>` | Compare `t33` instance records of two consecutive frames |
| `check_motion.py <prev dump> <cur dump> <prev t33.bin> <cur t33.bin>` | Validate the motion vector math against static objects of a 2-frame capture (ground truth vs shader formula) |
| `minidump_info.py <crash.dmp> [module ...]` | Exception, faulting address and raw stack scan of a crash dump (no debugger needed); resolve EDAA offsets with a `/MAP` build |

## Packaging

`package.ps1`: Release build, then `dist\EDAA-<date>\` (`EDAA.addon64`, `INSTALL.txt`) and `dist\EDAA-<date>.zip`.
