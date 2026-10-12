// Frame tracking: what the game is drawing, draw by draw.
//
//  * Scene detection: the scene depth and HDR colour are the largest targets
//    bound as a G-buffer (4 real colour targets + depth, back buffer aspect).
//  * Sub-pixel jitter through the viewport origin: shifting D3D11 viewport
//    TopLeftX/Y by a sub-pixel amount moves rasterised geometry exactly like a
//    projection jitter, whatever matrix the shader used (cb1 for models, cb0
//    for holograms/particles, cb2 for the sky). Applied only to scene
//    geometry before the TAA resolve: draws into the scene depth with a
//    reverse-Z depth test (GREATER/GREATER_EQUAL/EQUAL), full-size viewport,
//    not fullscreen triangles; plus decals, light volumes and glows (other
//    depth tests, or the HDR target only), which otherwise z-fight.
//  * TAA resolve point: before the cockpit HUD pass (see check_resolve_point).
//  * Frame trace for diagnostics (one frame of binds and draws in the log).

#include "events.hpp"
#include "state.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>

using namespace reshade::api;

namespace edaa
{
    namespace
    {
        // Per-command-list (= per D3D11 device context) tracking.
        struct __declspec(uuid("6b1f3e2a-5c47-4d0e-9a51-0f2d8b7c3e91")) context_state
        {
            bool scene_targets = false; // depth target is the render-size scene depth
            bool depth_test_ok = false; // bound depth-stencil state is a reverse-Z geometry test
            viewport game_viewport = {}; // last viewport set by the game
            bool has_viewport = false;
            bool jitter_applied = false; // our jittered viewport is currently bound
            bool resolve_ps = false; // bound pixel shader is the game's first bloom downsample
            bool hud_ps = false; // bound pixel shader is the cockpit HUD/hologram shader
            bool map_ps = false; // bound pixel shader belongs to the system map
            uint64_t rt0 = 0; // first bound render target (resource handle), its size and format
            uint32_t rt0_w = 0, rt0_h = 0;
            format rt0_format = format::unknown;
            // Frame trace (diagnostics)
            uint64_t ps_hash = 0;
            bool blend_on = false, depth_write = false;
            int depth_func = -1; // compare_op of the bound depth state, -1 = depth test off
            uint32_t trace_lines = 0;
        };

        // Depth-stencil state objects that pass the geometry test, and pixel
        // shaders marking the resolve point.
        std::mutex pipelines_mutex;
        std::unordered_map<uint64_t, bool> depth_states;
        std::unordered_set<uint64_t> resolve_shaders;
        std::unordered_set<uint64_t> hud_shaders;
        std::unordered_set<uint64_t> map_shaders;
        // For the frame trace: pixel shader hashes, blend and depth-write states.
        std::unordered_map<uint64_t, uint64_t> pixel_hashes;
        std::unordered_map<uint64_t, bool> blend_states, depth_writes;
        std::unordered_map<uint64_t, int> depth_funcs;

        // 3DMigoto hash (FNV-1, seed 0) of the bloom downsample pixel shader that
        // first reads the HDR scene (docs/reverse-engineering.md section 3).
        constexpr uint64_t bloom_downsample_ps = 0xb196cbbdf516e150ull;
        // Cockpit HUD / hologram pixel shader (also the one the ED Screenshot
        // 3DMigoto fork hides). Seen in the live frame trace in the last group
        // of draws into the HDR scene, after the exposure downsample.
        constexpr uint64_t hud_ps_hash = 0xb4523c39a461fd35ull;
        // System map pixel shaders (orbits, bodies, rings), drawn on every map
        // frame and in no other captured frame (sysmap.rdc).
        constexpr uint64_t map_ps_hashes[] = { 0xd7e5ff175bc127a8ull, 0xf5938f31368af521ull, 0x723db111682fbc7bull };


