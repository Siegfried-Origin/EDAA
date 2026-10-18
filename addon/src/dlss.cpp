// NVIDIA DLSS through NGX D3D11. See dlss.hpp and docs/addon.md.
//
// Two uses, one feature (recreated when its sizes, mode or flags change):
//   - DLAA (evaluate): at the resolve point, render size = output size, on
//     the game's HDR scene (jittered through the viewport);
//   - upscaling (upscale): at the end of the frame, on the game's tonemapped
//     image, from the render size to the display size, with the quality mode
//     whose render size range (NGX optimal settings) contains the render size.
// Other inputs: the scene's reverse-Z depth, the EDAA camera motion vectors
// (UV, previous - current, jitter removed: scaled to render pixels with the
// MV scale) and the jitter offset in render pixels. NGX keeps the immediate
// context state on D3D11. nvngx_dlss.dll is loaded from the add-on folder.

#include "dlss.hpp"

#include "state.hpp"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <Windows.h>
#include <d3d11.h>

#if EDAA_DLSS
#include <nvsdk_ngx_helpers.h>
#endif

namespace edaa::dlss
{
#if EDAA_DLSS
    namespace
    {
        // GUID-like project id required for custom engines (same as the installer AppId).
        constexpr const char* project_id = "6c1b8a52-3e1d-4f7b-9b0e-2a9f5d8e7c41";

        // DLAA: HDR scene before tonemapping and exposure (exposure estimated by DLSS).
        // Upscaling: tonemapped image. Both: motion vectors at render size, reverse-Z depth.
        constexpr int dlaa_flags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure |
            NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
        constexpr int upscale_flags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;

        struct quality_mode
        {
            NVSDK_NGX_PerfQuality_Value value;
            const char* name;
        };

        const quality_mode scaling_modes[] = {
            { NVSDK_NGX_PerfQuality_Value_UltraQuality, "Ultra Quality" },
            { NVSDK_NGX_PerfQuality_Value_MaxQuality, "Quality" },
            { NVSDK_NGX_PerfQuality_Value_Balanced, "Balanced" },
            { NVSDK_NGX_PerfQuality_Value_MaxPerf, "Performance" },
            { NVSDK_NGX_PerfQuality_Value_UltraPerformance, "Ultra Performance" },
        };

        ID3D11Device* ngx_device = nullptr; // device NGX was initialised for
        bool init_failed = false; // do not retry every frame
        NVSDK_NGX_Parameter* params = nullptr;
        NVSDK_NGX_Handle* feature = nullptr;

        // What the current feature was created for.
        struct feature_key
        {
            uint32_t in_w = 0, in_h = 0, out_w = 0, out_h = 0;
            int mode = -1, flags = 0;

            bool operator==(const feature_key&) const = default;
        };

        feature_key current;

        std::atomic<int> probed{ -1 }; // probe() result
        std::mutex status_mutex; // the overlay reads the status and info from its own thread
        std::string status_text = "not used yet";
        upscale_info last_info;
        uint32_t info_w = 0, info_h = 0; // display size of last_info


        void set_status(const std::string& text)
        {
            const std::lock_guard<std::mutex> lock(status_mutex);
            status_text = text;
        }


        std::string result_text(const char* what, NVSDK_NGX_Result result)
        {
            char text[256];
            snprintf(text, sizeof(text), "%s (NGX error 0x%08x: %ls)", what, static_cast<unsigned>(result), GetNGXResultAsString(result));
            return text;
        }


        // NGX writes its logs there: %LOCALAPPDATA%\EDAA.
        std::wstring data_folder()
        {
            wchar_t base[MAX_PATH] = {};

            if (GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH) == 0) {
                return addon_folder();
            }

            std::wstring folder = std::wstring(base) + L"\\EDAA";
            CreateDirectoryW(folder.c_str(), nullptr);
            return folder;
        }


        void release_feature()
        {
            if (feature != nullptr) {
                NVSDK_NGX_D3D11_ReleaseFeature(feature);
                feature = nullptr;
            }

            current = {};
        }


        void shutdown()
        {
            release_feature();

            if (params != nullptr) {
                NVSDK_NGX_D3D11_DestroyParameters(params);
                params = nullptr;
            }

            if (ngx_device != nullptr) {
                NVSDK_NGX_D3D11_Shutdown1(ngx_device);
                ngx_device = nullptr;
            }

            info_w = info_h = 0;
        }


