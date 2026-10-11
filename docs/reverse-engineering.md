# Elite Dangerous Odyssey (DX11) - rendering reverse engineering

Findings for adding TAA (later XeSS/DLSS) to Elite Dangerous Odyssey.
Game build `2026.09.03.332841`, RenderDoc 1.46, DX11.

Sources: the seven RenderDoc captures in
`Documents\Elite Dangerous\project ED Screenshot\Hack\` (Coriolis, Freecam,
On Foot, Ship inside, Menu, Loading, Light distord) plus two pairs of
consecutive frames (`Cockpit turn-frame6427/6428`, `SRV-frame25785/25786`,
see section 5), analysed with the scripts in [`tools/`](../tools/README.md). Event ids (EID) below refer to
`Coriolis.rdc` unless stated otherwise. How the camera data was located is
explained step by step in [finding-camera-matrices.md](finding-camera-matrices.md).

Capture settings (from `Options\Graphics`): display 2560x1440,
`SSAAMultiplier=1.5` -> internal render size **3840x2160**, `AAMode=4`
(SMAA, see pass list), `FOV=58.67` (ship), `HumanoidFOV=56.25` (on foot),
`UpscalingQuality=0`, `StereoscopicMode=0`.

---

## 1. Camera constant buffer

### 1.1 Where it is

One per-view constant buffer, bound as **`cb1` in VS, PS and CS**
(same D3D11 buffer for the whole frame: `#3476` in Coriolis, `#3466` in most
other captures). Shaders declare `cb1[276]` (4416 bytes) up to `cb1[333]`;
the buffer object itself is 64 KB, so data past the declared size is stale.

The buffer is **Map(WRITE_DISCARD)'d many times per frame**: once per shadow
view, once per main-camera batch, once for the HUD, etc. Its content describes
the view currently being drawn plus copies of main-camera data.

Packing: `float4` registers `cN` (byte offset = 16*N).

> **Live vs RenderDoc.** In the running game (ReShade add-on reading the
> mapped memory at Unmap) `c189` and `c192-c199` are **all zeros** in every
> Map: the game does not write them. RenderDoc keeps a persistent shadow copy
> of mapped buffers, so registers that are not rewritten show stale values
> from some earlier write. The disassembly confirms it: across all captured
> shaders, `cb1[196..199]` is read by **no** shader, and `cb1[189]`,
> `cb1[192..195]` by a single on-foot pixel shader. Only trust registers that
> shaders actually read (grep `analysis/*/shaders`): `c270-c275` (255 and 322
> shader files) and `c277-c279` are safe. Rows below marked *(stale)* must
> not be used.

| Register(s) | Content | Notes |
|---|---|---|
| `c189` | `(renderW, renderH, 1/renderW, 1/renderH)` *(stale)* | not written live; take the render size from the G-buffer depth target |
| `c192-c195` | "Reference" camera viewProj, camera-relative *(stale)* | constant for the whole frame. Equals the rendered viewProj when idle, differs by ~0.2 deg in flight, holds an unrelated matrix in the SRV: **do not use to identify the main camera** (section 5) |
| `c196-c199` | Main camera projection P *(stale)* | not written live, read by no shader; P is derived from `c270-c273` instead (see 1.2) |
| `c229-c232` | camera->world rotation (3x3 in xyz, `c232=(0,0,0,1)`) | |
| `c233-c236` | camera->world with camera position in `.w` | |
| `c237-c240` | screen-UV/depth -> view-space ray (inverse P in UV space) *(read by no captured shader)* | `c237=(2/sx,0,0,-1/sx)`, `c238=(0,-2/sy,0,1/sy)`, `c240.z=1/near` |
| `c241`, `c275`, `c276` | camera position (world, float3) | instance positions are made camera-relative with `pos - c275` |
| `c242`, `c274` | camera forward vector | |
| `c270-c273` | **viewProj of the view currently drawn**, camera-relative | the matrix actually used by the VS; shadow/other views put their own matrix here |
| `c277-c279` | world->view rotation (transpose of `c229-c231`) | used by PS for view-space normals |
| `c35-c38` / `c41-c44`, `c45-c48` | inverse viewProj / viewProj expressed in a second, differently oriented frame (sky / star-system orientation, has translation) | used by sky shaders (`cb2` of EID 8402 holds the same rotation); purpose not fully confirmed |
| `c297` | `(3072, 2048, 1/3072, 1/2048)` | sun shadow cascade tile size (6144x4096 atlas) |
| `c298-c331` | sun shadow cascade matrices/params | |