        uint64_t shader_hash(const void* code, size_t size)
        {
            uint64_t h = 0;

            for (size_t i = 0; i < size; ++i) {
                h *= 0x100000001B3ull;
                h ^= static_cast<const uint8_t*>(code)[i];
            }

            return h;
        }


        float halton(uint32_t index, uint32_t base)
        {
            float f = 1.0f, result = 0.0f;

            for (; index > 0; index /= base) {
                f /= static_cast<float>(base);
                result += f * static_cast<float>(index % base);
            }

            return result;
        }


        void on_init_command_list(command_list* cmd_list)
        {
            cmd_list->create_private_data<context_state>();
        }


        void on_destroy_command_list(command_list* cmd_list)
        {
            cmd_list->destroy_private_data<context_state>();
        }


        void on_init_pipeline(device*, pipeline_layout, uint32_t count, const pipeline_subobject* subobjects, pipeline pipeline)
        {
            for (uint32_t i = 0; i < count; ++i) {
                if (subobjects[i].type == pipeline_subobject_type::pixel_shader && subobjects[i].data != nullptr) {
                    const auto& desc = *static_cast<const shader_desc*>(subobjects[i].data);
                    const uint64_t hash = desc.code != nullptr ? shader_hash(desc.code, desc.code_size) : 0;
                    {
                        const std::lock_guard<std::mutex> lock(pipelines_mutex);
                        pixel_hashes[pipeline.handle] = hash;
                    }

                    if (hash == bloom_downsample_ps) {
                        const std::lock_guard<std::mutex> lock(pipelines_mutex);
                        resolve_shaders.insert(pipeline.handle);
                        reshade::log::message(reshade::log::level::info, "EDAA: bloom downsample shader found (TAA resolve point)");
                    }
                    else if (std::find(std::begin(map_ps_hashes), std::end(map_ps_hashes), hash) != std::end(map_ps_hashes)) {
                        const std::lock_guard<std::mutex> lock(pipelines_mutex);
                        map_shaders.insert(pipeline.handle);
                    }
                    else if (hash == hud_ps_hash) {
                        const std::lock_guard<std::mutex> lock(pipelines_mutex);
                        hud_shaders.insert(pipeline.handle);
                        reshade::log::message(reshade::log::level::info, "EDAA: cockpit HUD shader found");
                    }

                    continue;
                }

                if (subobjects[i].type == pipeline_subobject_type::blend_state && subobjects[i].data != nullptr) {
                    const std::lock_guard<std::mutex> lock(pipelines_mutex);
                    blend_states[pipeline.handle] = static_cast<const blend_desc*>(subobjects[i].data)->blend_enable[0];
                    continue;
                }

                if (subobjects[i].type != pipeline_subobject_type::depth_stencil_state || subobjects[i].data == nullptr) {
                    continue;
                }

                {
                    const auto& ds = *static_cast<const depth_stencil_desc*>(subobjects[i].data);
                    const std::lock_guard<std::mutex> lock(pipelines_mutex);
                    depth_writes[pipeline.handle] = ds.depth_enable && ds.depth_write_mask;
                    depth_funcs[pipeline.handle] = ds.depth_enable ? static_cast<int>(ds.depth_func) : -1;
                }
                const auto& desc = *static_cast<const depth_stencil_desc*>(subobjects[i].data);
                const bool ok = desc.depth_enable && (desc.depth_func == compare_op::greater || desc.depth_func == compare_op::greater_equal || desc.depth_func == compare_op::equal);
                const std::lock_guard<std::mutex> lock(pipelines_mutex);
                depth_states[pipeline.handle] = ok;
            }
        }


        void on_destroy_pipeline(device*, pipeline pipeline)
        {
            const std::lock_guard<std::mutex> lock(pipelines_mutex);
            depth_states.erase(pipeline.handle);
            resolve_shaders.erase(pipeline.handle);
            pixel_hashes.erase(pipeline.handle);
            blend_states.erase(pipeline.handle);
            depth_writes.erase(pipeline.handle);
            depth_funcs.erase(pipeline.handle);
            hud_shaders.erase(pipeline.handle);
            map_shaders.erase(pipeline.handle);
        }


