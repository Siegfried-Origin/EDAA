// Watches updates of constant buffers (Map/Unmap and UpdateSubresource) and
// decodes camera buffer updates. The main camera is not recognised here but
// at draw time: it is the perspective camera bound while drawing the scene
// (see frame_tracking.cpp), the one used by the most scene draws in a frame.

#include "profile.hpp"
#include "events.hpp"
#include "state.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>

using namespace reshade::api;

namespace edaa
{
    namespace
    {
        // Mapped constant buffers waiting for Unmap: resource handle -> CPU pointer.
        std::mutex mapped_mutex;
        std::unordered_map<uint64_t, void*> mapped;


        bool is_large_constant_buffer(device* device, resource resource)
        {
            const resource_desc desc = device->get_resource_desc(resource);
            return desc.type == resource_type::buffer &&
                (desc.usage & resource_usage::constant_buffer) != resource_usage::undefined &&
                desc.buffer.size >= camera_buffer_min_size;
        }


        void copy_diag_registers(const float* regs, float out[11][4])
        {
            const int indices[11] = { reg::render_size, 196, 197, 198, 199, 270, 271, 272, 273, 274, 275 };

            for (int i = 0; i < 11; ++i) {
                std::memcpy(out[i], regs + indices[i] * 4, 16);
            }
        }


        void log_update(const camera_update& u, uint64_t resource)
        {
            char msg[1024];
            int n = snprintf(msg, sizeof(msg), "EDAA dump: cb %llx via %s -> %s (%s), sx %g sy %g |w| %g near %g\n",
                static_cast<unsigned long long>(resource), u.source, to_string(u.info.kind), u.info.reason,
                u.info.sx, u.info.sy, u.info.w_length, u.info.near_plane);
            const char* names[11] = { "c189", "c196", "c197", "c198", "c199", "c270", "c271", "c272", "c273", "c274", "c275" };

            for (int i = 0; i < 11 && n > 0 && n < static_cast<int>(sizeof(msg)); ++i) {
                n += snprintf(msg + n, sizeof(msg) - n, "  %s = %g %g %g %g\n", names[i], u.regs[i][0], u.regs[i][1], u.regs[i][2], u.regs[i][3]);
            }

            reshade::log::message(reshade::log::level::info, msg);
        }


        // Decodes one update of a large constant buffer; 'regs' holds its new content.
        void process_update(uint64_t resource, const float* regs, const char* source)
        {
            const camera_info info = decode_camera(regs);

            addon_state& s = state();
            const std::lock_guard<std::recursive_mutex> lock(s.mutex);

            camera_update update;

            if (info.kind == view_kind::perspective || s.dumping) {
                update.info = info;
                update.source = source;
                copy_diag_registers(regs, update.regs);
            }

            if (s.dumping) {
                log_update(update, resource);
            }

            if (info.kind == view_kind::none) {
                return;
            }

            s.current.camera_updates[static_cast<int>(info.kind)]++;

            if (s.camera_buffer != resource) {
                s.camera_buffer = resource;
                reshade::log::message(reshade::log::level::info, "EDAA: camera constant buffer detected");
            }

            if (info.kind != view_kind::perspective) {
                return;
            }

            // High-resolution screenshot (Alt+F10): the game renders the image as
            // zoomed, off-centre tiles, one per frame. Pause jitter and TAA from
            // this very frame on (the update comes before the scene draws), and
            // for a while after the last tile. Views with a near plane below 2 cm
            // (small offscreen views) are off-centre in normal frames too.
            if (info.near_plane >= 0.02f && (std::fabs(info.off_center[0]) > 0.01f || std::fabs(info.off_center[1]) > 0.01f)) {
                if (!s.screenshot_mode) {
                    reshade::log::message(reshade::log::level::info, "EDAA: high-resolution screenshot, TAA paused");
                }

                s.screenshot_mode = true;
                s.screenshot_hold = 30;
            }

            if (s.current.update_count < frame_stats::max_updates) {
                s.current.updates[s.current.update_count++] = update;
            }

            // Remember it: the following scene draws use this camera.
            camera_frame& v = s.current_view;
            std::memcpy(v.view_proj, regs + reg::view_proj * 4, sizeof(v.view_proj));
            std::memcpy(v.position, regs + reg::camera_pos * 4, sizeof(v.position));
            v.info = info;
            v.scene_draws = 0;
            v.valid = true;
        }