        // NGX init and DLSS availability on this system (driver, GPU).
        bool init(ID3D11Device* device)
        {
            if (ngx_device == device && params != nullptr) {
                return true;
            }

            if (ngx_device != nullptr) {
                shutdown();
                init_failed = false;
            }

            if (init_failed) {
                return false;
            }

            init_failed = true; // until everything below succeeds

            std::wstring folder = addon_folder();
            wchar_t* paths[] = { folder.data() };
            NVSDK_NGX_FeatureCommonInfo info = {};
            info.PathListInfo.Path = paths;
            info.PathListInfo.Length = 1;

            NVSDK_NGX_Result result = NVSDK_NGX_D3D11_Init_with_ProjectID(project_id, NVSDK_NGX_ENGINE_TYPE_CUSTOM, version,
                data_folder().c_str(), device, &info);

            if (NVSDK_NGX_FAILED(result)) {
                set_status(result_text("NGX not available (NVIDIA RTX GPU and driver required)", result));
                return false;
            }

            ngx_device = device;

            // Fails on drivers too old for this SDK (then DLSS is reported unavailable).
            // Kept as the parameter object of every call: only it holds the
            // callback behind NGX_DLSS_GET_OPTIMAL_SETTINGS (an allocated one
            // has none, and every upscaling mode then looks unavailable).
            NVSDK_NGX_Parameter* caps = nullptr;

            if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D11_GetCapabilityParameters(&caps))) {
                caps = nullptr;
            }

            int needs_driver = 0, available = 0, init_ok = 0;
            unsigned major = 0, minor = 0;

            if (caps != nullptr) {
                NVSDK_NGX_Parameter_GetI(caps, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
                NVSDK_NGX_Parameter_GetUI(caps, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &major);
                NVSDK_NGX_Parameter_GetUI(caps, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minor);
                NVSDK_NGX_Parameter_GetI(caps, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
                NVSDK_NGX_Parameter_GetI(caps, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &init_ok);
                params = caps; // destroyed by shutdown()
            }

            if (needs_driver) {
                char text[128];
                snprintf(text, sizeof(text), "NVIDIA driver too old for DLSS (%u.%u or newer needed)", major, minor);
                set_status(text);
                shutdown();
                return false;
            }

            if (params == nullptr) {
                set_status("NGX capability parameters not available (driver too old?)");
                shutdown();
                return false;
            }

            if (!available || !init_ok) {
                set_status("DLSS not available on this GPU (or nvngx_dlss.dll missing next to EDAA.addon64)");
                shutdown();
                return false;
            }

            init_failed = false;
            set_status("ready");
            reshade::log::message(reshade::log::level::info, "EDAA: NVIDIA DLSS available");
            return true;
        }


        // DLSS feature for these sizes, mode and flags (recreated when any changes).
        bool create_feature(ID3D11DeviceContext* ctx, const feature_key& key)
        {
            if (feature != nullptr && current == key) {
                return true;
            }

            release_feature();

            NVSDK_NGX_DLSS_Create_Params create = {};
            create.Feature.InWidth = key.in_w;
            create.Feature.InHeight = key.in_h;
            create.Feature.InTargetWidth = key.out_w;
            create.Feature.InTargetHeight = key.out_h;
            create.Feature.InPerfQualityValue = static_cast<NVSDK_NGX_PerfQuality_Value>(key.mode);
            create.InFeatureCreateFlags = key.flags;

            const NVSDK_NGX_Result result = NGX_D3D11_CREATE_DLSS_EXT(ctx, &feature, params, &create);

            if (NVSDK_NGX_FAILED(result)) {
                feature = nullptr;
                set_status(result_text("DLSS feature creation failed", result));
                return false;
            }

            current = key;
            char text[128];
            snprintf(text, sizeof(text), "EDAA: DLSS feature created, %ux%u -> %ux%u, quality value %d", key.in_w, key.in_h, key.out_w,
                key.out_h, key.mode);
            reshade::log::message(reshade::log::level::info, text);
            return true;
        }


        // Evaluates the current feature on 'in' (render size width x height).
        bool run(ID3D11DeviceContext* ctx, const upscaler_inputs& in)
        {
            NVSDK_NGX_D3D11_DLSS_Eval_Params eval = {};
            eval.Feature.pInColor = in.color;
            eval.Feature.pInOutput = in.output;
            eval.pInDepth = in.depth;
            eval.pInMotionVectors = in.motion;
            eval.InJitterOffsetX = in.jitter_x;
            eval.InJitterOffsetY = in.jitter_y;
            eval.InReset = in.reset ? 1 : 0;
            // Motion vectors are in UV of the render size: scale them to pixels.
            eval.InMVScaleX = static_cast<float>(in.width);
            eval.InMVScaleY = static_cast<float>(in.height);
            eval.InRenderSubrectDimensions = { in.width, in.height };
            // HUD drawn after the resolve: not jittered, no motion vectors, so no history there.
            eval.pInBiasCurrentColorMask = in.hud_mask;

            const NVSDK_NGX_Result result = NGX_D3D11_EVALUATE_DLSS_EXT(ctx, feature, params, &eval);

            if (NVSDK_NGX_FAILED(result)) {
                set_status(result_text("DLSS evaluation failed", result));
                release_feature(); // recreated next frame
                return false;
            }

            return true;
        }


        // Render size ranges of every quality mode for a display size (cached per size).
        const upscale_info& modes_for(uint32_t display_w, uint32_t display_h)
        {
            if (info_w == display_w && info_h == display_h) {
                return last_info;
            }

            upscale_info info;

            for (const quality_mode& m : scaling_modes) {
                upscale_mode mode;
                mode.name = m.name;
                float sharpness = 0;

                if (NVSDK_NGX_FAILED(NGX_DLSS_GET_OPTIMAL_SETTINGS(params, display_w, display_h, m.value, &mode.optimal_w, &mode.optimal_h,
                        &mode.max_w, &mode.max_h, &mode.min_w, &mode.min_h, &sharpness)) ||
                    mode.optimal_w == 0) {
                    continue; // mode not offered for this display size
                }

                info.modes.push_back(mode);
            }

            for (const upscale_mode& m : info.modes) {
                char text[192];
                snprintf(text, sizeof(text), "EDAA: DLSS %s for %ux%u: optimal %ux%u, range %ux%u - %ux%u", m.name, display_w, display_h,
                    m.optimal_w, m.optimal_h, m.min_w, m.min_h, m.max_w, m.max_h);
                reshade::log::message(reshade::log::level::info, text);
            }

            const std::lock_guard<std::mutex> lock(status_mutex);
            last_info = info;
            info_w = display_w;
            info_h = display_h;
            return last_info;
        }
    }


    bool evaluate(ID3D11Device* device, ID3D11DeviceContext* ctx, const upscaler_inputs& in)
    {
        if (!init(device)) {
            return false;
        }

        if (in.width < 32 || in.height < 32) {
            set_status("render size too small for DLSS");
            return false;
        }

        const feature_key key = { in.width, in.height, in.width, in.height, NVSDK_NGX_PerfQuality_Value_DLAA, dlaa_flags };

        if (!create_feature(ctx, key) || !run(ctx, in)) {
            return false;
        }

        set_status("running (DLAA)");
        return true;
    }


    bool upscale(ID3D11Device* device, ID3D11DeviceContext* ctx, const upscaler_inputs& in)
    {
        if (!init(device)) {
            return false;
        }

        // The mode whose accepted range contains the render size, closest to its
        // optimal size. Ranges can be a single size (no dynamic resolution for
        // that mode): then the mode with the closest optimal size.
        const upscale_info& modes = modes_for(in.output_width, in.output_height);
        int best = -1, closest = -1;
        uint32_t best_distance = UINT32_MAX, closest_distance = UINT32_MAX;

        for (size_t i = 0; i < modes.modes.size(); ++i) {
            const upscale_mode& m = modes.modes[i];
            const uint32_t distance = (in.width > m.optimal_w ? in.width - m.optimal_w : m.optimal_w - in.width);

            if (distance < closest_distance) {
                closest_distance = distance;
                closest = static_cast<int>(i);
            }

            if (in.width < m.min_w || in.width > m.max_w || in.height < m.min_h || in.height > m.max_h) {
                continue;
            }

            if (distance < best_distance) {
                best_distance = distance;
                best = static_cast<int>(i);
            }
        }

        if (best < 0) {
            best = closest;
        }

        if (best < 0) {
            set_status("DLSS offers no upscaling mode for this display size");
            const std::lock_guard<std::mutex> lock(status_mutex);
            last_info.mode = "";
            return false;
        }

        const upscale_mode& mode = modes.modes[best];
        NVSDK_NGX_PerfQuality_Value value = NVSDK_NGX_PerfQuality_Value_MaxQuality;

        for (const quality_mode& m : scaling_modes) {
            value = m.name == mode.name ? m.value : value;
        }

        // Created for the actual render size (fixed by the game setting): the input must never exceed it.
        const feature_key key = { in.width, in.height, in.output_width, in.output_height, value, upscale_flags };

        if (!create_feature(ctx, key) || !run(ctx, in)) {
            return false;
        }

        {
            const std::lock_guard<std::mutex> lock(status_mutex);
            last_info.mode = mode.name;
        }

        set_status(std::string("running (upscaling, ") + mode.name + ")");
        return true;
    }


    upscale_info info()
    {
        const std::lock_guard<std::mutex> lock(status_mutex);
        return last_info;
    }


    void probe(ID3D11Device* device)
    {
        // NGX init and capability query only: the DLSS feature is created on first use.
        probed = init(device) ? 1 : 0;
    }


    int availability()
    {
        return probed;
    }


    std::string status()
    {
        const std::lock_guard<std::mutex> lock(status_mutex);
        return status_text;
    }


    void release(ID3D11Device* device)
    {
        if (ngx_device != nullptr && (device == nullptr || device == ngx_device)) {
            shutdown();
            set_status("not used yet");
        }

        init_failed = false;
    }


#else
    bool evaluate(ID3D11Device*, ID3D11DeviceContext*, const upscaler_inputs&)
    {
        return false;
    }


    bool upscale(ID3D11Device*, ID3D11DeviceContext*, const upscaler_inputs&)
    {
        return false;
    }


    upscale_info info()
    {
        return {};
    }


    void probe(ID3D11Device*)
    {
    }


    int availability()
    {
        return 0;
    }


    std::string status()
    {
        return "this build has no DLSS (DLSS SDK not found at build time)";
    }


    void release(ID3D11Device*)
    {
    }
#endif
}
