// Recording of the anti-aliasing inputs for offline replay (recorder.hpp,
// docs/recording.md). Threads: the render thread copies and maps (D3D11
// immediate context, gpu_mutex held), one worker thread copies the mapped
// memory into RAM and, after the stop, writes the files. The worker holds a
// reference on the add-on module and leaves with FreeLibraryAndExitThread, so
// an unload never pulls its code away.

#include "recorder.hpp"

#include "state.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace edaa::recorder
{
    namespace
    {
        constexpr int ring_size = 8;
        constexpr int map_delay = 2; // frames between the copy and the first Map attempt
        constexpr uint32_t crop_w_default = 1600, crop_h_default = 900;

        // Planes in file order: color, depth, motion, mask, reactive.
        constexpr int plane_count = 5;
        const char* const plane_names[plane_count] = { "color", "depth", "motion", "mask", "reactive" };
        constexpr uint32_t tonemap_bytes = 256; // cb2 (16 float4)
        constexpr uint32_t luminance_bytes = 16; // first texels of the luminance texture

        enum class phase_t { idle, recording, stopping, writing };

        enum class slot_state { free, copied, mapped };

        struct slot
        {
            ComPtr<ID3D11Texture2D> planes[plane_count];
            ComPtr<ID3D11Texture2D> luminance;
            ComPtr<ID3D11Buffer> tonemap;
            slot_state state = slot_state::free;
            uint64_t copied_at = 0;
            frame_params params;
            bool has_reactive = false, has_tonemap = false;
            D3D11_MAPPED_SUBRESOURCE maps[plane_count + 2] = {};
            std::atomic<bool> worker_done = false;
        };

        struct recorded_frame
        {
            frame_params params;
            std::vector<uint8_t> planes[plane_count];
            uint8_t tonemap[tonemap_bytes + luminance_bytes] = {};
        };

        // Render thread only (gpu_mutex held).
        struct ring_t
        {
            slot slots[ring_size];
            uint32_t crop_w = 0, crop_h = 0, color_format = 0;
            uint32_t tonemap_size = 0; // ByteWidth of the tonemap staging buffers
            D3D11_TEXTURE2D_DESC luminance_desc = {};
            uint64_t counter = 0;
            bool gap = false;
        } ring;

        std::atomic<phase_t> phase = phase_t::idle;
        std::atomic<bool> toggle_requested = false;

        // Worker queue and the recording in memory.
        std::mutex mutex;
        std::condition_variable wake;
        std::deque<int> jobs; // slot indices, mapped
        std::vector<std::unique_ptr<recorded_frame>> frames;
        uint64_t bytes = 0, byte_limit = 0;
        uint32_t dropped = 0;
        bool limit_reached = false;
        bool worker_running = false;
        std::wstring folder_base; // <capture folder>\recordings
        std::wstring folder; // this recording's folder
        std::string message; // last status line when idle
        float write_progress = 0;
        double started = 0, last_time = 0;
        std::atomic<double> start_at = 0; // countdown (record_delay_s): start time, 0 = none
        double finished_at = 0; // when the last recording was saved or failed (on-screen line)
        bool finished_ok = false;
        uint32_t rec_render_w = 0, rec_render_h = 0, rec_crop_x = 0, rec_crop_y = 0, rec_crop_w = 0, rec_crop_h = 0, rec_color_format = 0;


        double now()
        {
            LARGE_INTEGER f, c;
            QueryPerformanceFrequency(&f);
            QueryPerformanceCounter(&c);
            return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
        }


        uint32_t bytes_per_pixel(uint32_t format)
        {
            switch (static_cast<DXGI_FORMAT>(format)) {
                case DXGI_FORMAT_R11G11B10_FLOAT:
                case DXGI_FORMAT_R8G8B8A8_UNORM:
                case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
                case DXGI_FORMAT_B8G8R8A8_UNORM:
                case DXGI_FORMAT_R10G10B10A2_UNORM:
                case DXGI_FORMAT_R32_FLOAT:
                case DXGI_FORMAT_R16G16_FLOAT:
                    return 4;
                case DXGI_FORMAT_R16G16B16A16_FLOAT:
                    return 8;
                case DXGI_FORMAT_R8_UNORM:
                    return 1;
                default:
                    return 0;
            }
        }


        uint32_t plane_format(int plane)
        {
            switch (plane) {
                case 0: return ring.color_format;
                case 1: return DXGI_FORMAT_R32_FLOAT;
                case 2: return DXGI_FORMAT_R16G16_FLOAT;
                default: return DXGI_FORMAT_R8_UNORM;
            }
        }


        std::wstring widen(const std::string& s)
        {
            if (s.empty()) {
                return {};
            }

            const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
            std::wstring w(static_cast<size_t>(n), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
            return w;
        }


        std::string narrow(const std::wstring& w)
        {
            if (w.empty()) {
                return {};
            }

            const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
            std::string s(static_cast<size_t>(n), '\0');
            WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
            return s;
        }


        void log(const std::string& text)
        {
            reshade::log::message(reshade::log::level::info, ("EDAA recorder: " + text).c_str());
        }


        DWORD WINAPI beep_thread(LPVOID param)
        {
            Beep(static_cast<DWORD>(reinterpret_cast<uintptr_t>(param)), 150);
            HMODULE self = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&beep_thread), &self);
            FreeLibraryAndExitThread(self, 0);
        }


        // Audible tone (MessageBeep stays silent with the Windows sound scheme
        // off): Beep() blocks for its length, so it runs on its own thread, which
        // holds a reference on the add-on until it is done.
        void beep(DWORD frequency)
        {
            HMODULE self = nullptr;

            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&beep_thread), &self)) {
                return;
            }

            HANDLE thread = CreateThread(nullptr, 0, beep_thread, reinterpret_cast<LPVOID>(static_cast<uintptr_t>(frequency)), 0, nullptr);

            if (thread != nullptr) {
                CloseHandle(thread);
            }
            else {
                FreeLibrary(self);
            }
        }


        // Copies one mapped slot into a recorded frame (worker thread).
        std::unique_ptr<recorded_frame> take(slot& sl)
        {
            auto f = std::make_unique<recorded_frame>();
            f->params = sl.params;

            for (int p = 0; p < plane_count; ++p) {
                const uint32_t bpp = bytes_per_pixel(plane_format(p));
                const size_t row = size_t(ring.crop_w) * bpp;
                f->planes[p].resize(row * ring.crop_h);

                if (p == 4 && !sl.has_reactive) {
                    continue; // zeros
                }

                const auto* src = static_cast<const uint8_t*>(sl.maps[p].pData);

                for (uint32_t y = 0; y < ring.crop_h; ++y) {
                    std::memcpy(f->planes[p].data() + y * row, src + size_t(y) * sl.maps[p].RowPitch, row);
                }
            }

            if (sl.has_tonemap) {
                const uint32_t offset = static_cast<uint32_t>(sl.params.reserved[0]);
                std::memcpy(f->tonemap, static_cast<const uint8_t*>(sl.maps[plane_count + 1].pData) + offset, tonemap_bytes);
                std::memcpy(f->tonemap + tonemap_bytes, sl.maps[plane_count].pData, luminance_bytes);
            }

            return f;
        }


        bool write_all(std::string& error)
        {
            namespace fs = std::filesystem;
            std::vector<std::unique_ptr<recorded_frame>> list;
            {
                const std::lock_guard<std::mutex> lock(mutex);
                list.swap(frames);
            }

            if (list.empty()) {
                error = "nothing recorded";
                return false;
            }

            std::error_code ec;
            fs::create_directories(folder, ec);

            if (ec) {
                error = "cannot create " + narrow(folder);
                return false;
            }

            uint64_t total = 0;

            for (const auto& f : list) {
                for (const auto& p : f->planes) {
                    total += p.size();
                }
            }

            const fs::space_info space = fs::space(folder, ec);

            if (!ec && space.available < total + (512ull << 20)) {
                error = "not enough disk space (" + std::to_string(total >> 20) + " MB needed)";
                fs::remove(folder, ec); // the empty folder just made
                return false;
            }

            const fs::path dir(folder);
            const size_t steps = list.size() * (plane_count + 2);
            size_t done = 0;

            auto write_stream = [&](const char* name, auto&& part) {
                std::ofstream out(dir / name, std::ios::binary);

                for (const auto& f : list) {
                    part(out, *f);
                    ++done;
                    const std::lock_guard<std::mutex> lock(mutex);
                    write_progress = static_cast<float>(done) / static_cast<float>(steps);
                }

                return static_cast<bool>(out);
            };

            bool ok = write_stream("params.bin", [](std::ofstream& o, const recorded_frame& f) {
                o.write(reinterpret_cast<const char*>(&f.params), sizeof(f.params));
            });
            ok = write_stream("tonemap.bin", [](std::ofstream& o, const recorded_frame& f) {
                o.write(reinterpret_cast<const char*>(f.tonemap), sizeof(f.tonemap));
            }) && ok;

            for (int p = 0; p < plane_count; ++p) {
                const std::string name = std::string(plane_names[p]) + ".bin";
                ok = write_stream(name.c_str(), [p](std::ofstream& o, const recorded_frame& f) {
                    o.write(reinterpret_cast<const char*>(f.planes[p].data()), static_cast<std::streamsize>(f.planes[p].size()));
                }) && ok;
            }

            // record.json: what the binary files hold.
            std::ofstream json(dir / "record.json");
            const double duration = list.back()->params.time - list.front()->params.time;
            json << "{\n"
                 << "  \"format\": \"edaa-record-1\",\n"
                 << "  \"addon_version\": \"" << version << "\",\n"
                 << "  \"build\": \"" << build_stamp << "\",\n"
                 << "  \"frames\": " << list.size() << ",\n"
                 << "  \"dropped\": " << dropped << ",\n"
                 << "  \"duration_s\": " << duration << ",\n"
                 << "  \"render_size\": [" << rec_render_w << ", " << rec_render_h << "],\n"
                 << "  \"crop\": [" << rec_crop_x << ", " << rec_crop_y << ", " << rec_crop_w << ", " << rec_crop_h << "],\n"
                 << "  \"params_bytes\": " << sizeof(frame_params) << ",\n"
                 << "  \"tonemap_bytes\": " << tonemap_bytes + luminance_bytes << ",\n"
                 << "  \"planes\": [\n";

            for (int p = 0; p < plane_count; ++p) {
                const uint32_t format = p == 0 ? rec_color_format : plane_format(p);
                json << "    {\"name\": \"" << plane_names[p] << "\", \"dxgi_format\": " << format << ", \"bytes_per_pixel\": "
                     << bytes_per_pixel(format) << "}" << (p + 1 < plane_count ? "," : "") << "\n";
            }

            json << "  ]\n}\n";
            ok = ok && static_cast<bool>(json);

            if (!ok) {
                error = "write error in " + narrow(folder);
            }

            return ok;
        }


        // Worker thread: copies mapped slots into memory, then writes the files.
        DWORD WINAPI worker(LPVOID module)
        {
            for (;;) {
                int job = -1;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    wake.wait_for(lock, std::chrono::milliseconds(50), [] { return !jobs.empty() || phase.load() == phase_t::writing; });

                    if (!jobs.empty()) {
                        job = jobs.front();
                        jobs.pop_front();
                    }
                }

                if (job >= 0) {
                    slot& sl = ring.slots[job];
                    auto f = take(sl);
                    uint64_t size = 0;

                    for (const auto& p : f->planes) {
                        size += p.size();
                    }
                    {
                        const std::lock_guard<std::mutex> lock(mutex);
                        last_time = f->params.time;
                        frames.push_back(std::move(f));
                        bytes += size;

                        if (bytes >= byte_limit) {
                            limit_reached = true;
                        }
                    }
                    sl.worker_done.store(true);
                    continue;
                }

                if (phase.load() == phase_t::writing) {
                    std::string error;
                    const bool ok = write_all(error);
                    {
                        const std::lock_guard<std::mutex> lock(mutex);
                        message = ok ? "saved to " + narrow(folder) : "not saved: " + error;
                        finished_at = now();
                        finished_ok = ok;
                        frames.clear();
                        bytes = 0;
                        worker_running = false;
                    }
                    log(ok ? "saved " + narrow(folder) : "not saved: " + error);
                    Beep(ok ? 660 : 220, ok ? 150 : 600); // this thread, before it lets the add-on go
                    phase.store(phase_t::idle);
                    FreeLibraryAndExitThread(static_cast<HMODULE>(module), 0);
                }
            }
        }


        bool start_worker()
        {
            HMODULE module = nullptr;

            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&worker), &module)) {
                return false;
            }

            HANDLE thread = CreateThread(nullptr, 0, worker, module, 0, nullptr);

            if (thread == nullptr) {
                FreeLibrary(module);
                return false;
            }

            SetThreadPriority(thread, THREAD_PRIORITY_BELOW_NORMAL);
            CloseHandle(thread);
            return true;
        }


        void reset_ring()
        {
            for (slot& sl : ring.slots) {
                for (auto& p : sl.planes) {
                    p.Reset();
                }

                sl.luminance.Reset();
                sl.tonemap.Reset();
                sl.state = slot_state::free;
            }

            ring.crop_w = ring.crop_h = ring.color_format = ring.tonemap_size = 0;
            ring.luminance_desc = {};
        }


        bool ring_idle()
        {
            for (const slot& sl : ring.slots) {
                if (sl.state != slot_state::free) {
                    return false;
                }
            }

            return true;
        }


        bool create_staging(ID3D11Device* device, uint32_t w, uint32_t h, uint32_t format, ComPtr<ID3D11Texture2D>& tex)
        {
            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = w;
            desc.Height = h;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = static_cast<DXGI_FORMAT>(format);
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            return SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &tex));
        }


        // Unmaps the slot's mapped resources (render thread).
        void unmap(ID3D11DeviceContext* ctx, slot& sl, int count)
        {
            for (int p = 0; p < count && p < plane_count; ++p) {
                ctx->Unmap(sl.planes[p].Get(), 0);
            }

            if (count > plane_count && sl.has_tonemap) {
                ctx->Unmap(sl.luminance.Get(), 0);
            }

            if (count > plane_count + 1 && sl.has_tonemap) {
                ctx->Unmap(sl.tonemap.Get(), 0);
            }
        }


        // Maps the copied slots that are old enough, unmaps those the worker is done with.
        void service(ID3D11DeviceContext* ctx)
        {
            for (int i = 0; i < ring_size; ++i) {
                slot& sl = ring.slots[i];

                if (sl.state == slot_state::mapped && sl.worker_done.load()) {
                    unmap(ctx, sl, plane_count + 2);
                    sl.state = slot_state::free;
                }
            }

            // Oldest first, so the frames reach the worker in order.
            for (;;) {
                int oldest = -1;

                for (int i = 0; i < ring_size; ++i) {
                    const slot& sl = ring.slots[i];

                    if (sl.state == slot_state::copied && sl.copied_at + map_delay <= ring.counter &&
                        (oldest < 0 || sl.copied_at < ring.slots[oldest].copied_at)) {
                        oldest = i;
                    }
                }

                if (oldest < 0) {
                    return;
                }

                slot& sl = ring.slots[oldest];
                int mapped = 0; // resources mapped so far, in unmap()'s order
                bool ok = true;

                for (int p = 0; p < plane_count && ok; ++p) {
                    ok = SUCCEEDED(ctx->Map(sl.planes[p].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &sl.maps[p]));
                    mapped += ok ? 1 : 0;
                }

                if (ok && sl.has_tonemap) {
                    ok = SUCCEEDED(ctx->Map(sl.luminance.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &sl.maps[plane_count]));
                    mapped += ok ? 1 : 0;

                    if (ok) {
                        ok = SUCCEEDED(ctx->Map(sl.tonemap.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &sl.maps[plane_count + 1]));
                        mapped += ok ? 1 : 0;
                    }
                }

                if (!ok) {
                    // Still on the GPU: try again next frame.
                    unmap(ctx, sl, mapped);
                    return;
                }

                sl.worker_done.store(false);
                sl.state = slot_state::mapped;
                {
                    const std::lock_guard<std::mutex> lock(mutex);
                    jobs.push_back(oldest);
                }
                wake.notify_one();
            }
        }
    }


    void toggle()
    {
        // The folder is read here: frame() runs with gpu_mutex held and must not take the state lock.
        std::string base;
        float delay = 0;
        {
            addon_state& s = state();
            const std::lock_guard<std::recursive_mutex> lock(s.mutex);
            base = s.capture_path;
            delay = std::clamp(s.config.record_delay_s, 0.0f, 10.0f);
        }
        {
            const std::lock_guard<std::mutex> lock(mutex);
            folder_base = widen(base) + L"\\recordings";
        }

        // Countdown running: a second press cancels it.
        if (start_at.load() > 0) {
            start_at.store(0);
            const std::lock_guard<std::mutex> lock(mutex);
            message = "start cancelled";
            log("start cancelled");
            return;
        }

        // Start after the delay (frame() starts it), stop at once.
        if (phase.load() == phase_t::idle && delay > 0) {
            start_at.store(now() + delay);
            log("starting in " + std::to_string(static_cast<int>(std::ceil(delay))) + " s");
            return;
        }

        toggle_requested.store(true);
    }


    bool active()
    {
        return phase.load() == phase_t::recording;
    }


    std::string status()
    {
        const phase_t p = phase.load();
        const std::lock_guard<std::mutex> lock(mutex);
        char text[256];

        switch (p) {
            case phase_t::recording:
            case phase_t::stopping:
                snprintf(text, sizeof(text), "%s %.1f s, %zu frames, %.2f GB of %.1f (%u dropped)", p == phase_t::recording ? "REC" : "stopping",
                    last_time, frames.size(), static_cast<double>(bytes) / 1073741824.0, static_cast<double>(byte_limit) / 1073741824.0, dropped);
                return text;
            case phase_t::writing:
                snprintf(text, sizeof(text), "writing %d %% to %s", static_cast<int>(write_progress * 100.0f), narrow(folder).c_str());
                return text;
            default:
                if (start_at.load() > 0) {
                    snprintf(text, sizeof(text), "starting in %.1f s (Ctrl+Shift+R cancels)", std::max(0.0, start_at.load() - now()));
                    return text;
                }

                return message;
        }
    }


    std::string osd(bool& alert)
    {
        const phase_t p = phase.load();
        const double t = now();
        char text[160];
        alert = true;

        if (p == phase_t::idle && start_at.load() > 0) {
            snprintf(text, sizeof(text), "REC in %d", static_cast<int>(std::ceil(std::max(0.0, start_at.load() - t))));
            return text;
        }

        const std::lock_guard<std::mutex> lock(mutex);

        switch (p) {
            case phase_t::recording:
                snprintf(text, sizeof(text), "REC %.1f s", last_time);
                return text;
            case phase_t::stopping:
            case phase_t::writing:
                alert = false;
                snprintf(text, sizeof(text), "saving %d %%", static_cast<int>(write_progress * 100.0f));
                return text;
            default:
                if (finished_at > 0 && t - finished_at < 5.0) {
                    alert = !finished_ok;
                    return finished_ok ? std::string("saved") : message;
                }

                return {};
        }
    }


    void frame(ID3D11Device* device, ID3D11DeviceContext* ctx, const sources& in, frame_params params, bool full_frame, uint32_t max_mb)
    {
        // Countdown over: start now.
        if (const double at = start_at.load(); at > 0 && now() >= at && phase.load() == phase_t::idle) {
            start_at.store(0);
            toggle_requested.store(true);
        }

        if (toggle_requested.exchange(false)) {
            const phase_t p = phase.load();

            if (p == phase_t::idle) {
                SYSTEMTIME t;
                GetLocalTime(&t);
                wchar_t name[32];
                swprintf(name, 32, L"%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
                bool ok;
                {
                    const std::lock_guard<std::mutex> lock(mutex);
                    folder = folder_base + L"\\" + name;
                    frames.clear();
                    jobs.clear();
                    bytes = 0;
                    byte_limit = uint64_t(std::max<uint32_t>(max_mb, 256)) << 20;
                    dropped = 0;
                    limit_reached = false;
                    last_time = 0;
                    message.clear();
                    write_progress = 0;
                    ok = !worker_running;
                    worker_running = ok && start_worker();
                    ok = worker_running;
                }

                if (ok) {
                    started = now();
                    ring.gap = false;
                    phase.store(phase_t::recording);
                    beep(990);
                    log("started");
                }
                else {
                    const std::lock_guard<std::mutex> lock(mutex);
                    message = "cannot start (still writing?)";
                }
            }
            else if (p == phase_t::recording) {
                phase.store(phase_t::stopping);
                log("stopping");
            }
        }

        ++ring.counter;
        service(ctx);

        if (phase.load() == phase_t::recording) {
            bool limit;
            {
                const std::lock_guard<std::mutex> lock(mutex);
                limit = limit_reached;
            }

            if (limit) {
                phase.store(phase_t::stopping);
                log("memory limit reached");
            }
        }

        if (phase.load() == phase_t::stopping && ring_idle()) {
            phase.store(phase_t::writing);
            wake.notify_one();
            return;
        }

        if (phase.load() != phase_t::recording) {
            return;
        }

        const uint32_t bpp = bytes_per_pixel(in.color_format);

        if (device == nullptr || in.color == nullptr || in.depth == nullptr || in.motion == nullptr || in.mask == nullptr || bpp == 0 ||
            params.render_w < 64 || params.render_h < 64) {
            ring.gap = true;
            return;
        }

        // The crop: the centre, or the whole frame.
        const uint32_t cw = full_frame ? params.render_w : std::min(crop_w_default, params.render_w);
        const uint32_t ch = full_frame ? params.render_h : std::min(crop_h_default, params.render_h);
        const uint32_t cx = (params.render_w - cw) / 2, cy = (params.render_h - ch) / 2;

        // A different size or format mid-recording: stop there (one recording, one layout).
        if (ring.crop_w != 0 && (ring.crop_w != cw || ring.crop_h != ch || ring.color_format != in.color_format)) {
            if (!ring_idle()) {
                return;
            }

            phase.store(phase_t::stopping);
            log("size or format changed: stopped");
            return;
        }

        if (ring.crop_w == 0) {
            reset_ring();
            ring.crop_w = cw;
            ring.crop_h = ch;
            ring.color_format = in.color_format;

            for (slot& sl : ring.slots) {
                for (int p = 0; p < plane_count; ++p) {
                    if (!create_staging(device, cw, ch, plane_format(p), sl.planes[p])) {
                        reset_ring();
                        phase.store(phase_t::stopping);
                        log("cannot create staging textures");
                        return;
                    }
                }
            }

            const std::lock_guard<std::mutex> lock(mutex);
            rec_render_w = params.render_w;
            rec_render_h = params.render_h;
            rec_crop_x = cx;
            rec_crop_y = cy;
            rec_crop_w = cw;
            rec_crop_h = ch;
            rec_color_format = in.color_format;
        }

        int free_slot = -1;

        for (int i = 0; i < ring_size && free_slot < 0; ++i) {
            free_slot = ring.slots[i].state == slot_state::free ? i : -1;
        }

        if (free_slot < 0) {
            ring.gap = true;
            const std::lock_guard<std::mutex> lock(mutex);
            ++dropped;
            return;
        }

        slot& sl = ring.slots[free_slot];
        const D3D11_BOX box = { cx, cy, 0, cx + cw, cy + ch, 1 };
        ID3D11Resource* const sources_list[plane_count] = { in.color, in.depth, in.motion, in.mask, in.reactive };

        for (int p = 0; p < plane_count; ++p) {
            if (sources_list[p] != nullptr) {
                ctx->CopySubresourceRegion(sl.planes[p].Get(), 0, 0, 0, 0, sources_list[p], 0, &box);
            }
        }

        // The tonemap constants and luminance, as captured at last frame's tonemap draw.
        sl.has_tonemap = false;

        if (in.tonemap != nullptr && in.luminance != nullptr && in.tonemap_age <= 2) {
            D3D11_BUFFER_DESC bd;
            in.tonemap->GetDesc(&bd);
            ComPtr<ID3D11Texture2D> lum;
            D3D11_TEXTURE2D_DESC ld = {};

            if (SUCCEEDED(in.luminance->QueryInterface(IID_PPV_ARGS(&lum)))) {
                lum->GetDesc(&ld);
            }

            const bool layout_ok = lum && in.tonemap_offset + tonemap_bytes <= bd.ByteWidth && bd.ByteWidth <= 65536 && ld.Width >= 1 &&
                ld.Width <= 64 && ld.Height <= 4;

            if (layout_ok && (bd.ByteWidth != ring.tonemap_size || ld.Width != ring.luminance_desc.Width || ld.Format != ring.luminance_desc.Format)) {
                for (slot& other : ring.slots) {
                    if (other.state == slot_state::free) {
                        other.tonemap.Reset();
                        other.luminance.Reset();
                    }
                }

                ring.tonemap_size = bd.ByteWidth;
                ring.luminance_desc = ld;
            }

            if (layout_ok && !sl.tonemap) {
                D3D11_BUFFER_DESC sd = {};
                sd.ByteWidth = bd.ByteWidth;
                sd.Usage = D3D11_USAGE_STAGING;
                sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                D3D11_TEXTURE2D_DESC td = ld;
                td.Usage = D3D11_USAGE_STAGING;
                td.BindFlags = 0;
                td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                td.MiscFlags = 0;

                if (FAILED(device->CreateBuffer(&sd, nullptr, &sl.tonemap)) || FAILED(device->CreateTexture2D(&td, nullptr, &sl.luminance))) {
                    sl.tonemap.Reset();
                    sl.luminance.Reset();
                }
            }

            if (layout_ok && sl.tonemap && sl.luminance) {
                ctx->CopyResource(sl.tonemap.Get(), in.tonemap);
                ctx->CopyResource(sl.luminance.Get(), in.luminance);
                sl.has_tonemap = true;
            }
        }

        params.crop_x = cx;
        params.crop_y = cy;
        params.crop_w = cw;
        params.crop_h = ch;
        params.time = now() - started;
        params.reserved[0] = static_cast<float>(in.tonemap_offset); // read by take()
        params.flags |= (ring.gap ? flag_gap : 0) | (in.reactive != nullptr ? flag_reactive : 0) | (sl.has_tonemap ? flag_tonemap : 0);
        ring.gap = false;
        sl.params = params;
        sl.has_reactive = in.reactive != nullptr;
        sl.copied_at = ring.counter;
        sl.state = slot_state::copied;
    }


    void release()
    {
        // The worker may still read mapped memory: let it finish (at most ~2 s).
        for (int wait = 0; wait < 200; ++wait) {
            bool busy = false;

            for (const slot& sl : ring.slots) {
                busy = busy || (sl.state == slot_state::mapped && !sl.worker_done.load());
            }

            if (!busy) {
                break;
            }

            Sleep(10);
        }

        // No context to unmap with any more: the device goes away with its resources.
        reset_ring();

        if (phase.load() == phase_t::recording || phase.load() == phase_t::stopping) {
            phase.store(phase_t::writing);
            wake.notify_one();
        }
    }
}