        void on_bind_pipeline(command_list* cmd_list, pipeline_stage stages, pipeline pipeline)
        {
            const bool depth = (stages & pipeline_stage::depth_stencil) != 0;
            const bool pixel = (stages & pipeline_stage::pixel_shader) != 0;
            const bool blend = (stages & pipeline_stage::output_merger) != 0;

            if (!depth && !pixel && !blend) {
                return;
            }

            auto* ctx = cmd_list->get_private_data<context_state>();

            if (ctx == nullptr) {
                return;
            }

            const std::lock_guard<std::mutex> lock(pipelines_mutex);

            if (depth) {
                const auto it = depth_states.find(pipeline.handle);
                ctx->depth_test_ok = it != depth_states.end() && it->second;
                const auto w = depth_writes.find(pipeline.handle);
                ctx->depth_write = w != depth_writes.end() && w->second;
                const auto f = depth_funcs.find(pipeline.handle);
                ctx->depth_func = f != depth_funcs.end() ? f->second : -1;
            }

            if (blend) {
                const auto it = blend_states.find(pipeline.handle);
                ctx->blend_on = it != blend_states.end() && it->second;
            }

            if (pixel) {
                ctx->resolve_ps = resolve_shaders.count(pipeline.handle) != 0;
                const auto h = pixel_hashes.find(pipeline.handle);
                ctx->ps_hash = h != pixel_hashes.end() ? h->second : 0;
                ctx->hud_ps = hud_shaders.count(pipeline.handle) != 0;
                ctx->map_ps = map_shaders.count(pipeline.handle) != 0;
            }
        }


        // The scene is rendered at display size x SSAA, so it has the back buffer's aspect.
        bool same_aspect_as_back_buffer(const addon_state& s, uint32_t w, uint32_t h)
        {
            if (s.back_buffer_w == 0 || s.back_buffer_h == 0 || h == 0) {
                return true; // not known yet
            }

            const float a = static_cast<float>(w) / h;
            const float b = static_cast<float>(s.back_buffer_w) / s.back_buffer_h;
            return std::fabs(a / b - 1.0f) < 0.01f;
        }


        // True when 'rtvs' holds at least 4 real render targets of the depth buffer's size (G-buffer).
        bool is_gbuffer_binding(device* dev, uint32_t count, const resource_view* rtvs, const resource_desc& depth_desc)
        {
            uint32_t real = 0;

            for (uint32_t i = 0; i < count; ++i) {
                if (rtvs[i].handle == 0) {
                    continue; // shadow passes bind depth with empty colour slots
                }

                const resource_desc desc = dev->get_resource_desc(dev->get_resource_from_view(rtvs[i]));

                if (desc.type != resource_type::texture_2d ||
                    desc.texture.width != depth_desc.texture.width || desc.texture.height != depth_desc.texture.height) {
                    return false;
                }

                real++;
            }

            return real >= 4;
        }


        // Frame trace: one log line per render target binding and per draw,
        // for the frame after a "dump" request (overlay button or auto capture).
        bool tracing(context_state* ctx)
        {
            addon_state& s = state();
            const std::lock_guard<std::recursive_mutex> lock(s.mutex);

            if (!s.dumping) {
                ctx->trace_lines = 0;
                return false;
            }

            return ctx->trace_lines < 20000;
        }


