// Inputs shared by the neural backends (NVIDIA DLSS, Intel XeSS): at native
// resolution instead of the EDAA TAA resolve (anti-aliasing), or at the end of
// the frame in place of the game's final upscale (upscaling).

#pragma once

#include <cstdint>
#include <vector>

struct ID3D11Resource;

namespace edaa
{
    struct upscaler_inputs
    {
        ID3D11Resource* color = nullptr; // HDR scene (R11G11B10) or the tonemapped image when upscaling, jittered
        ID3D11Resource* depth = nullptr; // scene depth, reverse-Z (DLSS: the game's D32S8, XeSS: an R32_FLOAT copy)
        ID3D11Resource* motion = nullptr; // RG16F, previous UV - current UV, jitter removed
        ID3D11Resource* output = nullptr; // UAV texture at the output size, copied into the scene / back buffer afterwards
        ID3D11Resource* hud_mask = nullptr; // upscaling: R8 at render size, 1 on the cockpit HUD (use the current frame there)
        uint32_t width = 0, height = 0; // render size
        uint32_t output_width = 0, output_height = 0; // upscaling: display size (0: same as the render size)
        float jitter_x = 0, jitter_y = 0; // pixels, the viewport offset of this frame
        bool reset = false; // camera cut: drop the history
        bool hdr = true; // false: tonemapped image (upscaling at the end of the frame)
    };


    // One quality mode of an upscaler for a display size, as the API reports it.
    struct upscale_mode
    {
        const char* name = "";
        uint32_t optimal_w = 0, optimal_h = 0; // render size the API recommends
        uint32_t min_w = 0, min_h = 0, max_w = 0, max_h = 0; // render sizes it accepts
    };


    // Modes for the display size and the one in use (overlay).
    struct upscale_info
    {
        std::vector<upscale_mode> modes;
        const char* mode = ""; // the one in use, "" if none
    };
}
