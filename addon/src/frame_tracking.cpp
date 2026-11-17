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
//  * Upscaling: the game's final upscale is the first draw into a display-size
//    target after the resolve (see check_upscale_draw).
//  * Frame trace for diagnostics (one frame of binds and draws in the log).

#include "profile.hpp"
#include "events.hpp"
#include "state.hpp"
#include "exposure.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if EDAA_DEVELOPER
#include <Windows.h>
#include <d3dcompiler.h>
#endif

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
            uint64_t cs_hash = 0; // bound compute shader (lighting jitter fix, frame trace)
            bool blend_on = false, depth_write = false;
            int depth_func = -1; // compare_op of the bound depth state, -1 = depth test off
            uint32_t trace_lines = 0;
            bool ds_bound = false; // a depth-stencil view is bound (frame trace)
            // Pixel shader textures (slots 0-7) and their sizes, kept only while tracing.
            uint64_t ps_srv[8] = {};
            uint64_t ps_pipeline = 0; // bound pixel shader pipeline
            uint8_t panel_srv = 0; // hud_learn: slots holding a HUD panel texture
            uint32_t ps_srv_w[8] = {}, ps_srv_h[8] = {};
            // Vertex shader and its textures (slots 0-3, kept only while tracing).
            uint64_t vs_hash = 0, vs_pipeline = 0;
            uint64_t vs_srv[4] = {};
            uint32_t vs_srv_w[4] = {}, vs_srv_h[4] = {}, vs_srv_fmt[4] = {};
            // Elite's exposure (exposure.hpp): vertex shader texture 0 and pixel shader cb2.
            uint64_t vs_srv0 = 0, ps_cb2 = 0, ps_cb2_offset = 0;
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
        std::unordered_map<uint64_t, uint64_t> vertex_hashes;
#if EDAA_DEVELOPER
        // Developer build: vertex shader bytecode, disassembled into the frame trace
        // for draws whose vertex shader reads textures (e.g. the tonemap's exposure).
        std::unordered_map<uint64_t, std::vector<uint8_t>> vertex_code;
        std::unordered_set<uint64_t> vertex_logged; // per trace
