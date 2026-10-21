# EDAA ReShade add-on

Temporal anti-aliasing for Elite Dangerous Odyssey (DX11), as a ReShade
add-on. Background on the game's rendering:
[reverse-engineering.md](reverse-engineering.md); motion vectors:
[motion-vectors.md](motion-vectors.md).

## How it works

Per frame, in the order the game draws:

1. **Camera tracking** (`camera_tracker.cpp`, `camera.cpp`): every update of
   the camera constant buffer `cb1` (Map/Unmap or UpdateSubresource) is
   decoded. A perspective camera is `c273 = (0,0,near,0)` with `c270-c272` =
   rotation * projection; FOV, aspect and near come from it, the position
   from `c275`. `c189` and `c192-c199` are not used: they are not written in
   the running game (reverse-engineering.md, note in 1.1).
2. **Scene detection** (`frame_tracking.cpp`): the scene depth and HDR colour
   target are the largest depth buffer bound with 4 real colour targets of
   the same size and the back buffer's aspect (G-buffer pass); RT3 is the
   HDR scene (`R11G11B10_FLOAT`). Re-evaluated every frame. The main camera
   is the perspective camera used by the most draws into the scene depth.
3. **Jitter** (`frame_tracking.cpp`): Halton(2,3) sub-pixel offset of the
   viewport origin (8 phases, 1 px), only for scene geometry drawn before
   the resolve: draws into the scene depth with a reverse-Z depth test
   (`GREATER`, `GREATER_EQUAL`, `EQUAL`), full-size viewport, not fullscreen
   triangles. Also, unless "Jitter decals, light volumes and glows" is off
   (developer), every other draw into the scene depth or HDR target before
   the resolve except single 4-vertex quads (fog composite): decal and light
   volumes drawn as boxes with `LESS_EQUAL` (back faces), glows and sky
   meshes without depth test. Left unjittered over jittered surfaces they
   z-fight: station signs (the text area) and lamps flickered. Matrix-agnostic:
   works for every shader family.
4. **Resolve point** (`frame_tracking.cpp`): right before the cockpit HUD
   pass, i.e. the first draw into the HDR scene after the exposure
   downsample (`R11G11B10` at 1/4 render size), or the first draw of the HUD
   pixel shader `b4523c39a461fd35`. Everything of the world is in the HDR
   target then; the HUD/holograms are drawn after the TAA: not jittered, not
   accumulated, sharp. If no HUD draw goes into the scene after the exposure
   downsample (on foot), the first draw into any other target after it is
   used (the world is complete once the exposure is measured): with bloom
   turned off in the game options there is no bloom marker, and the TAA used
   to stop there ("resolve point not reached"). Fallback (option off or
   markers missing): right before the first bloom downsample (pixel shader
   `b196cbbdf516e150`). When no marker is found, the status line lists which
   ones the frame had (G-buffer, HDR draws, exposure, HUD, bloom).
5. **GPU passes at the resolve point** (`passes.cpp`, D3D11 with the game's
   state saved/restored):
   1. motion vectors (`motion.hlsl`): camera reprojection from depth, jitter
      removed, `R16G16_FLOAT` (previous UV - current UV); vehicle mask
      `R8_UNORM` from stencil bit `0x80` (player's cockpit / SRV). Those
      pixels get the vehicle's own motion (option "Vehicle motion", on):
      `vehicle_motion` (`object_motion.cpp`) matches the `t33` records within
      20 m of the camera with the previous frame's (same place once the
      camera motion is removed, same kind and scale), takes the rigid motion
      most of them share (vote; animated parts disagree) and passes it as a
      3x4 matrix, current to previous camera-relative position. Head
      movements (free look, G-forces, shake) then move the cockpit on screen
      as they should. When no motion is found, those pixels get none (they
      mostly move with the camera). Checked offline on the cockpit_turn pair:
      62 records, one motion, 0.5 mm error at most;
   2. TAA (`taa.hlsl`): 3x3 neighbourhood in YCoCg on HDR compressed by
      `c / (1 + max)`, variance clip (`mean +- gamma * sigma` inside
      min/max; gamma 5 for still pixels, 1.25 in motion), history at
      `uv + motion` of the closest-depth pixel, 5-tap Catmull-Rom,
      luminance-weighted blend with anti-flicker (history weight 0.93, down
      to the minimum weight when current and history disagree), soft screen
      edges; history `R16G16B16A16_FLOAT` ping-pong, reset on camera cut
      (> 200 m or > 20 deg per frame) or render size change;
   3. copy into the game's HDR target, which the game's HUD, bloom, tonemap,
      SMAA and downsample then use.
