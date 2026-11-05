// Frame generation (experimental, off by default: [EDAA] fg_present_test=1).
//
// DLSS Frame Generation (Streamline) only works on a D3D12 or Vulkan swap chain,
// and the game presents with D3D11:
//   - the game, ReShade (effects, overlay), 3DMigoto/EDHM and EDAA keep their
//     D3D11 swap chain untouched;
//   - at reshade_present (after ReShade's effects and overlay) the final image is
//     copied into a texture shared with D3D12 (D3D11 creates it, D3D12 opens it,
//     a shared fence orders the work, as in xess.cpp);
//   - a D3D12 flip-model swap chain on a child window covering the game's client
//     area presents that copy.
// Step 2 ([EDAA] fg_generate=1): the D3D12 device, factory and swap chain are
// Streamline proxies; resolve() hands over depth and motion vectors (fg.hpp),
// tagged for DLSS-G with the camera constants, Reflex and the PCL markers.
// The Streamline DLLs are loaded from the add-on folder at run time.

#include "events.hpp"
#include "fg.hpp"
#include "state.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <Windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#if EDAA_SL
#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#endif

using Microsoft::WRL::ComPtr;
using namespace reshade::api;

namespace edaa
{
    namespace
    {
        constexpr UINT buffer_count = 3;

        struct presenter
        {
            bool failed = false; // stop trying after an error (logged once)
            ID3D11Device* device11 = nullptr; // the game's device the resources belong to
            ComPtr<ID3D11Device5> device11_5;
            ComPtr<ID3D12Device> device12;
            ComPtr<ID3D12CommandQueue> queue;
            ComPtr<ID3D12CommandAllocator> allocators[buffer_count];
            uint64_t allocator_values[buffer_count] = {};
            ComPtr<ID3D12GraphicsCommandList> list;
            ComPtr<ID3D12Fence> fence12;
            ComPtr<ID3D11Fence> fence11;
            HANDLE fence_event = nullptr;
            uint64_t fence_value = 0; // last value signalled by either side
            uint64_t copy_done = 0; // D3D12 finished the last present (and frame generation) up to this value
            uint64_t list_done = 0; // D3D12 finished the last command list: the shared image is free again

            // Shared texture (D3D11 writes, D3D12 reads).
            ComPtr<ID3D11Texture2D> shared11;
            ComPtr<ID3D12Resource> shared12;
            uint32_t width = 0, height = 0;
            DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;

            // Child window and its swap chain.
            HWND parent = nullptr;
            std::atomic<HWND> child{ nullptr };
            std::thread window_thread;
            ComPtr<IDXGIFactory2> factory;
            ComPtr<IDXGISwapChain3> swapchain;
            bool tearing = false;
            uint64_t frames = 0;

            // Step 2: depth and motion vectors shared with D3D12 (copied at resolve).
            // Two sets, written alternately: the next frame's inputs are copied
            // while D3D12 may still read the previous set (no wait, no skip).
            ComPtr<ID3D11Texture2D> depth11[2], motion11[2];
            ComPtr<ID3D12Resource> depth12[2], motion12[2];
            uint64_t set_done[2] = {}; // the command list that read this set finished at this value
            int write_set = 0; // set the next capture writes
            int ready_set = -1; // set holding the latest captured inputs
            fg::frame_inputs inputs;
            bool inputs_fresh = false; // captured since the last present
            uint32_t stale_frames = 1000; // presents without a resolve (no scene: menus, maps, loading)
            bool resolve_seen = false; // resolve ran since the last present (inputs maybe skipped)
            uint64_t busy_skips = 0; // captures skipped while D3D12 was busy
            int fps_limit = -1; // Reflex limit last set
            // Overlay status.
            LARGE_INTEGER last_time = {};
            float real_fps = 0;
            uint32_t presented = 1; // DLSS-G frames per real frame (last query)
            unsigned dlssg_status = 0;
            bool focus = true;
            const char* error = nullptr;
            bool hud_protected = false;
            bool sl_on = false; // Streamline loaded, D3D12 objects are its proxies
        };

        presenter p;
        std::mutex presenter_mutex;

        // DLSS-G presents from its own pacing thread while the game presents on
        // its render thread. Hooks of other programs on Present (Steam overlay:
        // gameoverlayrenderer64.dll) are not made for two threads at once and
        // crash. While frame generation runs, the two presents never overlap:
        // the lock is taken right before each one and released after it
        // (finish_present), with a time limit so nothing can hang.
        std::timed_mutex present_serial;
        thread_local bool present_serial_held = false;
        std::atomic<bool> serialize_presents{ false };


        void serial_lock()
        {
            if (present_serial_held || !serialize_presents) {
                return;
            }

            if (present_serial.try_lock_for(std::chrono::milliseconds(100))) {
                present_serial_held = true;
            }
        }


        void serial_unlock()
        {
            if (present_serial_held) {
                present_serial_held = false;
                present_serial.unlock();
            }
        }
        std::atomic<bool> inputs_wanted{ false };


        void log_info(const char* text)
        {
            reshade::log::message(reshade::log::level::info, text);
        }


        // Diagnostics: each step of the first frames, to find where a crash happens.
        void step(const char* name)
        {
            if (p.frames < 3) {
                char msg[128];
                snprintf(msg, sizeof(msg), "EDAA FG test: frame %llu, %s", static_cast<unsigned long long>(p.frames), name);
                log_info(msg);
            }
        }


        bool fail(const char* what, HRESULT hr)
        {
            if (SUCCEEDED(hr)) {
                return false;
            }

            char msg[192];
            snprintf(msg, sizeof(msg), "EDAA FG test: %s (0x%08lX), presentation test stopped", what, static_cast<unsigned long>(hr));
            reshade::log::message(reshade::log::level::error, msg);
            p.failed = true;
            p.error = what;
            return true;
        }


