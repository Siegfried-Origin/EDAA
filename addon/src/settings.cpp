// Persistent settings: stored in ReShade.ini, section [EDAA], one key per
// field. The debug view is not saved (it always starts off).

#include "state.hpp"

#include <cstddef>
#include <cstring>

namespace edaa
{
    namespace
    {
        enum class field_type
        {
            boolean,
            integer,
            real
        };

        struct field
        {
            const char* key;
            field_type type;
            size_t offset;
        };

#define EDAA_FIELD(name, type) { #name, field_type::type, offsetof(settings, name) }
        const field fields[] = {
            EDAA_FIELD(taa_enabled, boolean),
            EDAA_FIELD(jitter_enabled, boolean),
            EDAA_FIELD(jitter_phases, integer),
            EDAA_FIELD(jitter_scale, real),
            EDAA_FIELD(taa_history_weight, real),
            EDAA_FIELD(taa_history_weight_motion, real),
            EDAA_FIELD(taa_clip_gamma, real),
            EDAA_FIELD(taa_clip_gamma_static, real),
            EDAA_FIELD(taa_unjitter, boolean),
            EDAA_FIELD(taa_reconstruction, boolean),
            EDAA_FIELD(taa_anti_flicker, boolean),
            EDAA_FIELD(taa_history_weight_min, real),
            EDAA_FIELD(taa_soft_offscreen, boolean),
            EDAA_FIELD(taa_sharp, boolean),
            EDAA_FIELD(spec_stable, boolean),
            EDAA_FIELD(spec_stable_strength, real),
            EDAA_FIELD(spec_stable_contrast, real),
            EDAA_FIELD(spec_stable_clip, real),
            EDAA_FIELD(taa_hdr_scale, real),
            EDAA_FIELD(taa_before_hud, boolean),
            EDAA_FIELD(hud_learn, boolean),
            EDAA_FIELD(hud_motion, boolean),
            EDAA_FIELD(aa_ship_steady, boolean),
            EDAA_FIELD(aa_ship_distance, real),
            EDAA_FIELD(map_detect_no_shadows, boolean),
            EDAA_FIELD(motion_enabled, boolean),
            EDAA_FIELD(mask_source, integer),
            EDAA_FIELD(mask_stencil_bits, integer),
            EDAA_FIELD(mask_distance, real),
            EDAA_FIELD(mask_auto_off_on_foot, boolean),
            EDAA_FIELD(debug_motion_pixels, real),
            EDAA_FIELD(object_analysis, boolean),
            EDAA_FIELD(jitter_overlay_draws, boolean),
            EDAA_FIELD(fix_sun_shadows, boolean),
            EDAA_FIELD(vehicle_motion, boolean),
            EDAA_FIELD(aa_method, integer),
            EDAA_FIELD(game_upscale, boolean),
            EDAA_FIELD(upscale_hud_mask, boolean),
            EDAA_FIELD(upscale_mip_bias, boolean),
            EDAA_FIELD(record_full_frame, boolean),
            EDAA_FIELD(record_max_mb, integer),
            EDAA_FIELD(record_delay_s, real),
            EDAA_FIELD(upscale_sharpness, real),
            EDAA_FIELD(dlss_sharpness, real),
            EDAA_FIELD(dlss_preset, integer),
            EDAA_FIELD(dlss_exposure_mode, integer),
            EDAA_FIELD(dlss_exposure, real),
            EDAA_FIELD(dlss_exposure_scale, real),
            EDAA_FIELD(dlss_reactive_mask, boolean),
            EDAA_FIELD(dlss_reactive_strength, real),
            EDAA_FIELD(dlss_reactive_threshold, real),
            EDAA_FIELD(fg_present_test, boolean),
            EDAA_FIELD(fg_factory, integer),
            EDAA_FIELD(fg_generate, boolean),
            EDAA_FIELD(fg_hud_protect, boolean),
            EDAA_FIELD(fg_fps_limit, integer),
            EDAA_FIELD(gpu_profiler, boolean),
        };
#undef EDAA_FIELD

        constexpr const char* section = "EDAA";
    }


    bool load_settings(settings& config)
    {
        auto* const base = reinterpret_cast<char*>(&config);
        bool method_saved = false;

        for (const field& f : fields) {
            void* const p = base + f.offset;

            switch (f.type) {
                case field_type::boolean:
                    {
                        int v;

                        if (reshade::get_config_value(nullptr, section, f.key, v)) {
                            *static_cast<bool*>(p) = v != 0;
                        }
                        break;
                    }
                case field_type::integer:
                    if (reshade::get_config_value(nullptr, section, f.key, *static_cast<int*>(p)) && std::strcmp(f.key, "aa_method") == 0) {
                        method_saved = true;
                    }
                    break;
                case field_type::real:
                    reshade::get_config_value(nullptr, section, f.key, *static_cast<float*>(p));
                    break;
            }
        }

        config.debug = static_cast<int>(debug_view::off);

        // Methods this build does not have (3 and up): the TAA.
        if (config.aa_method < 0 || config.aa_method > static_cast<int>(aa_method::xess)) {
            config.aa_method = static_cast<int>(aa_method::taa);
        }
#if EDAA_VENDOR_AMD
        // AMD build: no DLSS (docs/vendors.md).
        if (config.aa_method == static_cast<int>(aa_method::dlaa)) {
            config.aa_method = static_cast<int>(aa_method::taa);
        }
#endif
        sync_upscaler(config);
        return method_saved;
    }


    void save_settings(const settings& config)
    {
        const auto* const base = reinterpret_cast<const char*>(&config);

        for (const field& f : fields) {
            const void* const p = base + f.offset;

            switch (f.type) {
                case field_type::boolean:
                    reshade::set_config_value(nullptr, section, f.key, *static_cast<const bool*>(p) ? 1 : 0);
                    break;
                case field_type::integer:
                    reshade::set_config_value(nullptr, section, f.key, *static_cast<const int*>(p));
                    break;
                case field_type::real:
                    reshade::set_config_value(nullptr, section, f.key, *static_cast<const float*>(p));
                    break;
            }
        }
    }
}