6. **Maps** (`frame_tracking.cpp`): when the system map shaders
   (`d7e5ff175bc127a8`, `f5938f31368af521`, `723db111682fbc7b`) are drawn, or
   (option "Detect maps also by missing sun shadows", on) when the scene is
   drawn without any sun shadow pass and with fewer than 32 G-buffer draws,
   the next frames get no jitter and no TAA, and the history is reset when
   the map closes. The main menu hangar (476 G-buffer draws) keeps the TAA;
   loading screens and most likely the galaxy map are paused.
   Besides, the jitter stops after 4 frames in a row without a successful
   resolve: the galaxy map is not detected as a map, but it has no main perspective
   camera, so the resolve stops ("waiting for two frames with a main
   camera") and the jitter, left alone, made the map shake.
   See reverse-engineering.md section 8.
6b. **NVIDIA DLAA** (`dlss.cpp`, "Method" in the overlay): DLSS at native
   resolution (render size = output size) through NGX D3D11, run at the same
   resolve point instead of the TAA pass. Inputs: the game's HDR scene (flag
   IsHDR, exposure estimated by DLSS: AutoExposure), its reverse-Z depth
   (DepthInverted), the EDAA camera motion vectors (UV, previous - current,
   jitter removed; MV scale = render size) and the viewport jitter in pixels
   (same sign, "negate the jitter offset" in the developer section flips it).
   History reset on camera cuts. Output: an RGBA16F UAV texture copied back
   into the scene target. NGX keeps the D3D11 context state. If DLSS cannot
   run (no RTX GPU, old driver, `nvngx_dlss.dll` missing next to the add-on),
   the TAA runs instead and the overlay line "DLSS:" says why. NGX logs go to
   `%LOCALAPPDATA%\EDAA`.
6c. **Intel XeSS Native AA** (`xess.cpp`, "Method" in the overlay): XeSS
   Super Resolution at 1.0x through a D3D12 bridge, because XeSS on D3D11
   only runs on Intel Arc GPUs. A D3D12 device is created on the game's GPU;
   colour, an R32_FLOAT depth copy and the motion vectors are copied into
   textures shared with D3D11, a shared fence orders the work (D3D11 copies
   -> signal -> D3D12 queue waits, runs XeSS, signals -> D3D11 waits), and
   the shared output is copied back like the DLAA output. Same inputs and
   conventions as DLAA (inverted depth, auto exposure, velocity scale =
   render size, jitter in pixels). `libxess.dll` (78 MB) is delay-loaded
   from the add-on folder and needs the Visual C++ 2015-2022 runtime; if it
   cannot run, the TAA runs instead and the overlay line "XeSS:" says why.
6d. **Which methods are offered**: at the first resolve (main menu), each
   backend is probed once per device (`dlss::probe`: NGX init and
   capabilities; `xess::probe`: libxess.dll, D3D12 bridge and XeSS context,
   freed right after). The "Method" list shows all three; those that cannot
   run are greyed out ("not available on this system") and cannot be
   selected in the normal build. A saved method that cannot run falls back
   to the TAA silently. Developer builds can select every method, with the
   "DLSS:" / "XeSS:" line saying why one cannot run.