        // Inverse of a 4x4 matrix (row-major), in double precision. Returns false if singular.
        bool invert4(const float m[16], float out[16])
        {
            double a[4][8];

            for (int r = 0; r < 4; ++r) {
                for (int c = 0; c < 4; ++c) {
                    a[r][c] = m[r * 4 + c];
                    a[r][c + 4] = r == c ? 1.0 : 0.0;
                }
            }

            for (int c = 0; c < 4; ++c) {
                int pivot = c;

                for (int r = c + 1; r < 4; ++r) {
                    if (std::fabs(a[r][c]) > std::fabs(a[pivot][c])) {
                        pivot = r;
                    }
                }

                if (std::fabs(a[pivot][c]) < 1e-20) {
                    return false;
                }

                for (int k = 0; k < 8; ++k) {
                    std::swap(a[c][k], a[pivot][k]);
                }

                const double d = 1.0 / a[c][c];

                for (int k = 0; k < 8; ++k) {
                    a[c][k] *= d;
                }

                for (int r = 0; r < 4; ++r) {
                    if (r != c) {
                        const double f = a[r][c];

                        for (int k = 0; k < 8; ++k) {
                            a[r][k] -= f * a[c][k];
                        }
                    }
                }
            }

            for (int r = 0; r < 4; ++r) {
                for (int c = 0; c < 4; ++c) {
                    out[r * 4 + c] = static_cast<float>(a[r][c + 4]);
                }
            }

            return true;
        }


        void multiply4(const float a[16], const float b[16], float out[16])
        {
            for (int r = 0; r < 4; ++r) {
                for (int c = 0; c < 4; ++c) {
                    float v = 0;

                    for (int k = 0; k < 4; ++k) {
                        v += a[r * 4 + k] * b[k * 4 + c];
                    }

                    out[r * 4 + c] = v;
                }
            }
        }


#if EDAA_SL
        // Streamline, loaded from the add-on folder (no import library: the add-on
        // still loads without the DLLs).
        struct streamline
        {
            HMODULE module = nullptr;
            PFun_slInit* init = nullptr;
            PFun_slShutdown* shutdown = nullptr;
            PFun_slSetD3DDevice* set_device = nullptr;
            PFun_slUpgradeInterface* upgrade = nullptr;
            PFun_slGetNewFrameToken* new_token = nullptr;
            PFun_slSetTagForFrame* set_tags = nullptr;
            PFun_slSetConstants* set_constants = nullptr;
            PFun_slGetFeatureFunction* feature_function = nullptr;
            PFun_slIsFeatureSupported* supported = nullptr;
            PFun_slDLSSGSetOptions* dlssg_set_options = nullptr;
            PFun_slDLSSGGetState* dlssg_get_state = nullptr;
            PFun_slReflexSetOptions* reflex_set_options = nullptr;
            PFun_slReflexSleep* reflex_sleep = nullptr;
            PFun_slPCLSetMarker* pcl_marker = nullptr;

            sl::FrameToken* token = nullptr; // frame being simulated / rendered
            bool simulation_ended = false;
            bool dlssg_on = false; // mode last set
            uint32_t mvec_w = 0, mvec_h = 0, color_w = 0, color_h = 0; // sizes last set
            std::wstring folder;
        };

        streamline sl_api;
        std::atomic<int> sl_info_lines{ 0 };


        void sl_log(sl::LogType type, const char* text)
        {
            if (text == nullptr) {
                return;
            }

            std::string line = std::string("EDAA FG Streamline: ") + text;

            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
                line.pop_back();
            }

            switch (type) {
                case sl::LogType::eError:
                    reshade::log::message(reshade::log::level::error, line.c_str());
                    break;
                case sl::LogType::eWarn:
                    reshade::log::message(reshade::log::level::warning, line.c_str());
                    break;
                default:
                    if (sl_info_lines.fetch_add(1) < 400) {
                        reshade::log::message(reshade::log::level::info, line.c_str());
                    }
                    break;
            }
        }


        bool sl_check(const char* what, sl::Result result)
        {
            if (result == sl::Result::eOk) {
                return true;
            }

            char msg[192];
            snprintf(msg, sizeof(msg), "EDAA FG: %s failed (Streamline result %d)", what, static_cast<int>(result));
            reshade::log::message(reshade::log::level::warning, msg);
            return false;
        }


        template <typename T>
        bool load_proc(T*& fn, const char* name)
        {
            fn = reinterpret_cast<T*>(GetProcAddress(sl_api.module, name));

            if (fn == nullptr) {
                char msg[160];
                snprintf(msg, sizeof(msg), "EDAA FG: %s not found in sl.interposer.dll", name);
                log_info(msg);
            }

            return fn != nullptr;
        }


        template <typename T>
        bool load_feature_proc(sl::Feature feature, T*& fn, const char* name)
        {
            void* f = nullptr;

            if (!sl_check(name, sl_api.feature_function(feature, name, f)) || f == nullptr) {
                return false;
            }

            fn = reinterpret_cast<T*>(f);
            return true;
        }


        // Before the D3D12 device exists (Streamline requirement).
        bool sl_start()
        {
            sl_api.folder = addon_folder();
            const std::wstring dll = sl_api.folder + L"\\sl.interposer.dll";
            sl_api.module = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);

            if (sl_api.module == nullptr) {
                log_info("EDAA FG: sl.interposer.dll not found next to EDAA.addon64, frame generation off");
                return false;
            }

            if (!load_proc(sl_api.init, "slInit") || !load_proc(sl_api.shutdown, "slShutdown") ||
                !load_proc(sl_api.set_device, "slSetD3DDevice") || !load_proc(sl_api.upgrade, "slUpgradeInterface") ||
                !load_proc(sl_api.new_token, "slGetNewFrameToken") || !load_proc(sl_api.set_tags, "slSetTagForFrame") ||
                !load_proc(sl_api.set_constants, "slSetConstants") || !load_proc(sl_api.feature_function, "slGetFeatureFunction") ||
                !load_proc(sl_api.supported, "slIsFeatureSupported")) {
                return false;
            }

            static const sl::Feature features[] = { sl::kFeatureReflex, sl::kFeaturePCL, sl::kFeatureDLSS_G };
            static std::wstring plugin_path;
            static const wchar_t* paths[1];
            plugin_path = sl_api.folder;
            paths[0] = plugin_path.c_str();

            sl::Preferences pref;
            pref.showConsole = false;
            pref.logLevel = sl::LogLevel::eDefault;
            pref.pathsToPlugins = paths;
            pref.numPathsToPlugins = 1;
            pref.pathToLogsAndData = plugin_path.c_str(); // sl.log next to the add-on
            pref.logMessageCallback = sl_log;
            pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eDisableDebugText |
                sl::PreferenceFlags::eUseDXGIFactoryProxy | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
            pref.featuresToLoad = features;
            pref.numFeaturesToLoad = static_cast<uint32_t>(std::size(features));
            pref.engine = sl::EngineType::eCustom;
            pref.engineVersion = "EDAA";
            pref.projectId = "6c0d4f0e-3b8a-4d43-9a3e-6e1f2d6a0e44";
            pref.renderAPI = sl::RenderAPI::eD3D12;

