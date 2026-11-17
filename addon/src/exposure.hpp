// Elite's exposure for DLAA (dlss_exposure_mode 2, passes.cpp).
//
// The tonemap's vertex shader (vertex_shader_hash) reads the adapted scene
// luminance from the first texel of a small R32 texture; its pixel shader
// turns it into the exposure with constants from its cb2
// (docs/reverse-engineering.md, "Exposure"). frame_tracking.cpp copies both
// at the tonemap draw (GPU copies only); the next frame's resolve evaluates
// the same formula into the exposure texture DLSS reads.

#pragma once

#include <cstdint>

struct ID3D11DeviceContext;
struct ID3D11Resource;
struct ID3D11Buffer;

namespace edaa::exposure
{
    // Vertex shader of Elite's tonemap draw (not replaced by EDHM).
    constexpr uint64_t vertex_shader_hash = 0x936d466fbd388547ull;

    // Whether the next tonemap draw should be copied (set by the resolve). Any thread.
    bool wanted();

    // At the tonemap draw: copies the luminance texture and the pixel shader's
    // cb2 (offset in bytes into 'constants'). Render thread.
    void capture(ID3D11DeviceContext* ctx, ID3D11Resource* luminance, ID3D11Buffer* constants, uint64_t offset);

    // Last value read back from the GPU (a few frames late), 0 = none. Any thread.
    float last_value();
}