        void on_map_buffer_region(device* device, resource resource, uint64_t offset, uint64_t size, map_access access, void** data)
        {
            EDAA_PROFILE(maps);

            if (!is_game_device(device)) {
                return;
            }

            if (data == nullptr || *data == nullptr || access == map_access::read_only || !is_large_constant_buffer(device, resource)) {
                return;
            }

            {
                addon_state& s = state();
                const std::lock_guard<std::recursive_mutex> lock(s.mutex);
                s.current.large_cb_maps++;
            }

            if (offset != 0 || (size != UINT64_MAX && size != 0 && size < camera_buffer_min_size)) {
                return;
            }

            const std::lock_guard<std::mutex> lock(mapped_mutex);
            mapped[resource.handle] = *data;
        }


        void on_unmap_buffer_region(device* device, resource resource)
        {
            EDAA_PROFILE(maps);

            if (!is_game_device(device)) {
                return;
            }

            void* data;
            {
                const std::lock_guard<std::mutex> lock(mapped_mutex);
                const auto it = mapped.find(resource.handle);

                if (it == mapped.end()) {
                    return;
                }

                data = it->second;
                mapped.erase(it);
            }
            // Called before the native Unmap, so the mapping is still valid.
            process_update(resource.handle, static_cast<const float*>(data), "Map");
        }


        bool on_update_buffer_region(device* device, const void* data, resource dest, uint64_t dest_offset, uint64_t size)
        {
            EDAA_PROFILE(maps);

            if (!is_game_device(device)) {
                return false;
            }

            if (data == nullptr || !is_large_constant_buffer(device, dest)) {
                return false;
            }

            {
                addon_state& s = state();
                const std::lock_guard<std::recursive_mutex> lock(s.mutex);
                s.current.large_cb_updates++;
            }

            if (dest_offset == 0 && size >= camera_buffer_min_size) {
                process_update(dest.handle, static_cast<const float*>(data), "UpdateSubresource");
            }

            return false;
        }
    }


    const char* to_string(frame_mode mode)
    {
        switch (mode) {
            case frame_mode::camera_fixed: return "camera fixed in its frame (ship space / still)";
            case frame_mode::camera_moving: return "camera moving (local world frame)";
            default: return "unknown";
        }
    }


    void count_scene_camera(addon_state& s)
    {
        const camera_frame& v = s.current_view;

        if (!v.valid) {
            s.current.scene_draws_no_camera++;
            return;
        }

        for (int i = 0; i < s.candidate_count; ++i) {
            camera_frame& c = s.candidates[i];

            if (std::memcmp(c.view_proj, v.view_proj, sizeof(c.view_proj)) == 0 && std::memcmp(c.position, v.position, sizeof(c.position)) == 0) {
                c.scene_draws++;
                return;
            }
        }

        if (s.candidate_count == addon_state::max_candidates) {
            return;
        }

        camera_frame& c = s.candidates[s.candidate_count++];
        c = v;
        c.scene_draws = 1;
    }


    void finish_camera_frame(addon_state& s)
    {
        // A requested dump covers the whole next frame.
        if (s.dumping) {
            s.dumping = false;
            reshade::log::message(reshade::log::level::info, "EDAA dump: end of frame");
        }

        if (s.dump_requested) {
            s.dump_requested = false;
            s.dumping = true;
            reshade::log::message(reshade::log::level::info, "EDAA dump: start of frame");
        }

        s.prev_camera = s.camera;
        s.camera = {};

        for (int i = 0; i < s.candidate_count; ++i) {
            if (s.candidates[i].scene_draws > s.camera.scene_draws) {
                s.camera = s.candidates[i];
            }
        }

        s.current.camera_count = s.candidate_count;
        s.candidate_count = 0;

        if (!s.camera.valid || !s.prev_camera.valid) {
            s.mode = frame_mode::unknown;
            return;
        }

        float d2 = 0;

        for (int i = 0; i < 3; ++i) {
            const float d = s.camera.position[i] - s.prev_camera.position[i];
            d2 += d * d;
        }

        // Smoothed over ~10 frames; 1 cm/frame separates "fixed" from "moving".
        s.camera_speed += (std::sqrt(d2) - s.camera_speed) * 0.1f;
        s.mode = s.camera_speed < 0.01f ? frame_mode::camera_fixed : frame_mode::camera_moving;
    }


    void register_camera_events()
    {
        reshade::register_event<reshade::addon_event::map_buffer_region>(on_map_buffer_region);
        reshade::register_event<reshade::addon_event::unmap_buffer_region>(on_unmap_buffer_region);
        reshade::register_event<reshade::addon_event::update_buffer_region>(on_update_buffer_region);
    }


    void unregister_camera_events()
    {
        reshade::unregister_event<reshade::addon_event::map_buffer_region>(on_map_buffer_region);
        reshade::unregister_event<reshade::addon_event::unmap_buffer_region>(on_unmap_buffer_region);
        reshade::unregister_event<reshade::addon_event::update_buffer_region>(on_update_buffer_region);
    }
}
