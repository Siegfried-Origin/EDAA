// "EDAA" window in the ReShade overlay.
//
// Player build: what a player needs or likes to change (method, model,
// sharpness, frame generation, a few HUD options) and one status line each.
// Developer build (CMake option EDAA_DEVELOPER, or a Debug build): also the
// TAA tuning, masks and debug views, GPU timings, jitter, auto capture,
// camera decoding and diagnostics. Hidden values keep their ReShade.ini value;
// "Reset to defaults" resets them too. Every change is saved to ReShade.ini.

#include "dlss.hpp"
#include "fg.hpp"
#include "gpu_profiler.hpp"
#include "xess.hpp"
#include "state.hpp"
#include "recorder.hpp"
#include "exposure.hpp"

#include <cstdio>
#include <cstdlib>
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


        // Render and display sizes, and the upscaling of the chosen method below 1x.
        void draw_upscaling(addon_state& s, change_tracker& edit)
        {
            ImGui::Separator();
            const uint32_t render_w = s.render_w, display_w = s.back_buffer_w;
            const float scale = display_w != 0 ? static_cast<float>(render_w) / static_cast<float>(display_w) : 0.0f;
            ImGui::Text("Resolution: %u x %u -> %u x %u (%.2f x)", render_w, s.render_h, display_w, s.back_buffer_h, scale);

            if (render_w == 0 || render_w >= display_w) {
                ImGui::TextDisabled("Anti-aliasing only. In-game supersampling below 1.0 upscales with the same method.");
                return;
            }

            const auto used = static_cast<upscaler>(s.upscaler_used);

            if (s.config.game_upscale) {
                ImGui::TextUnformatted("Upscaling: by the game, after the anti-aliasing (Advanced settings).");
                return;
            }

            const bool use_xess = used == upscaler::xess;
            ImGui::Text("Upscaling (%s): %s", use_xess ? "XeSS" : used == upscaler::taa ? "EDAA TAA" : "DLSS",
                s.upscale_status);
            edit(ImGui::Checkbox("Cockpit HUD without trails", &s.config.upscale_hud_mask));

            if (used == upscaler::taa) {
                edit(ImGui::SliderFloat("Sharpness", &s.config.upscale_sharpness, 0.0f, 1.0f, "%.2f"));
            }

#if EDAA_DEVELOPER
            edit(ImGui::Checkbox("Texture mip bias (display resolution texture detail)", &s.config.upscale_mip_bias));
            ImGui::Text("Jitter: %d phases, texture LOD bias %+.2f", s.jitter_phases, s.mip_bias_value.load());

            if (used == upscaler::taa) {
                return;
            }

            const upscale_info info = use_xess ? xess::info() : dlss::info();

            if (info.modes.empty()) {
                return;
            }

            // Each mode's range comes from the API for this display size. The game
            // picks the render size: its supersampling should fall in the range.
            if (ImGui::BeginTable("modes", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) {
                ImGui::TableSetupColumn(use_xess ? "XeSS preset" : "DLSS mode");
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
#endif
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
#if !EDAA_VENDOR_AMD
                { aa_method::dlaa, "NVIDIA DLSS", dlss::availability() },
#endif
                { aa_method::xess, "Intel XeSS", xess::availability() },
            };
            // A saved method that cannot run is replaced by the TAA (passes.cpp).
            // The same choice sets the upscaler used below 1x (sync_upscaler).
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

                // Model (render preset hint). Values are NVSDK_NGX_DLSS_Hint_Render_Preset.
                struct preset_choice
                {
                    int value;
                    const char* label;
                };
                static const preset_choice presets[] = {
                    { 0, "Default (NVIDIA: K for DLAA)" },
                    { 11, "K - transformer, best quality" },
                    { 10, "J - transformer, less ghosting, more flicker" },
                    { 13, "M - 2nd gen transformer (Performance default)" },
                    { 12, "L - 2nd gen transformer (Ultra Performance default)" },
                };

                const char* current = presets[0].label;

                for (const preset_choice& p : presets) {
                    current = p.value == s.config.dlss_preset ? p.label : current;
                }

                if (ImGui::BeginCombo("DLSS model", current)) {
                    for (const preset_choice& p : presets) {
                        if (ImGui::Selectable(p.label, p.value == s.config.dlss_preset) && p.value != s.config.dlss_preset) {
                            s.config.dlss_preset = p.value;
                            edit.changed = true;
                        }
                    }

                    ImGui::EndCombo();
                }

                // Exposure DLAA works with (recreates the feature when it changes).
                edit(ImGui::Combo("Exposure", &s.config.dlss_exposure_mode, "Estimated by DLSS\0Fixed\0From Elite (experimental)\0"));

                if (s.config.dlss_exposure_mode == 1 || s.config.dlss_exposure_mode == 2) {
                    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
                    edit(ImGui::SliderFloat(s.config.dlss_exposure_mode == 1 ? "Exposure value" : "Exposure value when not found", &s.config.dlss_exposure, 0.1f,
                        1000.0f, "%.1f", ImGuiSliderFlags_Logarithmic));
                }

#if EDAA_DEVELOPER
                if (s.config.dlss_exposure_mode == 2) {
                    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
                    edit(ImGui::SliderFloat("Elite exposure scale", &s.config.dlss_exposure_scale, 0.1f, 10.0f, "%.2f", ImGuiSliderFlags_Logarithmic));
                    ImGui::SameLine();
                    ImGui::Text("now %.2f", exposure::last_value());
                }
#endif

                // Sharpening of the DLAA output (ps_sharpen_hdr), off at 0.
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
                edit(ImGui::SliderFloat("Sharpening (0 = off)##dlss", &s.config.dlss_sharpness, 0.0f, 1.0f, "%.2f"));

                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Gives DLAA's edges and fine detail back some contrast.\n0.3 is a good start; too much makes edges harsh and dark areas grainy.\nHUD and holograms are not sharpened.");
                }

                // Reactive mask: the current frame where transparents were drawn.
                edit(ImGui::Checkbox("Reactive mask (particles, glass, glows: no trails)", &s.config.dlss_reactive_mask));

#if EDAA_DEVELOPER
                if (s.config.dlss_reactive_mask) {
                    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
                    edit(ImGui::SliderFloat("Reactive mask strength", &s.config.dlss_reactive_strength, 0.0f, 2.0f, "%.2f"));
                    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
                    edit(ImGui::SliderFloat("Reactive mask threshold (ignore haze)", &s.config.dlss_reactive_threshold, 0.0f, 0.95f, "%.2f"));
                    bool show = s.config.debug == static_cast<int>(debug_view::reactive);

                    // Debug view, not saved (the debug view always starts off).
                    if (ImGui::Checkbox("Show reactive mask (yellow)", &show)) {
                        s.config.debug = show ? static_cast<int>(debug_view::reactive) : static_cast<int>(debug_view::off);
                    }
                }
#endif
            }
            else if (s.config.aa_method == static_cast<int>(aa_method::xess) && (EDAA_DEVELOPER || xess::availability() == 1)) {
                ImGui::Text("XeSS: %s", xess::status().c_str());
            }

            // Own ship: the outer hull and nose carry no stencil bit and got the world's motion.
            edit(ImGui::Checkbox("Keep own ship steady (sharp hull lines when turning)", &s.config.aa_ship_steady));

            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Finds the own ship while flying (moves with the camera) and gives its hull the ship's motion.\n"
                                  "Every method, frame generation included. If something outside smears, turn it off.");
            }

            if (s.config.aa_ship_steady) {
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
                edit(ImGui::SliderFloat("Own ship up to (m)##aa", &s.config.aa_ship_distance, 0.0f, 150.0f, "%.0f"));
                ImGui::SameLine();
                ImGui::TextDisabled("0 = found automatically");
            }

            // Glint stabiliser (ps_spec_stable) after DLAA / XeSS / FSR at native resolution.
            if (s.config.aa_method != static_cast<int>(aa_method::taa)) {
                edit(ImGui::Checkbox("Calm blinking glints (after DLSS / XeSS / FSR)", &s.config.spec_stable));

                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Glints on metal and edges that blink from frame to frame are blended with the last frame.\n"
                                      "Only on glints, limited to what is around them. Stars, engines and lights are left alone.\n"
                                      "Not with upscaling.");
                }

                if (s.config.spec_stable) {
                    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
                    edit(ImGui::SliderFloat("Glint calming strength", &s.config.spec_stable_strength, 0.0f, 1.0f, "%.2f"));
#if EDAA_DEVELOPER
                    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
                    edit(ImGui::SliderFloat("Glint contrast", &s.config.spec_stable_contrast, 0.0f, 0.3f, "%.3f"));
                    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
                    edit(ImGui::SliderFloat("Glint clip box (std dev)", &s.config.spec_stable_clip, 0.0f, 4.0f, "%.2f"));
#endif
                }
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
                s.pick_method = true; // DLSS, else XeSS, else TAA
                edit.changed = true;
            }
        }


        void draw_advanced(addon_state& s, change_tracker& edit)
        {
            const bool below = s.render_w != 0 && s.render_w < s.back_buffer_w;
            edit(ImGui::Checkbox("Resolve before the HUD pass (HUD sharp, not anti-aliased)", &s.config.taa_before_hud));

            if (!s.config.taa_before_hud && !s.last.bloom_shader) {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "No bloom pass found: resolving before the HUD anyway");
            }

            edit(ImGui::Checkbox("Find the cockpit HUD by its panel textures (needed with EDHM)", &s.config.hud_learn));
            edit(ImGui::Checkbox("Cockpit HUD moves with the cockpit (sharp panels when upscaling)", &s.config.hud_motion));

            if (below || s.config.game_upscale) {
                edit(ImGui::Checkbox("Below 1x, let the game upscale (bilinear or FSR) after the anti-aliasing", &s.config.game_upscale));
            }
        }


