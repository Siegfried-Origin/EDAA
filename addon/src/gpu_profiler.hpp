// GPU time of EDAA's passes on the game's D3D11 queue (overlay "GPU timings",
// setting gpu_profiler, off by default). Timestamp queries in a ring of frame
// sets, read without waiting (D3D11_ASYNC_GETDATA_DONOTFLUSH): a set whose
// results are not ready when its slot is needed again is dropped and counted.
//
// Not measured: work on other queues (DLSS-G generation and the swap chain
// copy on the D3D12 queue of fg_present.cpp) and other add-ons' work unless
// it runs inside one of the sections (GITC Uplift's NR: compare with it off).

#pragma once

#include <cstdint>

struct ID3D11DeviceContext;

namespace edaa::gpu_profiler
{
    enum section : int
    {
        motion, // motion vectors + vehicle mask (resolve)
        fg_in, // frame generation inputs: depth copy, capture, HUD motion
        hud, // HUD snapshot and mask
        dlss, // DLSS / DLAA / XeSS incl. input and output copies
        taa, // EDAA TAA / TAA upscaling incl. sharpening and copy back
        fg_out, // frame generation: back buffer copy into the shared texture
        reactive, // reactive mask: scene snapshot, mask, combination with the HUD mask
        count
    };

    // Results over the last window (2 s for the overlay).
    struct results
    {
        bool valid = false;
        float frame_mean = 0, frame_p99 = 0; // present to present on the GPU timeline (idle included)
        float mean[count] = {}, p99[count] = {};
        float edaa_mean = 0, edaa_p99 = 0; // sum of the sections
        float elite_mean = 0; // frame - EDAA (the game, other add-ons, idle)
        uint32_t frames = 0; // frames measured in the window
        uint32_t skipped = 0; // frames dropped since the start (results not ready or disjoint)
    };

    // Times the enclosing scope as one section (nothing when the profiler is off).
    struct scope
    {
        ID3D11DeviceContext* ctx;
        section which;
        bool open;
        scope(ID3D11DeviceContext* c, section s);
        ~scope();
        scope(const scope&) = delete;
        scope& operator=(const scope&) = delete;
    };

    // At the game's Present (render thread): closes the frame set, reads the
    // finished ones, opens the next. 'enabled' is the setting.
    void frame(ID3D11DeviceContext* ctx, bool enabled);

    results last();
    const char* section_name(section s);

    // Test scene marker for the log (Ctrl+Shift+N): -, A, B, C, D, E.
    void next_scene();
    char scene();

    // Device destroyed: drops the queries.
    void release();
}
