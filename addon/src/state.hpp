// Global add-on state shared between event callbacks and the overlay.

#pragma once

#include "camera.hpp"
#include "reshade_includes.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace edaa
{
    enum class debug_view : int
    {
        off = 0,
        motion = 1, // motion vectors (grey = still)
        mask = 2, // vehicle-attached mask
        depth = 3, // linear depth, to check the depth buffer is found
        rejection = 4, // where the TAA clipped the reprojected history (red)
        objects = 5, // moving instances and how far camera reprojection misses them
        temporal_change = 6, // how much the TAA output changed since the last frame (flicker finder, camera still)
        hud_mask = 7, // upscaling: where the cockpit HUD was found (DLSS uses the current frame there)
    };

    // Which resolve replaces the game's anti-aliasing.
    enum class aa_method : int
    {
        taa = 0, // EDAA TAA (taa.hlsl)
        dlaa = 1, // NVIDIA DLSS at native resolution (dlss.cpp); falls back to TAA if unavailable
        xess = 2, // Intel XeSS Native AA through a D3D12 bridge (xess.cpp); falls back to TAA if unavailable
    };

    // What replaces the game's final upscale when it renders below the display size.
    enum class upscaler : int
    {
        game = 0, // the game's own (bilinear or FSR), after the anti-aliasing above
        dlss = 1, // NVIDIA DLSS on the tonemapped image (passes.cpp run_upscale)
        xess = 2, // Intel XeSS on the tonemapped image, through the D3D12 bridge
    };


    struct settings
    {
        int aa_method = 0; // aa_method
        int upscaling = 0; // upscaler
        bool upscale_hud_mask = true; // upscaling: the cockpit HUD uses the current frame only (no trails)
        bool upscale_mip_bias = true; // upscaling: scene textures sampled with the display resolution mips (mip_bias.cpp)
        float mip_bias_extra = 0; // developer, not saved: added to the upscaling mip bias
        bool upscaler_jitter_flip = false; // developer, not saved: negate the jitter passed to DLSS/XeSS (sign convention check)
        bool jitter_enabled = true; // follows taa_enabled unless changed in developer builds
        int jitter_phases = 8; // Halton(2,3) sequence length
        float jitter_scale = 1.0f; // in pixels; > 1 only to make the effect obvious when testing

        bool taa_enabled = true;
        float taa_history_weight = 0.93f; // share of history in the blend (when current and history agree)
        float taa_history_weight_motion = 0.9f; // history share at high motion (lower = less ghosting, more aliasing)
        float taa_clip_gamma = 1.25f; // variance clip box size in standard deviations
        bool taa_unjitter = false; // sample the current frame at the unjittered position (softer)
        bool taa_reconstruction = false; // Gaussian 3x3 reconstruction of the current sample (softer; no visible flicker gain in game)
        bool taa_anti_flicker = true; // luminance-difference history weighting
        float taa_history_weight_min = 0.95f; // history weight when current and history disagree
        float taa_clip_gamma_static = 5.0f; // clip box for still pixels (larger = less flicker)
        bool map_detect_no_shadows = true; // also treat frames without sun shadow pass as a map
        bool taa_before_hud = true; // resolve before the HUD pass: HUD/holograms not anti-aliased, no ghosting
        bool taa_soft_offscreen = true; // reuse the edge of the history for pixels entering the screen
        float taa_hdr_scale = 30.0f; // scene values are multiplied by this before the HDR compression c/(1+c) (30: chosen in game)

        bool motion_enabled = true;
        int mask_source = 0; // 0: game stencil bit (player's vehicle), 1: distance
        int mask_stencil_bits = 0x80; // stencil bit(s) the game sets on the player's vehicle
        float mask_distance = 4.0f; // metres: closer pixels are treated as attached to the vehicle (distance mode)
        bool mask_auto_off_on_foot = true; // no vehicle on foot: the ground near the feet must move
        bool vehicle_motion = true; // cockpit/SRV pixels get the vehicle's motion (t33), else none
        bool fix_sun_shadows = true; // jittered view rays for the sun shadow mask and the tiled lighting (shadow_fix.cpp)
        bool jitter_overlay_draws = true; // also jitter decals, light volumes and glows (else they z-fight and flicker)
        bool object_analysis = false; // developer: read t33 and match instances across frames
        int debug = 0; // debug_view
        float debug_motion_pixels = 16.0f; // motion that saturates the debug colours
    };

    // One perspective camera: c270-c273 (row-vector viewProj) and c275.
    struct camera_frame
    {
        float view_proj[16] = {};
        float position[3] = {};
        camera_info info;
        uint32_t scene_draws = 0; // scene draws made with this camera in the frame
        bool valid = false;
    };

    // One decoded camera buffer update, kept for the diagnostics view.
    struct camera_update
    {
        camera_info info;
        const char* source = ""; // "Map" or "UpdateSubresource"
        float regs[11][4] = {}; // c189, c196-c199, c270-c275
    };

    // Counters for one frame.
    struct frame_stats
    {
        uint32_t camera_updates[3] = {}; // indexed by view_kind
        uint32_t jittered_draws = 0;
        uint32_t scene_draws = 0; // draws into the scene targets (jittered or not)
        uint32_t scene_draws_no_camera = 0; // scene draws before any perspective camera update
        bool map_shader_drawn = false; // a system map pixel shader was used
        uint32_t gbuffer_draws = 0; // scene geometry draws into the G-buffer (not the HDR target)
        // Resolve point markers seen in the frame (diagnostics when it is not reached).
        uint32_t hdr_draws = 0; // draws into the HDR scene target
        bool exposure_downsample = false; // R11G11B10 draw at 1/4 render width
        bool hud_shader = false, bloom_shader = false;
        uint32_t gbuffer_binds = 0; // render target bindings that look like a G-buffer (4 targets + depth)
        uint32_t shadow_fixes = 0; // sun shadow mask draws given jittered constants
        uint32_t lighting_fixes = 0; // tiled lighting dispatches given jittered constants
        uint32_t occlusion_fixes = 0; // ambient occlusion dispatches given jittered constants

        uint32_t large_cb_maps = 0; // Map of constant buffers >= camera size
        uint32_t large_cb_updates = 0; // UpdateSubresource of constant buffers >= camera size
        static constexpr int max_updates = 16;
        camera_update updates[max_updates]; // first perspective updates of the frame
        int update_count = 0;
        int camera_count = 0; // distinct cameras used by scene draws
    };

    // A moving instance on screen (object_motion.cpp), for the debug view.
    struct object_marker
    {
        enum kind_t : int
        {
            moving = 1,
            vehicle = 2,
            spinning = 3
        };
        float x = 0, y = 0; // current position, UV
        float offset_x = 0, offset_y = 0; // ghost offset in UV: true previous position - camera reprojection
        float error_px = 0; // length of the offset in render pixels
        int kind = moving;
    };

    // Instance matching results of the last frame (object_motion.cpp).
    struct object_stats
    {
        uint32_t buffer_maps = 0; // Map/Unmap of the instance buffer (t33)
        uint32_t records = 0; // valid records read
        uint32_t still = 0; // same position and rotation as a previous record
        uint32_t spinning = 0; // same position, other rotation
        uint32_t moving = 0; // matched to a previous record at another position
        uint32_t vehicle = 0; // moving, within 20 m of the camera (player's vehicle: cockpit parts, SRV)
        uint32_t unmatched = 0; // no previous record found (new, or moved more than 32 m)
        uint32_t visible_moving = 0; // moving (not vehicle) records on screen
        float max_error_px = 0, mean_error_px = 0; // camera reprojection error on them
        float read_ms = 0, match_ms = 0; // CPU cost
    };

    // Reference frame of the scene, from how the camera position (c275) moves.
    enum class frame_mode
    {
        unknown,
        camera_fixed, // camera does not move in its frame: ship space (docked) or standing still
        camera_moving, // camera moves through a local world frame (flight, SRV, on foot)
    };

    struct addon_state
    {
        std::recursive_mutex mutex; // recursive: D3D11 calls may raise events re-entrantly
        settings config;

        uint64_t camera_buffer = 0; // resource handle of the detected camera buffer
        camera_frame current_view; // last perspective camera written to the buffer

        // Scene depth: the largest depth buffer bound together with 4 render
        // targets (G-buffer pass). Its size is the scene render size.
        uint64_t scene_depth = 0;
        uint32_t render_w = 0, render_h = 0;
        uint32_t back_buffer_w = 0, back_buffer_h = 0; // swap chain size, from Present (display size)
        int jitter_phases = 8; // Halton phases in use: the setting x (display / render)^2 when upscaling
        bool upscaling_active = false; // last frame was upscaled by DLSS/XeSS: the method above is not used
        uint64_t scene_color = 0; // HDR scene target (G-buffer RT3, R11G11B10), resolved by TAA
        uint64_t depth_candidate = 0; // largest such depth buffer in the frame in progress
        uint64_t color_candidate = 0; // its HDR target
        uint32_t candidate_w = 0, candidate_h = 0;

        // Cameras used by scene draws in the frame in progress; the most used is the main camera.
        static constexpr int max_candidates = 8;
        camera_frame candidates[max_candidates];
        int candidate_count = 0;
        camera_frame camera, prev_camera; // main camera of the last completed frame and the one before
        float camera_speed = 0; // smoothed |delta c275| per frame, metres
        frame_mode mode = frame_mode::unknown;
        bool vehicle_known = false; // last resolve found the vehicle's motion
        bool on_foot = false; // last resolve saw the first-person body camera (vehicle mask disabled)

        uint64_t frame = 0;
        float jitter_x = 0, jitter_y = 0; // offset in pixels used by the frame in progress
        frame_stats current, last;
        bool resolved_this_frame = false; // resolve point reached
        const char* resolve_point = "none"; // which one, for the overlay
        bool exposure_seen = false; // exposure downsample of the HDR scene drawn this frame
        uint64_t exposure_target = 0; // its render target
        char status_text[192] = {}; // storage for a composed motion_status
        bool map_mode = false; // system/galaxy map on screen (decided from the previous frame)
        bool screenshot_mode = false; // high-resolution screenshot tiles being rendered (camera_tracker.cpp)
        int screenshot_hold = 0; // frames left before leaving screenshot mode
        const char* map_reason = "no";
        bool history_reset = false; // drop the TAA history at the next resolve
        // Atomic: the per-draw hooks test it before taking the lock.
        std::atomic<bool> upscale_pending = false; // the resolve ran for upscaling: replace the game's upscale draw
        bool upscale_attempted = false; // first draw into a display-size target after the resolve seen
        bool hud_snapshot_stale = false; // a fullscreen pass was drawn into the scene after the resolve
        bool hud_mask_done = false; // HUD mask built this frame (at the bloom, or at the upscale without bloom)
        uint64_t ldr_image = 0; // last render-size R8G8B8A8 target drawn after the resolve (tonemap / SMAA output)
        const char* upscale_status = "off";
        bool motion_ok = false; // motion vectors generated last frame
        // Frames in a row whose resolve did not run: past a few, no jitter. Screens
        // the resolve cannot handle (galaxy map: no main camera) would otherwise
        // show the raw jitter. A single missed frame (main menu) keeps it.
        int resolve_misses = 0;
        bool jitter_allowed = true;
        const char* motion_status = "not started";

        bool capture_requested = false; // Ctrl+Shift+C / overlay button
        std::string capture_path; // UTF-8 base folder, "[EDAA] CapturePath" in ReShade.ini
        const char* capture_status = "idle";
        std::string last_capture_folder;

        // Per-object motion analysis (developer).
        object_stats objects;
        static constexpr int max_markers = 128;
        object_marker markers[max_markers];
        int marker_count = 0;

        // Developer builds: CPU time of EDAA's handlers last frame (profile.hpp slots) and frame time.
        float cpu_ms[6] = {};
        float frame_ms = 0;

        bool dump_requested = false; // overlay button: log the next frame's camera updates
        std::atomic<uint64_t> skip_ps = 0; // developer: draws with this pixel shader hash are skipped (0: none)
        std::atomic<uint64_t> skip_cs = 0; // developer: dispatches with this compute shader hash are skipped (0: none)
        std::atomic<bool> cycler_on = false; // developer: record the shaders used, for cycle_pass
        // Mip bias (mip_bias.cpp), read at every sampler bind without the lock:
        // on from the frame start to the resolve while upscaling.
        std::atomic<bool> mip_bias_on = false;
        std::atomic<float> mip_bias_value = 0.0f; // log2(render / display) + extra
        std::atomic<uint32_t> biased_binds = 0; // sampler binds given the bias this frame
        uint32_t last_biased_binds = 0; // same, last frame (overlay)
        std::atomic<bool> dumping = false; // logging the frame in progress (atomic: tested per draw without the lock)
    };

    addon_state& state();

    // Settings saved in ReShade.ini, section [EDAA] (settings.cpp).
    void load_settings(settings& config);
    void save_settings(const settings& config);


    // True for the game's D3D11 device. ReShade also raises events for the D3D12
    // device of the XeSS bridge: every event handler ignores those.
    inline bool is_game_device(reshade::api::device* device)
    {
        return device != nullptr && device->get_api() == reshade::api::device_api::d3d11;
    }


    // Folder of EDAA.addon64 (the game folder): nvngx_dlss.dll and libxess.dll are next to it.
    std::wstring addon_folder();

    // Build identification shown in the overlay and the log.
    extern const char* const build_stamp;
    extern const char* const version; // last git tag (+ commits since, "-dirty")

    // Called for each scene draw with the state locked: counts the camera in use.
    void count_scene_camera(addon_state& s);
    // Called at Present with the state locked: picks the frame's main camera.
    void finish_camera_frame(addon_state& s);
    // Called at Present with the state locked: next jitter offset, swaps counters.
    void advance_frame(addon_state& s);
    // Developer pass cycler: skips the next (step 1), previous (-1) or no (0)
    // shader among those used by the game, to find which pass misbehaves.
    void cycle_pass(int step);
    // Cycler state for the overlay; returns the skipped hash (0: none).
    uint64_t cycler_status(char* text, size_t size);
    // Makes the frame's largest G-buffer depth the scene depth (state locked).
    void use_depth_candidate(addon_state& s);

    // Called at Present with the state locked, after finish_camera_frame: matches
    // the instance records of the frame with the previous ones (object_motion.cpp).
    void analyse_objects(addon_state& s);

    // Rigid motion of the player's vehicle (cockpit, SRV) since the previous
    // frame, from the instance records (object_motion.cpp), as a 3x4 row-major
    // matrix: current camera-relative position -> previous camera-relative
    // position. False when it cannot be found (no vehicle, no agreement).
    bool vehicle_motion(const camera_frame& cur_cam, const camera_frame& prev_cam, float out[12]);

    // Sun shadow mask pass (shadow_fix.cpp): binds a copy of its cb2 with the
    // view ray shifted by the jitter; false if the draw is left as it is.
    // restore_shadow_constants() binds the game's buffer again (next draw).
    bool fix_shadow_constants(reshade::api::command_list* cmd_list, float jitter_x, float jitter_y, uint32_t width, uint32_t height);
    void restore_shadow_constants(reshade::api::command_list* cmd_list);
    // Same for the tiled lighting compute shaders (their cb0, at the dispatch).
    bool fix_lighting_constants(reshade::api::command_list* cmd_list, float jitter_x, float jitter_y, uint32_t width, uint32_t height);
    // Same for the ambient occlusion compute shader (its cb0).
    bool fix_occlusion_constants(reshade::api::command_list* cmd_list, float jitter_x, float jitter_y, uint32_t width, uint32_t height);

    // Runs the motion vector and TAA passes, right before the game's bloom (state not locked).
    void run_resolve(reshade::api::command_list* cmd_list);

    // At the first draw into a display-size target after the resolve, when
    // upscale_pending: runs the upscaler instead of the game's upscale draw.
    // Returns true when the draw must be skipped (state not locked).
    bool run_upscale(reshade::api::command_list* cmd_list);

    // Right before the game's bloom when upscaling: builds the HUD mask from the
    // scene saved at the resolve point (state not locked).
    void run_hud_mask(reshade::api::command_list* cmd_list);

    // Fullscreen passes drawn into the scene after the resolve (on foot: the
    // helmet visor tint) are not HUD: after each one, the HUD mask takes the
    // changes so far and the snapshot is taken again (state not locked).
    void refresh_hud_snapshot(reshade::api::command_list* cmd_list);

    // Auto capture state machine, at Present (state not locked).
    void capture_on_present(reshade::api::command_queue* queue, reshade::api::swapchain* swapchain);
    // Default capture folder (Documents\EDAA-captures) or the configured one.
    void load_capture_path(addon_state& s);

    // Overlay window registered with ReShade.
    void draw_overlay(reshade::api::effect_runtime* runtime);

    const char* to_string(frame_mode mode);
}