        // Every render target binding is traced (they show how the scene is
        // detected); draws only when they touch the scene depth or HDR target
        // (shadow pass draws alone would fill the log).
        void trace_bind(context_state* ctx, device* dev, uint32_t count, const resource_view* rtvs, resource_view dsv)
        {
            if (!tracing(ctx)) {
                return;
            }

            ctx->trace_lines++;
            char msg[512];
            int n = snprintf(msg, sizeof(msg), "EDAA trace: bind %u RT", count);
            addon_state& s = state();
            auto describe = [&](resource_view v, const char* label) {
                if (v.handle == 0 || n <= 0 || n >= static_cast<int>(sizeof(msg))) {
                    return;
                }

                const resource r = dev->get_resource_from_view(v);
                const resource_desc d = dev->get_resource_desc(r);
                const std::lock_guard<std::recursive_mutex> lock(s.mutex);
                n += snprintf(msg + n, sizeof(msg) - n, " %s=%llx %ux%u fmt%u%s", label, static_cast<unsigned long long>(r.handle),
                    d.texture.width, d.texture.height, static_cast<uint32_t>(d.texture.format),
                    r.handle == s.scene_color ? " [SCENE COLOR]" : (r.handle == s.scene_depth ? " [SCENE DEPTH]" : ""));
            };

            for (uint32_t i = 0; i < count && i < 8; ++i) {
                describe(rtvs[i], "rt");
            }

            describe(dsv, "ds");
            reshade::log::message(reshade::log::level::info, msg);
        }


        void trace_draw(context_state* ctx, const char* kind, uint32_t count, uint32_t instances)
        {
            if (!tracing(ctx)) {
                return;
            }

            addon_state& s = state();
            {
                const std::lock_guard<std::recursive_mutex> lock(s.mutex);

                if (!ctx->scene_targets && (ctx->rt0 == 0 || ctx->rt0 != s.scene_color)) {
                    return;
                }
            }
            ctx->trace_lines++;
            bool resolved, jitter;
            uint32_t render_w, render_h;
            {
                const std::lock_guard<std::recursive_mutex> lock(s.mutex);
                render_w = s.render_w;
                render_h = s.render_h;
                resolved = s.resolved_this_frame;
                jitter = s.config.jitter_enabled;
            }
            // zfunc: compare_op (-1 off, 1 less, 2 equal, 3 less_equal, 4 greater, 6 greater_equal, 7 always);
            // vpfull: viewport is the full render size.
            const bool vp_full = ctx->game_viewport.width == static_cast<float>(render_w) && ctx->game_viewport.height == static_cast<float>(render_h);
            char msg[320];
            snprintf(msg, sizeof(msg), "EDAA trace: %s %u x%u ps=%016llx ztest=%d zfunc=%d zwrite=%d blend=%d scene=%d hdr=%d vpfull=%d jittered=%d %s",
                kind, count, instances, static_cast<unsigned long long>(ctx->ps_hash), ctx->depth_test_ok ? 1 : 0, ctx->depth_func,
                ctx->depth_write ? 1 : 0, ctx->blend_on ? 1 : 0, ctx->scene_targets ? 1 : 0, ctx->rt0 == s.scene_color ? 1 : 0,
                vp_full ? 1 : 0, (ctx->jitter_applied && jitter) ? 1 : 0, resolved ? "(after resolve)" : "");
            reshade::log::message(reshade::log::level::info, msg);
        }


