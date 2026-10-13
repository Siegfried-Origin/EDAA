// NVIDIA DLSS through NGX D3D11: DLAA (render size = output size) as an
// alternative to the EDAA TAA resolve, or upscaling in place of the game's
// final upscale. Built in when the DLSS SDK is
// present (submodule third_party/dlss, EDAA_DLSS=1); otherwise every call reports
// that DLSS is not available.

#pragma once

#include "upscaler.hpp"

#include <string>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace edaa::dlss
{
    // Runs DLAA. Returns true on success; otherwise the caller falls back to the
    // EDAA TAA and status() says why. Render thread only (NGX is not
    // thread-safe). Initialises NGX on the first call.
    bool evaluate(ID3D11Device* device, ID3D11DeviceContext* ctx, const upscaler_inputs& in);

    // Upscaling (in.output_width/height larger than the render size): uses the
    // quality mode whose render size range (NGX optimal settings) contains the
    // render size. Same return convention as evaluate().
    bool upscale(ID3D11Device* device, ID3D11DeviceContext* ctx, const upscaler_inputs& in);

    // Modes for the last display size and the one used by upscale(). Any thread.
    upscale_info info();

    // Checks once per device whether DLSS can run on this system
    // (RTX GPU, driver, nvngx_dlss.dll), so the overlay only offers it when it can. Render thread.
    void probe(ID3D11Device* device);

    // Result of probe(): -1 not checked yet, 0 unavailable, 1 available. Any thread.
    int availability();

    // State of the DLSS runtime for the overlay ("not used yet", "running", or
    // the error). Safe to call from any thread.
    std::string status();

    // Releases the DLSS feature and shuts NGX down for this device.
    void release(ID3D11Device* device);
}