#if EDAA_DEVELOPER
        // TAA tuning and game fixes: their defaults fit, developer build only.
        void draw_tuning(addon_state& s, change_tracker& edit)
        {
            // Only what applies to what runs now: the TAA tuning when EDAA TAA
            // runs (its upscaling uses part of it), DLSS/XeSS have none.
            const bool neural = (s.config.aa_method == static_cast<int>(aa_method::dlaa) && dlss::availability() == 1) ||
                (s.config.aa_method == static_cast<int>(aa_method::xess) && xess::availability() == 1);
            const bool taau = s.upscaling_active && s.upscaler_used == static_cast<int>(upscaler::taa);
            const bool taa = !s.upscaling_active && !neural;

            if (taa || taau) {
                ImGui::TextUnformatted("History");
                edit(ImGui::SliderFloat("History weight", &s.config.taa_history_weight, 0.5f, 0.98f, "%.2f"));
                edit(ImGui::SliderFloat("History weight in motion", &s.config.taa_history_weight_motion, 0.3f, 0.95f, "%.2f"));
                edit(ImGui::SliderFloat("Min history weight (anti-flicker)", &s.config.taa_history_weight_min, 0.5f, 0.95f, "%.2f"));
                edit(ImGui::Checkbox("Anti-flicker (luminance weighting)", &s.config.taa_anti_flicker));
                ImGui::TextUnformatted("Clipping (lower = less ghosting, more flicker)");
                edit(ImGui::SliderFloat("Clip box in motion (std dev)", &s.config.taa_clip_gamma, 0.5f, 3.0f, "%.2f"));
                edit(ImGui::SliderFloat("Clip box when still (std dev)", &s.config.taa_clip_gamma_static, 0.5f, 5.0f, "%.2f"));
            }

            // Native resolution TAA only (the upscaling works on the tonemapped image).
            if (taa) {
                ImGui::TextUnformatted("Sampling");
                edit(ImGui::Checkbox("Reconstruction filter (less flicker on thin highlights)", &s.config.taa_reconstruction));

                if (!s.config.taa_reconstruction) {
                    edit(ImGui::Checkbox("Unjitter current sample (softer)", &s.config.taa_unjitter));
                }

                edit(ImGui::Checkbox("Soft screen edges (reuse edge history)", &s.config.taa_soft_offscreen));
                edit(ImGui::Checkbox("Sharper TAA", &s.config.taa_sharp));

                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Samples close to the pixel centre count more and the result is sharpened a little.\nSlightly sharper, slightly more flicker.");
                }
                edit(ImGui::SliderFloat("HDR compression scale (higher = bright edges flicker less)", &s.config.taa_hdr_scale, 0.01f, 1000.0f, "%.2f", ImGuiSliderFlags_Logarithmic));
            }

            if (!taa && !taau) {
                ImGui::TextDisabled("The TAA tuning applies to EDAA TAA only.");
            }

            ImGui::TextUnformatted("Scene");
            edit(ImGui::Checkbox("Detect maps also by missing sun shadows", &s.config.map_detect_no_shadows));
            edit(ImGui::Combo("Vehicle mask", &s.config.mask_source, "Game stencil (player's vehicle)\0Distance to camera\0"));

            if (s.config.mask_source == 1) {
                edit(ImGui::SliderFloat("Vehicle mask distance (m)", &s.config.mask_distance, 0.0f, 20.0f, "%.1f"));
                edit(ImGui::Checkbox("No vehicle mask on foot", &s.config.mask_auto_off_on_foot));
            }

            edit(ImGui::Checkbox("Vehicle motion (cockpit follows head movements)", &s.config.vehicle_motion));
            edit(ImGui::Checkbox("Lighting follows the jitter (stable shadow edges)", &s.config.fix_sun_shadows));
        }


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
            // Draw hooks include the passes they run; Present is counted on the next frame.
            ImGui::Text("Frame %.2f ms. EDAA CPU: draws %.2f (passes %.2f), binds %.2f, maps %.2f, present %.2f, mip bias %.2f ms",
                s.frame_ms, s.cpu_ms[0], s.cpu_ms[3], s.cpu_ms[1], s.cpu_ms[2], s.cpu_ms[4], s.cpu_ms[5]);
            ImGui::Text("Mip bias: %u sampler binds biased last frame", s.last_biased_binds);
            edit(ImGui::SliderFloat("Upscaling: extra mip bias", &s.config.mip_bias_extra, -4.0f, 2.0f, "%+.1f"));
            ImGui::Text("Last frame: %u G-buffer binds, %u draws into the HDR target, exposure downsample %s, HUD shader %s, bloom shader %s",
                s.last.gbuffer_binds, s.last.hdr_draws, s.last.exposure_downsample ? "yes" : "no",
                s.last.hud_shader ? "yes" : "no", s.last.bloom_shader ? "yes" : "no");
            ImGui::Text("Map on screen: %s", s.map_reason);
            ImGui::Text("On foot (first-person body drawn): %s", s.on_foot ? "yes" : "no");
            const char* vehicle = !s.config.vehicle_motion ? "off"
                : s.on_foot                                ? "not used on foot"
                : s.vehicle_known                          ? "found"
                                                           : "not found (vehicle pixels get no motion)";
            ImGui::Text("Vehicle motion: %s", vehicle);
            ImGui::Text("Jittered view rays last frame: %u sun shadow mask draws, %u tiled lighting, %u occlusion dispatches",
                s.last.shadow_fixes, s.last.lighting_fixes, s.last.occlusion_fixes);

            ImGui::Separator();
            ImGui::TextUnformatted("Jitter (Ctrl+Shift+J)");
            edit(ImGui::Checkbox("Enable sub-pixel jitter", &s.config.jitter_enabled));
            edit(ImGui::SliderInt("Halton phases", &s.config.jitter_phases, 1, 32));
            edit(ImGui::SliderFloat("Amplitude (pixels)", &s.config.jitter_scale, 1.0f, 16.0f, "%.1f"));
            edit(ImGui::Checkbox("Jitter decals, light volumes and glows (off: they z-fight and flicker)", &s.config.jitter_overlay_draws));
            edit(ImGui::Checkbox("DLSS/XeSS: negate the jitter offset (sign convention check)", &s.config.upscaler_jitter_flip));
            bool flush = s.xess_flush;

            if (ImGui::Checkbox("XeSS: flush D3D11 before XeSS (frame pacing test)", &flush)) {
                s.xess_flush = flush;
            }

            ImGui::Text("Current offset: %+.3f, %+.3f px", s.jitter_x, s.jitter_y);

            // Switches one pass off to find what flickers (hashes as in the trace).
            static char skip_text[17] = "";

            if (ImGui::InputText("Skip draws with pixel shader (hex hash)", skip_text, sizeof(skip_text), ImGuiInputTextFlags_CharsHexadecimal)) {
                s.skip_ps = std::strtoull(skip_text, nullptr, 16);
            }

            // Pass cycler: switches the game's shaders off one at a time.
            ImGui::TextUnformatted("Pass cycler (Ctrl+Shift+Page Down / Page Up, Home: none)");

            if (ImGui::Button("Previous")) {
                cycle_pass(-1);
            }

            ImGui::SameLine();

            if (ImGui::Button("Next")) {
                cycle_pass(1);
            }

            ImGui::SameLine();

            if (ImGui::Button("None")) {
                cycle_pass(0);
            }

            char cycler_text[96];
            const uint64_t skipped = cycler_status(cycler_text, sizeof(cycler_text));
            ImGui::SameLine();

            if (ImGui::Button("Copy hash") && skipped != 0) {
                char hash[17];
                snprintf(hash, sizeof(hash), "%016llx", static_cast<unsigned long long>(skipped));
                ImGui::SetClipboardText(hash);
            }

            ImGui::TextUnformatted(cycler_text);

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
            ImGui::Combo("Debug view", &s.config.debug, "Off\0Motion vectors\0Vehicle mask\0Depth\0History rejection\0Moving objects\0Temporal change (flicker)\0HUD mask (upscaling)\0Reactive mask (DLSS)\0Ship auto (green: detected, orange: stencil only)\0");
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

            ImGui::Separator();
            ImGui::TextUnformatted("Recording for replay (Ctrl+Shift+R): AA inputs into <capture folder>\\recordings");

            if (ImGui::Button(recorder::active() ? "Stop recording" : "Start recording")) {
                recorder::toggle();
            }

            ImGui::SameLine();
            ImGui::TextUnformatted(recorder::status().c_str());
            edit(ImGui::Checkbox("Whole frame (else the 1600x900 centre)##rec", &s.config.record_full_frame));
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
            edit(ImGui::SliderInt("Memory limit (MB)##rec", &s.config.record_max_mb, 256, 32768, "%d", ImGuiSliderFlags_Logarithmic));
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
            edit(ImGui::SliderFloat("Start delay (s)##rec", &s.config.record_delay_s, 0.0f, 10.0f, "%.0f"));

            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Time between Ctrl+Shift+R and the start, to get moving first. Pressing again during the countdown cancels.");
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


    namespace
    {
        // Frame generation (fg_present.cpp): NVIDIA DLSS-G, on/off, frame limit, status.
        void draw_frame_generation(addon_state& s, change_tracker& edit)
        {
            const fg::status st = fg::get_status();

            if (edit(ImGui::Checkbox("Frame Generation (2x)", &s.config.fg_generate)) && s.config.fg_generate) {
                s.config.fg_present_test = true; // the D3D12 swap chain it needs
            }

            edit(ImGui::Checkbox("Keep the cockpit HUD steady (no smearing in generated frames)", &s.config.fg_hud_protect));

            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
            edit(ImGui::SliderInt("Frame limit (displayed FPS, 0 = off)", &s.config.fg_fps_limit, 0, 360));
            ImGui::TextDisabled("Use this limit only (no external limiter): set it just below the refresh rate, e.g. 140 at 144 Hz.");

            if (s.config.fg_generate && !st.streamline) {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), st.error != nullptr ? "Stopped: %s" : "Restart the game to start frame generation.",
                    st.error != nullptr ? st.error : "");
                return;
            }

            if (!st.d3d12_path) {
                ImGui::TextDisabled("Status: off");
                return;
            }

            if (!s.config.fg_generate) {
                ImGui::Text("Status: off (%.0f FPS)", st.real_fps);

#if EDAA_DEVELOPER
                // Players: the app's quick choice does this when the frame generation is off.
                if (s.config.fg_present_test) {
                    if (ImGui::Button("Present through D3D11 again at the next game start")) {
                        s.config.fg_present_test = false;
                        edit(true);
                    }
                }
#endif
                return;
            }

            if (st.generating) {
                ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Status: running, %.0f real -> %.0f FPS displayed", st.real_fps, st.shown_fps);
            }
            else if (st.background) {
                ImGui::Text("Status: paused (game in the background), %.0f FPS", st.real_fps);
            }
            else if (st.waiting) {
                ImGui::Text("Status: paused (menu, map or loading), %.0f FPS", st.real_fps);
            }
            else {
                ImGui::Text("Status: starting, %.0f FPS", st.real_fps);
            }

            ImGui::Text("Reflex low latency: %s", st.reflex ? "on" : "off");