        void on_bind_render_targets(command_list* cmd_list, uint32_t count, const resource_view* rtvs, resource_view dsv)
        {
            auto* ctx = cmd_list->get_private_data<context_state>();

            if (ctx == nullptr) {
                return;
            }

            ctx->scene_targets = false;
            device* const dev = cmd_list->get_device();
            trace_bind(ctx, dev, count, rtvs, dsv);

            // First real render target (D3D11 binds always report 8 slots).
            ctx->rt0 = 0;

            for (uint32_t i = 0; i < count; ++i) {
                if (rtvs[i].handle == 0) {
                    continue;
                }

                const resource r = dev->get_resource_from_view(rtvs[i]);
                const resource_desc d = dev->get_resource_desc(r);
                ctx->rt0 = r.handle;
                ctx->rt0_w = d.texture.width;
                ctx->rt0_h = d.texture.height;
                ctx->rt0_format = d.texture.format;
                break;
            }

            if (dsv.handle == 0) {
                return;
            }

            const resource depth = dev->get_resource_from_view(dsv);

            addon_state& s = state();
            const std::lock_guard<std::recursive_mutex> lock(s.mutex);

            // The scene depth is bound with the 4 G-buffer targets (4 real, same-size
            // render targets; shadow passes bind 4 empty slots). The 256x256
            // reflection probe does the same, so keep the largest one. Other
            // depth buffers (HUD, shadows) are never treated as the scene.
            // The choice is re-made every frame (advance_frame), so a smaller depth
            // buffer after a resolution change is picked up too.
            if (count >= 4 && rtvs[3].handle != 0) {
                s.current.gbuffer_binds++;
            }

            if (count >= 4 && depth.handle != s.depth_candidate) {
                const resource_desc desc = dev->get_resource_desc(depth);

                if (desc.type == resource_type::texture_2d && desc.texture.samples <= 1 &&
                    uint64_t(desc.texture.width) * desc.texture.height > uint64_t(s.candidate_w) * s.candidate_h &&
                    same_aspect_as_back_buffer(s, desc.texture.width, desc.texture.height) &&
                    is_gbuffer_binding(dev, count, rtvs, desc)) {
                    s.depth_candidate = depth.handle;
                    s.color_candidate = dev->get_resource_from_view(rtvs[3]).handle; // RT3 = HDR scene
                    s.candidate_w = desc.texture.width;
                    s.candidate_h = desc.texture.height;

                    if (s.scene_depth == 0) {
                        use_depth_candidate(s);
                    }
                }
            }

            ctx->scene_targets = depth.handle == s.scene_depth;
        }


        void on_bind_viewports(command_list* cmd_list, uint32_t first, uint32_t count, const viewport* viewports)
        {
            auto* ctx = cmd_list->get_private_data<context_state>();

            if (ctx == nullptr || first != 0 || count == 0) {
                return;
            }

            ctx->game_viewport = viewports[0];
            ctx->has_viewport = true;
            ctx->jitter_applied = false; // the game's call replaced ours
        }


        // Binds the jittered or the original viewport before a draw, as needed.
        void update_viewport(command_list* cmd_list, bool fullscreen_triangle, bool single_quad = false)
        {
            auto* ctx = cmd_list->get_private_data<context_state>();

            if (ctx == nullptr || !ctx->has_viewport) {
                return;
            }

            addon_state& s = state();
            bool want;
            float jx, jy;
            {
                const std::lock_guard<std::recursive_mutex> lock(s.mutex);
                const bool scene_draw = ctx->scene_targets && ctx->depth_test_ok && !fullscreen_triangle &&
                    ctx->game_viewport.width == static_cast<float>(s.render_w) &&
                    ctx->game_viewport.height == static_cast<float>(s.render_h);
                // Also jitter the other draws into the scene before the resolve: decal
                // and light volumes (LESS_EQUAL test on back faces), glows and sky
                // meshes without depth test. Left unjittered over jittered surfaces,
                // they z-fight and flicker (station signs, lamps).
                // Not single 4-vertex quads: the fog composite is one, drawn fullscreen.
                const bool overlay_draw = s.config.jitter_overlay_draws && !fullscreen_triangle && !single_quad &&
                    (ctx->scene_targets || (ctx->rt0 != 0 && ctx->rt0 == s.scene_color)) &&
                    ctx->game_viewport.width == static_cast<float>(s.render_w) &&
                    ctx->game_viewport.height == static_cast<float>(s.render_h);
                // Nothing drawn after the TAA resolve is accumulated: do not jitter it.
                want = (scene_draw || overlay_draw) && s.config.jitter_enabled && !s.resolved_this_frame && !s.map_mode;
                jx = s.jitter_x;
                jy = s.jitter_y;

                if (scene_draw) {
                    s.current.scene_draws++;

                    if (ctx->rt0 != s.scene_color) {
                        s.current.gbuffer_draws++;
                    }

                    count_scene_camera(s);
                }

                if (want) {
                    s.current.jittered_draws++;
                }
            }

            if (want == ctx->jitter_applied) {
                return;
            }

            viewport vp = ctx->game_viewport;

            if (want) {
                vp.x += jx;
                vp.y += jy;
            }

            // Calls through the API do not raise bind_viewports again.
            cmd_list->bind_viewports(0, 1, &vp);
            ctx->jitter_applied = want;
        }


