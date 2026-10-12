// Decoding of Elite Dangerous' per-view camera constant buffer (cb1).
// Layout: docs/reverse-engineering.md, section 1.
//
// Live, only some registers are written on each Map (WRITE_DISCARD): c270-c279
// are, c189-c199 are not (zero in game; RenderDoc shows stale values there).
// So everything is derived from c270-c275.

#pragma once

#include <cstddef>
#include <cstdint>

namespace edaa
{
    // Float4 register indices inside the camera buffer.
    namespace reg
    {
        constexpr int render_size = 189; // (w, h, 1/w, 1/h) - not written live
        constexpr int main_proj = 196; // c196-c199 - not written live
        constexpr int view_proj = 270; // c270-c273, view currently drawn
        constexpr int camera_pos = 275; // float3
        constexpr int count = 276; // registers declared by the game's shaders
    }

    // Smallest buffer that can hold the camera layout.
    constexpr size_t camera_buffer_min_size = reg::count * 16;

    enum class view_kind
    {
        none, // not a camera buffer update
        perspective, // reverse-Z infinite perspective (main camera, spot shadows, probes, HUD...)
        orthographic, // sun shadow cascades
    };

    const char* to_string(view_kind kind);

    struct camera_info
    {
        view_kind kind = view_kind::none;
        const char* reason = "not decoded"; // why this kind was chosen

        // Derived from c270-c273 = rotation * projection (perspective only)
        float sx = 0, sy = 0; // lengths of the x/y columns
        float w_length = 0; // length of the w column (1 for a perspective viewProj)
        float near_plane = 0; // c273.z
        float vfov_deg = 0, aspect = 0;
        float forward[3] = {}; // w column = camera forward

        float camera_pos[3] = {}; // c275
    };

    // Decodes a camera buffer update. 'regs' points to float4 registers.
    camera_info decode_camera(const float* regs);
}