            if (!sl_check("slInit", sl_api.init(pref, sl::kSDKVersion))) {
                return false;
            }

            log_info("EDAA FG: Streamline initialised (DLSS-G, Reflex, PCL)");
            return true;
        }


        // Reflex low latency and its frame limiter. With DLSS-G on, the limit is
        // for the displayed frames (real + generated). One limiter only: an
        // external one fights the Reflex sleep (uneven pacing).
        bool sl_reflex(int fps_limit)
        {
            sl::ReflexOptions reflex;
            reflex.mode = sl::ReflexMode::eLowLatency;
            reflex.frameLimitUs = fps_limit > 0 ? static_cast<uint32_t>(1000000 / fps_limit) : 0;

            if (!sl_check("slReflexSetOptions", sl_api.reflex_set_options(reflex))) {
                return false;
            }

            p.fps_limit = fps_limit;
            return true;
        }


        // Right after the D3D12 device is created: set it, replace it by its proxy.
        bool sl_device(ComPtr<ID3D12Device>& device, IDXGIAdapter* adapter, int fps_limit)
        {
            if (!sl_check("slSetD3DDevice", sl_api.set_device(device.Get()))) {
                return false;
            }

            DXGI_ADAPTER_DESC desc = {};

            if (adapter != nullptr && SUCCEEDED(adapter->GetDesc(&desc))) {
                sl::AdapterInfo info;
                info.deviceLUID = reinterpret_cast<uint8_t*>(&desc.AdapterLuid);
                info.deviceLUIDSizeInBytes = sizeof(LUID);
                const sl::Result supported = sl_api.supported(sl::kFeatureDLSS_G, info);

                if (!sl_check("DLSS Frame Generation support check", supported)) {
                    return false;
                }
            }

            void* raw = device.Detach();

            if (!sl_check("slUpgradeInterface (D3D12 device)", sl_api.upgrade(&raw))) {
                device.Attach(static_cast<ID3D12Device*>(raw));
                return false;
            }

            device.Attach(static_cast<ID3D12Device*>(raw));

            if (!load_feature_proc(sl::kFeatureDLSS_G, sl_api.dlssg_set_options, "slDLSSGSetOptions") ||
                !load_feature_proc(sl::kFeatureDLSS_G, sl_api.dlssg_get_state, "slDLSSGGetState") ||
                !load_feature_proc(sl::kFeatureReflex, sl_api.reflex_set_options, "slReflexSetOptions") ||
                !load_feature_proc(sl::kFeatureReflex, sl_api.reflex_sleep, "slReflexSleep") ||
                !load_feature_proc(sl::kFeaturePCL, sl_api.pcl_marker, "slPCLSetMarker")) {
                return false;
            }

            if (!sl_reflex(fps_limit)) {
                return false;
            }

            char msg[160];
            snprintf(msg, sizeof(msg), "EDAA FG: D3D12 device upgraded to a Streamline proxy, Reflex low latency on, limit %d fps", fps_limit);
            log_info(msg);
            return true;
        }


        bool sl_factory(ComPtr<IDXGIFactory2>& factory)
        {
            void* raw = factory.Detach();

            if (!sl_check("slUpgradeInterface (DXGI factory)", sl_api.upgrade(&raw))) {
                factory.Attach(static_cast<IDXGIFactory2*>(raw));
                return false;
            }

            ComPtr<IUnknown> upgraded;
            upgraded.Attach(static_cast<IUnknown*>(raw));
            return SUCCEEDED(upgraded.As(&factory));
        }


        void sl_marker(sl::PCLMarker marker)
        {
            if (sl_api.token != nullptr) {
                sl_api.pcl_marker(marker, *sl_api.token);
            }
        }


        // New frame: token, Reflex sleep, simulation start.
        void sl_begin_frame()
        {
            if (sl_api.token != nullptr) {
                return;
            }

            sl::FrameToken* token = nullptr;

            if (sl_api.new_token(token, nullptr) != sl::Result::eOk || token == nullptr) {
                return;
            }

            sl_api.token = token;
            sl_api.simulation_ended = false;
            sl_api.reflex_sleep(*token);
            sl_marker(sl::PCLMarker::eSimulationStart);
        }


        void sl_end_simulation()
        {
            if (sl_api.token != nullptr && !sl_api.simulation_ended) {
                sl_marker(sl::PCLMarker::eSimulationEnd);
                sl_marker(sl::PCLMarker::eRenderSubmitStart);
                sl_api.simulation_ended = true;
            }
        }


        sl::float4x4 to_sl(const float m[16])
        {
            sl::float4x4 out;

            for (uint32_t r = 0; r < 4; ++r) {
                out.setRow(r, sl::float4(m[r * 4], m[r * 4 + 1], m[r * 4 + 2], m[r * 4 + 3]));
            }

            return out;
        }


        sl::float3 normalized(float x, float y, float z)
        {
            const float l = std::sqrt(x * x + y * y + z * z);
            return l > 0 ? sl::float3(x / l, y / l, z / l) : sl::float3(0, 0, 0);
        }


        // Camera constants of the captured frame (row vectors, like c270-c273).
        void sl_constants(const fg::frame_inputs& in)
        {
            sl::Constants c;
            const float n = in.near_plane > 0 ? in.near_plane : 0.01f;
            // Reverse-Z infinite perspective: clip = (x*sx, y*sy, near, z).
            const float view_to_clip[16] = { in.sx, 0, 0, 0, 0, in.sy, 0, 0, 0, 0, 0, 1, 0, 0, n, 0 };
            const float clip_to_view[16] = { in.sx != 0 ? 1 / in.sx : 0, 0, 0, 0, 0, in.sy != 0 ? 1 / in.sy : 0, 0, 0, 0, 0, 0, 1 / n, 0, 0, 1, 0 };
            const float identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
            // clip -> world (relative to the current camera) -> previous camera -> previous clip.
            const float translate[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, in.camera_delta[0], in.camera_delta[1], in.camera_delta[2], 1 };
            float tmp[16], to_prev[16], from_prev[16];
            multiply4(in.inv_view_proj, translate, tmp);
            multiply4(tmp, in.prev_view_proj, to_prev);

            if (!invert4(to_prev, from_prev)) {
                std::memcpy(from_prev, identity, sizeof(from_prev));
                std::memcpy(to_prev, identity, sizeof(to_prev));
            }

            c.cameraViewToClip = to_sl(view_to_clip);
            c.clipToCameraView = to_sl(clip_to_view);
            c.clipToLensClip = to_sl(identity);
            c.clipToPrevClip = to_sl(to_prev);
            c.prevClipToClip = to_sl(from_prev);
            c.jitterOffset = sl::float2(in.jitter_x, in.jitter_y);
            c.mvecScale = sl::float2(1.0f, 1.0f); // EDAA motion vectors are UV offsets
            c.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
            c.cameraPos = sl::float3(in.camera_pos[0], in.camera_pos[1], in.camera_pos[2]);
            const float* m = in.view_proj;
            c.cameraRight = normalized(m[0], m[4], m[8]);
            c.cameraUp = normalized(m[1], m[5], m[9]);
            c.cameraFwd = normalized(m[3], m[7], m[11]);
            c.cameraNear = n;
            c.cameraFar = 1.0e9f;
            c.cameraFOV = in.vfov_deg * 3.14159265f / 180.0f;
            c.cameraAspectRatio = in.aspect;
            c.depthInverted = sl::Boolean::eTrue;
            c.cameraMotionIncluded = sl::Boolean::eTrue;
            c.motionVectors3D = sl::Boolean::eFalse;
            c.reset = in.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
            c.orthographicProjection = sl::Boolean::eFalse;
            c.motionVectorsDilated = sl::Boolean::eFalse;
            c.motionVectorsJittered = sl::Boolean::eFalse;

            static bool logged = false;
            const sl::Result result = sl_api.set_constants(c, *sl_api.token, sl::ViewportHandle(0));

            if (result != sl::Result::eOk && !logged) {
                logged = true;
                sl_check("slSetConstants", result);
            }
        }


        // DLSS-G on (inputs this frame) or off, and the sizes it allocates for.
        void sl_options(bool on)
        {
            const uint32_t mw = p.inputs.render_w, mh = p.inputs.render_h;

            if (on == sl_api.dlssg_on && (!on || (mw == sl_api.mvec_w && mh == sl_api.mvec_h && p.width == sl_api.color_w && p.height == sl_api.color_h))) {
                return;
            }

            sl::DLSSGOptions o;
            o.mode = on ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
            o.numFramesToGenerate = 1;
            o.numBackBuffers = buffer_count;
            o.mvecDepthWidth = mw;
            o.mvecDepthHeight = mh;
            o.colorWidth = p.width;
            o.colorHeight = p.height;
            o.colorBufferFormat = static_cast<uint32_t>(p.format);
            o.mvecBufferFormat = static_cast<uint32_t>(DXGI_FORMAT_R16G16_FLOAT);
            o.depthBufferFormat = static_cast<uint32_t>(DXGI_FORMAT_R32_FLOAT);

            if (sl_check("slDLSSGSetOptions", sl_api.dlssg_set_options(sl::ViewportHandle(0), o))) {
                sl_api.dlssg_on = on;
                sl_api.mvec_w = mw;
                sl_api.mvec_h = mh;
                sl_api.color_w = p.width;
                sl_api.color_h = p.height;
                char msg[160];
                snprintf(msg, sizeof(msg), "EDAA FG: DLSS-G %s (inputs %ux%u, output %ux%u)", on ? "on" : "off", mw, mh, p.width, p.height);
                log_info(msg);
            }
        }


        // Every 15 frames: DLSS-G state for the overlay, logged now and then.
        void sl_log_state()
        {
            if (p.frames % 15 != 0) {
                return;
            }

            p.focus = GetForegroundWindow() == GetAncestor(p.parent, GA_ROOT);
            sl::DLSSGState st;

            if (sl_api.dlssg_get_state(sl::ViewportHandle(0), st, nullptr) != sl::Result::eOk) {
                return;
            }

            p.presented = st.numFramesActuallyPresented;
            p.dlssg_status = static_cast<unsigned>(st.status);

            if (p.frames != 30 && p.frames % 1800 != 0) {
                return;
            }

            char msg[256];
            snprintf(msg, sizeof(msg), "EDAA FG: DLSS-G state at frame %llu: status 0x%X, frames presented %u, frames without a scene %u, reset %d, focus %d, busy skips %llu",
                static_cast<unsigned long long>(p.frames), static_cast<unsigned>(st.status), st.numFramesActuallyPresented,
                p.stale_frames, p.inputs.reset ? 1 : 0, p.focus ? 1 : 0, static_cast<unsigned long long>(p.busy_skips));
            log_info(msg);
        }
