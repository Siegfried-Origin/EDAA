// Intel XeSS Super Resolution in Native AA mode (render size = output size),
// as an alternative to the EDAA TAA resolve. Built in when the XeSS SDK is
// present (submodule third_party/xess, EDAA_XESS=1).
//
// XeSS supports D3D11 only on Intel Arc GPUs, so it runs on D3D12 for every
// GPU through a bridge: a D3D12 device on the same adapter, textures and a
// fence shared with the game's D3D11 device (xess.cpp).

#pragma once

#include "upscaler.hpp"

#include <string>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace edaa::xess
{
    // Runs XeSS Native AA. Returns true on success; otherwise the caller falls
    // back to the EDAA TAA and status() says why. Render thread only.
    // Initialises the D3D12 bridge and XeSS on the first call.
    bool evaluate(ID3D11Device* device, ID3D11DeviceContext* ctx, const upscaler_inputs& in);

    // Checks once per device whether XeSS can run on this system
    // (libxess.dll, D3D12 bridge, GPU support), so the overlay only offers it when it can. Render thread.
    void probe(ID3D11Device* device);

    // Result of probe(): -1 not checked yet, 0 unavailable, 1 available. Any thread.
    int availability();

    // State for the overlay ("not used yet", "running (D3D12 bridge)", or the error). Any thread.
    std::string status();

    // Waits for pending work, destroys the XeSS context and the D3D12 bridge.
    void release(ID3D11Device* device);
}
