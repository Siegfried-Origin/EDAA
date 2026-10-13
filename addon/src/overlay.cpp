// "EDAA" window in the ReShade overlay.
//
// Normal build: TAA on/off, status, reset to defaults, and an "Advanced
// settings" section with the tuning values. Developer build (CMake option
// EDAA_DEVELOPER, or a Debug build): also jitter, debug views, auto capture,
// camera decoding and diagnostics. Every change is saved to ReShade.ini.

#include "dlss.hpp"
#include "xess.hpp"
#include "state.hpp"

#include <cstdio>
#include <cstring>

namespace edaa
{
    namespace
    {
        // Tracks whether any widget changed a setting this frame.
        struct change_tracker
        {
            bool changed = false;


            bool operator()(bool widget_changed)
            {
                changed |= widget_changed;
                return widget_changed;
            }
        };


        // Upscaling choice, the render and display sizes and the modes the upscaler offers.
        void draw_upscaling(addon_state& s, change_tracker& edit)
        {
            ImGui::Separator();
            const int dlss_available = dlss::availability();
            const bool dlss_usable = dlss_available == 1 || EDAA_DEVELOPER;
            const char* dlss_label = dlss_available == 1 ? "NVIDIA DLSS"
                : dlss_available == 0                    ? "NVIDIA DLSS - not available on this system"
                                                         : "NVIDIA DLSS - checking...";
            const bool dlss_selected = s.config.upscaling == static_cast<int>(upscaler::dlss) && dlss_usable;

            if (ImGui::BeginCombo("Upscaling", dlss_selected ? "NVIDIA DLSS" : "Game (bilinear or FSR)")) {
                if (ImGui::Selectable("Game (bilinear or FSR)", !dlss_selected) && dlss_selected) {
                    s.config.upscaling = static_cast<int>(upscaler::game);
                    s.history_reset = true;
                    edit.changed = true;
                }

                if (ImGui::Selectable(dlss_label, dlss_selected, dlss_usable ? 0 : ImGuiSelectableFlags_Disabled) && !dlss_selected) {
                    s.config.upscaling = static_cast<int>(upscaler::dlss);
                    s.history_reset = true;
                    edit.changed = true;
                }

                ImGui::EndCombo();
            }

            const uint32_t render_w = s.render_w, display_w = s.back_buffer_w;
            const float scale = display_w != 0 ? static_cast<float>(render_w) / static_cast<float>(display_w) : 0.0f;
            ImGui::Text("Rendering resolution: %u x %u (%.2f x the display)", render_w, s.render_h, scale);
            ImGui::Text("Display resolution:   %u x %u", display_w, s.back_buffer_h);

            if (!dlss_selected) {
                return;
            }

            if (render_w == 0 || render_w >= display_w) {
                ImGui::TextUnformatted("Upscaling inactive: the game renders at the display resolution.");
                ImGui::TextUnformatted("Set the in-game supersampling below 1.0 (or an FSR mode) to use DLSS.");
                return;
            }

            ImGui::Text("Upscaling: %s", s.upscale_status);
            ImGui::Text("DLSS: %s", dlss::status().c_str());
            ImGui::Text("Jitter: %d phases", s.jitter_phases);

            const upscale_info info = dlss::info();

            if (info.modes.empty()) {
                return;
            }

            // Each mode's range comes from the API for this display size. The game
            // picks the render size: its supersampling should fall in the range.
            if (ImGui::BeginTable("modes", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) {
                ImGui::TableSetupColumn("DLSS mode");
                ImGui::TableSetupColumn("Optimal");
                ImGui::TableSetupColumn("Accepted render sizes");
                ImGui::TableSetupColumn("Scale");
                ImGui::TableHeadersRow();

                for (const upscale_mode& m : info.modes) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%s%s", m.name, std::strcmp(m.name, info.mode) == 0 ? " (in use)" : "");
                    ImGui::TableNextColumn();
                    ImGui::Text("%u x %u", m.optimal_w, m.optimal_h);
                    ImGui::TableNextColumn();
                    ImGui::Text("%u x %u - %u x %u", m.min_w, m.min_h, m.max_w, m.max_h);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.2f", static_cast<float>(m.optimal_w) / static_cast<float>(display_w));
                }

                ImGui::EndTable();
            }

            ImGui::TextUnformatted("Scale: the in-game supersampling closest to the mode's optimal size.");
        }