6e. **DLSS upscaling** ("Upscaling" in the overlay, work in progress): when
   the game renders below the display size (in-game supersampling < 1.0 or
   FSR), it tonemaps and applies SMAA at render size, then draws that image
   into the back buffer with one bilinear (or FSR) draw before the 2D UI
   (`analysis/fsr-frame5208`, EID 9677). With DLSS selected, the resolve
   point only builds the motion vectors (no TAA, no DLAA), and the first
   draw into a display-size target after it is examined: a render-size
   texture in PS slot 0, a render target, no depth (`run_upscale`). With FSR
   that draw reads the display-size output of a compute pass (EASU) instead:
   the input is then the last render-size R8G8B8A8 target drawn after the
   resolve (SMAA output). The image is typeless (R8G8B8A8_TYPELESS), so it
   is copied into a typed texture. DLSS
   then upscales that tonemapped image (no IsHDR, no AutoExposure) with the
   scene depth, the motion vectors and the jitter into a display-size
   R8G8B8A8 UAV texture, which is drawn into the game's target, and the
   game's draw is skipped. The quality mode is the one whose render size
   range (`NGX_DLSS_GET_OPTIMAL_SETTINGS`: min/max for each mode at this
   display size, logged once) contains the render size, closest to its
   optimal size, else the mode with the closest optimal size; the
   feature is created for the actual render size. The overlay shows the
   rendering and display resolutions, the modes with their ranges and the
   mode in use. The Halton sequence grows to `phases x (display / render)^2`
   (NVIDIA: 8 x ratio^2) so each display pixel gets as many samples as at
   native resolution. If the draw is not recognised or DLSS fails, the game's
   draw runs, the TAA is used for these sizes and upscaling is retried after
   600 frames.
   HUD mask ("HUD mask" in the overlay, on): the cockpit HUD is drawn into
   the scene after the resolve, so it is neither jittered nor covered by the
   motion vectors, and DLSS would smear it. The scene is copied at the
   resolve point; at the first bloom downsample draw (or at the upscale when
   there is no bloom) `ps_hud_mask` marks the pixels that changed since
   (3x3 dilated) into an R8 render-size mask, passed to DLSS as the "bias
   current colour" mask: those pixels use the current frame only. Debug view
   "HUD mask (upscaling)" shows it in cyan.
   No texture mip bias: the game's samplers have none (MipLODBias 0,
   checked in game), but replacing them with biased copies at every bind
   gave almost no visible gain (ED textures are mostly magnified at these
   distances) for a CPU cost on every sampler bind; it was removed.
   XeSS upscaling ("Upscaling" = Intel XeSS): same path, through the D3D12
   bridge of 6c. Preset: the one whose input range
   (`xessGetOptimalInputResolution`, logged once per display size) contains
   the render size, else the closest optimal size. Flags INVERTED_DEPTH,
   LDR_INPUT_COLOR and RESPONSIVE_PIXEL_MASK: the HUD mask goes to the
   responsive mask (cleared when there is none). Depth is copied to
   R32_FLOAT first, as for XeSS Native AA.
7. **At Present** (`passes.cpp`, `capture.cpp`): optional debug view over the
   back buffer, auto capture, next jitter phase.

The overlay window "EDAA" (`overlay.cpp`) shows all values and settings.

## Build

Requirements: Visual Studio 2022/2026 with the C++ desktop workload, the
Windows 10/11 SDK (for `fxc.exe`), CMake 3.20+.