#if EDAA_DEVELOPER
            if (s.config.fg_hud_protect && st.generating) {
                ImGui::Text("HUD protection: %s", st.hud_protected ? "active" : "no HUD found this frame");
            }

            if (st.dlssg_status != 0) {
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "DLSS-G reports problem flags 0x%X (see ReShade.log)", st.dlssg_status);
            }
#endif
        }
    }


#if EDAA_DEVELOPER
    namespace
    {
        // GPU time of EDAA's passes on the game's D3D11 queue (gpu_profiler.cpp).
        void draw_gpu_timings(addon_state& s, change_tracker& edit)
        {
            edit(ImGui::Checkbox("Measure (small cost; log line every 10 s)", &s.config.gpu_profiler));

            if (!s.config.gpu_profiler) {
                return;
            }

            const gpu_profiler::results r = gpu_profiler::last();
            ImGui::Text("Test scene: %c (Ctrl+Shift+N: next)", gpu_profiler::scene());

            if (!r.valid) {
                ImGui::TextUnformatted("Waiting for results...");
                return;
            }

            ImGui::Text("Frame (GPU timeline, idle included): %.2f ms, P99 %.2f ms, %u frames in 2 s", r.frame_mean, r.frame_p99, r.frames);

            if (ImGui::BeginTable("gpu_timings", 3, ImGuiTableFlags_SizingFixedFit)) {
                ImGui::TableSetupColumn("Pass");
                ImGui::TableSetupColumn("Mean ms");
                ImGui::TableSetupColumn("P99 ms");
                ImGui::TableHeadersRow();

                for (int i = 0; i < gpu_profiler::count; ++i) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(gpu_profiler::section_name(static_cast<gpu_profiler::section>(i)));
                    ImGui::TableNextColumn();
                    ImGui::Text("%.2f", r.mean[i]);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.2f", r.p99[i]);
                }

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted("EDAA total");
                ImGui::TableNextColumn();
                ImGui::Text("%.2f", r.edaa_mean);
                ImGui::TableNextColumn();
                ImGui::Text("%.2f", r.edaa_p99);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted("Elite + rest");
                ImGui::TableNextColumn();
                ImGui::Text("%.2f", r.elite_mean);
                ImGui::EndTable();
            }

            ImGui::Text("Frames dropped (results not ready): %u", r.skipped);
            ImGui::TextDisabled("Not measured: DLSS-G generation (D3D12 queue). GITC Uplift NR (before upscaling) shows up in dlss.");
        }
    }
