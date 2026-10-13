// Intel XeSS (Native AA) through a D3D12 bridge. See xess.hpp and docs/addon.md.
//
// Per frame, on the game's immediate D3D11 context and a D3D12 queue of our
// own on the same GPU:
//   1. D3D11 copies colour, depth (R32_FLOAT) and motion vectors into textures
//      it shares with D3D12 (NT handles), signals a shared fence and flushes;
//   2. D3D12 waits for that fence, runs XeSS (inputs in the non-pixel shader
//      resource state, output in the UAV state, back to COMMON afterwards),
//      signals the fence;
//   3. D3D11 waits for it and copies the shared output into in.output.
// libxess.dll is delay-loaded from the add-on folder: without it the add-on
// still loads and XeSS reports itself unavailable.

#include "xess.hpp"

#include "state.hpp"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <Windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#if EDAA_XESS
#include <xess/xess_d3d12.h>
#endif

using Microsoft::WRL::ComPtr;

namespace edaa::xess
{
#if EDAA_XESS
    namespace
    {
        std::atomic<int> probed{ -1 }; // probe() result
        std::mutex status_mutex; // the overlay reads the status from its own thread
        std::string status_text = "not used yet";


        void set_status(const std::string& text)
        {
            const std::lock_guard<std::mutex> lock(status_mutex);
            status_text = text;
        }


        const char* result_name(xess_result_t result)
        {
            switch (result) {
                case XESS_RESULT_ERROR_UNSUPPORTED_DEVICE: return "unsupported GPU";
                case XESS_RESULT_ERROR_UNSUPPORTED_DRIVER: return "driver too old";
                case XESS_RESULT_ERROR_DEVICE_OUT_OF_MEMORY: return "out of GPU memory";
                case XESS_RESULT_ERROR_CANT_LOAD_LIBRARY: return "cannot load a XeSS library";
                case XESS_RESULT_ERROR_INVALID_ARGUMENT: return "invalid argument";
                default: return "error";
            }
        }


        bool failed(const char* what, xess_result_t result)
        {
            if (result >= XESS_RESULT_SUCCESS) {
                return false;
            }

            char text[160];
            snprintf(text, sizeof(text), "%s: %s (XeSS %d)", what, result_name(result), static_cast<int>(result));
            set_status(text);
            return true;
        }


        bool failed(const char* what, HRESULT hr)
        {
            if (SUCCEEDED(hr)) {
                return false;
            }

            char text[160];
            snprintf(text, sizeof(text), "%s (HRESULT 0x%08x)", what, static_cast<unsigned>(hr));
            set_status(text);
            return true;
        }


        // A texture created by D3D12 and opened by the game's D3D11 device.
        struct shared_texture
        {
            ComPtr<ID3D12Resource> d3d12;
            ComPtr<ID3D11Texture2D> d3d11;
        };


        struct bridge
        {
            ID3D11Device* device11 = nullptr; // the game's device, not owned
            ComPtr<ID3D11Device5> device11_5;
            ComPtr<ID3D12Device> device12;
            ComPtr<ID3D12CommandQueue> queue;
            ComPtr<ID3D12CommandAllocator> allocators[2];
            ComPtr<ID3D12GraphicsCommandList> list;
            ComPtr<ID3D12Fence> fence12;
            ComPtr<ID3D11Fence> fence11; // the same fence, opened by D3D11
            HANDLE fence_event = nullptr;
            uint64_t fence_value = 0;
            uint64_t allocator_done[2] = {}; // fence value after which each allocator can be reset
            uint32_t frame = 0;

            shared_texture color, depth, motion, output;
            uint32_t width = 0, height = 0;
            xess_context_handle_t context = nullptr;
        };

        bridge b;
        bool init_failed = false;


        // CPU wait until D3D12 reached 'value' on the shared fence.
        void wait_for(uint64_t value)
        {
            if (b.fence12 && b.fence12->GetCompletedValue() < value) {
                b.fence12->SetEventOnCompletion(value, b.fence_event);
                WaitForSingleObject(b.fence_event, 5000);
            }
        }


        void shutdown()
        {
            wait_for(b.fence_value);

            if (b.context != nullptr) {
                xessDestroyContext(b.context);
            }

            if (b.fence_event != nullptr) {
                CloseHandle(b.fence_event);
            }

            b = {};
        }


        bool library_loaded()
        {
            // Delay-loaded: loading it here first gives a clear message if it is missing
            // (or its Visual C++ runtime is), instead of a delay-load exception.
            const std::wstring path = addon_folder() + L"\\libxess.dll";

            if (LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH) == nullptr) {
                set_status("libxess.dll not found next to EDAA.addon64 (or Visual C++ 2015-2022 runtime missing)");
                return false;
            }

            return true;
        }