#endif


        LRESULT CALLBACK child_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
        {
            switch (msg) {
                case WM_NCHITTEST:
                    return HTTRANSPARENT; // mouse input belongs to the game window
                case WM_MOUSEACTIVATE:
                    return MA_NOACTIVATE;
                case WM_ERASEBKGND:
                    return 1;
                default:
                    return DefWindowProcW(hwnd, msg, wparam, lparam);
            }
        }


        // The child window lives on its own thread with its own message loop, so
        // the game's window thread never waits for the render thread.
        bool create_child(HWND parent, uint32_t width, uint32_t height)
        {
            HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);

            p.window_thread = std::thread([parent, width, height, ready]() {
                const HINSTANCE instance = GetModuleHandleW(nullptr);
                WNDCLASSEXW wc = { sizeof(wc) };
                wc.lpfnWndProc = child_proc;
                wc.hInstance = instance;
                wc.lpszClassName = L"EDAA_FG_Present";
                RegisterClassExW(&wc);

                const HWND hwnd = CreateWindowExW(WS_EX_NOPARENTNOTIFY, wc.lpszClassName, L"", WS_CHILD | WS_VISIBLE | WS_DISABLED | WS_CLIPSIBLINGS,
                    0, 0, static_cast<int>(width), static_cast<int>(height), parent, nullptr, instance, nullptr);
                p.child = hwnd;
                SetEvent(ready);

                if (hwnd == nullptr) {
                    return;
                }

                MSG msg;

                while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            });

            WaitForSingleObject(ready, 5000);
            CloseHandle(ready);

            if (p.child.load() == nullptr) {
                log_info("EDAA FG test: child window creation failed, presentation test stopped");
                p.failed = true;
                return false;
            }

            return true;
        }


        // System dxgi.dll: ReShade's proxy would add a second effect runtime.
        bool create_factory(int variant)
        {
            wchar_t path[MAX_PATH];
            const UINT len = GetSystemDirectoryW(path, MAX_PATH);
            const std::wstring dll = std::wstring(path, len) + L"\\dxgi.dll";
            // 1: the dxgi.dll the game loaded (ReShade's proxy): ReShade wraps the swap
            // chain like the queue, and gets an effect runtime for it (effects disabled there).
            const HMODULE module = variant == 1 ? GetModuleHandleW(L"dxgi.dll") : LoadLibraryExW(dll.c_str(), nullptr, 0);
            {
                char msg[96];
                snprintf(msg, sizeof(msg), "EDAA FG test: DXGI factory from %s dxgi.dll", variant == 1 ? "the game's (ReShade)" : "the system");
                log_info(msg);
            }
            using create_t = HRESULT(WINAPI*)(UINT, REFIID, void**);
            const auto create = module != nullptr ? reinterpret_cast<create_t>(GetProcAddress(module, "CreateDXGIFactory2")) : nullptr;

            if (create == nullptr) {
                log_info("EDAA FG test: CreateDXGIFactory2 not found in the system dxgi.dll, presentation test stopped");
                p.failed = true;
                return false;
            }

            if (fail("cannot create a DXGI factory", create(0, IID_PPV_ARGS(&p.factory)))) {
                return false;
            }

#if EDAA_SL
            if (p.sl_on && !sl_factory(p.factory)) {
                log_info("EDAA FG: no Streamline DXGI factory, presenting without frame generation");
                p.sl_on = false;

                if (!p.factory) {
                    p.failed = true;
                    return false;
                }
            }
#endif

            ComPtr<IDXGIFactory5> factory5;
            BOOL allow = FALSE;

            if (SUCCEEDED(p.factory.As(&factory5)) &&
                SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow)))) {
                p.tearing = allow != FALSE;
            }

            return true;
        }


        bool create_device(ID3D11Device* device, int fps_limit)
        {
            if (fail("Windows 10 1703 or newer needed (ID3D11Device5)", device->QueryInterface(IID_PPV_ARGS(&p.device11_5)))) {
                return false;
            }

            ComPtr<IDXGIDevice> dxgi;
            ComPtr<IDXGIAdapter> adapter;

            if (fail("cannot get the game's GPU", device->QueryInterface(IID_PPV_ARGS(&dxgi))) ||
                fail("cannot get the game's GPU", dxgi->GetAdapter(&adapter))) {
                return false;
            }

            const HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
            const auto create = d3d12 != nullptr ? reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(d3d12, "D3D12CreateDevice")) : nullptr;

            if (create == nullptr) {
                log_info("EDAA FG test: d3d12.dll not available, presentation test stopped");
                p.failed = true;
                return false;
            }

            if (fail("cannot create a D3D12 device on the game's GPU", create(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&p.device12)))) {
                return false;
            }