#endif
        std::unordered_map<uint64_t, bool> blend_states, depth_writes;
        std::unordered_map<uint64_t, int> depth_funcs;

        // 3DMigoto hash (FNV-1, seed 0) of the bloom downsample pixel shader that
        // first reads the HDR scene (docs/reverse-engineering.md section 3).
        constexpr uint64_t bloom_downsample_ps = 0xb196cbbdf516e150ull;
        // Cockpit HUD / hologram pixel shader (also the one the ED Screenshot
        // 3DMigoto fork hides). Seen in the live frame trace in the last group
        // of draws into the HDR scene, after the exposure downsample.
        constexpr uint64_t hud_ps_hash = 0xb4523c39a461fd35ull;
        // Sun shadow mask pass: rebuilds positions from SV_Position (shadow_fix.cpp).
        constexpr uint64_t sun_shadow_ps = 0x77cb0c225019d08bull;
        // Tiled lighting of local lights: same ray layout in all four (shadow_fix.cpp).
        constexpr uint64_t tiled_lighting_cs[] = { 0x932e687ab8739aebull, 0xa6542bbe3011f87full, 0xb499f7667d34932cull, 0x083dc2646abe2d51ull };
        // Ambient occlusion (HBAO+ style, on deinterleaved depth).
        constexpr uint64_t occlusion_cs = 0x403797e056b7801dull;
        std::unordered_map<uint64_t, uint64_t> compute_hashes; // compute pipeline -> shader hash


        bool is_tiled_lighting(uint64_t hash)
        {
            return std::find(std::begin(tiled_lighting_cs), std::end(tiled_lighting_cs), hash) != std::end(tiled_lighting_cs);
        }


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


        // Last context looked up on this thread: ReShade's private data is a COM
        // GetPrivateData call, and the game draws thousands of times per frame on
        // one context. Any command list creation or destruction invalidates every cache.
        std::atomic<uint32_t> contexts_generation{ 0 };


        context_state* context_of(command_list* cmd_list)
        {
            thread_local command_list* cached_list = nullptr;
            thread_local context_state* cached_state = nullptr;
            thread_local uint32_t cached_generation = ~0u;
            const uint32_t generation = contexts_generation.load(std::memory_order_acquire);

            if (cmd_list != cached_list || generation != cached_generation) {
                cached_state = cmd_list->get_private_data<context_state>();
                cached_list = cmd_list;
                cached_generation = generation;
            }

            return cached_state;
        }


        void on_init_command_list(command_list* cmd_list)
        {
            if (!is_game_device(cmd_list->get_device())) {
                return; // the XeSS bridge's D3D12 lists: no tracking (other handlers see no private data)
            }

            cmd_list->create_private_data<context_state>();
            contexts_generation.fetch_add(1, std::memory_order_acq_rel); // a cached "none" for this list is stale
        }


        void on_destroy_command_list(command_list* cmd_list)
        {
            if (!is_game_device(cmd_list->get_device())) {
                return;
            }

            contexts_generation.fetch_add(1, std::memory_order_acq_rel);
            cmd_list->destroy_private_data<context_state>();
        }


        void on_init_pipeline(device* device, pipeline_layout, uint32_t count, const pipeline_subobject* subobjects, pipeline pipeline)
        {
            if (!is_game_device(device)) {
                return;
            }

            for (uint32_t i = 0; i < count; ++i) {
                if (subobjects[i].type == pipeline_subobject_type::compute_shader && subobjects[i].data != nullptr) {
                    const auto& desc = *static_cast<const shader_desc*>(subobjects[i].data);
                    const uint64_t hash = desc.code != nullptr ? shader_hash(desc.code, desc.code_size) : 0;

                    const std::lock_guard<std::mutex> lock(pipelines_mutex);
                    compute_hashes[pipeline.handle] = hash;

                    continue;
                }

                if (subobjects[i].type == pipeline_subobject_type::vertex_shader && subobjects[i].data != nullptr) {
                    const auto& desc = *static_cast<const shader_desc*>(subobjects[i].data);
                    const uint64_t hash = desc.code != nullptr ? shader_hash(desc.code, desc.code_size) : 0;

                    const std::lock_guard<std::mutex> lock(pipelines_mutex);
                    vertex_hashes[pipeline.handle] = hash;
#if EDAA_DEVELOPER
                    if (desc.code != nullptr) {
                        const auto* const code = static_cast<const uint8_t*>(desc.code);
                        vertex_code[pipeline.handle].assign(code, code + desc.code_size);
                    }
#endif
                    continue;
                }

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


        void on_destroy_pipeline(device* device, pipeline pipeline)
        {
            if (!is_game_device(device)) {
                return;
            }

            const std::lock_guard<std::mutex> lock(pipelines_mutex);
            depth_states.erase(pipeline.handle);
            resolve_shaders.erase(pipeline.handle);
            pixel_hashes.erase(pipeline.handle);
            blend_states.erase(pipeline.handle);
            depth_writes.erase(pipeline.handle);
            depth_funcs.erase(pipeline.handle);
            hud_shaders.erase(pipeline.handle);
            compute_hashes.erase(pipeline.handle);
            map_shaders.erase(pipeline.handle);
        }


        void on_bind_pipeline(command_list* cmd_list, pipeline_stage stages, pipeline pipeline)
        {
            EDAA_PROFILE(binds);
            const bool depth = (stages & pipeline_stage::depth_stencil) != 0;
            const bool pixel = (stages & pipeline_stage::pixel_shader) != 0;
            const bool blend = (stages & pipeline_stage::output_merger) != 0;
            const bool compute = (stages & pipeline_stage::compute_shader) != 0;
            const bool vertex = (stages & pipeline_stage::vertex_shader) != 0;

            if (!depth && !pixel && !blend && !compute && !vertex) {
                return;
            }

            auto* ctx = context_of(cmd_list);

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

            if (vertex) {
                ctx->vs_pipeline = pipeline.handle;
                const auto h = vertex_hashes.find(pipeline.handle);
                ctx->vs_hash = h != vertex_hashes.end() ? h->second : 0;
            }

            if (compute) {
                const auto h = compute_hashes.find(pipeline.handle);
                ctx->cs_hash = h != compute_hashes.end() ? h->second : 0;
            }

            if (pixel) {
                ctx->ps_pipeline = pipeline.handle;
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

            if (!s.dumping) { // without the lock: called for every draw and bind
                ctx->trace_lines = 0;
                return false;
            }

            const std::lock_guard<std::recursive_mutex> lock(s.mutex);

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
                    r.handle == s.scene_color ? " [SCENE COLOR]" : (r.handle == s.scene_depth ? " [SCENE DEPTH]" :
                    (d.texture.width == s.back_buffer_w && d.texture.height == s.back_buffer_h ? " [DISPLAY SIZE]" : "")));
            };

            for (uint32_t i = 0; i < count && i < 8; ++i) {
                describe(rtvs[i], "rt");
            }

            describe(dsv, "ds");
            reshade::log::message(reshade::log::level::info, msg);
        }


#if EDAA_DEVELOPER
        // Frame trace: the disassembly of a vertex shader that reads textures, once per trace.
        void log_vertex_disassembly(uint64_t pipeline_handle, uint64_t hash)
        {
            std::vector<uint8_t> code;
            {
                const std::lock_guard<std::mutex> lock(pipelines_mutex);

                if (vertex_logged.size() >= 16 || !vertex_logged.insert(hash).second) {
                    return;
                }

                const auto it = vertex_code.find(pipeline_handle);

                if (it == vertex_code.end()) {
                    return;
                }

                code = it->second;
            }

            using disassemble_fn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, UINT, LPCSTR, ID3DBlob**);
            static const auto disassemble = [] {
                HMODULE module = GetModuleHandleW(L"d3dcompiler_47.dll");
                module = module != nullptr ? module : LoadLibraryW(L"d3dcompiler_47.dll");
                return module != nullptr ? reinterpret_cast<disassemble_fn>(GetProcAddress(module, "D3DDisassemble")) : nullptr;
            }();
            ID3DBlob* text = nullptr;

            if (disassemble == nullptr || FAILED(disassemble(code.data(), code.size(), 0, nullptr, &text)) || text == nullptr) {
                reshade::log::message(reshade::log::level::warning, "EDAA trace: vertex shader disassembly not available");
                return;
            }

            const std::string all(static_cast<const char*>(text->GetBufferPointer()), text->GetBufferSize());
            text->Release();
            char head[64];
            snprintf(head, sizeof(head), "EDAA vs %016llx:", static_cast<unsigned long long>(hash));
            size_t start = 0;

            while (start < all.size()) {
                size_t end = all.find('\n', start);
                end = end == std::string::npos ? all.size() : end;
                const std::string line = all.substr(start, end - start);

                if (line.find_first_not_of(" \r\t/", 0) != std::string::npos && line[0] != '\0') {
                    reshade::log::message(reshade::log::level::info, (std::string(head) + " " + line).c_str());
                }

                start = end + 1;
            }
        }