        // D3D12 device on the game's GPU, queue, command list, shared fence, XeSS context.
        bool create_bridge(ID3D11Device* device)
        {
            if (!library_loaded()) {
                return false;
            }

            if (failed("Windows 10 1703 or newer needed (ID3D11Device5)", device->QueryInterface(IID_PPV_ARGS(&b.device11_5)))) {
                return false;
            }

            ComPtr<IDXGIDevice> dxgi;
            ComPtr<IDXGIAdapter> adapter;

            if (failed("cannot get the game's GPU", device->QueryInterface(IID_PPV_ARGS(&dxgi))) ||
                failed("cannot get the game's GPU", dxgi->GetAdapter(&adapter))) {
                return false;
            }

            if (failed("cannot create a D3D12 device on the game's GPU",
                    D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&b.device12)))) {
                return false;
            }

            D3D12_COMMAND_QUEUE_DESC queue = {};
            queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

            if (failed("D3D12 queue creation failed", b.device12->CreateCommandQueue(&queue, IID_PPV_ARGS(&b.queue))) ||
                failed("D3D12 allocator creation failed", b.device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&b.allocators[0]))) ||
                failed("D3D12 allocator creation failed", b.device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&b.allocators[1]))) ||
                failed("D3D12 command list creation failed",
                    b.device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, b.allocators[0].Get(), nullptr, IID_PPV_ARGS(&b.list)))) {
                return false;
            }

            b.list->Close();

            // Shared fence: D3D12 creates it, D3D11 opens it.
            HANDLE handle = nullptr;

            if (failed("shared fence creation failed", b.device12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&b.fence12))) ||
                failed("shared fence creation failed", b.device12->CreateSharedHandle(b.fence12.Get(), nullptr, GENERIC_ALL, nullptr, &handle))) {
                return false;
            }

            const HRESULT opened = b.device11_5->OpenSharedFence(handle, IID_PPV_ARGS(&b.fence11));
            CloseHandle(handle);

            if (failed("D3D11 cannot open the shared fence", opened)) {
                return false;
            }

            b.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

            if (failed("XeSS context creation failed", xessD3D12CreateContext(b.device12.Get(), &b.context))) {
                b.context = nullptr;
                return false;
            }

            b.device11 = device;
            reshade::log::message(reshade::log::level::info, "EDAA: Intel XeSS available (D3D12 bridge)");
            return true;
        }


        bool init(ID3D11Device* device)
        {
            if (b.device11 == device && b.context != nullptr) {
                return true;
            }

            if (b.device11 != nullptr) {
                shutdown();
                init_failed = false;
            }

            if (init_failed) {
                return false; // do not retry every frame
            }

            if (!create_bridge(device)) {
                shutdown(); // whatever was created before the failure
                init_failed = true;
                return false;
            }

            return true;
        }


        // Created by D3D11 (shared NT handle) and opened by D3D12: the direction
        // drivers support best (D3D11 refused D3D12-created textures with E_INVALIDARG).
        bool create_shared(const char* name, DXGI_FORMAT format, UINT bind_flags, shared_texture& t)
        {
            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = b.width;
            desc.Height = b.height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = format;
            desc.SampleDesc.Count = 1;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | bind_flags;
            desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
            char what[96];
            snprintf(what, sizeof(what), "cannot create the shared %s texture", name);

            if (failed(what, b.device11_5->CreateTexture2D(&desc, nullptr, &t.d3d11))) {
                return false;
            }

            ComPtr<IDXGIResource1> resource;
            HANDLE handle = nullptr;

            if (failed(what, t.d3d11.As(&resource)) ||
                failed(what, resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle))) {
                return false;
            }

            const HRESULT opened = b.device12->OpenSharedHandle(handle, IID_PPV_ARGS(&t.d3d12));
            CloseHandle(handle);
            snprintf(what, sizeof(what), "D3D12 cannot open the shared %s texture", name);
            return !failed(what, opened);
        }


        // Shared textures and XeSS initialisation for the render size.
        bool resize(uint32_t width, uint32_t height)
        {
            if (b.width == width && b.height == height && b.output.d3d11) {
                return true;
            }

            wait_for(b.fence_value); // XeSS must be idle before it is initialised again
            b.color = b.depth = b.motion = b.output = {};
            b.width = width;
            b.height = height;

            if (!create_shared("colour", DXGI_FORMAT_R11G11B10_FLOAT, 0, b.color) ||
                !create_shared("depth", DXGI_FORMAT_R32_FLOAT, 0, b.depth) ||
                !create_shared("motion", DXGI_FORMAT_R16G16_FLOAT, 0, b.motion) ||
                !create_shared("output", DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_UNORDERED_ACCESS, b.output)) {
                b.width = b.height = 0;
                return false;
            }

            // Native AA (1.0x), HDR colour before exposure (auto exposure),
            // reverse-Z depth, motion vectors at render size (in UV: scaled to pixels).
            xess_d3d12_init_params_t params = {};
            params.outputResolution = { width, height };
            params.qualitySetting = XESS_QUALITY_SETTING_AA;
            params.initFlags = XESS_INIT_FLAG_INVERTED_DEPTH | XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE;

            if (failed("XeSS initialisation failed", xessD3D12Init(b.context, &params)) ||
                failed("XeSS velocity scale", xessSetVelocityScale(b.context, static_cast<float>(width), static_cast<float>(height)))) {
                b.width = b.height = 0;
                return false;
            }

            char text[96];
            snprintf(text, sizeof(text), "EDAA: XeSS Native AA initialised for %ux%u", width, height);
            reshade::log::message(reshade::log::level::info, text);
            return true;
        }


        void transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to, D3D12_RESOURCE_BARRIER& barrier)
        {
            barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = resource;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = from;
            barrier.Transition.StateAfter = to;
        }


        // Input and output textures between COMMON (shared with D3D11) and the XeSS states.
        void transitions(bool to_xess)
        {
            const D3D12_RESOURCE_STATES input = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            const D3D12_RESOURCE_STATES common = D3D12_RESOURCE_STATE_COMMON;
            D3D12_RESOURCE_BARRIER barriers[4];
            transition(b.color.d3d12.Get(), to_xess ? common : input, to_xess ? input : common, barriers[0]);
            transition(b.depth.d3d12.Get(), to_xess ? common : input, to_xess ? input : common, barriers[1]);
            transition(b.motion.d3d12.Get(), to_xess ? common : input, to_xess ? input : common, barriers[2]);
            transition(b.output.d3d12.Get(), to_xess ? common : D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                to_xess ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : common, barriers[3]);
            b.list->ResourceBarrier(4, barriers);
        }
    }


    bool evaluate(ID3D11Device* device, ID3D11DeviceContext* ctx, const upscaler_inputs& in)
    {
        if (!init(device)) {
            return false;
        }

        if (!resize(in.width, in.height)) {
            shutdown(); // keep the error in the status, do not retry every frame
            init_failed = true;
            return false;
        }

        ComPtr<ID3D11DeviceContext4> ctx4;

        if (failed("Windows 10 1703 or newer needed (ID3D11DeviceContext4)", ctx->QueryInterface(IID_PPV_ARGS(&ctx4)))) {
            return false;
        }

        // 1. D3D11: inputs into the shared textures, then hand over to D3D12.
        ctx->CopyResource(b.color.d3d11.Get(), in.color);
        ctx->CopyResource(b.depth.d3d11.Get(), in.depth);
        ctx->CopyResource(b.motion.d3d11.Get(), in.motion);
        const uint64_t inputs_ready = ++b.fence_value;
        ctx4->Signal(b.fence11.Get(), inputs_ready);
        ctx->Flush();

        // 2. D3D12: XeSS on our queue, after the D3D11 copies.
        const uint32_t slot = b.frame++ % 2;
        wait_for(b.allocator_done[slot]);
        b.allocators[slot]->Reset();
        b.list->Reset(b.allocators[slot].Get(), nullptr);
        transitions(true);

        xess_d3d12_execute_params_t params = {};
        params.pColorTexture = b.color.d3d12.Get();
        params.pVelocityTexture = b.motion.d3d12.Get();
        params.pDepthTexture = b.depth.d3d12.Get();
        params.pOutputTexture = b.output.d3d12.Get();
        params.jitterOffsetX = in.jitter_x;
        params.jitterOffsetY = in.jitter_y;
        params.exposureScale = 1.0f;
        params.resetHistory = in.reset ? 1 : 0;
        params.inputWidth = in.width;
        params.inputHeight = in.height;
        const xess_result_t result = xessD3D12Execute(b.context, b.list.Get(), &params);

        transitions(false);
        b.list->Close();
        b.queue->Wait(b.fence12.Get(), inputs_ready);
        ID3D12CommandList* const lists[] = { b.list.Get() };
        b.queue->ExecuteCommandLists(1, lists);
        const uint64_t output_ready = ++b.fence_value;
        b.queue->Signal(b.fence12.Get(), output_ready);
        b.allocator_done[slot] = output_ready;

        // 3. D3D11: wait for XeSS on the GPU, take the output.
        ctx4->Wait(b.fence11.Get(), output_ready);

        if (failed("XeSS execution failed", result)) {
            return false;
        }

        ctx->CopyResource(in.output, b.output.d3d11.Get());
        set_status("running (D3D12 bridge)");
        return true;
    }


    void probe(ID3D11Device* device)
    {
        // Creates the bridge and a XeSS context once, then frees them: a D3D12
        // device should not stay around for players who do not use XeSS.
        probed = init(device) ? 1 : 0;

        if (probed == 1) {
            shutdown();
            set_status("available (D3D12 bridge)");
        }
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
        if (b.device11 != nullptr && (device == nullptr || device == b.device11)) {
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
        return "this build has no XeSS (XeSS SDK not found at build time)";
    }


    void release(ID3D11Device*)
    {
    }
#endif
}