In the **main menu** (`Menu.rdc`) `c189-c199` are all zero; only `c270-c275`
are filled. Do not rely on `c192-c199` being present outside of the game world.

### 1.2 Conventions

Vertex shaders transform with (from the disassembly of every model VS):

```
p      = worldPos - cb1[275].xyz          // camera-relative position
clip   = p.x*cb1[270] + p.y*cb1[271] + p.z*cb1[272] + cb1[273]
```

i.e. **row-vector convention** (`clip = p * M`), registers are the rows of
`M` (HLSL `row_major float4x4` with `mul(p, M)`). The w column of `M`
(`c270.w, c271.w, c272.w`) is the camera forward vector.

The projection `c196-c199` is stored the other way round
(column-vector convention, `clip = P * v`, registers are rows of `P`):

```
c196 = ( sx, 0,  jx, 0    )      sx = 1.00088  (= sy / aspect)
c197 = ( 0,  sy, jy, 0    )      sy = 1.77935  (= 1/tan(vfov/2), vfov = 58.67 deg)
c198 = ( 0,  0,  0,  near )      near = 0.025 m
c199 = ( 0,  0,  1,  0    )      clip.w = +viewZ  (left-handed, +z forward)
```

* **Left-handed, +z forward, infinite far plane, reverse-Z**:
  `ndcDepth = near / viewZ` (1 at the near plane, -> 0 at infinity).
  Depth test is `GREATER_EQUAL` for opaque geometry.
* `viewZ = 0.025 / depth`. Depth buffer: `D32_FLOAT_S8X24_UINT`.
* **Vertical FOV** comes straight from the game option (`FOV` / `HumanoidFOV`);
  aspect = render aspect.
* `jx`, `jy` (`c196.z`, `c197.z`) are stored as **-0.0**, i.e. the engine
  already computes an off-centre term (probably for VR asymmetric frusta).
  This is where a projection jitter naturally goes.
* All camera matrices are **camera-relative** (no translation): the camera
  position is subtracted separately (`c275`). The "world" is a local
  reference frame (station, ship, planet surface), not galactic coordinates:
  coordinates stay within a few km.

### 1.3 Per-object constant buffer (non-instanced draws)

`cb0` (buffer `#3475`, 128-192 bytes declared) is a per-draw buffer used by
holograms, billboards, signage, particles and the HUD:

| Register(s) | Content |
|---|---|
| `cb0[4..7]` | **world*viewProj**, column-vector convention (`clip.x = dot(cb0[4], pos)`, ...) |
| `cb0[9..11]` | object->camera-relative-world 3x4 (`.w` = translation) |
| `cb0[0..3]` | misc scale/offset (atlas, UV) |

For main-camera draws `cb0[4..7]` = `P * V * W` with the same `P` (near 0.025).
For the HUD it is the HUD projection (near 0.1, far 999.9, also reverse-Z).

---

## 2. Per-object transforms (for motion vectors)

Two families of vertex shaders (see `analysis/*/shaders`, produced by
`tools/rd_disasm_all.py`):

### 2.1 Instanced models (ships, stations, characters, most geometry)

* Vertex input slot 0: `INSTANCEANDMODELDATAINDEX` (`R32G32_UINT`, per
  instance) -> index into a **structured buffer `t33`, stride 336 bytes**
  (one record per instance, ~2000-6500 records per frame).
* Vertex input slot 1: packed vertex data (`PACKEDVERTEXDATAA/B/C`, uint).
* Skinning/bones: structured buffer **`t38`, stride 48 bytes** (3x4 matrices),
  indexed from the record.

Instance record layout (offsets in bytes, confirmed by shader reads):

| Offset | Content |
|---|---|
| `+0`  | `uint` bone/sub-object base index (0 = rigid) |
| `+4`  | `float` uniform scale |
| `+8`  | packed rotation quaternion, 4 x 16-bit (`x*(1/32768)-1`) |
| `+16` | `float3` world position (local reference frame); `+28` `uint` flags |
| `+32..+79` | 16-bit index lists (bones/material) |
| `+288` | `float` (unknown) |
| `+292..+319` | second copy of position + scale + rotation, never read by any shader. **Not the previous frame**: equal to the current `+0..+31` in every record, also in consecutive frames with moving objects (section 5) |
| `+320` | `uint` passed to the PS by 3 shaders |

