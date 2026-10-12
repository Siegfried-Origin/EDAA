// EDAA ReShade add-on entry point.

#include "events.hpp"
#include "state.hpp"

#include <Windows.h>

#include "build_stamp.h" // generated at every build

extern "C" __declspec(dllexport) const char* NAME = "EDAA";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Temporal anti-aliasing groundwork for Elite Dangerous: camera tracking, sub-pixel jitter, motion vectors.";

namespace edaa
{
    const char* const build_stamp = EDAA_BUILD_STAMP;
    const char* const version = EDAA_VERSION;


    addon_state& state()
    {
        static addon_state instance;
        return instance;
    }


    namespace
    {
        // Hotkeys: Ctrl+Shift+T toggles TAA (and jitter, saved). Developer builds
        // also: Ctrl+Shift+J jitter alone, Ctrl+Shift+M debug view, Ctrl+Shift+C capture.
        void on_reshade_present(reshade::api::effect_runtime* runtime)
        {
            if (!runtime->is_key_down(VK_CONTROL) || !runtime->is_key_down(VK_SHIFT)) {
                return;
            }

            addon_state& s = state();

            if (runtime->is_key_pressed('T')) {
                const std::lock_guard<std::recursive_mutex> lock(s.mutex);
                s.config.taa_enabled = !s.config.taa_enabled;
                s.config.jitter_enabled = s.config.taa_enabled; // TAA needs jitter, plain image does not
                save_settings(s.config);
                reshade::log::message(reshade::log::level::info,
                    s.config.taa_enabled ? "EDAA: TAA on" : "EDAA: TAA off");
            }

#if EDAA_DEVELOPER
            if (runtime->is_key_pressed('J')) {
                const std::lock_guard<std::recursive_mutex> lock(s.mutex);
                s.config.jitter_enabled = !s.config.jitter_enabled;
                save_settings(s.config);
            }

            if (runtime->is_key_pressed('C')) {
                const std::lock_guard<std::recursive_mutex> lock(s.mutex);
                s.capture_requested = true;
            }

            if (runtime->is_key_pressed('M')) {
                const std::lock_guard<std::recursive_mutex> lock(s.mutex);
                s.config.debug = (s.config.debug + 1) % 7;
            }
#endif
        }
    }
}


BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            if (!reshade::register_addon(module)) {
                return FALSE;
            }

            {
                edaa::addon_state& s = edaa::state();
                const std::lock_guard<std::recursive_mutex> lock(s.mutex);
                edaa::load_capture_path(s);
                edaa::load_settings(s.config);
            }
            edaa::register_camera_events();
            edaa::register_frame_events();
            edaa::register_object_events();
            edaa::register_pass_events();
            reshade::register_event<reshade::addon_event::reshade_present>(edaa::on_reshade_present);
            reshade::register_overlay("EDAA", edaa::draw_overlay);
            {
                char msg[160];
                snprintf(msg, sizeof(msg), "EDAA: loaded, version %s, build %s", edaa::version, edaa::build_stamp);
                reshade::log::message(reshade::log::level::info, msg);
            }
            break;
        case DLL_PROCESS_DETACH:
            reshade::unregister_overlay("EDAA", edaa::draw_overlay);
            reshade::unregister_event<reshade::addon_event::reshade_present>(edaa::on_reshade_present);
            edaa::unregister_pass_events();
            edaa::unregister_object_events();
            edaa::unregister_frame_events();
            edaa::unregister_camera_events();
            reshade::unregister_addon(module);
            break;
    }

    return TRUE;
}