        // TAA resolve point. Live frame order (docs/reverse-engineering.md
        // section 3): ... sky, planets, world transparents -> exposure downsample
        // of the HDR scene (R11G11B10 at 1/4 size) -> last group of draws into
        // the HDR scene, which holds the cockpit HUD/holograms -> bloom.
        // Default: resolve right before that last group (first draw into the HDR
        // scene after the exposure downsample, or the first HUD shader draw),
        // so the HUD is drawn after the TAA: neither jittered nor accumulated.
        // Fallback / option off: right before the first bloom downsample.
        void check_resolve_point(command_list* cmd_list)
        {
            auto* ctx = cmd_list->get_private_data<context_state>();

            if (ctx == nullptr) {
                return;
            }

            const char* point = nullptr;
            {
                addon_state& s = state();
                const std::lock_guard<std::recursive_mutex> lock(s.mutex);

                if (ctx->map_ps) {
                    s.current.map_shader_drawn = true;
                }

                s.current.hud_shader |= ctx->hud_ps;
                s.current.bloom_shader |= ctx->resolve_ps;

                if (ctx->rt0 != 0 && ctx->rt0 == s.scene_color) {
                    s.current.hdr_draws++;
                }

                if (ctx->rt0 != 0 && ctx->rt0 != s.scene_color && ctx->rt0_format == format::r11g11b10_float &&
                    ctx->rt0_w * 4 + 4 >= s.render_w && ctx->rt0_w * 4 <= s.render_w + 4) {
                    s.current.exposure_downsample = true;
                }

                // Maps (system, galaxy): no TAA, no jitter.
                if (s.resolved_this_frame || s.render_w == 0 || s.map_mode) {
                    return;
                }

                const bool into_scene = ctx->rt0 != 0 && ctx->rt0 == s.scene_color;

                if (!s.exposure_seen && ctx->rt0 != 0 && !into_scene && ctx->rt0_format == format::r11g11b10_float &&
                    ctx->rt0_w * 4 + 4 >= s.render_w && ctx->rt0_w * 4 <= s.render_w + 4) {
                    s.exposure_seen = true;
                }

                if (s.config.taa_before_hud && s.exposure_seen && into_scene) {
                    point = "before the HUD pass (after exposure downsample)";
                }
                else if (s.config.taa_before_hud && ctx->hud_ps) {
                    point = "before the HUD pass (HUD shader)";
                }
                else if (ctx->resolve_ps) {
                    point = "before bloom (HUD included)";
                }

                if (point != nullptr) {
                    s.resolve_point = point;
                }
            }

            if (point != nullptr) {
                run_resolve(cmd_list);
            }
        }


        bool on_draw(command_list* cmd_list, uint32_t vertex_count, uint32_t instance_count, uint32_t, uint32_t)
        {
            const bool fullscreen = vertex_count == 3 && instance_count == 1;
            check_resolve_point(cmd_list);
            update_viewport(cmd_list, fullscreen, vertex_count == 4 && instance_count == 1);

            if (auto* ctx = cmd_list->get_private_data<context_state>()) {
                trace_draw(ctx, "draw", vertex_count, instance_count);
            }

            return false;
        }


        bool on_draw_indexed(command_list* cmd_list, uint32_t index_count, uint32_t instance_count, uint32_t, int32_t, uint32_t)
        {
            check_resolve_point(cmd_list);
            update_viewport(cmd_list, false);

            if (auto* ctx = cmd_list->get_private_data<context_state>()) {
                trace_draw(ctx, "drawindexed", index_count, instance_count);
            }

            return false;
        }
    }