So per-instance world transform = `T(pos) * R(quat) * S(scale)` (+ bone matrix
for skinned/sub-objects). There is no matrix in a cbuffer for these draws.

### 2.2 Non-instanced draws

Use `cb0[4..7]` (WVP) and `cb0[9..11]` (world 3x4), see section 1.3. Particles and
sky use their own vertex formats / `cb2` (e.g. skybox EID 8402: `cb2[11..14]`).

### 2.3 Consequence for motion vectors

* **Camera motion**: reconstruct from depth with the main camera matrices of
  the current and previous frame (`c270-c273`/`c192-c195` + `c275`). Cheap and
  covers static geometry.
* **Object motion**: the game keeps no previous-frame transform, and the
  `t33` record order changes every frame. The add-on has to keep a copy of the
  previous frame's records and match them to the current ones (section 5),
  then run a velocity pass with the game's geometry and both transforms.

---

## 3. Frame structure and TAA injection point

Same structure in all in-game captures (Coriolis EIDs):

| EID | Pass | Targets |
|---|---|---|
| 47-176 | compute (culling, setup) | |
| 201-1168 | spot/point light shadows (perspective) | `R16` depth 512x512 |
| 1210-3625 | sun shadow cascades (ortho, 5 views) | `R16` depth 6144x4096 atlas |
| 3717-7812 | **G-buffer** (opaque) | RT0 `R10G10B10A2` (normals), RT1 `RGBA8` (albedo), RT2 `RGBA8` (material), RT3 `R11G11B10F` (emissive -> becomes the HDR scene), DS `D32S8` - all 3840x2160 |
| 7813 | copy of RT0 normals | |
| 7834-7847 | linear depth `R32F` + compute (depth pyramid) | |
| 7876-8014 | decals into G-buffer (depth test, no write) | |
| 8030-8148 | compute: AO, light culling | |
| 8172-8265 | deferred lights (volumes + indirect dispatch) | **HDR `R11G11B10F` #3917** + `R16` |
| 8289-8356 | half-res (1920x1080) volumetrics/fog, upsample to `RGBA16F` | |
| 8402-8446 | sky box, star field (7370 instanced quads) | HDR + depth |
| 8471-8528 | sun-occlusion view (other camera, 960x540) + compute | |
| 8552-8847 | planets, atmospheres, sun | HDR + depth |
| 8866 | fullscreen fog/volumetric composite | HDR + `R16` |
| 8890-10307 | **forward transparents** (particles, glass, holograms, lights) | HDR + depth |
| 10332-10393 | small 1024x1024 offscreen pass + compute + 4 draws into HDR (flare/glow, unidentified) | |
| 10394 | **copy HDR -> #3920** (scene copy for refraction) | |
| 10416-10458 | luminance/exposure downsample | 960x540, 320x180 |
| 10482-10598 | post-copy transparents (beams, shields, dust particles) | HDR + depth |
| **10618** | **bloom starts** (reads HDR) | 1920x1080 ... 120x67 |
| 10756-10806 | glare/DOF (480x270) + compute | |
| 10827 | **tonemap** -> LDR `RGBA8` 3840x2160 | |
| 10848-10885 | **SMAA** (edges `R8G8`, weights `RGBA8`, blend) | 3840x2160 |
| 10899 | **SSAA downsample** 3840x2160 -> 2560x1440 backbuffer | |
| 10927-10961 | HUD/UI overlay, own depth `D32S8` 2560x1440 | backbuffer |
| 10966 | Present | |

**TAA injection point**: after the last draw into the HDR scene target
(EID 10598 here) and **before the first bloom downsample** (EID 10618).
Detection that does not depend on EIDs: the first draw whose render target is
a half-size `R11G11B10_FLOAT` and that samples the 3840x2160 `R11G11B10_FLOAT`
scene texture (bloom downsample PS `b196cbbdf516e150` in this build; tonemap PS `380822531cf1f3d4`, SSAA downsample PS `2192bc2cb917f8bb`, main model VS `63d33a9c14411ed8` - all 3DMigoto hashes). At that point:

* scene color: `R11G11B10_FLOAT`, render size (no alpha; TAA history should
  be `R16G16B16A16_FLOAT`);
* depth: `D32_FLOAT_S8X24_UINT` (typeless `R32G8X24`), reverse-Z, includes
  transparents that write depth (particles mostly do not);
* SMAA runs later on the LDR image; with TAA it should be disabled (`AAMode`)
  or left as a light edge pass.

**Resolve point before the HUD (live frame trace)**: cockpit holograms do
not write depth, so camera-based motion vectors give them the motion of
what is behind them and they ghost under TAA. A one-frame trace of the live
game (EDAA "Dump/trace", 2560x1440, no supersampling, in flight) shows a
different order than the September RenderDoc captures: the fog composite
(live pixel shader `e569985b041c66db`, binds the scene depth too) comes
*before* sky, stars and planets, so it is not a good split point. The
cockpit HUD/holograms (pixel shader `b4523c39a461fd35`, depth test on, no
depth write, blending; the shader the ED Screenshot 3DMigoto fork hides)
are all in the last group of draws into the HDR scene, which starts right
after the exposure downsample of the HDR scene (`R11G11B10_FLOAT` at 1/4
render size, e.g. 640x360) and ends at the bloom. The add-on resolves at the
first draw into the HDR scene after that downsample (or at the first HUD
shader draw): the HUD is composited after the TAA, not jittered, not
accumulated. Note that D3D11 render target binds always report 8 slots
(empty ones included); count the non-empty ones.

**Resolution handling**: everything up to SMAA runs at
`display x SSAAMultiplier` (`cb1 c189`); a single fullscreen draw downsamples
to the backbuffer before the HUD. Bloom/glare work at fixed fractions of the
render size. For DLSS/XeSS later: render size = `SSAAMultiplier` < 1, but note
that post-processing (bloom, tonemap, SMAA) runs at render size *before* the
game's own upscale, so an upscaler injected at the TAA point would have to
output render-size x factor and the post chain would have to follow (needs
resizing the post targets or upscaling later, after tonemap).

---

## 4. Things that complicate jitter

1. **Shared camera cbuffer, many Maps per frame**. Shadow views, the sun
   occlusion view, the HUD and the main camera all go through `cb1`.
   Only the main camera may be jittered. Nothing in `cb1` itself says
   "main camera": `c192-c199` is not written live (see the note in 1.1), so
   any test using it fails in game. What the add-on does instead:
   * a perspective camera update is `c273 = (0,0,near,0)` with `c270-c272`
     = rotation * projection (w column of unit length; the x/y column
     lengths give sx, sy, hence FOV and aspect; `c273.z` is the near plane);
   * the **scene depth** is the largest depth buffer bound together with 4
     render targets (G-buffer pass; the 256x256 probe also binds 4); its size
     is the render size;
   * the **main camera** is the perspective camera bound (last written to
     `cb1`) during the most draws into the scene depth in the frame.
   This also covers the **first-person body/weapon on foot**, drawn into the
   same G-buffer with near 0.0675 and a narrower FOV (fewer draws, so not the
   main camera, but still jittered since it targets the scene). The sun
   occlusion view (same projection) renders elsewhere and is not counted.
   (`tools/check_layout.py` still classifies captures with `c196-c199`,
   which is only valid in RenderDoc.)
2. **Two matrix sources**. Instanced models use `cb1 c270-c273`; holograms,
   billboards, particles and decals use the per-draw `cb0[4..7]` (WVP); the sky
   uses `cb2`. Patching matrices means patching all of them.
   Alternative: **jitter through the viewport** (`RSSetViewports` with a
   sub-pixel `TopLeftX/Y`) for draws into the scene targets. It is
   matrix-agnostic and equals a projection jitter for rasterisation.
   Fullscreen/screen-space passes into the same targets (lighting, fog
   composite, 8014, 8866) must not be shifted: restrict to draws with depth
   test enabled, or exclude 3-vertex draws.
3. **Inverse matrices** derived from P (`c237-c240`, `c35-c38`, compute
   `cb1 c8-c11` of the light dispatch) are used by deferred lighting to
   rebuild positions. If P is jittered they should get the same offset;
   with viewport jitter they are off by < 0.5 px, which only matters for SSR
   and contact shadows.