#if EDAA_SL
            if (p.sl_on && !sl_device(p.device12, adapter.Get(), fps_limit)) {
                log_info("EDAA FG: Streamline cannot use the D3D12 device, presenting without frame generation");
                p.sl_on = false;
            }

            if (!p.device12) {
                p.failed = true;
                return false;
            }
#endif

            D3D12_COMMAND_QUEUE_DESC queue = {};
            queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

            if (fail("D3D12 queue creation failed", p.device12->CreateCommandQueue(&queue, IID_PPV_ARGS(&p.queue)))) {
                return false;
            }

            for (ComPtr<ID3D12CommandAllocator>& allocator : p.allocators) {
                if (fail("D3D12 allocator creation failed", p.device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)))) {
                    return false;
                }
            }

            if (fail("D3D12 command list creation failed",
                    p.device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, p.allocators[0].Get(), nullptr, IID_PPV_ARGS(&p.list)))) {
                return false;
            }

            p.list->Close();

            HANDLE handle = nullptr;

            if (fail("shared fence creation failed", p.device12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&p.fence12))) ||
                fail("shared fence creation failed", p.device12->CreateSharedHandle(p.fence12.Get(), nullptr, GENERIC_ALL, nullptr, &handle))) {
                return false;
            }

            const HRESULT opened = p.device11_5->OpenSharedFence(handle, IID_PPV_ARGS(&p.fence11));
            CloseHandle(handle);

            if (fail("D3D11 cannot open the shared fence", opened)) {
                return false;
            }

            p.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            p.device11 = device;
            return true;
        }


        // CPU wait for the D3D12 side; true if reached. The game's D3D11 queue
        // never waits on D3D12 on the GPU: a stalled D3D12 present (frame
        // generation pacing, a game hitch) must not freeze the game's GPU work.
        bool wait_for(uint64_t value, DWORD timeout_ms = 1000)
        {
            if (p.fence12->GetCompletedValue() >= value) {
                return true;
            }

            p.fence12->SetEventOnCompletion(value, p.fence_event);
            WaitForSingleObject(p.fence_event, timeout_ms);
            return p.fence12->GetCompletedValue() >= value;
        }


        void log_busy(const char* where)
        {
            static int count = 0;

            if (count < 20) {
                ++count;
                char msg[160];
                snprintf(msg, sizeof(msg), "EDAA FG: D3D12 side still busy at %s (frame %llu), skipped", where, static_cast<unsigned long long>(p.frames));
                reshade::log::message(reshade::log::level::warning, msg);
            }
        }


        // Shared texture and swap chain for this size and format.
        bool resize(uint32_t width, uint32_t height, DXGI_FORMAT format)
        {
            if (p.shared11 && p.width == width && p.height == height && p.format == format) {
                return true;
            }

            wait_for(p.fence_value); // nothing in flight reads the old resources
            p.shared12.Reset();
            p.shared11.Reset();

            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = width;
            desc.Height = height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = format;
            desc.SampleDesc.Count = 1;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;

            if (fail("cannot create the shared texture", p.device11_5->CreateTexture2D(&desc, nullptr, &p.shared11))) {
                return false;
            }

            ComPtr<IDXGIResource1> resource;
            HANDLE handle = nullptr;

            if (fail("cannot share the texture", p.shared11.As(&resource)) ||
                fail("cannot share the texture", resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle))) {
                return false;
            }

            const HRESULT opened = p.device12->OpenSharedHandle(handle, IID_PPV_ARGS(&p.shared12));
            CloseHandle(handle);

            if (fail("D3D12 cannot open the shared texture", opened)) {
                return false;
            }

            const HWND child = p.child.load();
            SetWindowPos(child, HWND_TOP, 0, 0, static_cast<int>(width), static_cast<int>(height), SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);

            const UINT flags = p.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

            if (p.swapchain) {
                if (fail("swap chain resize failed", p.swapchain->ResizeBuffers(buffer_count, width, height, format, flags))) {
                    return false;
                }
            }
            else {
                DXGI_SWAP_CHAIN_DESC1 sc = {};
                sc.Width = width;
                sc.Height = height;
                sc.Format = format;
                sc.SampleDesc.Count = 1;
                sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                sc.BufferCount = buffer_count;
                sc.Scaling = DXGI_SCALING_STRETCH;
                sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
                sc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
                sc.Flags = flags;
                ComPtr<IDXGISwapChain1> swapchain1;

                if (fail("D3D12 swap chain creation failed", p.factory->CreateSwapChainForHwnd(p.queue.Get(), child, &sc, nullptr, nullptr, &swapchain1)) ||
                    fail("D3D12 swap chain has no IDXGISwapChain3", swapchain1.As(&p.swapchain))) {
                    return false;
                }

                p.factory->MakeWindowAssociation(child, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
            }

            p.width = width;
            p.height = height;
            p.format = format;
            char msg[160];
            snprintf(msg, sizeof(msg), "EDAA FG test: presenting %ux%u through a D3D12 flip swap chain (tearing %s)", width, height, p.tearing ? "on" : "off");
            log_info(msg);
            return true;
        }


        // A D3D11 texture like 'source', shared with D3D12 (frame generation inputs).
        bool share_input(ID3D11Texture2D* source, ComPtr<ID3D11Texture2D>& t11, ComPtr<ID3D12Resource>& t12)
        {
            D3D11_TEXTURE2D_DESC src = {};
            source->GetDesc(&src);

            if (t11) {
                D3D11_TEXTURE2D_DESC cur = {};
                t11->GetDesc(&cur);

                if (cur.Width == src.Width && cur.Height == src.Height && cur.Format == src.Format) {
                    return true;
                }

                wait_for(p.fence_value);
                t12.Reset();
                t11.Reset();
            }

            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = src.Width;
            desc.Height = src.Height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = src.Format;
            desc.SampleDesc.Count = 1;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET; // motion: HUD protection draws into it
            desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;

            if (fail("cannot create a shared frame generation input", p.device11_5->CreateTexture2D(&desc, nullptr, &t11))) {
                return false;
            }

            ComPtr<IDXGIResource1> resource;
            HANDLE handle = nullptr;

            if (fail("cannot share a frame generation input", t11.As(&resource)) ||
                fail("cannot share a frame generation input", resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle))) {
                return false;
            }

            const HRESULT opened = p.device12->OpenSharedHandle(handle, IID_PPV_ARGS(&t12));
            CloseHandle(handle);
            return !fail("D3D12 cannot open a frame generation input", opened);
        }


        DXGI_FORMAT plain_format(DXGI_FORMAT format)
        {
            switch (format) {
                case DXGI_FORMAT_R8G8B8A8_TYPELESS:
                case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
                    return DXGI_FORMAT_R8G8B8A8_UNORM;
                case DXGI_FORMAT_B8G8R8A8_TYPELESS:
                case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
                    return DXGI_FORMAT_B8G8R8A8_UNORM;
                default:
                    return format;
            }
        }


        void present_frame(effect_runtime* runtime, int variant, bool generate, int fps_limit)
        {
            auto* const device = reinterpret_cast<ID3D11Device*>(runtime->get_device()->get_native());
            auto* const ctx = reinterpret_cast<ID3D11DeviceContext*>(runtime->get_command_queue()->get_immediate_command_list()->get_native());
            auto* const back_buffer = reinterpret_cast<ID3D11Texture2D*>(runtime->get_current_back_buffer().handle);

            if (device == nullptr || ctx == nullptr || back_buffer == nullptr) {
                return;
            }

            if (p.device11 != nullptr && p.device11 != device) {
                return; // another device (should not happen): leave it alone
            }

            D3D11_TEXTURE2D_DESC bb = {};
            back_buffer->GetDesc(&bb);

            if (p.device12 == nullptr) {
                p.parent = static_cast<HWND>(runtime->get_hwnd());
#if EDAA_SL
                p.sl_on = generate && sl_start();
#endif

                if (!create_factory(variant) || !create_device(device, fps_limit) || !create_child(p.parent, bb.Width, bb.Height)) {
                    return;
                }
            }

            if (!resize(bb.Width, bb.Height, plain_format(bb.Format))) {
                return;
            }

            step("resources ready");
            ComPtr<ID3D11DeviceContext4> ctx4;

            if (fail("Windows 10 1703 or newer needed (ID3D11DeviceContext4)", ctx->QueryInterface(IID_PPV_ARGS(&ctx4)))) {
                return;
            }

            // D3D11: wait until D3D12 read the previous copy, copy, signal.
            // D3D12 finished reading the previous copy (CPU wait, bounded).
            if (!wait_for(p.list_done, 100)) {
                log_busy("present");
                return;
            }

            ctx->CopyResource(p.shared11.Get(), back_buffer);
            const uint64_t ready = ++p.fence_value;
            ctx4->Signal(p.fence11.Get(), ready);
            ctx->Flush();
            step("D3D11 copy and signal submitted");

            // D3D12: wait for the copy, copy into the swap chain's buffer, present.
            const UINT index = p.swapchain->GetCurrentBackBufferIndex();
            wait_for(p.allocator_values[index]);
            step("back buffer index read, allocator free");
            ComPtr<ID3D12Resource> target;

            if (fail("cannot get the D3D12 back buffer", p.swapchain->GetBuffer(index, IID_PPV_ARGS(&target)))) {
                return;
            }

            step("D3D12 back buffer obtained");
            p.allocators[index]->Reset();
            p.list->Reset(p.allocators[index].Get(), nullptr);

#if EDAA_SL
            // DLSS-G inputs of this frame (copied by Streamline now, on this list).
            if (p.sl_on) {
                if (fps_limit != p.fps_limit) {
                    sl_reflex(fps_limit); // changed in the overlay
                }

                sl_begin_frame();
                sl_end_simulation();
                // Some frames have no resolve (menus, loading): keep DLSS-G on with the
                // last inputs (switching it off and on reallocates it, a long stall),
                // off only after a while without any.
                p.stale_frames = p.inputs_fresh || p.resolve_seen ? 0 : p.stale_frames + 1;
                const int set = p.ready_set;
                const bool on = generate && sl_api.token != nullptr && set >= 0 && p.depth12[set] && p.motion12[set] && p.inputs.render_w > 0 && p.stale_frames < 120;
                sl_options(on);

                if (on) {
                    p.inputs.reset = p.inputs.reset || !p.inputs_fresh; // last frame's inputs: no interpolation history
                    // Streamline copies the tagged inputs on this command list (eOnlyValidNow):
                    // the set is free once the list ran.
                    sl::Resource depth(sl::ResourceType::eTex2d, p.depth12[set].Get(), nullptr, nullptr, static_cast<uint32_t>(D3D12_RESOURCE_STATE_COMMON));
                    sl::Resource motion(sl::ResourceType::eTex2d, p.motion12[set].Get(), nullptr, nullptr, static_cast<uint32_t>(D3D12_RESOURCE_STATE_COMMON));
                    sl::Extent extent;
                    extent.width = p.inputs.render_w;
                    extent.height = p.inputs.render_h;
                    const sl::ResourceTag tags[] = {
                        sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow, &extent),
                        sl::ResourceTag(&motion, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow, &extent),
                    };
                    static bool tag_error_logged = false;
                    const sl::Result tagged = sl_api.set_tags(*sl_api.token, sl::ViewportHandle(0), tags, 2, p.list.Get());

                    if (tagged != sl::Result::eOk && !tag_error_logged) {
                        tag_error_logged = true;
                        sl_check("slSetTagForFrame", tagged);
                    }

                    sl_constants(p.inputs);
                }

                step("Streamline inputs tagged");
            }