    void use_depth_candidate(addon_state& s)
    {
        if (s.color_candidate != 0) {
            s.scene_color = s.color_candidate;
        }

        if (s.depth_candidate == s.scene_depth) {
            return;
        }

        s.scene_depth = s.depth_candidate;
        s.render_w = s.candidate_w;
        s.render_h = s.candidate_h;
        char msg[96];
        snprintf(msg, sizeof(msg), "EDAA: scene depth buffer found, render size %ux%u", s.render_w, s.render_h);
        reshade::log::message(reshade::log::level::info, msg);
    }


    void advance_frame(addon_state& s)
    {
        // Map detection, applied from the next frame on: a system map shader was
        // drawn, or (heuristic) the scene has no sun shadow pass (no orthographic
        // camera, which every in-game frame has) and almost no G-buffer geometry
        // (system map: 4 draws; main menu hangar: 476; in game: 119 and more).
        constexpr uint32_t map_max_gbuffer_draws = 32;
        const bool no_shadows = s.current.scene_draws > 0 &&
            s.current.camera_updates[static_cast<int>(view_kind::orthographic)] == 0 &&
            s.current.gbuffer_draws < map_max_gbuffer_draws;
        const bool map = s.current.map_shader_drawn || (s.config.map_detect_no_shadows && no_shadows);

        if (map != s.map_mode) {
            s.map_mode = map;
            s.history_reset = true; // history from before/inside the map is not reusable
            reshade::log::message(reshade::log::level::info, map ? "EDAA: map detected, TAA paused" : "EDAA: map closed, TAA resumed");
        }

        s.map_reason = !map ? "no" : (s.current.map_shader_drawn ? "yes (system map shaders)" : "yes (no sun shadow pass)");

        if (s.depth_candidate != 0) {
            use_depth_candidate(s);
        }

        s.exposure_seen = false;
        s.depth_candidate = s.color_candidate = 0;
        s.candidate_w = s.candidate_h = 0;

        s.last = s.current;
        s.current = {};
        s.frame++;

        // Next Halton(2,3) sample, centred on the pixel: [-0.5, 0.5).
        const uint32_t phases = s.config.jitter_phases > 0 ? static_cast<uint32_t>(s.config.jitter_phases) : 1;
        const uint32_t index = static_cast<uint32_t>(s.frame % phases) + 1;
        s.jitter_x = (halton(index, 2) - 0.5f) * s.config.jitter_scale;
        s.jitter_y = (halton(index, 3) - 0.5f) * s.config.jitter_scale;
    }


    void register_frame_events()
    {
        reshade::register_event<reshade::addon_event::init_command_list>(on_init_command_list);
        reshade::register_event<reshade::addon_event::destroy_command_list>(on_destroy_command_list);
        reshade::register_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
        reshade::register_event<reshade::addon_event::destroy_pipeline>(on_destroy_pipeline);
        reshade::register_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
        reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind_render_targets);
        reshade::register_event<reshade::addon_event::bind_viewports>(on_bind_viewports);
        reshade::register_event<reshade::addon_event::draw>(on_draw);
        reshade::register_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
    }


    void unregister_frame_events()
    {
        reshade::unregister_event<reshade::addon_event::init_command_list>(on_init_command_list);
        reshade::unregister_event<reshade::addon_event::destroy_command_list>(on_destroy_command_list);
        reshade::unregister_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
        reshade::unregister_event<reshade::addon_event::destroy_pipeline>(on_destroy_pipeline);
        reshade::unregister_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
        reshade::unregister_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind_render_targets);
        reshade::unregister_event<reshade::addon_event::bind_viewports>(on_bind_viewports);
        reshade::unregister_event<reshade::addon_event::draw>(on_draw);
        reshade::unregister_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
    }
}