        void draw_main(addon_state& s, change_tracker& edit)
        {
            if (edit(ImGui::Checkbox("Enable anti-aliasing (Ctrl+Shift+T)", &s.config.taa_enabled))) {
                s.config.jitter_enabled = s.config.taa_enabled;
            }

            // Every method is listed; those that cannot run on this system are greyed
            // out (developer builds can still select them, to see why they fail).
            struct choice
            {
                aa_method method;
                const char* name;
                int availability; // -1 not checked yet, 0 no, 1 yes
            };
            const choice choices[] = {
                { aa_method::taa, "EDAA TAA", 1 },
                { aa_method::dlaa, "NVIDIA DLAA (DLSS at native resolution)", dlss::availability() },
                { aa_method::xess, "Intel XeSS (Native AA)", xess::availability() },
            };
            // A saved method that cannot run is replaced by the TAA (passes.cpp).
            const choice* shown = &choices[0];

            for (const choice& c : choices) {
                if (s.config.aa_method == static_cast<int>(c.method) && (c.availability == 1 || EDAA_DEVELOPER)) {
                    shown = &c;
                }
            }

            if (ImGui::BeginCombo("Method", shown->name)) {
                for (const choice& c : choices) {
                    const bool usable = c.availability == 1 || EDAA_DEVELOPER;
                    char label[96];
                    snprintf(label, sizeof(label), "%s%s", c.name,
                        c.availability == 1 ? "" : c.availability == 0 ? " - not available on this system"
                                                                       : " - checking...");

                    if (ImGui::Selectable(label, &c == shown, usable ? 0 : ImGuiSelectableFlags_Disabled) &&
                        s.config.aa_method != static_cast<int>(c.method)) {
                        s.config.aa_method = static_cast<int>(c.method);
                        s.history_reset = true;
                        edit.changed = true;
                    }
                }

                ImGui::EndCombo();
            }

            ImGui::Text("Status: %s", s.config.taa_enabled ? s.motion_status : "off");

            if (s.config.aa_method == static_cast<int>(aa_method::dlaa) && (EDAA_DEVELOPER || dlss::availability() == 1)) {
                ImGui::Text("DLSS: %s", dlss::status().c_str());
            }
            else if (s.config.aa_method == static_cast<int>(aa_method::xess) && (EDAA_DEVELOPER || xess::availability() == 1)) {
                ImGui::Text("XeSS: %s", xess::status().c_str());
            }

            draw_upscaling(s, edit);

            if (s.map_mode) {
                ImGui::Text("Map on screen: TAA paused (%s)", s.map_reason);
            }

            if (s.screenshot_mode) {
                ImGui::TextUnformatted("High-resolution screenshot: TAA paused");
            }

            if (ImGui::Button("Reset to defaults")) {
                s.config = settings{};
                edit.changed = true;
            }
        }