#endif

            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = target.Get();
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            p.list->ResourceBarrier(1, &barrier);
            // The shared texture is in the common state: implicitly promoted to COPY_SOURCE.
            p.list->CopyResource(target.Get(), p.shared12.Get());
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            p.list->ResourceBarrier(1, &barrier);
            p.list->Close();
            step("D3D12 command list recorded");

            p.queue->Wait(p.fence12.Get(), ready);
            ID3D12CommandList* const lists[] = { p.list.Get() };
            p.queue->ExecuteCommandLists(1, lists);
            const uint64_t done = ++p.fence_value;
            p.queue->Signal(p.fence12.Get(), done);
            p.allocator_values[index] = done;
            p.list_done = done;

            if (p.ready_set >= 0) {
                p.set_done[p.ready_set] = done;

                if (p.inputs_fresh) {
                    p.write_set = 1 - p.ready_set; // next capture: the other set
                }
            }
            step("D3D12 work submitted, presenting");

#if EDAA_SL
            if (p.sl_on) {
                sl_marker(sl::PCLMarker::eRenderSubmitEnd);
                sl_marker(sl::PCLMarker::ePresentStart);
            }
#endif

            const HRESULT presented = p.swapchain->Present(0, p.tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);

            // The shared textures are free again once the present work (and frame generation) ran.
            p.copy_done = ++p.fence_value;
            p.queue->Signal(p.fence12.Get(), p.copy_done);
            p.inputs_fresh = false;
            p.resolve_seen = false;