```powershell
git submodule update --init --depth 1       # ReShade, ImGui, DLSS SDK (once)
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

Output: `build\Release\EDAA.addon64` (static CRT) and, with the SDKs,
`build\Release\nvngx_dlss.dll` (DLAA) and `build\Release\libxess.dll` (XeSS),
all three for the game folder. `-DEDAA_WITH_XESS=OFF` builds without XeSS.
Dependencies are git submodules in `third_party/`, pinned to a version:
ReShade (tag v6.8.0, add-on API headers), Dear ImGui (the commit ReShade
6.8.0 uses) and the NVIDIA DLSS SDK (310.9.1). The DLSS repository is large
(~1.6 GB even shallow); EDAA uses its headers, `nvsdk_ngx_s.lib` and the
release `nvngx_dlss.dll`. The submodule only references NVIDIA's repository:
the SDK license (NVIDIA RTX SDKs) allows shipping it only inside an
application. Without the DLSS submodule (or with `-DEDAA_WITH_DLSS=OFF`) the
add-on builds without DLSS:
`git submodule update --init --depth 1 third_party/reshade third_party/imgui`.
Developer build (debug views, jitter controls, auto capture, frame trace,
camera and diagnostics sections, extra hotkeys): add `-DEDAA_DEVELOPER=ON`,
e.g. `cmake -S . -B build-dev -A x64 -DEDAA_DEVELOPER=ON`; Debug builds always
include them. The normal build shows only TAA on/off, status, "Reset to
defaults" and an "Advanced settings" section.
`tools\package.ps1` builds and packs `EDAA.addon64` + `INSTALL.txt` into
`dist\EDAA-<date>\` and a zip (nothing is copied into the game folder).
Shaders in `addon/shaders/` are compiled to headers by fxc at build time.
The overlay's first line shows the version and the build date/time (both
regenerated at every build): check it after copying a new build. The version
is `git describe --tags --dirty` without the leading "v" (e.g.
`0.1.0-4-g4c6fe50-dirty` between releases); the release workflow sets it to
the tag with `-DEDAA_VERSION=<version>`.

## Install / uninstall

See the [README](../README.md).

## Hotkeys

* `Ctrl+Shift+T`: TAA on/off (switches the jitter with it, saved).

Developer builds only:

* `Ctrl+Shift+J`: jitter alone. TAA off + jitter on = compare mode.
* `Ctrl+Shift+M`: debug view: off, motion vectors, vehicle mask, depth,
  history rejection, moving objects, temporal change (red where the TAA
  output changed since the last frame: with the camera still, it shows what
  flickers in the TAA output; flicker on screen without red comes from after
  the resolve). Magenta = no data this frame ("Status" says why).
* `Ctrl+Shift+C`: auto capture.

## Settings (overlay)

TAA is on by default. Every change (overlay or `Ctrl+Shift+T`) is saved in
`ReShade.ini`, section `[EDAA]`, and loaded at the next start; "Reset to
defaults" restores the values below. The debug view always starts off.


| Setting | Default | Effect |
|---|---|---|
| History weight | 0.93 | share of history when current and history agree |
| History weight in motion | 0.9 | cap for fast motion (lower: less ghosting) |
| Clip box in motion / when still | 1.25 / 5.0 | variance clip in standard deviations (lower: less ghosting, more flicker) |
| Anti-flicker, min history weight | on, 0.95 | history weight when current and history disagree (at 0.95, above the history weight, it has no effect: chosen in game as the least flickering) |
| HDR compression scale | 30 | scene values (before the game's exposure) are multiplied by it before the c/(1+c) compression used for filtering and blending; higher = bright thin edges make their dark neighbours ripple less with the jitter. At 1 high-contrast edges in stations flickered; 30-50 fixes them; near 1000 highlights look wrong |
| Soft screen edges | on | pixels entering the screen reuse the edge of the history |
| Reconstruction filter | off | current sample = Gaussian 3x3 around the unjittered pixel centre; meant to make thin highlights fade instead of flicker, but in game it softens the image without a visible gain |
| Unjitter current sample | off | softer image (when the filter is off) |
| Resolve before the HUD pass | on | HUD/holograms out of the TAA |
| Upscaling | Game | Game (bilinear or FSR) or NVIDIA DLSS when rendering below the display size |
| Vehicle mask | game stencil | or distance to camera (with auto-off on foot) |
| Capture folder (developer) | `%USERPROFILE%\Documents\EDAA-captures` | stored as `[EDAA] CapturePath` in `ReShade.ini` |

## Diagnostics

* **Auto capture** (`Ctrl+Shift+C`): about 1 second; saves PNGs of the final
  image without the ReShade overlay (TAA on, TAA off, jitter only, motion
  vectors, vehicle mask, depth, history rejection, moving objects) and
  `info.txt` (build, render size, resolve point, status, moving object
  statistics) to `<capture folder>/<date_time>/`,
  and traces that frame into `ReShade.log`. Keep doing the same thing
  (still, or turning steadily) during the capture.
* **Frame trace** ("Dump/trace next frame"): one frame of all render target
  bindings and of the draws that touch the scene depth or HDR target (up to
  20000 lines) in `ReShade.log`, with pixel shader hash, depth test and compare
  function (`zfunc`: -1 off, 1 less, 2 equal, 3 less_equal, 4 greater,
  5 not_equal, 6 greater_equal, 7 always), depth write, blending, full
  viewport, jitter and the position of the TAA resolve. It is how the HUD
  pass and the unjittered decals were located.
* **Moving objects** section: reads the instance buffer `t33` each frame,
  matches its records with the previous frame and shows how many are still,
  spinning, moving, part of the player's vehicle or unmatched, the camera
  reprojection error on moving objects on screen (max/mean pixels) and the
  CPU cost. The "Moving objects" debug view draws a ring on each (yellow to
  red with the error, cyan for the vehicle) and a line to where its TAA
  ghost would be. Off unless the section's checkbox or the debug view is on.
  See [motion-vectors.md](motion-vectors.md) section 3.
* **Diagnostics** section: camera buffer updates of the last frame with the
  raw registers.
* Crash dumps: `tools/minidump_info.py` (exception and stack without a
  debugger; resolve EDAA offsets with a `/MAP` build).
* Motion vector math: `tools/check_motion.py` validates the shader formula on
  2-frame RenderDoc captures (0.0000 px on static objects).

## Known limits

* The cockpit HUD/holograms are not anti-aliased (drawn after the TAA).
* Objects that move by themselves (other ships, NPCs, spinning asteroids)
  only have camera motion; ghosting there is limited by the variance clip.
  Plan: [motion-vectors.md](motion-vectors.md) section 3.
* Thin very bright highlights can still flicker slightly.
* Rendering below 1x (supersampling < 1 or FSR) with the game's shadow
  quality on Ultra: shadow edges and large soft shadows flicker with the
  jitter, whatever the method (EDAA TAA, DLAA, DLSS/XeSS upscaling). The
  sun shadow mask pass (pixel shader `77cb0c225019d08b`) rebuilds positions
  from SV_Position and a view ray in its `cb2` (`ndc.x*cb2[1] +
  ndc.y*cb2[2] + cb2[4]`) times the jittered depth: the ray is the
  unjittered pixel centre, so positions are off by up to half a pixel along
  the depth slope and the shadow test flips on grazing surfaces. Shifting
  the viewport cannot reach SV_Position, so `shadow_fix.cpp` (option
  "Lighting follows the jitter", on) gives that draw a copy of its `cb2` range
  with `cb2[4] += -2 jx / w * cb2[1] + 2 jy / h * cb2[2]`, made on the GPU
  (`CopySubresourceRegion` into a raw buffer, compute shader
  `shadow_fix.hlsl`, copy into our constant buffer), and binds the game's
  buffer again before the next draw unless the game binds another first.
  Anything unexpected (no buffer, fewer than 37 registers, no D3D11.1
  context) leaves the draw as it is; a changed shader hash just disables it.
  The tiled lighting of local lights (compute shaders `932e687ab8739aeb`,
  `a6542bbe3011f87f`, `b499f7667d34932c`, `083dc2646abe2d51`) has the same
  flaw with thread ids: `uv = (pixel + 0.5) / size`, ray row i =
  `dot(cb0[10 + i].xyw, (u, v, 1))`; at their dispatch, `cb0[10..12].w`
  gets `- jx / w * x - jy / h * y` the same way (station interiors lit by
  local lights flickered below 1x). Stations launch them with
  `DispatchIndirect`, hooked as well (`draw_or_dispatch_indirect`). The
  ambient occlusion (`403797e056b7801d`, view.xy = `(uv * cb0[4].xy +
  cb0[4].zw) * z`) gets `cb0[4].zw += -j / size * cb0[4].xy`.
  What remains: the light right next to a strip light (`5a221a56fe342ec9`)
  changes so fast that it still flickers a little in motion below 1x.
* Map detection takes effect one frame after the map appears (one frame
  with jitter when it opens). The galaxy map is not flagged as a map but
  is not affected either: it has no G-buffer scene with a main camera, so
  the TAA does not run there (checked in game, no ghosting).
* High-resolution screenshots (Alt+F10): the game renders them as zoomed,
  off-centre tiles. A camera update with an off-centre projection (near
  plane >= 2 cm) pauses jitter and TAA from that frame on, until 30 frames
  after the last tile; the history is then reset. Overlay and log say
  "high-resolution screenshot". See reverse-engineering.md section 9.
* Shader hashes are those of game build `2026.09.03.332841`; a game update
  may change them (the structural markers keep working, the log says which
  shaders were found).
