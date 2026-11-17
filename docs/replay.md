# Replay: measuring the anti-aliasing offline

`edaa-replay` plays a recording ([recording.md](recording.md)) through the
anti-aliasing variants without the game and measures each of them.
`tools/replay/report.py` turns the results into a comparison page.

## Build

Developer tool, needs the DLSS SDK (`third_party/dlss`):

    cmake -S . -B build-replay -A x64 -DEDAA_BUILD_REPLAY=ON
    cmake --build build-replay --config Release --target edaa-replay

`nvngx_dlss.dll` is copied next to `edaa-replay.exe`.

## Run

    edaa-replay <recording folder> <output folder> [run,run,...]
    python tools/replay/report.py <output folder>

Runs (all by default):

| Run | What |
|---|---|
| `raw` | the recorded input, jittered, without anti-aliasing (baseline) |
| `taa` | EDAA TAA (`ps_taa`, the add-on's default settings, `taa_hdr_scale` as recorded) |
| `dlaa` | DLAA, NVIDIA's default model, exposure fixed 10; also sharpened 0.3 and 0.6 (`ps_sharpen_hdr`) |
| `dlaa_elite` | DLAA, exposure from Elite (computed from the recorded tonemap constants) |
| `dlaa_auto` | DLAA, exposure estimated by DLSS |
| `dlaa_noreact` | `dlaa` without the recorded reactive mask |
| `dlaa_j`, `dlaa_l`, `dlaa_m` | DLAA models J, L, M, exposure fixed 10 |

Each run uses the add-on's own code: the shaders of `taa.hlsl` and
`dlss.cpp` (with `reshade::log` routed to the console,
`tools/replay/reshade_shim.cpp`). The motion vectors are rescaled from the
whole render size to the crop. The result is written into an R11G11B10
target like the game's scene, then read back.

Reactive mask (12.11., all 12 recordings, `dlaa` against `dlaa_noreact`): the
output is identical to the last bit with models K and M, though the mask
covers up to a third of the pixels in stations and on foot. The transformer
models evidently ignore `pInBiasCurrentColorMask`; the mask (and the HUD mask
passed the same way) only costs its ~0.05 ms with them.

TAA experiments run with changed settings, in addition to the list ("-"
alone: only the experiments):

    edaa-replay <rec> <out> raw,taa,dlaa --taa sharp=jw=8;hs=1;sh=0.6 --taa soft=hw=0.97

Keys: `hw`, `hwm`, `hwmin`, `cg`, `cgs`, `af`, `rec`, `unj`, `soft`, `hdr`
(the `taa_*` settings), `jw` (jitter weighting strength), `hs` (history
sharpening), `sh` (output sharpening). An optional `labels.json`
(`{"sharp": "Sharper TAA"}`) names them on the page.

An optional `scenes.json` in the output folder names the recordings on the
page: `{"20261112-132417": "Foreign ship flying by"}`.

## Output

`<output folder>/<recording>/`:

- `metrics.json`: per variant the mean flicker, the flicker of the 5 % most
  unstable pixels, the sharpness, the GPU time of the anti-aliasing pass,
  and both measures per frame;
- `frames/<variant>_<frame>.png`: four whole frames;
- `burst/<variant>_<k>.png`: 16 consecutive frames, 480x270 around the centre;
- `heat/<variant>.png`: flicker per pixel over the recording.

## Measures

Images are mapped for display with Elite's exposure of that frame, then
Reinhard per channel and sRGB. This is an approximation of Elite's own
curve: absolute brightness differs from the game, the comparison between
variants holds.

- **Flicker**: mean absolute change of the display luminance against the
  previous frame, moved along this frame's motion vectors (bilinear). Left
  out: 16 pixels at the crop border, pixels whose depth differs by more than
  5 % from the reprojected one (disocclusion), the first 8 frames after the
  start or a cut/gap. Foreign ships have no motion vectors: there the
  measure also counts smearing.
- **Flicker (hot)**: the same per pixel, averaged over the recording, mean of
  the 5 % highest. The overall mean is dominated by empty sky.
- **Sharpness**: mean Sobel gradient magnitude of the display luminance.
- **GPU ms**: timestamp queries around the anti-aliasing pass (the crop, not
  the whole frame).