4. **The reference frame depends on the situation**. In `Ship inside.rdc`
   (docked) the camera forward is exactly `(0,0,-1)` at `(0.5, 0, -59.4)`:
   the scene is rendered in ship-local coordinates. In flight
   (`Cockpit turn`) and in the SRV, the frame is a local world frame
   (asteroid field, planet surface): the camera moves and rotates, static
   objects keep their position, and the ship/SRV and its cockpit move through
   their instance transforms (section 5). Either way some pixels are wrong
   with camera-only motion vectors: the exterior when docked, the cockpit in
   flight. Per-object motion (section 2.3) is required.
5. **Cockpit UI is diegetic**: the holographic panels are drawn in the main
   HDR pass with the main camera, so they would be jittered and accumulated.
   They are static in the ship frame, so they will reproject well, but thin
   text will lose some sharpness. The 2D HUD overlay (backbuffer, after the
   downsample) is not affected.
6. **Other views rendered in the same frame** (never jitter):
   spot/point shadows (512^2), sun cascades (6144x4096), sun occlusion (960x540),
   1024^2 offscreen pass, ship hologram target views (512^2, near 0.01,
   `Ship inside.rdc` EID 16339/16361), and a **complete 256x256 deferred
   scene** (environment/reflection probe, `Light distord.rdc` EID 16564-24076)
   whose draws are interleaved with the main pass. Filtering by render-target
   size handles all of them.
7. **Half-res volumetrics** (1920x1080, `RGBA16F`) are reconstructed with the
   full-res depth; jitter will make them shimmer slightly unless they are
   accumulated by TAA after the composite (they are, since the composite is
   before the injection point).
8. **Terrain checkerboard rendering** (`TerrainCheckerboardRenderingEnabled`,
   on in `Settings.xml`, off in `Custom.4.4.fxcfg`) is itself a temporal
   technique. Its interaction with jitter is unverified; test with it off.
9. **VR**: not captured (`StereoscopicMode=0`). Two eyes would mean two main
   views per frame with off-centre P (`c196.z/c197.z`), both need jitter and
   separate histories.
10. **No cubemaps** were seen in the main path apart from the 256^2 probe scene.
11. **EDHM** (3DMigoto) replaces 17 of the shaders used in this frame, including G-buffer PS/VS (e.g. VS `d3e3a357a39baea5`), particles, a bloom PS and the **tonemap PS `380822531cf1f3d4`**. None of them changes the camera constants, and the TAA point sits before the tonemap, so EDHM and TAA should coexist; EDHM was not loaded when the RenderDoc captures were made.
12. `libxess_dx11.dll` (XeSS SDK 2.0.2) sits in the game folder but the game
    executable does not reference it (only FSR1/CAS upscaling strings exist);
    it is not used by the game.

---

## 5. Consecutive frames: motion

Captures `Cockpit turn-frame6427/6428` (flying through an asteroid field
while turning) and `SRV-frame25785/25786` (driving), each two consecutive
frames saved as two files. Settings differ from the first set: display
1440x900, render size **2160x1350**, FOV 64.55 / 63.97 deg, aspect 1.6.
Resource ids are the same in both files of a pair (same process), e.g. the
`t33` buffer is `#56917` in all four. Tools: `tools/compare_instances.py`,
`tools/rd_buffer_dump.py`, `tools/check_layout.py`.

**Camera**

| | Cockpit turn | SRV |
|---|---|---|
| camera position `c275` | `(1278.5, 2694.3, -1856.3)` -> `(1285.0, 2698.7, -1858.5)`, 8.08 m/frame | `(-56.27, -102.92, 1.86)` -> `(-56.90, -103.08, 1.43)`, 0.77 m/frame |
| forward (w column of `c270-c272`) | `(0.8116, 0.5188, -0.2687)` -> `(0.8152, 0.5149, -0.2651)` | rotates too |
| `c192-c195` vs `c270-c273` | ~0.2 deg apart (head look / camera shake) | unrelated matrix (non-zero z column) |
| zoom of `c270` vs `c197` | 1.000 | 0.998 |

The camera itself moves in these frames: the reference frame is a local
world frame, not the ship (compare with `Ship inside.rdc`, section 4.4).

**Instance buffer `t33`**

