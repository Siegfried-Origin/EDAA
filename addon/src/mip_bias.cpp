// Texture mip bias when upscaling. Below the display resolution the game picks
// mip levels for the render size, so DLSS reconstructs blurry textures. Upscalers
// expect the scene drawn with a negative LOD bias of log2(render / display): the
// mips of the display resolution.
//
// D3D11 has no global bias: each pixel shader sampler bound before the resolve is
// replaced by a copy with MipLODBias added (ReShade raises push_descriptors right
// after the game's PSSetSamplers, the copies are bound over it on the native
// context). Copies are cached per game sampler and per bias.
//
// Lock order: no D3D11 call while holding samplers_mutex, which destroy_sampler
// takes from inside D3D11 (see the deadlock note in frame_tracking.cpp).

#include "events.hpp"
#include "state.hpp"

#include <algorithm>
#include <cmath>
#include <d3d11.h>
#include <cstdio>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;
using namespace reshade::api;

namespace edaa
{
    namespace
    {
        struct biased_sampler
        {
            float bias = 0;
            ComPtr<ID3D11SamplerState> sampler;
        };

        std::mutex samplers_mutex;
        std::unordered_map<uint64_t, biased_sampler> samplers; // game sampler -> biased copy


#if EDAA_DEVELOPER
        // Developer builds: each game sampler's description, once, in ReShade.log
        // (does the game already bias its samplers when rendering below 1x?).
        std::mutex logged_mutex;
        std::unordered_set<uint64_t> logged;


        void log_sampler(uint64_t handle, float render_scale)
        {
            {
                const std::lock_guard<std::mutex> lock(logged_mutex);

                if (!logged.insert(handle).second) {
                    return;
                }
            }

            D3D11_SAMPLER_DESC d;
            reinterpret_cast<ID3D11SamplerState*>(handle)->GetDesc(&d);
            char text[192];
            snprintf(text, sizeof(text), "EDAA: game sampler %llx: filter 0x%x, aniso %u, MipLODBias %+.3f, MinLOD %.2f, MaxLOD %.2f (render scale %.2f)",
                static_cast<unsigned long long>(handle), static_cast<unsigned>(d.Filter), d.MaxAnisotropy, d.MipLODBias, d.MinLOD, d.MaxLOD,
                render_scale);
            reshade::log::message(reshade::log::level::info, text);
        }
#endif


        ID3D11SamplerState* find(uint64_t handle, float bias)
        {
            const std::lock_guard<std::mutex> lock(samplers_mutex);
            const auto it = samplers.find(handle);
            return it != samplers.end() && it->second.bias == bias ? it->second.sampler.Get() : nullptr;
        }


        ID3D11SamplerState* biased_copy(ID3D11Device* device, uint64_t handle, float bias)
        {
            if (ID3D11SamplerState* found = find(handle, bias)) {
                return found;
            }

            D3D11_SAMPLER_DESC desc;
            reinterpret_cast<ID3D11SamplerState*>(handle)->GetDesc(&desc);
            desc.MipLODBias = std::fmax(-16.0f, std::fmin(15.99f, desc.MipLODBias + bias));
            ComPtr<ID3D11SamplerState> copy;

            if (FAILED(device->CreateSamplerState(&desc, &copy))) {
                return nullptr;
            }

            biased_sampler old;
            {
                const std::lock_guard<std::mutex> lock(samplers_mutex);
                biased_sampler& entry = samplers[handle];
                old = std::move(entry); // released after unlocking
                entry = { bias, copy };
            }

            return copy.Get(); // kept alive by the cache
        }


        void on_push_descriptors(command_list* cmd_list, shader_stage stages, pipeline_layout, uint32_t, const descriptor_table_update& update)
        {
            if (update.type != descriptor_type::sampler || (stages & shader_stage::pixel) == 0 || update.count == 0 ||
                !is_game_device(cmd_list->get_device())) {
                return;
            }

            const auto* const handles = static_cast<const sampler*>(update.descriptors);
            float bias, render_scale;
            bool apply;
            {
                addon_state& s = state();
                const std::lock_guard<std::recursive_mutex> lock(s.mutex);
                bias = s.mip_bias;
                render_scale = s.back_buffer_w != 0 ? static_cast<float>(s.render_w) / static_cast<float>(s.back_buffer_w) : 0.0f;
                // Only the scene: what is drawn after the resolve is not upscaled with history.
                apply = bias != 0 && !s.resolved_this_frame && !s.map_mode && !s.screenshot_mode;

                if (!s.resolved_this_frame) {
                    s.current.sampler_binds++;
                    s.current.biased_binds += apply ? 1 : 0;
                }
            }

#if EDAA_DEVELOPER
            for (uint32_t i = 0; i < update.count; ++i) {
                if (handles[i].handle != 0) {
                    log_sampler(handles[i].handle, render_scale);
                }
            }
#else
            (void)render_scale;
#endif

            if (!apply) {
                return;
            }

            auto* const device = reinterpret_cast<ID3D11Device*>(cmd_list->get_device()->get_native());
            auto* const ctx = reinterpret_cast<ID3D11DeviceContext*>(cmd_list->get_native());
            ID3D11SamplerState* replaced[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
            const uint32_t count = std::min<uint32_t>(update.count, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT);

            for (uint32_t i = 0; i < count; ++i) {
                replaced[i] = handles[i].handle != 0 ? biased_copy(device, handles[i].handle, bias) : nullptr;

                if (replaced[i] == nullptr) {
                    replaced[i] = reinterpret_cast<ID3D11SamplerState*>(handles[i].handle); // keep the game's
                }
            }

            // The native context: no event raised again.
            ctx->PSSetSamplers(update.binding, count, replaced);
        }


        void on_destroy_sampler(device* device, sampler sampler)
        {
            if (!is_game_device(device)) {
                return;
            }

            biased_sampler old;
            {
                const std::lock_guard<std::mutex> lock(samplers_mutex);
                const auto it = samplers.find(sampler.handle);

                if (it == samplers.end()) {
                    return;
                }

                old = std::move(it->second);
                samplers.erase(it);
            }
            // 'old' releases the copy here, outside the lock.
        }


        void on_destroy_device(device* device)
        {
            if (!is_game_device(device)) {
                return;
            }

            std::unordered_map<uint64_t, biased_sampler> old;
            {
                const std::lock_guard<std::mutex> lock(samplers_mutex);
                old.swap(samplers);
            }
        }
    }


    void register_mip_bias_events()
    {
        reshade::register_event<reshade::addon_event::push_descriptors>(on_push_descriptors);
        reshade::register_event<reshade::addon_event::destroy_sampler>(on_destroy_sampler);
        reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
    }


    void unregister_mip_bias_events()
    {
        reshade::unregister_event<reshade::addon_event::push_descriptors>(on_push_descriptors);
        reshade::unregister_event<reshade::addon_event::destroy_sampler>(on_destroy_sampler);
        reshade::unregister_event<reshade::addon_event::destroy_device>(on_destroy_device);
        const std::lock_guard<std::mutex> lock(samplers_mutex);
        samplers.clear();
    }
}