#if EDAA_SL
            if (p.sl_on) {
                sl_marker(sl::PCLMarker::ePresentEnd);
                sl_api.token = nullptr;
                sl_log_state();
                sl_begin_frame(); // next frame: Reflex sleep, simulation start
            }
#endif

            if (FAILED(presented)) {
                fail("D3D12 present failed", presented);
                return;
            }

            LARGE_INTEGER now, freq;
            QueryPerformanceCounter(&now);
            QueryPerformanceFrequency(&freq);

            if (p.last_time.QuadPart != 0 && now.QuadPart > p.last_time.QuadPart) {
                const float fps = static_cast<float>(freq.QuadPart) / static_cast<float>(now.QuadPart - p.last_time.QuadPart);
                p.real_fps = p.real_fps <= 0 ? fps : p.real_fps * 0.95f + fps * 0.05f;
            }

            p.last_time = now;

            if (++p.frames == 1) {
                log_info("EDAA FG test: first frame presented through D3D12");
            }
        }


        void on_reshade_present_fg(effect_runtime* runtime)
        {
            bool enabled, generate, hud_protect;
            int variant, fps_limit;
            {
                addon_state& s = state();
                const std::lock_guard<std::recursive_mutex> lock(s.mutex);
                enabled = s.config.fg_present_test;
                generate = s.config.fg_generate;
                variant = s.config.fg_factory;
                fps_limit = s.config.fg_fps_limit;
                hud_protect = s.config.fg_hud_protect;
            }

            // The runtime ReShade adds on our D3D12 swap chain presents too: ignore it.
            if (runtime->get_device()->get_api() != device_api::d3d11) {
                return;
            }

            // HUD protection: the cockpit HUD stands still in the motion vectors of
            // this frame (before the shared fence is signalled in present_frame).
            // passes.cpp takes gpu_mutex: not with presenter_mutex held.
            ComPtr<ID3D11Texture2D> hud_target;

            if (enabled && generate && hud_protect) {
                const std::lock_guard<std::mutex> lock(presenter_mutex);

                if (p.sl_on && !p.failed && p.inputs_fresh && p.ready_set >= 0) {
                    hud_target = p.motion11[p.ready_set];
                }
            }

            bool hud_done = false;

            if (hud_target) {
                auto* const ctx = reinterpret_cast<ID3D11DeviceContext*>(runtime->get_command_queue()->get_immediate_command_list()->get_native());
                hud_done = fg::apply_hud_motion(ctx, hud_target.Get());
            }

            const std::lock_guard<std::mutex> lock(presenter_mutex);

            if (hud_target || !hud_protect || !generate) {
                p.hud_protected = hud_done;
            }

            if (!enabled || p.failed) {
                inputs_wanted = false;
                return;
            }

            try {
                present_frame(runtime, variant, generate, fps_limit);
                inputs_wanted = p.sl_on && generate && !p.failed;
                // The game's own present follows right after this event.
                serialize_presents = p.sl_on && !p.failed;
                serial_lock();
            }
            catch (...) {
                log_info("EDAA FG test: exception, presentation test stopped");
                p.failed = true;
            }
        }


        void on_destroy_device_fg(device* device)
        {
            const std::lock_guard<std::mutex> lock(presenter_mutex);

            if (p.device11 == nullptr || reinterpret_cast<ID3D11Device*>(device->get_native()) != p.device11) {
                return;
            }

            if (p.fence12) {
                wait_for(p.fence_value);
            }

            const HWND child = p.child.load();

            if (child != nullptr) {
                PostMessageW(child, WM_CLOSE, 0, 0);
                PostThreadMessageW(GetThreadId(p.window_thread.native_handle()), WM_QUIT, 0, 0);
            }

            if (p.window_thread.joinable()) {
                p.window_thread.detach();
            }

            if (p.fence_event != nullptr) {
                CloseHandle(p.fence_event);
            }

            inputs_wanted = false;
            serialize_presents = false;
            p.swapchain.Reset();
            p.factory.Reset();
            p.shared12.Reset();
            p.shared11.Reset();
            for (int i = 0; i < 2; ++i) {
                p.depth12[i].Reset();
                p.depth11[i].Reset();
                p.motion12[i].Reset();
                p.motion11[i].Reset();
                p.set_done[i] = 0;
            }

            p.write_set = 0;
            p.ready_set = -1;
            p.list_done = 0;
            p.inputs_fresh = false;
            p.list.Reset();

            for (ComPtr<ID3D12CommandAllocator>& allocator : p.allocators) {
                allocator.Reset();
            }

            p.queue.Reset();
            p.fence12.Reset();
            p.fence11.Reset();
            p.device12.Reset();
            p.device11_5.Reset();
            p.device11 = nullptr;
            p.fence_event = nullptr;
            p.child = nullptr;
            p.width = p.height = 0;
            p.frames = 0;

#if EDAA_SL
            if (p.sl_on) {
                sl_api.token = nullptr;
                sl_api.dlssg_on = false;
                sl_api.shutdown();
                p.sl_on = false;
            }
#endif
        }
    }


    namespace
    {
        void on_init_effect_runtime_fg(effect_runtime* runtime)
        {
            const HWND child = p.child.load();

            if (child != nullptr && static_cast<HWND>(runtime->get_hwnd()) == child) {
                runtime->set_effects_state(false); // effects already applied on the game's D3D11 image
                log_info("EDAA FG test: ReShade runtime on the D3D12 swap chain, effects disabled there");
            }
        }
    }


    fg::status fg::get_status()
    {
        const std::lock_guard<std::mutex> lock(presenter_mutex);
        status st;
        st.d3d12_path = p.swapchain != nullptr && !p.failed;
        st.streamline = p.sl_on;
        st.reflex = p.sl_on && p.fps_limit >= 0;
        st.background = !p.focus;
        st.waiting = p.stale_frames >= 120;
        st.real_fps = p.real_fps;
#if EDAA_SL
        st.generating = st.d3d12_path && p.sl_on && sl_api.dlssg_on && p.presented > 1;
#endif
        st.shown_fps = p.real_fps * static_cast<float>(st.generating ? p.presented : 1);
        st.dlssg_status = p.dlssg_status;
        st.hud_protected = p.hud_protected;
        st.error = p.error;
        return st;
    }


    bool fg::wants_inputs()
    {
        return inputs_wanted.load();
    }


    void fg::capture(ID3D11DeviceContext* ctx, ID3D11Texture2D* depth, ID3D11Texture2D* motion, const frame_inputs& in)
    {
        const std::lock_guard<std::mutex> lock(presenter_mutex);

        if (!p.sl_on || p.failed || p.device11_5 == nullptr || depth == nullptr || motion == nullptr) {
            return;
        }

        try {
            const int w = p.write_set;

            if (!share_input(depth, p.depth11[w], p.depth12[w]) || !share_input(motion, p.motion11[w], p.motion12[w])) {
                return;
            }

            // D3D12 finished reading this set (two frames ago): normally already
            // true. Else a short CPU wait (the GPU never waits), then skip.
            p.resolve_seen = true;

            if (!wait_for(p.set_done[w], 8)) {
                ++p.busy_skips;
                log_busy("input capture");
                return;
            }

            ctx->CopyResource(p.depth11[w].Get(), depth);
            ctx->CopyResource(p.motion11[w].Get(), motion);
            p.inputs = in;
            p.ready_set = w;
            p.inputs_fresh = true;
#if EDAA_SL
            sl_end_simulation();
#endif
        }
        catch (...) {
            log_info("EDAA FG: exception while capturing the inputs, frame generation stopped");
            p.failed = true;
        }
    }


    namespace
    {
        // Present of our D3D12 swap chain, from whichever thread Streamline uses.
        void on_present_fg(command_queue*, swapchain* swapchain, const rect*, const rect*, uint32_t, const rect*)
        {
            const HWND child = p.child.load();

            if (child != nullptr && static_cast<HWND>(swapchain->get_hwnd()) == child) {
                serial_lock();
            }
        }


        void on_finish_present_fg(command_queue*, swapchain*)
        {
            serial_unlock();
        }
    }


    void register_fg_events()
    {
        reshade::register_event<reshade::addon_event::present>(on_present_fg);
        reshade::register_event<reshade::addon_event::finish_present>(on_finish_present_fg);
        reshade::register_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime_fg);
        reshade::register_event<reshade::addon_event::reshade_present>(on_reshade_present_fg);
        reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device_fg);
    }


    void unregister_fg_events()
    {
        reshade::unregister_event<reshade::addon_event::destroy_device>(on_destroy_device_fg);
        reshade::unregister_event<reshade::addon_event::reshade_present>(on_reshade_present_fg);
        reshade::unregister_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime_fg);
        reshade::unregister_event<reshade::addon_event::finish_present>(on_finish_present_fg);
        reshade::unregister_event<reshade::addon_event::present>(on_present_fg);
    }
}