* `+292..+319` equals the current `+0..+31` in all 7241 (cockpit) and
  1163 (SRV) records of the second frame, including those that moved:
  **it is not a previous-frame transform**.
* **Record order is not stable**: no record of the cockpit pair is at the
  same index in both frames (the same objects shift by ~119 slots). In the
  SRV pair most low indices are stable. Matching has to be by content.
* Static world objects keep exactly the same position: 7049/7241 cockpit
  records and 1046/1163 SRV records have a bit-identical position in the
  previous frame. Asteroids still change rotation every frame (they spin):
  7028 of those have a different quaternion.
* The ship's own instances (cockpit, within 60 m of the camera: 89 records)
  move by about the camera displacement, `(6.2, 4.5, -2.4)` m vs
  `(6.4, 4.3, -2.3)` for the camera, slightly different per part because
  the ship also rotates; their quaternion changes. Same for the SRV parts
  (~0.55-0.7 m/frame).
* `+288` changes every frame for most records (meaning unknown).

**Consequences for motion vectors**

1. Camera reprojection (depth + current/previous `c270-c273` and `c275`)
   is correct for static world geometry, which is most of the screen in
   flight and in the SRV. Asteroid spin is a small extra error.
2. The cockpit / SRV body is attached to the camera and moves with it. With
   camera-only vectors it would get large wrong motion (8 m/frame at 3 m
   distance), so it needs object motion. It is also the most visible part.
3. There is no previous transform in the game data: the add-on must keep
   last frame's `t33` (or the per-draw `cb0`) and find each instance's
   previous transform. Candidates: match by mesh (draw call: index buffer +
   first index + vertex buffer) and nearest previous position; for the
   ship body, a single rigid "ship delta" (previous -> current) applied to
   all instances close to the camera would cover the cockpit.
4. A cheap first version: camera reprojection everywhere plus a rejection
   test in the TAA resolve (neighbourhood clamp), and a mask for pixels
   whose depth is within a few metres of the camera (cockpit / SRV) that are
   reprojected with "zero screen motion relative to the camera" when the
   camera is attached to the vehicle. Good enough to start, refined later
   with real per-object vectors.

## 6. Open questions

* How to tell in the add-on which situation applies (ship-local frame when
  docked vs world frame in flight): candidate is whether the camera
  position `c275` changes from frame to frame.
* Exact meaning of `c35-c48` (second frame), `c191`, `+288`, and of the
  1024^2 offscreen pass.

## 7. Stencil: the player's vehicle

The scene depth-stencil buffer (`D32_FLOAT_S8X24_UINT`) carries per-pixel
flags. Bit `0x80` is set exactly on the player's own vehicle: cockpit in
flight (0x90/0x94/0x95), SRV interior, cockpit when docked (0xB0/0xB4/0xB5);
it is absent on foot and in external/free cameras. Other values seen: 0x05,
0x15, 0x20-0x25, 0x30/0x34 (world). Read with `tools/rd_stencil.py`; used by
the add-on as the "attached to the camera" mask
([motion-vectors.md](motion-vectors.md)).

## 8. System map

`sysmap.rdc` (1920x1080, no supersampling): the system map is rendered
through the same G-buffer and HDR path as the game world (4 G-buffer draws,
57 draws into the HDR target: bodies, orbits, rings, icons), then exposure,
bloom, tonemap and the 2D UI. There is no exposure-to-scene "HUD pass", so
the HUD resolve point is not found and the TAA would run at the bloom point,
over the whole map.

Signatures, checked against all 12 captures:

* 15 pixel shaders appear only in the map frame; drawn on every map frame:
  `d7e5ff175bc127a8` (18 draws), `f5938f31368af521` (18),
  `723db111682fbc7b` (3);
* no sun shadow cascade pass: 0 orthographic camera updates in `cb1`, against
  5-6 in every in-game frame (cockpit, station, SRV, on foot, free camera).
  The main menu and the loading screen have none either.

* almost no G-buffer geometry: 4 G-buffer draws in the map, against 476 in
  the main menu hangar (which has no sun shadows either) and 119 to 2873 in
  game. "No sun shadows and fewer than 32 G-buffer draws" separates the map
  from the menu; the loading screen (5 draws) also matches.

The galaxy map was not captured; it probably matches the heuristic (no sun
shadows, little geometry) but its shaders are unknown.