#endif

        void trace_draw(context_state* ctx, const char* kind, uint32_t count, uint32_t instances)
        {
            if (!tracing(ctx)) {
                return;
            }

            addon_state& s = state();
            {
                const std::lock_guard<std::recursive_mutex> lock(s.mutex);

                if (!s.resolved_this_frame && !ctx->scene_targets && (ctx->rt0 == 0 || ctx->rt0 != s.scene_color)) {
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
            char msg[720];
            snprintf(msg, sizeof(msg), "EDAA trace: %s %u x%u rt=%llx %ux%u fmt%u display=%d ds=%d ps=%016llx ztest=%d zfunc=%d zwrite=%d blend=%d scene=%d hdr=%d vpfull=%d jittered=%d %s",
                kind, count, instances, static_cast<unsigned long long>(ctx->rt0), ctx->rt0_w, ctx->rt0_h, static_cast<uint32_t>(ctx->rt0_format),
                ctx->rt0 != 0 && ctx->rt0_w == s.back_buffer_w && ctx->rt0_h == s.back_buffer_h ? 1 : 0, ctx->ds_bound ? 1 : 0,
                static_cast<unsigned long long>(ctx->ps_hash), ctx->depth_test_ok ? 1 : 0, ctx->depth_func,
                ctx->depth_write ? 1 : 0, ctx->blend_on ? 1 : 0, ctx->scene_targets ? 1 : 0, ctx->rt0 == s.scene_color ? 1 : 0,
                vp_full ? 1 : 0, (ctx->jitter_applied && jitter) ? 1 : 0, resolved ? "(after resolve)" : "");
            int m = static_cast<int>(strlen(msg));

            for (int i = 0; i < 8 && m > 0 && m < static_cast<int>(sizeof(msg)) - 48; ++i) {
                if (ctx->ps_srv[i] != 0) {
                    m += snprintf(msg + m, sizeof(msg) - m, " t%d=%llx %ux%u", i, static_cast<unsigned long long>(ctx->ps_srv[i]), ctx->ps_srv_w[i], ctx->ps_srv_h[i]);
                }
            }

            bool vs_reads = false;

            if (m > 0 && m < static_cast<int>(sizeof(msg)) - 32) {
                m += snprintf(msg + m, sizeof(msg) - m, " vs=%016llx", static_cast<unsigned long long>(ctx->vs_hash));
            }

            for (int i = 0; i < 4 && m > 0 && m < static_cast<int>(sizeof(msg)) - 48; ++i) {
                if (ctx->vs_srv[i] != 0) {
                    vs_reads = true;
                    m += snprintf(msg + m, sizeof(msg) - m, " vt%d=%llx %ux%u fmt%u", i, static_cast<unsigned long long>(ctx->vs_srv[i]), ctx->vs_srv_w[i],
                        ctx->vs_srv_h[i], ctx->vs_srv_fmt[i]);
                }
            }

            reshade::log::message(reshade::log::level::info, msg);

#if EDAA_DEVELOPER
            if (vs_reads) {
                log_vertex_disassembly(ctx->vs_pipeline, ctx->vs_hash);
            }
#else
            (void)vs_reads;
#endif
        }


        // Frame trace: which textures the pixel shader reads (finds the HUD panel draws).
        void on_push_descriptors_trace(command_list* cmd_list, shader_stage stages, pipeline_layout, uint32_t, const descriptor_table_update& update)
        {
            addon_state& s = state();

            // Elite's exposure (exposure.hpp): what the tonemap draw will read.
            const bool vs_texture0 = update.type == descriptor_type::shader_resource_view && (stages & shader_stage::vertex) != 0 && update.binding == 0 &&
                update.count > 0;
            const bool ps_cb2 = update.type == descriptor_type::constant_buffer && (stages & shader_stage::pixel) != 0 && update.binding <= 2 &&
                update.binding + update.count > 2;

            if (vs_texture0 || ps_cb2) {
                if (auto* c = context_of(cmd_list)) {
                    if (vs_texture0) {
                        const resource_view view = static_cast<const resource_view*>(update.descriptors)[0];
                        c->vs_srv0 = view.handle != 0 ? cmd_list->get_device()->get_resource_from_view(view).handle : 0;
                    }
                    else {
                        const buffer_range& range = static_cast<const buffer_range*>(update.descriptors)[2 - update.binding];
                        c->ps_cb2 = range.buffer.handle;
                        c->ps_cb2_offset = range.offset;
                    }
                }
            }

            const bool dumping = s.dumping.load(std::memory_order_relaxed), learning = s.hud_learning.load(std::memory_order_relaxed);

            if ((!dumping && !learning) || update.type != descriptor_type::shader_resource_view) {
                return;
            }

            auto* ctx = context_of(cmd_list);

            if (ctx == nullptr) {
                return;
            }

            device* const dev = cmd_list->get_device();
            const auto* const views = static_cast<const resource_view*>(update.descriptors);

            // Vertex shader textures (frame trace only).
            if (dumping && (stages & shader_stage::vertex) != 0) {
                for (uint32_t i = 0; i < update.count && update.binding + i < 4; ++i) {
                    const uint32_t slot = update.binding + i;
                    ctx->vs_srv[slot] = 0;
                    ctx->vs_srv_w[slot] = ctx->vs_srv_h[slot] = ctx->vs_srv_fmt[slot] = 0;

                    if (views[i].handle != 0) {
                        const resource r = dev->get_resource_from_view(views[i]);
                        const resource_desc d = dev->get_resource_desc(r);
                        ctx->vs_srv[slot] = r.handle;
                        ctx->vs_srv_w[slot] = d.texture.width;
                        ctx->vs_srv_h[slot] = d.texture.height;
                        ctx->vs_srv_fmt[slot] = static_cast<uint32_t>(d.texture.format);
                    }
                }
            }

            if ((stages & shader_stage::pixel) == 0) {
                return;
            }

            for (uint32_t i = 0; i < update.count; ++i) {
                const uint32_t slot = update.binding + i;

                if (slot >= 8) {
                    break;
                }

                ctx->ps_srv[slot] = 0;
                ctx->ps_srv_w[slot] = ctx->ps_srv_h[slot] = 0;
                ctx->panel_srv &= static_cast<uint8_t>(~(1u << slot));

                if (views[i].handle != 0) {
                    const resource r = dev->get_resource_from_view(views[i]);

                    if (learning) {
                        const std::lock_guard<std::recursive_mutex> lock(s.mutex);

                        for (int k = 0; k < s.hud_panel_count; ++k) {
                            if (s.hud_panels[k] == r.handle) {
                                ctx->panel_srv |= static_cast<uint8_t>(1u << slot);
                            }
                        }
                    }

                    if (!dumping) {
                        continue;
                    }

                    const resource_desc d = dev->get_resource_desc(r);
                    ctx->ps_srv[slot] = r.handle;
                    ctx->ps_srv_w[slot] = d.texture.width;
                    ctx->ps_srv_h[slot] = d.texture.height;
                }
            }
        }


        void on_bind_render_targets(command_list* cmd_list, uint32_t count, const resource_view* rtvs, resource_view dsv)
        {
            EDAA_PROFILE(binds);
            auto* ctx = context_of(cmd_list);

            if (ctx == nullptr) {
                return;
            }

            ctx->scene_targets = false;
            ctx->ds_bound = dsv.handle != 0;
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

            // hud_learn: the cockpit HUD panels are rendered offscreen into colour
            // targets with a D24S8 depth buffer, then blended into the scene.
            if (dsv.handle != 0 && ctx->rt0 != 0 && state().hud_learning.load(std::memory_order_relaxed) &&
                (ctx->rt0_format == format::r8g8b8a8_typeless || ctx->rt0_format == format::r8g8b8a8_unorm ||
                 ctx->rt0_format == format::r8g8b8a8_unorm_srgb)) {
                const format df = dev->get_resource_desc(dev->get_resource_from_view(dsv)).texture.format;

                if (df == format::r24_g8_typeless || df == format::d24_unorm_s8_uint) {
                    addon_state& s = state();
                    const std::lock_guard<std::recursive_mutex> lock(s.mutex);
                    bool known = ctx->rt0 == s.scene_color;

                    for (int k = 0; k < s.hud_panel_count; ++k) {
                        known |= s.hud_panels[k] == ctx->rt0;
                    }

                    if (!known && s.hud_panel_count < 8) {
                        s.hud_panels[s.hud_panel_count++] = ctx->rt0;
                    }
                }
            }

            if (dsv.handle == 0) {
                // Upscaling input when the game's upscale does not read it directly (FSR).
                const bool ldr = ctx->rt0_format == format::r8g8b8a8_typeless || ctx->rt0_format == format::r8g8b8a8_unorm ||
                    ctx->rt0_format == format::r8g8b8a8_unorm_srgb;

                if (ctx->rt0 != 0 && ldr) {
                    addon_state& s = state();
                    const std::lock_guard<std::recursive_mutex> lock(s.mutex);

                    if (s.resolved_this_frame && ctx->rt0_w == s.render_w && ctx->rt0_h == s.render_h) {
                        s.ldr_image = ctx->rt0;
                    }
                }

                return;
            }

            const resource depth = dev->get_resource_from_view(dsv);
            addon_state& s = state();

            // The scene depth is bound with the 4 G-buffer targets (4 real, same-size
            // render targets; shadow passes bind 4 empty slots). The 256x256
            // reflection probe does the same, so keep the largest one. Other
            // depth buffers (HUD, shadows) are never treated as the scene.
            // The choice is re-made every frame (advance_frame), so a smaller depth
            // buffer after a resolution change is picked up too.
            // No device call with the state locked: a game thread destroying a
            // resource holds the D3D11 lock and waits for the state lock in
            // on_destroy_resource (deadlock seen on resolution changes).
            uint64_t known_candidate;
            {
                const std::lock_guard<std::recursive_mutex> lock(s.mutex);
                known_candidate = s.depth_candidate;

                if (count >= 4 && rtvs[3].handle != 0) {
                    s.current.gbuffer_binds++;
                }
            }

            resource_desc desc = {};
            uint64_t color = 0;

            if (count >= 4 && depth.handle != known_candidate) {
                desc = dev->get_resource_desc(depth);

                if (desc.type == resource_type::texture_2d && desc.texture.samples <= 1 && is_gbuffer_binding(dev, count, rtvs, desc)) {
                    color = dev->get_resource_from_view(rtvs[3]).handle; // RT3 = HDR scene
                }
            }

            const std::lock_guard<std::recursive_mutex> lock(s.mutex);

            if (color != 0 && uint64_t(desc.texture.width) * desc.texture.height > uint64_t(s.candidate_w) * s.candidate_h &&
                same_aspect_as_back_buffer(s, desc.texture.width, desc.texture.height)) {
                s.depth_candidate = depth.handle;
                s.color_candidate = color;
                s.candidate_w = desc.texture.width;
                s.candidate_h = desc.texture.height;

                if (s.scene_depth == 0) {
                    use_depth_candidate(s);
                }
            }

            ctx->scene_targets = depth.handle == s.scene_depth;
        }


        void on_bind_viewports(command_list* cmd_list, uint32_t first, uint32_t count, const viewport* viewports)
        {
            EDAA_PROFILE(binds);
            auto* ctx = context_of(cmd_list);

            if (ctx == nullptr || first != 0 || count == 0) {
                return;
            }

            ctx->game_viewport = viewports[0];
            ctx->has_viewport = true;
            ctx->jitter_applied = false; // the game's call replaced ours
        }


        // Binds the jittered or the original viewport before a draw, as needed.
        // Whether this draw gets the jittered viewport (state locked). Counts the scene draws.
        bool jitter_decision(addon_state& s, context_state* ctx, bool fullscreen_triangle, bool single_quad, bool resolving)
        {
            if (!ctx->has_viewport) {
                return false;
            }

            const bool full_viewport = ctx->game_viewport.width == static_cast<float>(s.render_w) &&
                ctx->game_viewport.height == static_cast<float>(s.render_h);
            const bool scene_draw = ctx->scene_targets && ctx->depth_test_ok && !fullscreen_triangle && full_viewport;
            // Also jitter the other draws into the scene before the resolve: decal
            // and light volumes (LESS_EQUAL test on back faces), glows and sky
            // meshes without depth test. Left unjittered over jittered surfaces,
            // they z-fight and flicker (station signs, lamps).
            // Not single 4-vertex quads: the fog composite is one, drawn fullscreen.
            const bool overlay_draw = s.config.jitter_overlay_draws && !fullscreen_triangle && !single_quad &&
                (ctx->scene_targets || (ctx->rt0 != 0 && ctx->rt0 == s.scene_color)) && full_viewport;
            // Nothing drawn after the TAA resolve is accumulated: do not jitter it.
            const bool want = (scene_draw || overlay_draw) && s.config.jitter_enabled && s.jitter_allowed && !s.resolved_this_frame &&
                !resolving && !s.map_mode && !s.screenshot_mode;

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

            return want;
        }


        // Binds the jittered or the original viewport before a draw, as needed (no lock).
        void apply_viewport(command_list* cmd_list, context_state* ctx, bool want, float jx, float jy)
        {
            if (!ctx->has_viewport) {
                return;
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


        // Width of the game's exposure downsample (R11G11B10): a quarter of the
        // render width, but never below 320 pixels (seen at 960x540: 320x180).
        bool is_exposure_width(uint32_t w, uint32_t render_w)
        {
            const uint32_t expected = std::max(render_w / 4, 320u);
            return w + 1 >= expected && w <= expected + 1;
        }


        // TAA resolve point. Live frame order (docs/reverse-engineering.md
        // section 3): ... sky, planets, world transparents -> exposure downsample
        // of the HDR scene (R11G11B10 at 1/4 size) -> last group of draws into
        // the HDR scene, which holds the cockpit HUD/holograms -> bloom.
        // Default: resolve right before that last group (first draw into the HDR
        // scene after the exposure downsample, or the first HUD shader draw),
        // so the HUD is drawn after the TAA: neither jittered nor accumulated.
        // Fallback / option off: right before the first bloom downsample.
        // Also decides this draw's jitter (want, jx, jy), under the same lock.
        void check_resolve_point(command_list* cmd_list, context_state* ctx, bool fullscreen, bool single_quad, bool& want, float& jx, float& jy)
        {
            const char* point = nullptr;
            bool reactive_snapshot = false;
            uint64_t scene_color = 0;
            {
                addon_state& s = state();
                const std::lock_guard<std::recursive_mutex> lock(s.mutex);
                jx = s.jitter_x;
                jy = s.jitter_y;

                if (ctx->map_ps) {
                    s.current.map_shader_drawn = true;
                }

                s.current.hud_shader |= ctx->hud_ps;
                s.current.bloom_shader |= ctx->resolve_ps;

                if (ctx->rt0 != 0 && ctx->rt0 == s.scene_color) {
                    s.current.hdr_draws++;
                }

                if (ctx->rt0 != 0 && ctx->rt0 != s.scene_color && ctx->rt0_format == format::r11g11b10_float &&
                    is_exposure_width(ctx->rt0_w, s.render_w)) {
                    s.current.exposure_downsample = true;
                }

                // Maps (system, galaxy): no TAA, no jitter.
                if (s.resolved_this_frame || s.render_w == 0 || s.map_mode || s.screenshot_mode) {
                    want = jitter_decision(s, ctx, fullscreen, single_quad, false);
                    return;
                }

                const bool into_scene = ctx->rt0 != 0 && ctx->rt0 == s.scene_color;

                // Reactive mask: forward transparents (particles, glass, glows) are
                // blended, write no depth and test the scene depth with the reverse-Z
                // geometry test (GREATER / GREATER_EQUAL). The deferred light volumes
                // test LESS_EQUAL on back faces, the sun light and glows test nothing:
                // not counted. The scene is saved right before the first transparent
                // (pipeline state only, no shader hashes).
                const bool forward_test = ctx->depth_func == static_cast<int>(compare_op::greater) ||
                    ctx->depth_func == static_cast<int>(compare_op::greater_equal);

                if (into_scene && s.config.dlss_reactive_mask && ctx->scene_targets && ctx->blend_on && !ctx->depth_write && forward_test &&
                    !fullscreen && !single_quad) {
                    s.reactive_gate = true;
                    ++s.reactive_draws;

                    if (!s.reactive_snapped) {
                        s.reactive_snapped = true;
                        ++s.reactive_snapshots;
                        reactive_snapshot = true;
                        scene_color = s.scene_color;
                    }
                }

                if (!s.exposure_seen && ctx->rt0 != 0 && !into_scene && ctx->rt0_format == format::r11g11b10_float &&
                    is_exposure_width(ctx->rt0_w, s.render_w)) {
                    s.exposure_seen = true;
                    s.exposure_target = ctx->rt0;
                }

                // Option off but no bloom downsample drawn last frame (bloom shader
                // replaced, e.g. EDHM, or another variant at a low bloom setting):
                // the bloom point would never come, so no TAA, DLSS or frame
                // generation at all. Resolve before the HUD instead.
                const bool no_bloom = !s.config.taa_before_hud && !s.last.bloom_shader;
                const bool before_hud = s.config.taa_before_hud || no_bloom;

                if (before_hud && s.exposure_seen && into_scene) {
                    point = "before the HUD pass (after exposure downsample)";
                }
                else if (before_hud && ctx->hud_ps) {
                    point = "before the HUD pass (HUD shader)";
                }

                // The world is complete once the exposure is measured. Without a HUD
                // draw into the scene (on foot) and without bloom (graphics option
                // off), the next pass elsewhere is the last chance before tonemapping.
                else if (before_hud && s.exposure_seen && ctx->rt0 != 0 && ctx->rt0 != s.exposure_target) {
                    point = "after the exposure downsample (no HUD draw into the scene)";
                }
                else if (ctx->resolve_ps) {
                    point = "before bloom (HUD included)";
                }

                if (point != nullptr && no_bloom && !ctx->resolve_ps) {
                    point = "before the HUD pass (no bloom pass found)";

                    if (!s.bloom_fallback_logged) {
                        s.bloom_fallback_logged = true;
                        reshade::log::message(reshade::log::level::warning,
                            "EDAA: no bloom pass found, resolving before the HUD although that option is off");
                    }
                }

                if (point != nullptr) {
                    s.resolve_point = point;
                }

                want = jitter_decision(s, ctx, fullscreen, single_quad, point != nullptr);
            }

            if (point != nullptr) {
                run_resolve(cmd_list);
            }
            else if (reactive_snapshot) {
                run_reactive_snapshot(cmd_list, scene_color);
            }
        }


        // When rendering below the display size, the game draws its tonemapped,
        // render-size image into the back buffer (bilinear or FSR) before the 2D
        // UI. Only that first draw into a display-size target is examined.
        bool check_upscale_draw(command_list* cmd_list, context_state* ctx)
        {
            if (ctx == nullptr) {
                return false;
            }

            if (!state().upscale_pending) {
                return false;
            }

            {
                addon_state& s = state();
                const std::lock_guard<std::recursive_mutex> lock(s.mutex);

                if (!s.upscale_pending || s.upscale_attempted || ctx->rt0 == 0 || ctx->rt0 == s.scene_color ||
                    ctx->rt0_w != s.back_buffer_w || ctx->rt0_h != s.back_buffer_h) {
                    return false;
                }

                s.upscale_attempted = true;
            }

            return run_upscale(cmd_list);
        }


        // Upscaling: the first bloom downsample draw follows the cockpit HUD,
        // the last thing drawn into the scene: time to build the HUD mask.
        // Frame generation without a bloom pass (none drawn last frame): the first
        // draw into a display-size target after the resolve; the scene holds the
        // HUD by then. Upscaling builds its mask in run_upscale.
        void check_hud_mask(command_list* cmd_list, context_state* ctx)
        {
            if (ctx == nullptr || !(state().upscale_pending || state().fg_hud_pending)) {
                return;
            }

            {
                addon_state& s = state();
                const std::lock_guard<std::recursive_mutex> lock(s.mutex);

                if (!(s.upscale_pending ? s.config.upscale_hud_mask : s.fg_hud_pending.load()) || s.hud_mask_done) {
                    return;
                }

                const bool late = !ctx->resolve_ps && !s.upscale_pending && !s.last.bloom_shader && s.resolved_this_frame &&
                    ctx->rt0 != 0 && ctx->rt0 != s.scene_color && ctx->rt0_w == s.back_buffer_w && ctx->rt0_h == s.back_buffer_h;

                if (!ctx->resolve_ps && !late) {
                    return;
                }

                s.hud_mask_done = true;
            }

            run_hud_mask(cmd_list);
        }


        // Before each draw: a fullscreen pass drawn into the scene after the
        // resolve has completed, the HUD mask snapshot is taken again.
        void check_hud_snapshot(command_list* cmd_list, context_state* ctx, bool fullscreen)
        {
            if (ctx == nullptr || !(state().upscale_pending || state().fg_hud_pending)) {
                return;
            }

            bool refresh;
            {
                addon_state& s = state();
                const std::lock_guard<std::recursive_mutex> lock(s.mutex);

                if (!(s.upscale_pending ? s.config.upscale_hud_mask : s.fg_hud_pending.load()) || s.hud_mask_done) {
                    return;
                }

                refresh = s.hud_snapshot_stale;
                s.hud_snapshot_stale = fullscreen && ctx->rt0 != 0 && ctx->rt0 == s.scene_color;
            }

            if (refresh) {
                refresh_hud_snapshot(cmd_list);
            }
        }


        // The game's cb2 comes back after the sun shadow draw; that draw gets
        // the jittered copy when the frame is jittered.
        void check_sun_shadows(command_list* cmd_list, context_state* ctx)
        {
            if (ctx->ps_hash != sun_shadow_ps) {
                return;
            }

            float jx, jy;
            uint32_t w, h;
            {
                addon_state& s = state();
                const std::lock_guard<std::recursive_mutex> lock(s.mutex);

                if (!s.config.fix_sun_shadows || !s.config.jitter_enabled || !s.jitter_allowed || s.resolved_this_frame || s.map_mode ||
                    s.screenshot_mode) {
                    return;
                }

                jx = s.jitter_x;
                jy = s.jitter_y;
                w = s.render_w;
                h = s.render_h;
                s.current.shadow_fixes++;
            }

            fix_shadow_constants(cmd_list, jx, jy, w, h);
        }


        // Every check of a draw, with the context state looked up once.
        // Developer pass cycler: every shader the game uses, in order of first use.
        struct cycled_shader
        {
            uint64_t hash;
            bool compute;
        };

        std::mutex cycler_mutex;
        std::vector<cycled_shader> cycler_list;
        int cycler_index = -1; // skipped entry, -1: none


        void cycler_record(uint64_t hash, bool compute)
        {
            if (hash == 0 || !state().cycler_on.load(std::memory_order_relaxed)) {
                return;
            }

            const std::lock_guard<std::mutex> lock(cycler_mutex);

            for (const cycled_shader& c : cycler_list) {
                if (c.hash == hash && c.compute == compute) {
                    return;
                }
            }

            cycler_list.push_back({ hash, compute });
        }


        // hud_learn: a draw into the HDR scene that reads a HUD panel texture is the
        // cockpit HUD composite; its pixel shader becomes the HUD shader.
        void learn_hud_shader(context_state* ctx)
        {
            if (ctx->panel_srv == 0 || ctx->hud_ps || ctx->ps_pipeline == 0 || !state().hud_learning.load(std::memory_order_relaxed)) {
                return;
            }

            {
                addon_state& s = state();
                const std::lock_guard<std::recursive_mutex> lock(s.mutex);

                if (ctx->rt0 == 0 || ctx->rt0 != s.scene_color) {
                    return;
                }
            }

            bool added;
            {
                const std::lock_guard<std::mutex> lock(pipelines_mutex);
                added = hud_shaders.insert(ctx->ps_pipeline).second;
            }

            ctx->hud_ps = true;

            if (added) {
                char msg[128];
                snprintf(msg, sizeof(msg), "EDAA: cockpit HUD shader found by its panel textures (ps %016llx)", static_cast<unsigned long long>(ctx->ps_hash));
                reshade::log::message(reshade::log::level::info, msg);
            }
        }


        bool on_any_draw(command_list* cmd_list, const char* kind, uint32_t count, uint32_t instances, bool fullscreen, bool single_quad)
        {
            restore_shadow_constants(cmd_list);
            auto* ctx = context_of(cmd_list);

            if (ctx == nullptr) {
                return false;
            }

            // Elite's tonemap: copy its exposure inputs for the next frame's DLAA (exposure.hpp).
            if (fullscreen && ctx->vs_hash == exposure::vertex_shader_hash && ctx->vs_srv0 != 0 && ctx->ps_cb2 != 0 && exposure::wanted()) {
                exposure::capture(reinterpret_cast<ID3D11DeviceContext*>(cmd_list->get_native()), reinterpret_cast<ID3D11Resource*>(ctx->vs_srv0),
                    reinterpret_cast<ID3D11Buffer*>(ctx->ps_cb2), ctx->ps_cb2_offset);
            }

            cycler_record(ctx->ps_hash, false);
            const uint64_t skip = state().skip_ps.load(std::memory_order_relaxed);

            if (skip != 0 && ctx->ps_hash == skip) {
                return true; // developer: pass switched off
            }

            check_sun_shadows(cmd_list, ctx);
            learn_hud_shader(ctx);
            bool want = false;
            float jx = 0, jy = 0;
            check_resolve_point(cmd_list, ctx, fullscreen, single_quad, want, jx, jy);
            check_hud_snapshot(cmd_list, ctx, fullscreen);
            check_hud_mask(cmd_list, ctx);

            if (check_upscale_draw(cmd_list, ctx)) {
                return true; // replaced by the upscaler
            }

            apply_viewport(cmd_list, ctx, want, jx, jy);
            trace_draw(ctx, kind, count, instances);
            return false;
        }


        // Tiled lighting dispatches before the resolve get the jittered view ray.
        bool on_dispatch(command_list* cmd_list, uint32_t, uint32_t, uint32_t)
        {
            EDAA_PROFILE(draws);
            restore_shadow_constants(cmd_list);
            auto* ctx = context_of(cmd_list);

            if (ctx == nullptr) {
                return false;
            }

            cycler_record(ctx->cs_hash, true);
            const uint64_t skip = state().skip_cs.load(std::memory_order_relaxed);

            if (skip != 0 && ctx->cs_hash == skip) {
                return true; // developer: pass switched off
            }

            if (tracing(ctx)) {
                ctx->trace_lines++;
                char msg[96];
                snprintf(msg, sizeof(msg), "EDAA trace: dispatch cs=%016llx", static_cast<unsigned long long>(ctx->cs_hash));
                reshade::log::message(reshade::log::level::info, msg);
            }

            const bool lighting = is_tiled_lighting(ctx->cs_hash), occlusion = ctx->cs_hash == occlusion_cs;

            if (!lighting && !occlusion) {
                return false;
            }

            float jx, jy;
            uint32_t w, h;
            {
                addon_state& s = state();
                const std::lock_guard<std::recursive_mutex> lock(s.mutex);

                if (!s.config.fix_sun_shadows || !s.config.jitter_enabled || !s.jitter_allowed || s.resolved_this_frame || s.map_mode ||
                    s.screenshot_mode) {
                    return false;
                }

                jx = s.jitter_x;
                jy = s.jitter_y;
                w = s.render_w;
                h = s.render_h;
                (lighting ? s.current.lighting_fixes : s.current.occlusion_fixes)++;
            }

            if (lighting) {
                fix_lighting_constants(cmd_list, jx, jy, w, h);
            }
            else {
                fix_occlusion_constants(cmd_list, jx, jy, w, h);
            }

            return false;
        }


        // In stations the tiled lighting runs through DispatchIndirect: same handling.
        bool on_indirect(command_list* cmd_list, indirect_command type, resource, uint64_t, uint32_t draw_count, uint32_t)
        {
            if (type != indirect_command::dispatch) {
                auto* ctx = context_of(cmd_list);

                if (ctx != nullptr) {
                    trace_draw(ctx, type == indirect_command::draw_indexed ? "indirect-drawindexed" : "indirect-draw", draw_count, 0);
                }

                return false;
            }

            return on_dispatch(cmd_list, 0, 0, 0);
        }


        // Frame trace: copies into the back buffer (the HUD might arrive that way).
        void trace_copy(command_list* cmd_list, const char* kind, resource src, resource dst)
        {
            auto* ctx = context_of(cmd_list);

            if (ctx == nullptr || !tracing(ctx)) {
                return;
            }

            ctx->trace_lines++;
            char msg[160];
            snprintf(msg, sizeof(msg), "EDAA trace: %s src=%llx dst=%llx", kind, static_cast<unsigned long long>(src.handle),
                static_cast<unsigned long long>(dst.handle));
            reshade::log::message(reshade::log::level::info, msg);
        }


        bool on_copy_resource(command_list* cmd_list, resource src, resource dst)
        {
            trace_copy(cmd_list, "copy_resource", src, dst);
            return false;
        }


        bool on_copy_texture_region(command_list* cmd_list, resource src, uint32_t, const subresource_box*, resource dst, uint32_t, const subresource_box*, filter_mode)
        {
            trace_copy(cmd_list, "copy_texture_region", src, dst);
            return false;
        }


        bool on_resolve_texture_region(command_list* cmd_list, resource src, uint32_t, const subresource_box*, resource dst, uint32_t, uint32_t, uint32_t, uint32_t, format)
        {
            trace_copy(cmd_list, "resolve_texture_region", src, dst);
            return false;
        }


        bool on_draw(command_list* cmd_list, uint32_t vertex_count, uint32_t instance_count, uint32_t, uint32_t)
        {
            EDAA_PROFILE(draws);
            return on_any_draw(cmd_list, "draw", vertex_count, instance_count, vertex_count == 3 && instance_count == 1,
                vertex_count == 4 && instance_count == 1);
        }


        bool on_draw_indexed(command_list* cmd_list, uint32_t index_count, uint32_t instance_count, uint32_t, int32_t, uint32_t)
        {
            EDAA_PROFILE(draws);
            return on_any_draw(cmd_list, "drawindexed", index_count, instance_count, false, false);
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

        // Diagnostics: status changes and stretches without an anti-aliasing
        // resolve (e.g. carrier jump or hyperspace transitions), with the time in the log.
        {
            static int logged = 0;

            if (s.resolve_ran) {
                if (s.frames_without_resolve >= 3 && logged < 400) {
                    ++logged;
                    char msg[160];
                    snprintf(msg, sizeof(msg), "EDAA status: anti-aliasing resolve back after %u frames without one", s.frames_without_resolve);
                    reshade::log::message(reshade::log::level::info, msg);
                }

                s.frames_without_resolve = 0;
            }
            else if (++s.frames_without_resolve == 3 && logged < 400) {
                ++logged;
                reshade::log::message(reshade::log::level::info, "EDAA status: no anti-aliasing resolve for 3 frames (no scene found)");
            }

            s.resolve_ran = false;

            if (s.motion_status != nullptr && std::strncmp(s.motion_status, s.logged_status, sizeof(s.logged_status) - 1) != 0 && logged < 400) {
                ++logged;
                std::strncpy(s.logged_status, s.motion_status, sizeof(s.logged_status) - 1);
                char msg[256];
                snprintf(msg, sizeof(msg), "EDAA status: %s", s.motion_status);
                reshade::log::message(reshade::log::level::info, msg);
            }
        }

        if (s.screenshot_mode && --s.screenshot_hold <= 0) {
            s.screenshot_mode = false;
            s.history_reset = true;
            reshade::log::message(reshade::log::level::info, "EDAA: high-resolution screenshot done, TAA resumed");
        }

        if (s.depth_candidate != 0) {
            use_depth_candidate(s);
        }

        s.exposure_seen = false;
        s.exposure_target = 0;
        const bool upscaled = s.upscale_pending; // this frame ran for the upscaler
        s.upscale_pending = false;
        s.upscale_attempted = false;
        s.fg_hud_pending = false;
        s.ldr_image = 0;
        s.hud_mask_done = false;
        s.hud_snapshot_stale = false;
        s.hud_learning = s.config.hud_learn && !s.current.hud_shader;
        s.hud_panel_count = 0;
        s.depth_candidate = s.color_candidate = 0;
        s.candidate_w = s.candidate_h = 0;

        s.last = s.current;
        s.current = {};
        s.frame++;

        // Upscaling: each display pixel must see as many samples as at native
        // resolution, so the sequence grows with the pixel area ratio
        // (NVIDIA: 8 x ratio^2 phases).
        s.jitter_phases = std::max(1, s.config.jitter_phases);

        s.upscaling_active = upscaled;
        s.last_biased_binds = s.biased_binds.exchange(0, std::memory_order_relaxed);
        float bias = 0;

        if (upscaled && s.render_w != 0 && s.render_w < s.back_buffer_w) {
            const float ratio = static_cast<float>(s.back_buffer_w) / static_cast<float>(s.render_w);
            s.jitter_phases = static_cast<int>(std::ceil(static_cast<float>(s.jitter_phases) * ratio * ratio));
            bias = s.config.upscale_mip_bias ? -std::log2(ratio) + s.config.mip_bias_extra : 0.0f;
        }

        // Scene samplers biased until the resolve (cleared there).
        s.mip_bias_value = bias;
        s.mip_bias_on = bias != 0 && !s.map_mode && !s.screenshot_mode;

        // Next Halton(2,3) sample, centred on the pixel: [-0.5, 0.5).
        const uint32_t phases = static_cast<uint32_t>(s.jitter_phases);
        const uint32_t index = static_cast<uint32_t>(s.frame % phases) + 1;
        s.jitter_x = (halton(index, 2) - 0.5f) * s.config.jitter_scale;
        s.jitter_y = (halton(index, 3) - 0.5f) * s.config.jitter_scale;
    }


    void cycle_pass(int step)
    {
        addon_state& s = state();
        s.cycler_on = true;
        cycled_shader skipped = {};
        int index, count;
        {
            const std::lock_guard<std::mutex> lock(cycler_mutex);
            count = static_cast<int>(cycler_list.size());
            // Positions -1 (none) to count - 1, wrapping around.
            cycler_index = step == 0 || count == 0 ? -1 : (cycler_index + 1 + step + 2 * (count + 1)) % (count + 1) - 1;
            index = cycler_index;

            if (index >= 0) {
                skipped = cycler_list[index];
            }
        }

        s.skip_ps = index >= 0 && !skipped.compute ? skipped.hash : 0;
        s.skip_cs = index >= 0 && skipped.compute ? skipped.hash : 0;

        char msg[128];
        snprintf(msg, sizeof(msg), "EDAA cycler: skipping %d / %d: %s %016llx", index + 1, count, skipped.compute ? "cs" : "ps",
            static_cast<unsigned long long>(skipped.hash));
        reshade::log::message(reshade::log::level::info, index >= 0 ? msg : "EDAA cycler: nothing skipped");
    }


    uint64_t cycler_status(char* text, size_t size)
    {
        const std::lock_guard<std::mutex> lock(cycler_mutex);
        const int count = static_cast<int>(cycler_list.size());

        if (cycler_index < 0 || cycler_index >= count) {
            snprintf(text, size, "Nothing skipped (%d shaders recorded)", count);
            return 0;
        }

        const cycled_shader& c = cycler_list[cycler_index];
        snprintf(text, size, "Skipping %d / %d: %s shader %016llx", cycler_index + 1, count, c.compute ? "compute" : "pixel",
            static_cast<unsigned long long>(c.hash));
        return c.hash;
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
        reshade::register_event<reshade::addon_event::dispatch>(on_dispatch);
        reshade::register_event<reshade::addon_event::draw_or_dispatch_indirect>(on_indirect);
        reshade::register_event<reshade::addon_event::copy_resource>(on_copy_resource);
        reshade::register_event<reshade::addon_event::push_descriptors>(on_push_descriptors_trace);
        reshade::register_event<reshade::addon_event::copy_texture_region>(on_copy_texture_region);
        reshade::register_event<reshade::addon_event::resolve_texture_region>(on_resolve_texture_region);
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
        reshade::unregister_event<reshade::addon_event::dispatch>(on_dispatch);
        reshade::unregister_event<reshade::addon_event::draw_or_dispatch_indirect>(on_indirect);
        reshade::unregister_event<reshade::addon_event::copy_resource>(on_copy_resource);
        reshade::unregister_event<reshade::addon_event::push_descriptors>(on_push_descriptors_trace);
        reshade::unregister_event<reshade::addon_event::copy_texture_region>(on_copy_texture_region);
        reshade::unregister_event<reshade::addon_event::resolve_texture_region>(on_resolve_texture_region);
    }
}
