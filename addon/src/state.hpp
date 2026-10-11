// Global add-on state shared between event callbacks and the overlay.

#pragma once

#include "camera.hpp"
#include "reshade_includes.hpp"

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
	};

	struct settings
	{
		bool jitter_enabled = true; // follows taa_enabled unless changed in developer builds
		int jitter_phases = 8; // Halton(2,3) sequence length
		float jitter_scale = 1.0f; // in pixels; > 1 only to make the effect obvious when testing

		bool taa_enabled = true;
		float taa_history_weight = 0.93f; // share of history in the blend (when current and history agree)
		float taa_history_weight_motion = 0.9f; // history share at high motion (lower = less ghosting, more aliasing)
		float taa_clip_gamma = 1.25f; // variance clip box size in standard deviations
		bool taa_unjitter = false; // sample the current frame at the unjittered position (softer)
		bool taa_reconstruction = true; // Gaussian 3x3 reconstruction of the current sample (less flicker)
		bool taa_anti_flicker = true; // luminance-difference history weighting
		float taa_history_weight_min = 0.95f; // history weight when current and history disagree
		float taa_clip_gamma_static = 5.0f; // clip box for still pixels (larger = less flicker)
		bool map_detect_no_shadows = true; // also treat frames without sun shadow pass as a map
		bool taa_before_hud = true; // resolve before the HUD pass: HUD/holograms not anti-aliased, no ghosting
		bool taa_soft_offscreen = true; // reuse the edge of the history for pixels entering the screen

		bool motion_enabled = true;
		int mask_source = 0; // 0: game stencil bit (player's vehicle), 1: distance
		int mask_stencil_bits = 0x80; // stencil bit(s) the game sets on the player's vehicle
		float mask_distance = 4.0f; // metres: closer pixels are treated as attached to the vehicle (distance mode)
		bool mask_auto_off_on_foot = true; // no vehicle on foot: the ground near the feet must move
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
		const char *source = ""; // "Map" or "UpdateSubresource"
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

		uint32_t large_cb_maps = 0; // Map of constant buffers >= camera size
		uint32_t large_cb_updates = 0; // UpdateSubresource of constant buffers >= camera size
		static constexpr int max_updates = 16;
		camera_update updates[max_updates]; // first perspective updates of the frame
		int update_count = 0;
		int camera_count = 0; // distinct cameras used by scene draws
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
		uint32_t back_buffer_w = 0, back_buffer_h = 0; // swap chain size, from Present
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
		bool on_foot = false; // last resolve saw the first-person body camera (vehicle mask disabled)

		uint64_t frame = 0;
		float jitter_x = 0, jitter_y = 0; // offset in pixels used by the frame in progress
		frame_stats current, last;
		bool resolved_this_frame = false; // resolve point reached
		const char *resolve_point = "none"; // which one, for the overlay
		bool exposure_seen = false; // exposure downsample of the HDR scene drawn this frame
		bool map_mode = false; // system/galaxy map on screen (decided from the previous frame)
		const char *map_reason = "no";
		bool history_reset = false; // drop the TAA history at the next resolve
		bool motion_ok = false; // motion vectors generated last frame
		const char *motion_status = "not started";

		bool capture_requested = false; // Ctrl+Shift+C / overlay button
		std::string capture_path; // UTF-8 base folder, "[EDAA] CapturePath" in ReShade.ini
		const char *capture_status = "idle";
		std::string last_capture_folder;

		bool dump_requested = false; // overlay button: log the next frame's camera updates
		bool dumping = false; // logging the frame in progress
	};

	addon_state &state();

	// Settings saved in ReShade.ini, section [EDAA] (settings.cpp).
	void load_settings(settings &config);
	void save_settings(const settings &config);

	// Build identification shown in the overlay and the log.
	extern const char *const build_stamp;

	// Called for each scene draw with the state locked: counts the camera in use.
	void count_scene_camera(addon_state &s);
	// Called at Present with the state locked: picks the frame's main camera.
	void finish_camera_frame(addon_state &s);
	// Called at Present with the state locked: next jitter offset, swaps counters.
	void advance_frame(addon_state &s);
	// Makes the frame's largest G-buffer depth the scene depth (state locked).
	void use_depth_candidate(addon_state &s);

	// Runs the motion vector and TAA passes, right before the game's bloom (state not locked).
	void run_resolve(reshade::api::command_list *cmd_list);

	// Auto capture state machine, at Present (state not locked).
	void capture_on_present(reshade::api::command_queue *queue, reshade::api::swapchain *swapchain);
	// Default capture folder (Documents\EDAA-captures) or the configured one.
	void load_capture_path(addon_state &s);

	// Overlay window registered with ReShade.
	void draw_overlay(reshade::api::effect_runtime *runtime);

	const char *to_string(frame_mode mode);
}
