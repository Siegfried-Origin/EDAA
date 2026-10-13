// Inputs shared by the neural anti-aliasing backends (NVIDIA DLAA, Intel
// XeSS), run at native resolution instead of the EDAA TAA resolve.

#pragma once

#include <cstdint>

struct ID3D11Resource;

namespace edaa
{
    struct upscaler_inputs
    {
        ID3D11Resource* color = nullptr; // HDR scene, jittered (R11G11B10)
        ID3D11Resource* depth = nullptr; // scene depth, reverse-Z (DLSS: the game's D32S8, XeSS: an R32_FLOAT copy)
        ID3D11Resource* motion = nullptr; // RG16F, previous UV - current UV, jitter removed
        ID3D11Resource* output = nullptr; // RGBA16F with UAV binding, copied into the scene afterwards
        uint32_t width = 0, height = 0; // render size = output size
        float jitter_x = 0, jitter_y = 0; // pixels, the viewport offset of this frame
        bool reset = false; // camera cut: drop the history
    };
}
