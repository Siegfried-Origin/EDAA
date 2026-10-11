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
   accumulated, sharp. Fallback (option off or markers missing): right
   before the first bloom downsample (pixel shader `b196cbbdf516e150`).
5. **GPU passes at the resolve point** (`passes.cpp`, D3D11 with the game's
   state saved/restored):
   1. motion vectors (`motion.hlsl`): camera reprojection from depth, jitter
      removed, `R16G16_FLOAT` (previous UV - current UV); vehicle mask
      `R8_UNORM` from stencil bit `0x80` (player's cockpit / SRV, moves with
      the camera: zero motion);
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
   See reverse-engineering.md section 8.
7. **At Present** (`passes.cpp`, `capture.cpp`): optional debug view over the
   back buffer, auto capture, next jitter phase.

The overlay window "EDAA" (`overlay.cpp`) shows all values and settings.

## Build

Requirements: Visual Studio 2022/2026 with the C++ desktop workload, the
Windows 10/11 SDK (for `fxc.exe`), CMake 3.20+.

```powershell
.\third_party\fetch.ps1                      # ReShade v6.8.0 + ImGui headers (once)
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

Output: `build\Release\EDAA.addon64` (static CRT, depends only on KERNEL32).
Developer build (debug views, jitter controls, auto capture, frame trace,
camera and diagnostics sections, extra hotkeys): add `-DEDAA_DEVELOPER=ON`,
e.g. `cmake -S . -B build-dev -A x64 -DEDAA_DEVELOPER=ON`; Debug builds always
include them. The normal build shows only TAA on/off, status, "Reset to
defaults" and an "Advanced settings" section.
`tools\package.ps1` builds and packs `EDAA.addon64` + `INSTALL.txt` into
`dist\EDAA-<date>\` and a zip (nothing is copied into the game folder).
Shaders in `addon/shaders/` are compiled to headers by fxc at build time.
The overlay shows the build date/time: check it after copying a new build.

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
| HDR compression scale | 50 | scene values (before the game's exposure) are multiplied by it before the c/(1+c) compression used for filtering and blending; higher = bright thin edges make their dark neighbours ripple less with the jitter. At 1 high-contrast edges in stations flickered; 50 fixes them; near 1000 highlights look wrong |
| Soft screen edges | on | pixels entering the screen reuse the edge of the history |
| Reconstruction filter | off | current sample = Gaussian 3x3 around the unjittered pixel centre; meant to make thin highlights fade instead of flicker, but in game it softens the image without a visible gain |
| Unjitter current sample | off | softer image (when the filter is off) |
| Resolve before the HUD pass | on | HUD/holograms out of the TAA |
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
* **Frame trace** ("Dump/trace next frame"): one frame of the render target
  bindings and draws that touch the scene depth or HDR target (up to 20000
  lines) in `ReShade.log`, with pixel shader hash, depth test and compare
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
* Map detection takes effect one frame after the map appears (one frame
  with jitter when it opens). The galaxy map is not flagged as a map but
  is not affected either: it has no G-buffer scene with a main camera, so
  the TAA does not run there (checked in game, no ghosting).
* Shader hashes are those of game build `2026.09.03.332841`; a game update
  may change them (the structural markers keep working, the log says which
  shaders were found).
