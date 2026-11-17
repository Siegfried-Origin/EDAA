// Recording of the anti-aliasing inputs for offline replay (developer builds,
// Ctrl+Shift+R): per frame the jittered HDR scene, a typed copy of the depth,
// the motion vectors, the vehicle mask and the reactive mask (a crop or the
// whole frame), the tonemap constants and the frame's parameters.
//
// The game never waits: the resolve copies the inputs into a ring of staging
// textures; a few frames later they are mapped with DO_NOT_WAIT, a worker
// thread copies them into memory and the render thread unmaps them. A full
// ring drops the frame (marked as a gap). After the stop, or at the memory
// limit, a writer thread stores everything into
// <capture folder>/recordings/<date_time>/ (docs/recording.md).

#pragma once

#include <cstdint>
#include <string>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Resource;
struct ID3D11Buffer;

namespace edaa::recorder
{
    // Inputs of one frame, all at the render size (the recorder crops).
    struct sources
    {
        ID3D11Resource* color = nullptr; // jittered HDR scene, format 'color_format'
        uint32_t color_format = 0; // DXGI_FORMAT
        ID3D11Resource* depth = nullptr; // R32_FLOAT copy of the scene depth (reverse-Z)
        ID3D11Resource* motion = nullptr; // R16G16_FLOAT
        ID3D11Resource* mask = nullptr; // R8_UNORM vehicle mask
        ID3D11Resource* reactive = nullptr; // R8_UNORM, nullptr if not made this frame
        ID3D11Resource* luminance = nullptr; // Elite's adapted luminance (small R32 texture), nullptr if not captured
        ID3D11Buffer* tonemap = nullptr; // copy of the tonemap's constant buffer, nullptr if not captured
        uint32_t tonemap_offset = 0; // byte offset of cb2 inside it
        uint32_t tonemap_age = 0; // frames since the tonemap was captured
    };

    // Parameters of one frame, stored as they are (params.bin, fixed size).
    struct frame_params
    {
        uint32_t magic = 0x31524445; // "EDR1"
        uint32_t flags = 0; // flag_* below
        uint64_t frame = 0; // the add-on's frame counter
        double time = 0; // seconds since the recording started
        uint32_t render_w = 0, render_h = 0;
        uint32_t crop_x = 0, crop_y = 0, crop_w = 0, crop_h = 0;
        float jitter[2] = {}; // pixels, the viewport offset of this frame
        float view_proj[16] = {};
        float prev_view_proj[16] = {};
        float camera_pos[3] = {}, prev_camera_pos[3] = {};
        float near_plane = 0, vfov_deg = 0, aspect = 0, pad0 = 0;
        float vehicle[12] = {}; // vehicle_motion(), current -> previous camera-relative
        float motion_constants[64] = {}; // passes.cpp motion_constants as uploaded (first 256 bytes)
        int32_t aa_method = 0, dlss_preset = 0, dlss_exposure_mode = 0, pad1 = 0;
        float dlss_exposure = 0, dlss_exposure_scale = 0, dlss_sharpness = 0, taa_hdr_scale = 0;
        float elite_exposure = 0; // last value read back from ps_exposure (mode 2), 0 = none
        float reactive_strength = 0, reactive_threshold = 0, pad2 = 0;
        float reserved[16] = {};
    };

    static_assert(sizeof(frame_params) == 640, "params.bin layout (tools/record_check.py)");

    constexpr uint32_t flag_camera_cut = 1;
    constexpr uint32_t flag_gap = 2; // frames before this one were dropped
    constexpr uint32_t flag_reactive = 4; // the reactive mask is valid
    constexpr uint32_t flag_tonemap = 8; // the tonemap constants and luminance are valid
    constexpr uint32_t flag_on_foot = 16;
    constexpr uint32_t flag_vehicle_known = 32;

    // Ctrl+Shift+R / overlay: starts or stops a recording. Any thread.
    void toggle();

    // True while frames are being recorded (the tonemap capture runs then). Any thread.
    bool active();

    // One line for the overlay ("" when idle). Any thread.
    std::string status();

    // Short on-screen line drawn over the game even with the overlay closed
    // (countdown, REC, saving, saved for a few seconds), "" when there is
    // nothing to show. 'alert' is set for the countdown, REC and errors. Any thread.
    std::string osd(bool& alert);

    // At the resolve, after the motion vectors and the reactive mask (gpu_mutex
    // held, render thread): copies this frame's inputs, maps older ones.
    void frame(ID3D11Device* device, ID3D11DeviceContext* ctx, const sources& in, frame_params params, bool full_frame, uint32_t max_mb);

    // Device destroyed: drops the ring (the recording so far is written).
    void release();
}