        void draw_advanced(addon_state& s, change_tracker& edit)
        {
            ImGui::TextUnformatted("History");
            edit(ImGui::SliderFloat("History weight", &s.config.taa_history_weight, 0.5f, 0.98f, "%.2f"));
            edit(ImGui::SliderFloat("History weight in motion", &s.config.taa_history_weight_motion, 0.3f, 0.95f, "%.2f"));
            edit(ImGui::SliderFloat("Min history weight (anti-flicker)", &s.config.taa_history_weight_min, 0.5f, 0.95f, "%.2f"));
            edit(ImGui::Checkbox("Anti-flicker (luminance weighting)", &s.config.taa_anti_flicker));
            ImGui::TextUnformatted("Clipping (lower = less ghosting, more flicker)");
            edit(ImGui::SliderFloat("Clip box in motion (std dev)", &s.config.taa_clip_gamma, 0.5f, 3.0f, "%.2f"));
            edit(ImGui::SliderFloat("Clip box when still (std dev)", &s.config.taa_clip_gamma_static, 0.5f, 5.0f, "%.2f"));
            ImGui::TextUnformatted("Sampling");
            edit(ImGui::Checkbox("Reconstruction filter (less flicker on thin highlights)", &s.config.taa_reconstruction));
            edit(ImGui::Checkbox("Unjitter current sample (softer, used when the filter is off)", &s.config.taa_unjitter));
            edit(ImGui::Checkbox("Soft screen edges (reuse edge history)", &s.config.taa_soft_offscreen));
            edit(ImGui::SliderFloat("HDR compression scale (higher = bright edges flicker less)", &s.config.taa_hdr_scale, 0.01f, 1000.0f, "%.2f", ImGuiSliderFlags_Logarithmic));
            ImGui::TextUnformatted("Scene");
            edit(ImGui::Checkbox("Resolve before the HUD pass (HUD sharp, not anti-aliased)", &s.config.taa_before_hud));
            edit(ImGui::Checkbox("Detect maps also by missing sun shadows", &s.config.map_detect_no_shadows));
            edit(ImGui::Combo("Vehicle mask", &s.config.mask_source, "Game stencil (player's vehicle)\0Distance to camera\0"));

            if (s.config.mask_source == 1) {
                edit(ImGui::SliderFloat("Vehicle mask distance (m)", &s.config.mask_distance, 0.0f, 20.0f, "%.1f"));
                edit(ImGui::Checkbox("No vehicle mask on foot", &s.config.mask_auto_off_on_foot));
            }
        }


#if EDAA_DEVELOPER
        void draw_camera(const addon_state& s)
        {
            ImGui::Text("Scene render size  %u x %u (from the G-buffer depth)", s.render_w, s.render_h);

            if (!s.camera.valid) {
                ImGui::TextUnformatted("Main camera: none last frame (no scene draw with a perspective camera).");
                return;
            }

            const camera_info& c = s.camera.info;
            ImGui::Text("Main camera: %u scene draws, %d camera(s) used by scene draws",
                s.camera.scene_draws, s.last.camera_count);
            ImGui::Text("Vertical FOV    %.3f deg", c.vfov_deg);
            ImGui::Text("Aspect          %.4f", c.aspect);
            ImGui::Text("Near plane      %g (reverse-Z, infinite far)", c.near_plane);
            ImGui::Text("Proj scale      sx %.5f  sy %.5f", c.sx, c.sy);
            ImGui::Text("Camera pos      %.3f, %.3f, %.3f", c.camera_pos[0], c.camera_pos[1], c.camera_pos[2]);
            ImGui::Text("Forward         %.4f, %.4f, %.4f", c.forward[0], c.forward[1], c.forward[2]);
            ImGui::Text("Frame mode      %s", to_string(s.mode));
            ImGui::Text("Camera speed    %.3f m/frame", s.camera_speed);
            ImGui::Text("Camera buffer updates last frame: %u perspective, %u orthographic",
                s.last.camera_updates[static_cast<int>(view_kind::perspective)],
                s.last.camera_updates[static_cast<int>(view_kind::orthographic)]);
        }


        void draw_objects(addon_state& s, change_tracker& edit)
        {
            edit(ImGui::Checkbox("Analyse instance motion (also on with the \"Moving objects\" debug view)", &s.config.object_analysis));

            if (!s.config.object_analysis && s.config.debug != static_cast<int>(debug_view::objects)) {
                return;
            }

            const object_stats& o = s.objects;
            ImGui::Text("Instance buffer Map/Unmap last frame: %u (read %.2f ms, match %.2f ms)", o.buffer_maps, o.read_ms, o.match_ms);
            ImGui::Text("Records %u: still %u, spinning %u, moving %u, player's vehicle %u, unmatched %u",
                o.records, o.still, o.spinning, o.moving, o.vehicle, o.unmatched);
            ImGui::Text("Moving objects on screen: %u, camera reprojection error max %.1f px, mean %.1f px",
                o.visible_moving, o.max_error_px, o.mean_error_px);
            ImGui::TextUnformatted("Debug view: rings = moving objects (yellow to red with the error), cyan = vehicle,");
            ImGui::TextUnformatted("green dots = some spinning objects (asteroids: they rotate in place, no ring);");
            ImGui::TextUnformatted("the line points to where the TAA history ghost of the object would be.");
        }


