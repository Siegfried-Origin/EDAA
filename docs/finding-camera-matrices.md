# Finding camera matrices in a DX11 capture

A reusable method to locate view/projection matrices in constant buffers,
using RenderDoc's Python replay API and the scripts in [`tools/`](../tools/README.md).
It was used to produce [reverse-engineering.md](reverse-engineering.md) and
works for any DX11 game, including after an Elite Dangerous update moves
things around.

## 0. Prerequisites

* RenderDoc (tested with 1.46) installed in `C:\Program Files\RenderDoc`.
  Its `qrenderdoc.exe` embeds Python 3.8 with the `renderdoc` module; no
  separate Python module is needed.
* Python 3 with `numpy` for the offline analysis scripts (`py -3`).
* A `.rdc` capture of the game.

`tools/run_rd.ps1` runs a replay script headless: it starts
`qrenderdoc.exe --python <script>`, passes arguments through environment
variables and exits when the script is done.

## 1. Dump the frame

```powershell
.\tools\run_rd.ps1 -Script rd_dump_frame.py -Capture "...\Coriolis.rdc" -Out analysis\coriolis
```

This walks every draw/dispatch, and for each one records render targets,
depth target, viewport, depth state, the shaders (with their **3DMigoto hash**,
so they match `ShaderFixes\` and `ShaderUsage.txt`) and every bound constant
buffer. Constant-buffer contents are stored once per unique content in
`cbuffers.json`. 2-3 GB captures take 1-4 minutes.

```powershell
py -3 tools\frame_passes.py analysis\coriolis          # pass list
py -3 tools\frame_passes.py analysis\coriolis --draws  # with every draw
```

Read the pass list first: shadow passes (depth-only, square or atlas
targets), the G-buffer (several render-size targets + depth), the HDR scene
target, post-processing (small targets, fullscreen 3-vertex draws), the
backbuffer.

## 2. Scan constant buffers for a projection

```powershell
py -3 tools\find_matrices.py analysis\coriolis > analysis\coriolis\matrices.txt
```

For every unique cbuffer content, the scanner slides a 4x4 window over the
declared size at register (16-byte) alignment and tests the window `M` and its
transpose `M^T` for the shape of a perspective projection in column-vector form
(`clip = P * v`):

```
P = | sx  0  jx  0 |
    | 0  sy  jy  0 |
    | 0   0   a  b |
    | 0   0  +-1  0 |      <- clip.w = +-viewZ
```

Mostly zeros, one `+-1`, positive `sx`, `sy`. From `a`, `b` it derives:

* the view depths where NDC depth is 0 and 1 -> **near/far** and **reverse-Z**
  (depth 1 at near) or not; `a = 0` means an infinite far plane;
* `vfov = 2*atan(1/sy)`, `aspect = sy/sx`;
* handedness from the sign of the `+-1`;
* `jx`, `jy`: existing off-centre/jitter terms.

Which of `M` / `M^T` matched tells the storage convention:
*registers = rows* (`clip.x = dot(c0, v)`, HLSL `row_major` with `mul(M, v)`
or `column_major` with `mul(v, M)`), or *registers = columns*.

Sanity checks:

* `vfov` should equal the in-game FOV option (ED: `FOV`, vertical, degrees).
* `aspect` should equal the render-target aspect.
* The candidate used by the most draws, in a buffer bound to most VS, is the
  main camera.
* An inverse projection can also look like a projection (here near=40,
  vfov=121 deg); the scanner drops a candidate when it is the inverse of a
  better-used one.

## 3. Find the matrices built from it

For each projection `P` the scanner then tests every window `X` (and `X^T`):

| Test | Meaning |
|---|---|
| `P^-1*X` is rigid (orthonormal 3x3, last row `0 0 0 1`) | `X` is a **viewProj**; `P^-1*X` is the view matrix |
| `X*P ~ I` | inverse projection |
| `X*VP ~ I` for a viewProj found above | inverse viewProj |

For each viewProj it prints the camera forward vector and translation. A
**zero translation means camera-relative rendering** (the camera position is
then stored separately - look for a float3 equal to `-R^-1*t` of an absolute
viewProj, or for the value subtracted in the VS). A viewProj with a different
forward vector is another camera or another reference frame.

## 4. Confirm in the shader code

Matrices found in a buffer are only candidates. Disassemble the shaders that
read them:

```powershell
.\tools\run_rd.ps1 -Script rd_inspect_draw.py -Capture "...\Coriolis.rdc" -Out analysis\coriolis\draws -ScriptArgs "5001+ps,9002"
.\tools\run_rd.ps1 -Script rd_disasm_all.py   -Capture "...\Coriolis.rdc" -Out analysis\coriolis
```

`rd_inspect_draw.py` prints the input layout, vertex buffers (with the first
per-instance elements), VS SRVs and the VS/PS disassembly; `rd_disasm_all.py`
writes every shader to `analysis/<capture>/shaders/<3dmigoto-hash>-<stage>.txt`
so they can be grepped. Look for the instruction that writes `SV_Position`:

```
add  r4.xyz, r4.xyzx, -cb1[275].xyzx     // position - camera position
mul  r1.xyzw, r0.xxxx, cb1[270].xyzw     // x * row 0
mul  r3.xyzw, r0.yyyy, cb1[271].xyzw
...
add  o4.xyzw, r0.xyzw, cb1[273].xyzw     // -> SV_Position
```

This shows the exact registers, the multiplication order and where the
per-object transform comes from (here a structured buffer `t33` indexed by a
per-instance vertex attribute). Grepping all shaders for the register range
(`cb1\[27[0-3]\]`) lists every consumer; grepping for the structured-buffer
stride (`stride=336`) and its offsets maps the per-instance record layout.

## 5. Follow the buffer through the frame

```powershell
py -3 tools\cb_timeline.py analysis\coriolis vs 1 189,196,197,199,270,273,275
py -3 tools\cb_dump.py     analysis\coriolis 5000 vs 1 [first_reg [last_reg]]
py -3 tools\check_layout.py analysis\coriolis
```

* `cb_timeline.py` prints one entry each time the content bound to a stage/slot
  changes, with chosen registers - this reveals buffers that are re-Mapped per
  view (shadow cascades, other cameras, HUD).
* `cb_dump.py` prints a whole buffer as float4 registers at an event.
* `check_layout.py` validates the documented ED layout on a new capture and
  lists every distinct view (main camera vs others, with their targets). Run
  it first after a game update: if it still reports one `MAIN CAMERA` view
  drawing into the G-buffer, the layout is unchanged.

## 6. Cross-check with depth

`rd_check_depth.py` takes a main-camera draw, reads post-VS clip positions of
some of its vertices, projects them to pixels and compares `z/w` with the
depth buffer at that pixel after the draw:

```powershell
.\tools\run_rd.ps1 -Script rd_check_depth.py -Capture "...\Coriolis.rdc" -Out analysis\coriolis -ScriptArgs "5001"
```

Result for ED (Coriolis, EID 5001): every vertex has `clip.z = 0.025`,
`clip.w = viewZ` (~115 m), and `0.025 / viewZ` matches the `D32_FLOAT`
depth at its pixel to 4 significant digits (e.g. 0.000216219 vs 0.000216087);
the sky is 0. This confirms reverse-Z, infinite far plane, near = 0.025.
## 6b. Pitfall: stale registers in RenderDoc

RenderDoc keeps a persistent shadow copy of every mapped buffer. When a game
Maps a constant buffer with `WRITE_DISCARD` but only writes part of it, the
registers it does not write keep **old values** in the capture (from an
earlier frame, a level load...), while live they are undefined (zeros in
ED). A matrix found by the scanner can therefore be perfectly plausible and
still never exist in the running game. ED example: `cb1 c196-c199` looks like
the main projection in every capture, but is all zeros live.

Before relying on a register, check that shaders read it:

```bash
grep -l "cb1\[196\]" analysis/*/shaders/*.txt | wc -l    # 0 -> not used, do not trust
grep -l "cb1\[270\]" analysis/*/shaders/*.txt | wc -l    # 255 -> safe
```

and confirm in game (the EDAA add-on's diagnostics/dump shows the live
registers).

## 6c. Pitfall: the live frame is not the captured frame

* Pass order and shaders can differ between a RenderDoc capture and the
  running game (other settings, other shader permutations). In ED the fog
  composite moved before the sky and planets and used another shader. Check
  injection points on a **live** frame: the EDAA add-on can log one frame of
  binds and draws ("Dump/trace next frame", see docs/addon.md).
* D3D11 `OMSetRenderTargets` binds as seen by ReShade always report 8 slots,
  empty ones included: count the non-empty views.
* Shader hashes change with game updates; prefer structural markers (target
  formats and sizes, depth state) and keep hashes as a second check.

## 7. Per-instance data

When the VS has no world matrix in any cbuffer, look at the input layout for
per-instance attributes and at VS SRVs (structured buffers). Dump the buffer:

```powershell
.\tools\run_rd.ps1 -Script rd_buffer_dump.py -Capture "...\Coriolis.rdc" -Out analysis\coriolis\buffers -ScriptArgs "5001;63773:0:0"
```

(`EID;resourceId:offset:size`, size 0 = whole buffer; resource id from
`rd_inspect_draw.py`). The `.bin` output can be compared between records or
between two consecutive captured frames to find previous-frame data.
