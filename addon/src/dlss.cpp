// NVIDIA DLSS (DLAA) through NGX D3D11. See dlss.hpp and docs/addon.md.
//
// Inputs map directly to what the TAA already uses: the game's HDR scene
// target (jittered through the viewport), its reverse-Z depth buffer, the
// EDAA camera motion vectors (UV, previous - current, jitter removed: scaled
// to pixels with the MV scale) and the jitter offset in pixels. NGX keeps the
// immediate context state on D3D11. nvngx_dlss.dll is loaded from the add-on
// folder (the game folder).

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

        ID3D11Device* ngx_device = nullptr; // device NGX was initialised for
        bool init_failed = false; // do not retry every frame
        NVSDK_NGX_Parameter* params = nullptr;
        NVSDK_NGX_Handle* feature = nullptr;
        uint32_t feature_w = 0, feature_h = 0;
        std::atomic<int> probed{ -1 }; // probe() result
        std::mutex status_mutex; // the overlay reads the status from its own thread
        std::string status_text = "not used yet";


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

            feature_w = feature_h = 0;
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

                NVSDK_NGX_D3D11_DestroyParameters(caps);
            }

            if (needs_driver) {
                char text[128];
                snprintf(text, sizeof(text), "NVIDIA driver too old for DLSS (%u.%u or newer needed)", major, minor);
                set_status(text);
                shutdown();
                return false;
            }

            if (!available || !init_ok) {
                set_status("DLSS not available on this GPU (or nvngx_dlss.dll missing next to EDAA.addon64)");
                shutdown();
                return false;
            }

            if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D11_AllocateParameters(&params))) {
                set_status("NGX parameter allocation failed");
                shutdown();
                return false;
            }

            init_failed = false;
            set_status("ready");
            reshade::log::message(reshade::log::level::info, "EDAA: NVIDIA DLSS available");
            return true;
        }


        // DLAA feature for the render size (recreated when it changes).
        bool create_feature(ID3D11DeviceContext* ctx, uint32_t width, uint32_t height)
        {
            if (feature != nullptr && feature_w == width && feature_h == height) {
                return true;
            }

            release_feature();

            NVSDK_NGX_DLSS_Create_Params create = {};
            create.Feature.InWidth = width;
            create.Feature.InHeight = height;
            create.Feature.InTargetWidth = width;
            create.Feature.InTargetHeight = height;
            create.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_DLAA;
            // HDR scene before tonemapping and exposure, motion vectors at
            // render size, reverse-Z depth, exposure estimated by DLSS.
            create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                NVSDK_NGX_DLSS_Feature_Flags_DepthInverted | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;

            const NVSDK_NGX_Result result = NGX_D3D11_CREATE_DLSS_EXT(ctx, &feature, params, &create);

            if (NVSDK_NGX_FAILED(result)) {
                feature = nullptr;
                set_status(result_text("DLSS feature creation failed", result));
                return false;
            }

            feature_w = width;
            feature_h = height;
            char text[96];
            snprintf(text, sizeof(text), "EDAA: DLAA feature created for %ux%u", width, height);
            reshade::log::message(reshade::log::level::info, text);
            return true;
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

        if (!create_feature(ctx, in.width, in.height)) {
            return false;
        }

        NVSDK_NGX_D3D11_DLSS_Eval_Params eval = {};
        eval.Feature.pInColor = in.color;
        eval.Feature.pInOutput = in.output;
        eval.pInDepth = in.depth;
        eval.pInMotionVectors = in.motion;
        eval.InJitterOffsetX = in.jitter_x;
        eval.InJitterOffsetY = in.jitter_y;
        eval.InReset = in.reset ? 1 : 0;
        // Motion vectors are in UV: scale them to pixels.
        eval.InMVScaleX = static_cast<float>(in.width);
        eval.InMVScaleY = static_cast<float>(in.height);
        eval.InRenderSubrectDimensions = { in.width, in.height };

        const NVSDK_NGX_Result result = NGX_D3D11_EVALUATE_DLSS_EXT(ctx, feature, params, &eval);

        if (NVSDK_NGX_FAILED(result)) {
            set_status(result_text("DLSS evaluation failed", result));
            release_feature(); // recreated next frame
            return false;
        }

        set_status("running");
        return true;
    }


    void probe(ID3D11Device* device)
    {
        // NGX init and capability query only: the DLAA feature is created on first use.
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