        void draw_developer(addon_state& s, change_tracker& edit)
        {
            ImGui::Text("Resolve point: %s", s.resolve_point);
            ImGui::Text("Last frame: %u G-buffer binds, %u draws into the HDR target, exposure downsample %s, HUD shader %s, bloom shader %s",
                s.last.gbuffer_binds, s.last.hdr_draws, s.last.exposure_downsample ? "yes" : "no",
                s.last.hud_shader ? "yes" : "no", s.last.bloom_shader ? "yes" : "no");
            ImGui::Text("Map on screen: %s", s.map_reason);
            ImGui::Text("On foot (first-person body drawn): %s", s.on_foot ? "yes" : "no");

            ImGui::Separator();
            ImGui::TextUnformatted("Jitter (Ctrl+Shift+J)");
            edit(ImGui::Checkbox("Enable sub-pixel jitter", &s.config.jitter_enabled));
            edit(ImGui::SliderInt("Halton phases", &s.config.jitter_phases, 1, 32));
            edit(ImGui::SliderFloat("Amplitude (pixels)", &s.config.jitter_scale, 1.0f, 16.0f, "%.1f"));
            edit(ImGui::Checkbox("Jitter decals, light volumes and glows (off: they z-fight and flicker)", &s.config.jitter_overlay_draws));
            edit(ImGui::Checkbox("DLSS/XeSS: negate the jitter offset (sign convention check)", &s.config.upscaler_jitter_flip));
            ImGui::Text("Current offset: %+.3f, %+.3f px", s.jitter_x, s.jitter_y);
            ImGui::Text("Last frame: %u of %u scene draws jittered", s.last.jittered_draws, s.last.scene_draws);

            if (s.config.taa_enabled && !s.config.jitter_enabled) {
                ImGui::TextUnformatted("Note: jitter is off, TAA only smooths (no sub-pixel detail).");
            }

            if (!s.config.taa_enabled && s.config.jitter_enabled) {
                ImGui::TextUnformatted("Compare mode: jitter on, TAA off (the image shimmers).");
            }

            ImGui::Separator();
            ImGui::TextUnformatted("Debug views (Ctrl+Shift+M)");
            edit(ImGui::Checkbox("Generate motion vectors", &s.config.motion_enabled));
            ImGui::Combo("Debug view", &s.config.debug, "Off\0Motion vectors\0Vehicle mask\0Depth\0History rejection\0Moving objects\0Temporal change (flicker)\0");
            edit(ImGui::SliderFloat("Colour saturation (px)", &s.config.debug_motion_pixels, 1.0f, 64.0f, "%.0f"));

            ImGui::Separator();
            ImGui::TextUnformatted("Auto capture (Ctrl+Shift+C): TAA on/off, jitter only, debug views as PNG");
            char path[1024];
            strncpy_s(path, s.capture_path.c_str(), _TRUNCATE);

            if (ImGui::InputText("Capture folder", path, sizeof(path))) {
                s.capture_path = path;
                reshade::set_config_value(nullptr, "EDAA", "CapturePath", static_cast<const char*>(path));
            }

            if (ImGui::Button("Capture now")) {
                s.capture_requested = true;
            }

            ImGui::Text("Capture: %s %s", s.capture_status, s.last_capture_folder.c_str());

            ImGui::Separator();

            if (ImGui::CollapsingHeader("Moving objects (instance buffer t33)")) {
                draw_objects(s, edit);
            }

            if (ImGui::CollapsingHeader("Camera")) {
                draw_camera(s);
            }

            if (ImGui::CollapsingHeader("Diagnostics")) {
                const frame_stats& f = s.last;
                ImGui::Text("Large cbuffer Maps: %u, UpdateSubresource: %u", f.large_cb_maps, f.large_cb_updates);
                ImGui::Text("Scene draws before any camera update: %u", f.scene_draws_no_camera);

                if (ImGui::Button("Dump/trace next frame to ReShade.log")) {
                    s.dump_requested = true;
                }

                ImGui::Text("Perspective updates last frame (first %d):", f.update_count);

                for (int i = 0; i < f.update_count; ++i) {
                    const camera_update& u = f.updates[i];
                    ImGui::PushID(i);

                    if (ImGui::TreeNode("update", "#%d %s: near %g, vfov %.2f, aspect %.4f", i, u.source,
                            u.info.near_plane, u.info.vfov_deg, u.info.aspect)) {
                        const char* names[11] = { "c189", "c196", "c197", "c198", "c199", "c270", "c271", "c272", "c273", "c274", "c275" };

                        for (int r = 0; r < 11; ++r) {
                            ImGui::Text("%s  % 12.6g % 12.6g % 12.6g % 12.6g", names[r], u.regs[r][0], u.regs[r][1], u.regs[r][2], u.regs[r][3]);
                        }

                        ImGui::TreePop();
                    }

                    ImGui::PopID();
                }
            }
        }
#endif
    }


    void draw_overlay(reshade::api::effect_runtime*)
    {
        addon_state& s = state();
        const std::lock_guard<std::recursive_mutex> lock(s.mutex);

        ImGui::Text("EDAA %s (build %s%s)", version, build_stamp, EDAA_DEVELOPER ? ", developer" : "");
        ImGui::Separator();

        change_tracker edit;
        draw_main(s, edit);

        if (ImGui::CollapsingHeader("Advanced settings")) {
            draw_advanced(s, edit);
        }

#if EDAA_DEVELOPER
        if (ImGui::CollapsingHeader("Developer", ImGuiTreeNodeFlags_DefaultOpen)) {
            draw_developer(s, edit);
        }
#endif

        ImGui::Separator();
        ImGui::TextDisabled("Fly safe, fly smooth - CMDR Siegfried-Origin");

        if (edit.changed) {
            save_settings(s.config);
        }
    }
}
