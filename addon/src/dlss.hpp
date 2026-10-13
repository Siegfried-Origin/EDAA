// NVIDIA DLSS in DLAA mode (render size = output size) through NGX D3D11,
// as an alternative to the EDAA TAA resolve. Built in when the DLSS SDK is
// present (third_party/fetch.ps1, EDAA_DLSS=1); otherwise every call reports
// that DLSS is not available.

#pragma once

#include <cstdint>
#include <string>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Resource;

namespace edaa::dlss
{
    struct inputs
    {
        ID3D11Resource* color = nullptr; // HDR scene, jittered (R11G11B10)
        ID3D11Resource* depth = nullptr; // scene depth, reverse-Z (D32S8)
        ID3D11Resource* motion = nullptr; // RG16F, previous UV - current UV, jitter removed
        ID3D11Resource* output = nullptr; // RGBA16F with UAV binding
        uint32_t width = 0, height = 0; // render size = output size
        float jitter_x = 0, jitter_y = 0; // pixels, the viewport offset of this frame
        bool reset = false; // camera cut: drop the history
    };


    // Runs DLAA. Returns true on success; otherwise the caller falls back to the
    // EDAA TAA and status() says why. Render thread only (NGX is not
    // thread-safe). Initialises NGX on the first call.
    bool evaluate(ID3D11Device* device, ID3D11DeviceContext* ctx, const inputs& in);

    // State of the DLSS runtime for the overlay ("not used yet", "running", or
    // the error). Safe to call from any thread.
    std::string status();

    // Releases the DLSS feature and shuts NGX down for this device.
    void release(ID3D11Device* device);
}
