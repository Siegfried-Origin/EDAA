// Persistent settings: stored in ReShade.ini, section [EDAA], one key per
// field. The debug view is not saved (it always starts off).

#include "state.hpp"

#include <cstddef>

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
            EDAA_FIELD(taa_hdr_scale, real),
            EDAA_FIELD(taa_before_hud, boolean),
            EDAA_FIELD(map_detect_no_shadows, boolean),
            EDAA_FIELD(motion_enabled, boolean),
            EDAA_FIELD(mask_source, integer),
            EDAA_FIELD(mask_stencil_bits, integer),
            EDAA_FIELD(mask_distance, real),
            EDAA_FIELD(mask_auto_off_on_foot, boolean),
            EDAA_FIELD(debug_motion_pixels, real),
            EDAA_FIELD(object_analysis, boolean),
            EDAA_FIELD(jitter_overlay_draws, boolean),
            EDAA_FIELD(aa_method, integer),
            EDAA_FIELD(upscaler_jitter_flip, boolean),
            EDAA_FIELD(upscaling, integer),
        };
#undef EDAA_FIELD

        constexpr const char* section = "EDAA";
    }


    void load_settings(settings& config)
    {
        auto* const base = reinterpret_cast<char*>(&config);

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
                    reshade::get_config_value(nullptr, section, f.key, *static_cast<int*>(p));
                    break;
                case field_type::real:
                    reshade::get_config_value(nullptr, section, f.key, *static_cast<float*>(p));
                    break;
            }
        }

        config.debug = static_cast<int>(debug_view::off);
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
