// Texture mip bias when upscaling. Below the display resolution the game picks
// mip levels for the render size, so DLSS/XeSS reconstruct blurry textures.
// Upscalers expect the scene drawn with a negative LOD bias of
// log2(render / display): the mips of the display resolution.
//
// D3D11 has no global bias: each pixel shader sampler bound before the resolve is
// replaced by a copy with MipLODBias added (ReShade raises push_descriptors right
// after the game's PSSetSamplers, the copies are bound over it on the native
// context). Copies are cached per game sampler and per bias.
//
// Cost: when not upscaling, one atomic test per sampler bind (s.mip_bias_on, set
// at each frame start and cleared at the resolve); no lock, no device call.
//
// Lock order: no D3D11 call while holding samplers_mutex, which destroy_sampler
// takes from inside D3D11 (see the deadlock note in frame_tracking.cpp).

#include "events.hpp"
#include "profile.hpp"
#include "state.hpp"

#include <algorithm>
#include <cmath>
#include <d3d11.h>
#include <mutex>
#include <unordered_map>
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
            addon_state& s = state();

            // Fast exit first: this runs at every sampler bind of the game.
            if (!s.mip_bias_on.load(std::memory_order_relaxed)) {
                return;
            }

            EDAA_PROFILE(samplers);

            if (update.type != descriptor_type::sampler || (stages & shader_stage::pixel) == 0 || update.count == 0 ||
                !is_game_device(cmd_list->get_device())) {
                return;
            }

            const float bias = s.mip_bias_value.load(std::memory_order_relaxed);
            const auto* const handles = static_cast<const sampler*>(update.descriptors);
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
            s.biased_binds.fetch_add(1, std::memory_order_relaxed);
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
