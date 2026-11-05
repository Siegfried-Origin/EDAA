# DLSS Frame Generation

Experimental, off by default. Needs an RTX 40/50, Windows hardware-accelerated GPU
scheduling and a game restart after turning it on.

## How it works (`addon/src/fg_present.cpp`, `fg.hpp`, hooks in `passes.cpp`)

1. **D3D12 output.** In `reshade_present`, after ReShade's effects and overlay, the finished
   D3D11 image is copied into a shared texture (NT handle, created by D3D11, opened by D3D12,
   with a shared fence as in the XeSS path). A D3D12 flip swap chain on a child window over the
   game window shows it. The DXGI factory comes from **ReShade's `dxgi.dll`** (`fg_factory=1`):
   the system one crashed in Present, on ReShade's wrapped queue. ReShade's second effect
   runtime on the child window is turned off with `set_effects_state(false)`.
2. **Streamline 2.14.16.** `slInit` runs before the D3D12 device is created. Device and factory
   go through `slUpgradeInterface` (eUseDXGIFactoryProxy, eUseFrameBasedResourceTagging). The
   DLLs are loaded at run time from the add-on folder with GetProcAddress, there is no import
   library. Reflex (eLowLatency with `frameLimitUs`), PCL markers, DLSS-G eOn, 1 generated frame.
3. **Inputs.** `resolve()` hands depth (an R32F copy) and EDAA's motion vectors (RG16F, previous
   minus current UV, no jitter, mvecScale 1) to `fg::capture`. They are copied into **two**
   alternating shared sets and tagged with `eOnlyValidNow` on our own command list at Present.
   Camera data: infinite reverse Z, clipToPrevClip from EDAA's matrices.
4. **HUD protection** (`fg_hud_protect`). EDAA's HUD mask (scene after AA against scene at the
   bloom) is also built for frame generation, and `ps_fg_motion` sets the vectors on HUD pixels
   to 0 at Present. It only works fully with `taa_before_hud=1`.
5. **Overlay.** A "Frame Generation" section in the EDAA tab: on/off live, HUD protection, frame
   limit live, real and displayed frames, Reflex and HUD status.

## Rules that came from bugs

| Symptom | Cause | Fix |
|---|---|---|
| Crash at the first D3D12 Present | System dxgi with ReShade's wrapped queue | Factory from ReShade's dxgi |
| Heavy stutter in menus | DLSS-G switched on and off every frame without inputs | Hysteresis, keep using the last inputs |
| Stutter every ~2 s in flight | The second ReShade runtime reported "no inputs needed" | Ignore the D3D12 runtime in `reshade_present` |
| Uneven pacing with an external limiter | Two limiters at once | Use the Reflex limiter (`fg_fps_limit`) |
| 70 instead of 140 FPS | With FG the Reflex limit applies to **displayed** frames | Limit 140 = 70 real |
| GPU hang or crash at stations | GPU-side waits between D3D11 and D3D12 | Bounded CPU waits only |
| Stutter when toggling | Skipped inputs counted as "no scene" | Separate counters, 8 ms CPU wait |
| ~2.5 % skipped frames | One input set, released only after Present | Two sets, released after the command list |
| Crash in `gameoverlayrenderer64.dll` | DLSS-G presents from its pacer thread while the game presents from the render thread, and the Steam overlay hook is not thread-safe | The two presents never run at the same time |

So: the GPU never waits on the other API, the two presents never overlap, and DLSS-G is not
switched off and on without need, because rebuilding it stutters.

## Log messages (`ReShade.log`)

- `EDAA FG: DLSS-G state at frame N: status 0x0, frames presented 2, frames without a scene X, reset R, focus F, busy skips B`:
  presented 2 = generated frames active, focus 0 = game in the background (DLSS-G pauses on
  purpose), frames without a scene = menu, map or loading, busy skips should stay 0.
- `EDAA FG: DLSS-G on/off`: should be rare (start, menus, loading).
- `EDAA status: ...`: every status change.
- `Frame rate over 100.00ms` (Streamline): hang, loading or focus change. Permanently: GPU overloaded.
- `Wait on gpu fence timed out` (Streamline): the GPU hangs, a crash follows.

## Limits

- DLSS-G presets cannot be set through the API in Streamline 2.14.16 / DLSS SDK 310.9.16.
- Dynamic Multi Frame Generation is RTX 50 only.
- Tested on one system only (RTX 4070 Ti, 1440p, about 68 real and 137 displayed FPS).
- `fg_present_test` is read at game start. Turning it off while the game runs, for example with
  "Reset to defaults", stops DLSS-G at once and returns to D3D11 at the next start.
