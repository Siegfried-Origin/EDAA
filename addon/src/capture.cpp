// Auto capture (Ctrl+Shift+C): over a few seconds, saves the final image
// (before the ReShade overlay) with TAA on, TAA off, jitter only and each
// debug view as PNG files into <capture path>/<timestamp>/, then restores
// the settings. Off unless requested; the read-back stalls only then.

#include "state.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace edaa
{
    namespace
    {
        struct capture_step
        {
            const char* name;
            bool taa, jitter;
            debug_view debug;
            int wait_frames; // frames to let the history settle before saving
        };

        const capture_step steps[] = {
            { "1_taa_on", true, true, debug_view::off, 60 },
            { "2_taa_off", false, false, debug_view::off, 5 },
            { "3_jitter_only", false, true, debug_view::off, 5 },
            { "4_motion_vectors", true, true, debug_view::motion, 60 },
            { "5_vehicle_mask", true, true, debug_view::mask, 5 },
            { "6_depth", true, true, debug_view::depth, 5 },
            { "7_history_rejection", true, true, debug_view::rejection, 5 },
            { "8_moving_objects", true, true, debug_view::objects, 5 },
            { "9_temporal_change", true, true, debug_view::temporal_change, 5 },
        };
        constexpr int step_count = sizeof(steps) / sizeof(steps[0]);

        struct capture_state
        {
            int step = -1; // -1 = idle
            int wait = 0;
            settings saved;
            std::filesystem::path folder;
        } cap;

        uint32_t crc_table[256];


        uint32_t crc32(uint32_t crc, const uint8_t* data, size_t size)
        {
            if (crc_table[1] == 0) {
                for (uint32_t n = 0; n < 256; ++n) {
                    uint32_t c = n;

                    for (int k = 0; k < 8; ++k) {
                        c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                    }

                    crc_table[n] = c;
                }
            }

            crc = ~crc;

            for (size_t i = 0; i < size; ++i) {
                crc = crc_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
            }

            return ~crc;
        }


        void put32(std::vector<uint8_t>& v, uint32_t x)
        {
            v.push_back(uint8_t(x >> 24));
            v.push_back(uint8_t(x >> 16));
            v.push_back(uint8_t(x >> 8));
            v.push_back(uint8_t(x));
        }


        void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data)
        {
            put32(out, static_cast<uint32_t>(data.size()));
            std::vector<uint8_t> body(type, type + 4);
            body.insert(body.end(), data.begin(), data.end());
            out.insert(out.end(), body.begin(), body.end());
            put32(out, crc32(0, body.data(), body.size()));
        }


        // Minimal PNG writer: RGB 8-bit, zlib "stored" (uncompressed) blocks.
        bool write_png(const std::filesystem::path& path, uint32_t w, uint32_t h, const std::vector<uint8_t>& rgb)
        {
            std::vector<uint8_t> raw;
            raw.reserve(size_t(h) * (w * 3 + 1));

            for (uint32_t y = 0; y < h; ++y) {
                raw.push_back(0); // filter: none
                raw.insert(raw.end(), rgb.begin() + size_t(y) * w * 3, rgb.begin() + size_t(y + 1) * w * 3);
            }

            std::vector<uint8_t> z = { 0x78, 0x01 };
            uint32_t a = 1, b = 0;

            for (uint8_t c : raw) {
                a = (a + c) % 65521;
                b = (b + a) % 65521;
            }

            for (size_t pos = 0; pos < raw.size(); pos += 65535) {
                const size_t n = std::min<size_t>(65535, raw.size() - pos);
                z.push_back(pos + n == raw.size() ? 1 : 0);
                z.push_back(uint8_t(n));
                z.push_back(uint8_t(n >> 8));
                z.push_back(uint8_t(~n));
                z.push_back(uint8_t(~n >> 8));
                z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
            }

            put32(z, (b << 16) | a);

            std::vector<uint8_t> out = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
            std::vector<uint8_t> ihdr;
            put32(ihdr, w);
            put32(ihdr, h);
            ihdr.insert(ihdr.end(), { 8, 2, 0, 0, 0 }); // 8-bit RGB
            chunk(out, "IHDR", ihdr);
            chunk(out, "IDAT", z);
            chunk(out, "IEND", {});

            FILE* f = nullptr;

            if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || f == nullptr) {
                return false;
            }

            const bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
            fclose(f);
            return ok;
        }


        // Copies the back buffer to the CPU and saves it.
        bool save_back_buffer(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11Resource* back_buffer, const std::filesystem::path& path)
        {
            ComPtr<ID3D11Texture2D> tex;

            if (FAILED(back_buffer->QueryInterface(IID_PPV_ARGS(&tex)))) {
                return false;
            }

            D3D11_TEXTURE2D_DESC desc;
            tex->GetDesc(&desc);
            const bool bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
            const bool rgba = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS;

            if ((!bgra && !rgba) || desc.SampleDesc.Count != 1) {
                return false;
            }

            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            desc.MiscFlags = 0;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            ComPtr<ID3D11Texture2D> staging;

            if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) {
                return false;
            }

            ctx->CopyResource(staging.Get(), back_buffer);

            D3D11_MAPPED_SUBRESOURCE mapped;

            if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                return false;
            }

            std::vector<uint8_t> rgb(size_t(desc.Width) * desc.Height * 3);

            for (uint32_t y = 0; y < desc.Height; ++y) {
                const uint8_t* row = static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch;
                uint8_t* dst = rgb.data() + size_t(y) * desc.Width * 3;

                for (uint32_t x = 0; x < desc.Width; ++x) {
                    dst[x * 3 + 0] = row[x * 4 + (bgra ? 2 : 0)];
                    dst[x * 3 + 1] = row[x * 4 + 1];
                    dst[x * 3 + 2] = row[x * 4 + (bgra ? 0 : 2)];
                }
            }

            ctx->Unmap(staging.Get(), 0);
            return write_png(path, desc.Width, desc.Height, rgb);
        }


        void apply_step(addon_state& s, int index)
        {
            const capture_step& st = steps[index];
            s.config.taa_enabled = st.taa;
            s.config.jitter_enabled = st.jitter;
            s.config.debug = static_cast<int>(st.debug);
            cap.wait = st.wait_frames;
        }


        std::wstring widen(const std::string& utf8)
        {
            std::wstring w(MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, w.data(), static_cast<int>(w.size()));
            w.resize(wcslen(w.c_str()));
            return w;
        }


        std::string narrow(const std::wstring& wide)
        {
            std::string s(WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr), '\0');
            WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, s.data(), static_cast<int>(s.size()), nullptr, nullptr);
            s.resize(strlen(s.c_str()));
            return s;
        }


        std::filesystem::path timestamp_folder(const std::string& base)
        {
            SYSTEMTIME t;
            GetLocalTime(&t);
            wchar_t name[32];
            swprintf_s(name, L"%04d%02d%02d_%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
            return std::filesystem::path(widen(base)) / name;
        }
    }


    void load_capture_path(addon_state& s)
    {
        char value[1024] = {};
        size_t size = sizeof(value);

        if (reshade::get_config_value(nullptr, "EDAA", "CapturePath", value, &size) && value[0] != '\0') {
            s.capture_path = value;
            return;
        }

        wchar_t profile[MAX_PATH] = {};
        GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
        s.capture_path = narrow(std::wstring(profile) + L"\\Documents\\EDAA-captures");
    }


    // Called at Present after the debug view, with the state not locked.
    void capture_on_present(reshade::api::command_queue* queue, reshade::api::swapchain* swapchain)
    {
        addon_state& s = state();
        int step;
        std::filesystem::path path;
        {
            const std::lock_guard<std::recursive_mutex> lock(s.mutex);

            if (cap.step < 0) {
                if (!s.capture_requested) {
                    return;
                }

                s.capture_requested = false;
                s.dump_requested = true; // also trace the next frame into ReShade.log
                cap.saved = s.config;
                cap.folder = timestamp_folder(s.capture_path);
                std::error_code ec;
                std::filesystem::create_directories(cap.folder, ec);

                if (ec) {
                    s.capture_status = "cannot create the capture folder";
                    return;
                }

                cap.step = 0;
                apply_step(s, 0);
                s.capture_status = "capturing...";
                return;
            }

            if (cap.wait-- > 0) {
                return;
            }

            step = cap.step;
            path = cap.folder / (widen(steps[step].name) + L".png");
        }

        auto* const device = reinterpret_cast<ID3D11Device*>(queue->get_device()->get_native());
        auto* const ctx = reinterpret_cast<ID3D11DeviceContext*>(queue->get_immediate_command_list()->get_native());
        bool ok = false;
        try {
            ok = save_back_buffer(device, ctx, reinterpret_cast<ID3D11Resource*>(swapchain->get_current_back_buffer().handle), path);
        }
        catch (...) {
        }

        const std::lock_guard<std::recursive_mutex> lock(s.mutex);

        if (!ok) {
            reshade::log::message(reshade::log::level::warning, "EDAA: capture failed (back buffer format?)");
        }

        if (++cap.step < step_count) {
            apply_step(s, cap.step);
            return;
        }

        // Context for whoever reads the images.
        FILE* info = nullptr;

        if (_wfopen_s(&info, (cap.folder / L"info.txt").c_str(), L"w") == 0 && info != nullptr) {
            fprintf(info, "version %s\nbuild %s\nrender size %ux%u\nresolve point %s\nstatus %s\non foot %d\nframe mode %s\nmap %s\n",
                version, build_stamp, s.render_w, s.render_h, s.resolve_point, s.motion_status, s.on_foot ? 1 : 0, to_string(s.mode), s.map_reason);
            const object_stats& o = s.objects;
            fprintf(info, "objects: maps %u records %u still %u spinning %u moving %u vehicle %u unmatched %u visible moving %u "
                          "error max %.1f px mean %.1f px, read %.2f ms match %.2f ms\n",
                o.buffer_maps, o.records, o.still, o.spinning, o.moving, o.vehicle, o.unmatched, o.visible_moving,
                o.max_error_px, o.mean_error_px, o.read_ms, o.match_ms);
            fclose(info);
        }

        s.config = cap.saved;
        cap.step = -1;
        s.last_capture_folder = narrow(cap.folder.wstring());
        s.capture_status = "done";
        const std::string msg = "EDAA: captures saved to " + s.last_capture_folder;
        reshade::log::message(reshade::log::level::info, msg.c_str());
    }
}