#endif


    void draw_recorder_osd(reshade::api::effect_runtime*)
    {
#if EDAA_DEVELOPER
        bool alert = false;
        const std::string line = recorder::osd(alert);

        if (line.empty()) {
            return;
        }

        ImGui::SetNextWindowPos(ImVec2(24, 24), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.7f);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove;

        if (ImGui::Begin("##edaa_recorder_osd", nullptr, flags)) {
            ImGui::TextColored(alert ? ImVec4(1.0f, 0.25f, 0.2f, 1.0f) : ImVec4(0.9f, 0.9f, 0.9f, 1.0f), "%s", line.c_str());
        }

        ImGui::End();
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

        if (ImGui::CollapsingHeader("Frame Generation", ImGuiTreeNodeFlags_DefaultOpen)) {
            draw_frame_generation(s, edit);
        }

        if (ImGui::CollapsingHeader("Advanced settings")) {
            draw_advanced(s, edit);
        }

#if EDAA_DEVELOPER
        if (ImGui::CollapsingHeader("TAA tuning and game fixes")) {
            draw_tuning(s, edit);
        }

        if (ImGui::CollapsingHeader("GPU timings")) {
            draw_gpu_timings(s, edit);
        }

        if (ImGui::CollapsingHeader("Developer", ImGuiTreeNodeFlags_DefaultOpen)) {
            draw_developer(s, edit);
        }
#endif

        ImGui::Separator();
        ImGui::TextDisabled("Fly safe, fly smooth - CMDR Siegfried-Origin");

        if (edit.changed) {
            sync_upscaler(s.config);
            save_settings(s.config);
        }
    }
}
