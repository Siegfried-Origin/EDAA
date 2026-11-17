// edaa-replay: plays a recording (docs/recording.md) through the anti-aliasing
// variants offline and measures them (docs/replay.md).
//
//     edaa-replay <recording folder> <output folder> [run,run,...]
//
// Every variant goes through the add-on's own code: the EDAA TAA shaders
// (taa.hlsl) and DLSS through dlss.cpp, on the recorded crop. The result is
// written into the game's scene format (R11G11B10) like in game, read back,
// mapped for display with Elite's exposure of that frame (approximation:
// Reinhard per channel and sRGB instead of Elite's own curve) and measured:
//   - flicker: mean change of the display luminance against the previous
//     frame moved along the motion vectors (disocclusions, the crop border,
//     camera cuts and the first frames left out);
//   - sharpness: mean Sobel gradient of the display luminance.
// Output: metrics.json, a few whole frames, a burst of consecutive frames
// around the centre and a flicker heat map per variant (PNG); report.py
// turns them into the comparison page.

#include "dlss.hpp"
#include "recorder.hpp"
#include "state.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <execution>
#include <filesystem>
#include <functional>
#include <fstream>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

#include <Windows.h>
#include <d3d11.h>
#include <wincodec.h>
#include <wrl/client.h>

#include "motion_shaders.h"
#include "taa_shaders.h"

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace
{
    constexpr uint32_t warmup = 8; // frames before the histories count (also after a cut)
    constexpr uint32_t margin = 16; // pixels at the crop border left out of the measures
    constexpr uint32_t burst_frames = 16;
    constexpr uint32_t burst_w = 480, burst_h = 270;

    // passes.cpp taa_constants.
    struct alignas(16) taa_constants
    {
        float render_size[4];
        float jitter[4];
        float blend[4];
        float flicker[4];
        float hdr[4];
        float display_size[4];
        float sharpen[4];
    };

    // ---------------------------------------------------------------- recording

    struct recording
    {
        std::string name;
        uint32_t n = 0, w = 0, h = 0, render_w = 0, render_h = 0;
        std::vector<uint32_t> color; // R11G11B10
        std::vector<float> depth; // reverse-Z
        std::vector<uint32_t> motion; // R16G16_FLOAT, UV of the whole render size
        std::vector<uint8_t> reactive;
        std::vector<edaa::recorder::frame_params> params;
        std::vector<float> exposure; // Elite's exposure per frame (0: not captured)
    };

    std::vector<double> json_numbers(const std::string& text, const char* key)
    {
        std::vector<double> out;
        const size_t at = text.find(std::string("\"") + key + "\"");

        if (at == std::string::npos) {
            return out;
        }

        size_t p = text.find(':', at) + 1;
        const bool array = text.find_first_not_of(" \t\r\n", p) != std::string::npos && text[text.find_first_not_of(" \t\r\n", p)] == '[';
        const size_t end = array ? text.find(']', p) : text.find_first_of(",}\n", p);
        std::string part = text.substr(p, end - p);
        std::replace_if(part.begin(), part.end(), [](char c) { return c == '[' || c == ','; }, ' ');
        std::istringstream in(part);
        double v;

        while (in >> v) {
            out.push_back(v);
        }

        return out;
    }

    template <typename T>
    bool read_file(const fs::path& path, std::vector<T>& out, size_t count)
    {
        out.resize(count);
        std::ifstream f(path, std::ios::binary);
        return f && f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(count * sizeof(T))).gcount() ==
            static_cast<std::streamsize>(count * sizeof(T));
    }

    // tools/record_check.py elite_exposure: the tonemap's exposure from cb2 and the adapted luminance.
    float elite_exposure(const float* cb2, float lum)
    {
        auto c = [&](int r, int i) { return static_cast<double>(cb2[r * 4 + i]); };

        if (!(lum > 0)) {
            return 0;
        }

        const double bias = c(8, 0) + (c(8, 2) != 0 ? 1.4 : 0.0);
        const double x = std::log2(8.0 * lum) - bias - 3;
        const double w2 = 2 * c(7, 3);
        const double curve = std::pow(w2 / (std::log10(1 + std::exp2(x * c(8, 1))) + w2), c(7, 3));
        const double e = (1.03 * c(7, 2) - curve) / lum * std::exp2(-c(7, 1));
        const double v = c(2, 0) * e * c(6, 1);
        return std::isfinite(v) && v > 0 ? static_cast<float>(v) : 0.0f;
    }

    bool load(const fs::path& folder, recording& rec)
    {
        std::ifstream meta_file(folder / "record.json");
        const std::string meta((std::istreambuf_iterator<char>(meta_file)), std::istreambuf_iterator<char>());
        const auto frames = json_numbers(meta, "frames");
        const auto crop = json_numbers(meta, "crop");
        const auto render = json_numbers(meta, "render_size");
        const auto params_bytes = json_numbers(meta, "params_bytes");
        const auto tonemap_bytes = json_numbers(meta, "tonemap_bytes");

        if (frames.size() != 1 || crop.size() != 4 || render.size() != 2 || params_bytes.empty() ||
            params_bytes[0] != sizeof(edaa::recorder::frame_params) || tonemap_bytes.empty()) {
            std::printf("%s: record.json missing or of another format\n", folder.string().c_str());
            return false;
        }

        const fs::path clean = folder.has_filename() ? folder : folder.parent_path(); // "rec/" -> "rec"
        rec.name = clean.filename().string();
        rec.n = static_cast<uint32_t>(frames[0]);
        rec.w = static_cast<uint32_t>(crop[2]);
        rec.h = static_cast<uint32_t>(crop[3]);
        rec.render_w = static_cast<uint32_t>(render[0]);
        rec.render_h = static_cast<uint32_t>(render[1]);
        const size_t pixels = static_cast<size_t>(rec.n) * rec.w * rec.h;
        const size_t tb = static_cast<size_t>(tonemap_bytes[0]);
        std::vector<uint8_t> tonemap;

        if (!read_file(folder / "color.bin", rec.color, pixels) || !read_file(folder / "depth.bin", rec.depth, pixels) ||
            !read_file(folder / "motion.bin", rec.motion, pixels) || !read_file(folder / "reactive.bin", rec.reactive, pixels) ||
            !read_file(folder / "params.bin", rec.params, rec.n) || !read_file(folder / "tonemap.bin", tonemap, rec.n * tb)) {
            std::printf("%s: a plane is missing or too short (tools/record_check.py)\n", rec.name.c_str());
            return false;
        }

        rec.exposure.assign(rec.n, 0.0f);

        for (uint32_t i = 0; i < rec.n; ++i) {
            if (rec.params[i].flags & edaa::recorder::flag_tonemap) {
                const float* cb2 = reinterpret_cast<const float*>(tonemap.data() + i * tb);
                rec.exposure[i] = elite_exposure(cb2, cb2[64]);
            }
        }

        return true;
    }

    // ---------------------------------------------------------------- decoding

    float half_to_float(uint16_t h)
    {
        const uint32_t sign = (h & 0x8000u) << 16, e = (h >> 10) & 31, m = h & 1023;
        uint32_t bits;

        if (e == 0) {
            if (m == 0) {
                bits = sign;
            }
            else {
                float f = std::ldexp(static_cast<float>(m), -24);
                return sign ? -f : f;
            }
        }
        else if (e == 31) {
            bits = sign | 0x7f800000u | (m << 13);
        }
        else {
            bits = sign | ((e + 112) << 23) | (m << 13);
        }

        float f;
        std::memcpy(&f, &bits, 4);
        return f;
    }

    struct tables
    {
        float f11[2048], f10[1024], half[65536];
        uint8_t srgb[4096]; // display value 0..1 (4095 steps) -> sRGB 8 bit

        tables()
        {
            for (int v = 0; v < 2048; ++v) {
                const int e = v >> 6, m = v & 63;
                f11[v] = e == 0 ? m / 64.0f * std::ldexp(1.0f, -14) : e == 31 ? 65000.0f : (1 + m / 64.0f) * std::ldexp(1.0f, e - 15);
            }

            for (int v = 0; v < 1024; ++v) {
                const int e = v >> 5, m = v & 31;
                f10[v] = e == 0 ? m / 32.0f * std::ldexp(1.0f, -14) : e == 31 ? 65000.0f : (1 + m / 32.0f) * std::ldexp(1.0f, e - 15);
            }

            for (int v = 0; v < 65536; ++v) {
                half[v] = half_to_float(static_cast<uint16_t>(v));
            }

            for (int i = 0; i < 4096; ++i) {
                const double x = i / 4095.0;
                const double s = x <= 0.0031308 ? 12.92 * x : 1.055 * std::pow(x, 1 / 2.4) - 0.055;
                srgb[i] = static_cast<uint8_t>(std::lround(std::clamp(s, 0.0, 1.0) * 255));
            }
        }
    };

    const tables& lut()
    {
        static const tables t;
        return t;
    }

    // ---------------------------------------------------------------- images

    ComPtr<IWICImagingFactory> wic;

    bool write_png(const fs::path& path, uint32_t w, uint32_t h, const uint8_t* rgb, size_t stride)
    {
        std::vector<uint8_t> bgr(static_cast<size_t>(w) * h * 3);

        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                const uint8_t* s = rgb + y * stride + x * 3;
                uint8_t* d = bgr.data() + (static_cast<size_t>(y) * w + x) * 3;
                d[0] = s[2];
                d[1] = s[1];
                d[2] = s[0];
            }
        }

        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;

        return SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
            SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
            SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) && SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) &&
            SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(w, h)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
            format == GUID_WICPixelFormat24bppBGR && SUCCEEDED(frame->WritePixels(h, w * 3, static_cast<UINT>(bgr.size()), bgr.data())) &&
            SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    }

    // ---------------------------------------------------------------- variants

    enum class kind
    {
        raw,
        taa,
        dlaa
    };

    struct run_def
    {
        std::string id;
        std::string label;
        kind type;
        int preset = 0; // NVSDK_NGX_DLSS_Hint_Render_Preset, 0 = NVIDIA's default
        int exposure_mode = 1; // 0 DLSS, 1 fixed, 2 from Elite (dlss_exposure_mode)
        float exposure = 10.0f;
        std::vector<float> sharpen = { 0.0f }; // one variant per value (DLAA only)
        std::string taa; // TAA only: setting overrides "key=value;..." (taa_override), from --taa
        bool reactive = true; // DLAA only: pass the recorded reactive mask (dlss_reactive_mask)
    };

    // Experiment values that are not add-on settings (yet).
    struct taa_extra
    {
        float sharpen = 0; // sh: output sharpening with ps_sharpen_hdr (not fed back)
        float jitter_weight = 0; // jw: ps_taa jitter weighting strength (hdr.y)
        float history_sharpen = 0; // hs: ps_taa history sharpening (hdr.z)
    };

    // TAA settings an experiment can change (--taa id=key=value;key=value).
    bool taa_override(edaa::settings& c, taa_extra& x, const std::string& spec)
    {
        std::stringstream in(spec);
        std::string item;

        while (std::getline(in, item, ';')) {
            const size_t eq = item.find('=');

            if (eq == std::string::npos) {
                return false;
            }

            const std::string key = item.substr(0, eq);
            const float v = std::stof(item.substr(eq + 1));

            if (key == "hw") c.taa_history_weight = v;
            else if (key == "hwm") c.taa_history_weight_motion = v;
            else if (key == "hwmin") c.taa_history_weight_min = v;
            else if (key == "cg") c.taa_clip_gamma = v;
            else if (key == "cgs") c.taa_clip_gamma_static = v;
            else if (key == "af") c.taa_anti_flicker = v != 0;
            else if (key == "rec") c.taa_reconstruction = v != 0;
            else if (key == "unj") c.taa_unjitter = v != 0;
            else if (key == "soft") c.taa_soft_offscreen = v != 0;
            else if (key == "hdr") c.taa_hdr_scale = v;
            else if (key == "sh") x.sharpen = v;
            else if (key == "jw") x.jitter_weight = v;
            else if (key == "hs") x.history_sharpen = v;
            else return false;
        }

        return true;
    }

    const std::vector<run_def>& all_runs()
    {
        static const std::vector<run_def> runs = {
            { "raw", "Eingang ohne Kantenglättung", kind::raw },
            { "taa", "EDAA TAA", kind::taa },
            { "dlaa", "DLAA, Belichtung fest 10", kind::dlaa, 0, 1, 10.0f, { 0.0f, 0.3f, 0.6f } },
            { "dlaa_elite", "DLAA, Belichtung von Elite", kind::dlaa, 0, 2, 10.0f },
            { "dlaa_auto", "DLAA, Belichtung von DLSS", kind::dlaa, 0, 0, 0.0f },
            { "dlaa_noreact", "DLAA, fest 10, ohne Reaktivmaske", kind::dlaa, 0, 1, 10.0f, { 0.0f }, {}, false },
            { "dlaa_j", "DLAA Modell J, fest 10", kind::dlaa, 10, 1, 10.0f },
            { "dlaa_l", "DLAA Modell L, fest 10", kind::dlaa, 12, 1, 10.0f },
            { "dlaa_m", "DLAA Modell M, fest 10", kind::dlaa, 13, 1, 10.0f },
        };
        return runs;
    }

    // Measures of one variant over the recording.
    struct variant
    {
        std::string id, label;
        std::vector<float> prev_y; // display luminance of the previous frame
        bool have_prev = false;
        std::vector<double> heat; // summed flicker per pixel
        std::vector<uint32_t> heat_count;
        std::vector<double> flicker_series, sharp_series; // per frame, -1 = not measured
        double gpu_ms = 0;
        uint32_t gpu_frames = 0;
    };

    struct study
    {
        const recording& rec;
        fs::path out;
        std::vector<uint32_t> picks;
        uint32_t burst_start = 0, burst_x = 0, burst_y = 0;
    };

    bool measured(const recording& rec, uint32_t t, uint32_t since_reset)
    {
        return since_reset >= warmup && t > 0 && !(rec.params[t].flags & (edaa::recorder::flag_camera_cut | edaa::recorder::flag_gap));
    }

    float display_exposure(const recording& rec, uint32_t t)
    {
        for (uint32_t i = t + 1; i-- > 0;) {
            if (rec.exposure[i] > 0) {
                return rec.exposure[i];
            }
        }

        for (uint32_t i = t; i < rec.n; ++i) {
            if (rec.exposure[i] > 0) {
                return rec.exposure[i];
            }
        }

        return 1.0f;
    }

    // One frame of a variant (R11G11B10 rows, 'pitch' in pixels) -> display, measures, images.
    void process(const study& st, variant& v, uint32_t t, uint32_t since_reset, const uint32_t* packed, size_t pitch)
    {
        const recording& rec = st.rec;
        const uint32_t w = rec.w, h = rec.h;
        const tables& L = lut();
        const float e = display_exposure(rec, t);
        std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3);
        std::vector<float> y_now(static_cast<size_t>(w) * h);
        std::vector<uint32_t> rows(h);
        std::iota(rows.begin(), rows.end(), 0u);

        std::for_each(std::execution::par, rows.begin(), rows.end(), [&](uint32_t y) {
            for (uint32_t x = 0; x < w; ++x) {
                const uint32_t p = packed[y * pitch + x];
                const float c[3] = { L.f11[p & 0x7ff], L.f11[(p >> 11) & 0x7ff], L.f10[p >> 22] };
                uint8_t* d = rgb.data() + (static_cast<size_t>(y) * w + x) * 3;

                for (int k = 0; k < 3; ++k) {
                    const float s = c[k] * e;
                    const float m = s / (1 + s);
                    d[k] = L.srgb[std::clamp(static_cast<int>(m * 4095 + 0.5f), 0, 4095)];
                }

                y_now[static_cast<size_t>(y) * w + x] = (0.2126f * d[0] + 0.7152f * d[1] + 0.0722f * d[2]) / 255.0f;
            }
        });

        // Sharpness: Sobel on the inner area.
        std::vector<double> row_sharp(h, 0.0), row_flicker(h, 0.0);
        std::vector<uint32_t> row_count(h, 0);

        std::for_each(std::execution::par, rows.begin() + margin, rows.end() - margin, [&](uint32_t y) {
            double sum = 0;

            for (uint32_t x = margin; x < w - margin; ++x) {
                auto Y = [&](int dx, int dy) { return y_now[(static_cast<size_t>(y) + dy) * w + x + dx]; };
                const float gx = Y(1, -1) + 2 * Y(1, 0) + Y(1, 1) - Y(-1, -1) - 2 * Y(-1, 0) - Y(-1, 1);
                const float gy = Y(-1, 1) + 2 * Y(0, 1) + Y(1, 1) - Y(-1, -1) - 2 * Y(0, -1) - Y(1, -1);
                sum += std::sqrt(gx * gx + gy * gy);
            }

            row_sharp[y] = sum;
        });

        const double inner = static_cast<double>(w - 2 * margin) * (h - 2 * margin);
        const bool count = measured(rec, t, since_reset);
        v.sharp_series[t] = std::accumulate(row_sharp.begin(), row_sharp.end(), 0.0) / inner;

        // Flicker: against the previous frame moved along this frame's motion vectors.
        if (count && v.have_prev) {
            const float sx = static_cast<float>(rec.render_w), sy = static_cast<float>(rec.render_h);
            const size_t frame = static_cast<size_t>(w) * h;
            const float* depth_now = rec.depth.data() + t * frame;
            const float* depth_prev = rec.depth.data() + (t - 1) * frame;
            const uint32_t* motion = rec.motion.data() + t * frame;

            std::for_each(std::execution::par, rows.begin() + margin, rows.end() - margin, [&](uint32_t y) {
                double sum = 0;
                uint32_t n = 0;

                for (uint32_t x = margin; x < w - margin; ++x) {
                    const size_t i = static_cast<size_t>(y) * w + x;
                    const uint32_t mv = motion[i];
                    // UV of the whole render size -> pixels.
                    const float px = x + L.half[mv & 0xffff] * sx;
                    const float py = y + L.half[mv >> 16] * sy;

                    if (!(px >= margin && py >= margin && px < w - margin - 1 && py < h - margin - 1)) {
                        continue;
                    }

                    const int x0 = static_cast<int>(px), y0 = static_cast<int>(py);
                    const float fx = px - x0, fy = py - y0;
                    const size_t j = static_cast<size_t>(y0) * w + x0;
                    const float dn = depth_now[i], dp = depth_prev[static_cast<size_t>(std::lround(py)) * w + std::lround(px)];

                    if (std::abs(dn - dp) > 0.05f * std::max(dn, dp) + 1e-9f) {
                        continue; // disocclusion: something else was there
                    }

                    const float prev = (v.prev_y[j] * (1 - fx) + v.prev_y[j + 1] * fx) * (1 - fy) +
                        (v.prev_y[j + w] * (1 - fx) + v.prev_y[j + w + 1] * fx) * fy;
                    const double d = std::abs(y_now[i] - prev);
                    sum += d;
                    ++n;
                    v.heat[i] += d;
                    ++v.heat_count[i];
                }

                row_flicker[y] = sum;
                row_count[y] = n;
            });

            const double n = std::accumulate(row_count.begin(), row_count.end(), 0.0);
            v.flicker_series[t] = n > 0 ? std::accumulate(row_flicker.begin(), row_flicker.end(), 0.0) / n : -1;
        }

        if (!count) {
            v.sharp_series[t] = -1;
        }

        v.prev_y.swap(y_now);
        v.have_prev = true;

        if (std::find(st.picks.begin(), st.picks.end(), t) != st.picks.end()) {
            write_png(st.out / "frames" / (v.id + "_" + std::to_string(t) + ".png"), w, h, rgb.data(), w * 3);
        }

        if (t >= st.burst_start && t < st.burst_start + burst_frames) {
            write_png(st.out / "burst" / (v.id + "_" + std::to_string(t - st.burst_start) + ".png"), burst_w, burst_h,
                rgb.data() + (static_cast<size_t>(st.burst_y) * w + st.burst_x) * 3, w * 3);
        }
    }

    void write_heat(const study& st, const variant& v)
    {
        const uint32_t w = st.rec.w, h = st.rec.h;
        std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3, 0);

        for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
            const double m = v.heat_count[i] ? v.heat[i] / v.heat_count[i] : 0;
            const double s = std::clamp(m / 0.08, 0.0, 1.0) * 3; // 8 % of the display range = white
            rgb[i * 3 + 0] = static_cast<uint8_t>(std::clamp(s, 0.0, 1.0) * 255);
            rgb[i * 3 + 1] = static_cast<uint8_t>(std::clamp(s - 1, 0.0, 1.0) * 255);
            rgb[i * 3 + 2] = static_cast<uint8_t>(std::clamp(s - 2, 0.0, 1.0) * 255);
        }

        write_png(st.out / "heat" / (v.id + ".png"), w, h, rgb.data(), static_cast<size_t>(w) * 3);
    }

    double mean_of(const std::vector<double>& series)
    {
        double sum = 0;
        int n = 0;

        for (double v : series) {
            if (v >= 0) {
                sum += v;
                ++n;
            }
        }

        return n ? sum / n : -1;
    }

    // Mean of the 5 % pixels that flicker most (per-pixel mean over the
    // recording): the overall mean is dominated by the empty sky.
    double hot_flicker(const variant& v)
    {
        std::vector<double> m;

        for (size_t i = 0; i < v.heat.size(); ++i) {
            if (v.heat_count[i] >= 8) {
                m.push_back(v.heat[i] / v.heat_count[i]);
            }
        }

        if (m.empty()) {
            return -1;
        }

        const size_t top = std::max<size_t>(1, m.size() / 20);
        std::nth_element(m.begin(), m.begin() + top, m.end(), std::greater<double>());
        return std::accumulate(m.begin(), m.begin() + top, 0.0) / top;
    }

    // ---------------------------------------------------------------- GPU

    struct texture
    {
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11ShaderResourceView> srv;
        ComPtr<ID3D11RenderTargetView> rtv;
    };

    struct gpu_state
    {
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> ctx;
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11PixelShader> ps_taa, ps_copy, ps_sharpen;
        ComPtr<ID3D11SamplerState> sampler;
        ComPtr<ID3D11Buffer> cb;
        texture color, depth, motion, reactive, history[2], dlss_out, final_out, exposure;
        ComPtr<ID3D11Texture2D> staging;
        ComPtr<ID3D11Query> disjoint, ts[2];
    };

    bool make_texture(gpu_state& g, uint32_t w, uint32_t h, DXGI_FORMAT format, UINT bind, texture& t, D3D11_USAGE usage = D3D11_USAGE_DEFAULT)
    {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Usage = usage;
        desc.BindFlags = bind;

        if (FAILED(g.device->CreateTexture2D(&desc, nullptr, &t.tex))) {
            return false;
        }

        return (!(bind & D3D11_BIND_SHADER_RESOURCE) || SUCCEEDED(g.device->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv))) &&
            (!(bind & D3D11_BIND_RENDER_TARGET) || SUCCEEDED(g.device->CreateRenderTargetView(t.tex.Get(), nullptr, &t.rtv)));
    }

    bool init_gpu(gpu_state& g, uint32_t w, uint32_t h)
    {
        const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };

        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &g.device, nullptr, &g.ctx))) {
            return false;
        }

        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = sizeof(taa_constants);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_TEXTURE2D_DESC staging = {};
        staging.Width = w;
        staging.Height = h;
        staging.MipLevels = staging.ArraySize = 1;
        staging.Format = DXGI_FORMAT_R11G11B10_FLOAT;
        staging.SampleDesc.Count = 1;
        staging.Usage = D3D11_USAGE_STAGING;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        D3D11_QUERY_DESC qd = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
        D3D11_QUERY_DESC qt = { D3D11_QUERY_TIMESTAMP, 0 };
        constexpr UINT srv = D3D11_BIND_SHADER_RESOURCE;

        return SUCCEEDED(g.device->CreateVertexShader(g_vs_fullscreen, sizeof(g_vs_fullscreen), nullptr, &g.vs)) &&
            SUCCEEDED(g.device->CreatePixelShader(g_ps_taa, sizeof(g_ps_taa), nullptr, &g.ps_taa)) &&
            SUCCEEDED(g.device->CreatePixelShader(g_ps_copy, sizeof(g_ps_copy), nullptr, &g.ps_copy)) &&
            SUCCEEDED(g.device->CreatePixelShader(g_ps_sharpen_hdr, sizeof(g_ps_sharpen_hdr), nullptr, &g.ps_sharpen)) &&
            SUCCEEDED(g.device->CreateSamplerState(&sd, &g.sampler)) && SUCCEEDED(g.device->CreateBuffer(&bd, nullptr, &g.cb)) &&
            make_texture(g, w, h, DXGI_FORMAT_R11G11B10_FLOAT, srv, g.color) && make_texture(g, w, h, DXGI_FORMAT_R32_FLOAT, srv, g.depth) &&
            make_texture(g, w, h, DXGI_FORMAT_R32G32_FLOAT, srv, g.motion) && make_texture(g, w, h, DXGI_FORMAT_R8_UNORM, srv, g.reactive) &&
            make_texture(g, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, srv | D3D11_BIND_RENDER_TARGET, g.history[0]) &&
            make_texture(g, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, srv | D3D11_BIND_RENDER_TARGET, g.history[1]) &&
            make_texture(g, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, srv | D3D11_BIND_UNORDERED_ACCESS, g.dlss_out) &&
            make_texture(g, w, h, DXGI_FORMAT_R11G11B10_FLOAT, D3D11_BIND_RENDER_TARGET, g.final_out) &&
            make_texture(g, 1, 1, DXGI_FORMAT_R32_FLOAT, srv, g.exposure) && SUCCEEDED(g.device->CreateTexture2D(&staging, nullptr, &g.staging)) &&
            SUCCEEDED(g.device->CreateQuery(&qd, &g.disjoint)) && SUCCEEDED(g.device->CreateQuery(&qt, &g.ts[0])) &&
            SUCCEEDED(g.device->CreateQuery(&qt, &g.ts[1]));
    }

    void upload(gpu_state& g, const recording& rec, uint32_t t)
    {
        const size_t frame = static_cast<size_t>(rec.w) * rec.h;
        const tables& L = lut();
        // Motion vectors: UV of the whole render size -> UV of the crop.
        std::vector<float> motion(frame * 2);
        const float sx = static_cast<float>(rec.render_w) / rec.w, sy = static_cast<float>(rec.render_h) / rec.h;
        const uint32_t* mv = rec.motion.data() + t * frame;

        for (size_t i = 0; i < frame; ++i) {
            motion[i * 2] = L.half[mv[i] & 0xffff] * sx;
            motion[i * 2 + 1] = L.half[mv[i] >> 16] * sy;
        }

        g.ctx->UpdateSubresource(g.color.tex.Get(), 0, nullptr, rec.color.data() + t * frame, rec.w * 4, 0);
        g.ctx->UpdateSubresource(g.depth.tex.Get(), 0, nullptr, rec.depth.data() + t * frame, rec.w * 4, 0);
        g.ctx->UpdateSubresource(g.motion.tex.Get(), 0, nullptr, motion.data(), rec.w * 8, 0);
        g.ctx->UpdateSubresource(g.reactive.tex.Get(), 0, nullptr, rec.reactive.data() + t * frame, rec.w, 0);
    }

    void fullscreen(gpu_state& g, ID3D11PixelShader* ps, ID3D11RenderTargetView* target, std::initializer_list<ID3D11ShaderResourceView*> srvs,
        uint32_t w, uint32_t h)
    {
        ID3D11ShaderResourceView* const none[8] = {};
        g.ctx->PSSetShaderResources(0, 8, none);
        g.ctx->OMSetRenderTargets(1, &target, nullptr);
        g.ctx->PSSetShaderResources(0, static_cast<UINT>(srvs.size()), srvs.begin());
        g.ctx->IASetInputLayout(nullptr);
        g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g.ctx->VSSetShader(g.vs.Get(), nullptr, 0);
        g.ctx->PSSetShader(ps, nullptr, 0);
        g.ctx->PSSetSamplers(0, 1, g.sampler.GetAddressOf());
        g.ctx->PSSetConstantBuffers(0, 1, g.cb.GetAddressOf());
        g.ctx->RSSetState(nullptr);
        g.ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        g.ctx->OMSetDepthStencilState(nullptr, 0);
        const D3D11_VIEWPORT vp = { 0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1 };
        g.ctx->RSSetViewports(1, &vp);
        g.ctx->Draw(3, 0);
        g.ctx->PSSetShaderResources(0, 8, none);
        g.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    }

    taa_constants constants(const recording& rec, uint32_t t, bool reset, float sharpen, const std::string& overrides = {})
    {
        edaa::settings config; // the add-on's TAA defaults
        const auto& p = rec.params[t];
        taa_extra x;
        const bool hdr_set = overrides.find("hdr=") != std::string::npos;
        taa_override(config, x, overrides);
        taa_constants tc = {};
        tc.render_size[0] = static_cast<float>(rec.w);
        tc.render_size[1] = static_cast<float>(rec.h);
        tc.render_size[2] = 1.0f / rec.w;
        tc.render_size[3] = 1.0f / rec.h;
        tc.jitter[0] = p.jitter[0] / rec.w;
        tc.jitter[1] = p.jitter[1] / rec.h;
        tc.jitter[2] = config.taa_unjitter ? 1.0f : 0.0f;
        tc.jitter[3] = config.taa_reconstruction ? 1.0f : 0.0f;
        tc.blend[0] = config.taa_history_weight;
        tc.blend[1] = config.taa_history_weight_motion;
        tc.blend[2] = reset ? 1.0f : 0.0f;
        tc.blend[3] = config.taa_clip_gamma;
        tc.flicker[0] = config.taa_anti_flicker ? 1.0f : 0.0f;
        tc.flicker[1] = config.taa_history_weight_min;
        tc.flicker[2] = config.taa_clip_gamma_static;
        tc.flicker[3] = config.taa_soft_offscreen ? 1.0f : 0.0f;
        tc.hdr[0] = p.taa_hdr_scale > 0 && !hdr_set ? p.taa_hdr_scale : config.taa_hdr_scale;
        tc.sharpen[0] = sharpen;
        // Jitter weighting: normalised by the mean of exp(-k d^2) over the jitter square.
        const double k = x.jitter_weight;
        const double axis = k > 0 ? std::sqrt(3.14159265358979 / k) * std::erf(std::sqrt(k) / 2) : 1.0;
        tc.hdr[1] = x.jitter_weight;
        tc.hdr[2] = x.history_sharpen;
        tc.hdr[3] = static_cast<float>(1.0 / (axis * axis));
        return tc;
    }

    // Reads final_out back and hands it to process().
    void read_back(gpu_state& g, const study& st, variant& v, uint32_t t, uint32_t since_reset)
    {
        g.ctx->CopyResource(g.staging.Get(), g.final_out.tex.Get());
        D3D11_MAPPED_SUBRESOURCE map = {};

        if (SUCCEEDED(g.ctx->Map(g.staging.Get(), 0, D3D11_MAP_READ, 0, &map))) {
            process(st, v, t, since_reset, static_cast<const uint32_t*>(map.pData), map.RowPitch / 4);
            g.ctx->Unmap(g.staging.Get(), 0);
        }
    }

    double gpu_time_ms(gpu_state& g)
    {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
        UINT64 a = 0, b = 0;

        while (g.ctx->GetData(g.disjoint.Get(), &dj, sizeof(dj), 0) == S_FALSE) {
        }

        while (g.ctx->GetData(g.ts[0].Get(), &a, sizeof(a), 0) == S_FALSE) {
        }

        while (g.ctx->GetData(g.ts[1].Get(), &b, sizeof(b), 0) == S_FALSE) {
        }

        return dj.Disjoint || dj.Frequency == 0 ? -1 : static_cast<double>(b - a) * 1000.0 / static_cast<double>(dj.Frequency);
    }

    // Plays the recording through one run; fills one variant per sharpening value.
    bool play(gpu_state& g, const study& st, const run_def& run, std::vector<variant>& out)
    {
        const recording& rec = st.rec;
        const size_t first = out.size();

        for (float s : run.type == kind::dlaa ? run.sharpen : std::vector<float>{ 0.0f }) {
            variant v;
            v.id = run.id;
            v.label = run.label;

            if (s > 0) {
                char suffix[32];
                std::snprintf(suffix, sizeof(suffix), "_s%02d", static_cast<int>(std::lround(s * 10)));
                v.id += suffix;
                std::snprintf(suffix, sizeof(suffix), ", nachgeschärft %.1f", s);
                v.label += suffix;
            }

            v.heat.assign(static_cast<size_t>(rec.w) * rec.h, 0.0);
            v.heat_count.assign(static_cast<size_t>(rec.w) * rec.h, 0);
            v.flicker_series.assign(rec.n, -1.0);
            v.sharp_series.assign(rec.n, -1.0);
            out.push_back(std::move(v));
        }

        std::printf("  %-12s", run.id.c_str());
        edaa::settings check;
        taa_extra extra;

        if (!taa_override(check, extra, run.taa)) {
            std::printf("bad --taa settings: %s\n", run.taa.c_str());
            return false;
        }
        const auto started = std::chrono::steady_clock::now();
        int history = 0;
        uint32_t since_reset = 0;

        for (uint32_t t = 0; t < rec.n; ++t) {
            const auto& p = rec.params[t];
            const bool reset = t == 0 || (p.flags & (edaa::recorder::flag_camera_cut | edaa::recorder::flag_gap));
            since_reset = reset ? 0 : since_reset + 1;

            if (run.type == kind::raw) {
                process(st, out[first], t, since_reset, rec.color.data() + static_cast<size_t>(t) * rec.w * rec.h, rec.w);
                continue;
            }

            upload(g, rec, t);
            g.ctx->Begin(g.disjoint.Get());
            g.ctx->End(g.ts[0].Get());

            if (run.type == kind::taa) {
                const taa_constants tc = constants(rec, t, reset, 0, run.taa);
                g.ctx->UpdateSubresource(g.cb.Get(), 0, nullptr, &tc, 0, 0);
                fullscreen(g, g.ps_taa.Get(), g.history[1 - history].rtv.Get(),
                    { g.color.srv.Get(), g.history[history].srv.Get(), g.motion.srv.Get(), g.depth.srv.Get() }, rec.w, rec.h);
                history = 1 - history;
            }
            else {
                edaa::upscaler_inputs u;
                u.color = g.color.tex.Get();
                u.depth = g.depth.tex.Get();
                u.motion = g.motion.tex.Get();
                u.output = g.dlss_out.tex.Get();
                u.width = rec.w;
                u.height = rec.h;
                u.jitter_x = p.jitter[0];
                u.jitter_y = p.jitter[1];
                u.reset = reset;
                u.dlss_preset = run.preset;
                u.dlss_exposure = run.exposure_mode == 0 ? 0.0f : run.exposure;
                u.hud_mask = run.reactive && (p.flags & edaa::recorder::flag_reactive) ? g.reactive.tex.Get() : nullptr;

                if (run.exposure_mode == 2) {
                    const float scale = p.dlss_exposure_scale > 0 ? p.dlss_exposure_scale : 1.0f;
                    const float e = rec.exposure[t] > 0 ? rec.exposure[t] * scale : run.exposure;
                    g.ctx->UpdateSubresource(g.exposure.tex.Get(), 0, nullptr, &e, 4, 0);
                    u.exposure_tex = g.exposure.tex.Get();
                }

                if (!edaa::dlss::evaluate(g.device.Get(), g.ctx.Get(), u)) {
                    std::printf("DLSS failed: %s\n", edaa::dlss::status().c_str());
                    return false;
                }
            }

            g.ctx->End(g.ts[1].Get());
            g.ctx->End(g.disjoint.Get());
            const double ms = gpu_time_ms(g);

            for (size_t k = 0; k < out.size() - first; ++k) {
                variant& v = out[first + k];

                if (ms >= 0 && since_reset >= warmup) {
                    v.gpu_ms += ms;
                    ++v.gpu_frames;
                }

                if (run.type == kind::taa) {
                    const taa_constants tc = constants(rec, t, false, std::min(extra.sharpen, 1.0f), run.taa);
                    g.ctx->UpdateSubresource(g.cb.Get(), 0, nullptr, &tc, 0, 0);
                    fullscreen(g, extra.sharpen > 0 ? g.ps_sharpen.Get() : g.ps_copy.Get(), g.final_out.rtv.Get(),
                        { nullptr, g.history[history].srv.Get() }, rec.w, rec.h);
                }
                else {
                    const float s = run.sharpen[k];
                    const taa_constants tc = constants(rec, t, false, std::min(s, 1.0f));
                    g.ctx->UpdateSubresource(g.cb.Get(), 0, nullptr, &tc, 0, 0);
                    fullscreen(g, s > 0 ? g.ps_sharpen.Get() : g.ps_copy.Get(), g.final_out.rtv.Get(), { nullptr, g.dlss_out.srv.Get() }, rec.w,
                        rec.h);
                }

                read_back(g, st, v, t, since_reset);
            }
        }

        for (size_t k = first; k < out.size(); ++k) {
            write_heat(st, out[k]);
        }

        std::printf(" %5.1f s, flicker %.4f, hot %.4f, sharpness %.4f\n",
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(), mean_of(out[first].flicker_series),
            hot_flicker(out[first]), mean_of(out[first].sharp_series));
        return true;
    }

    std::string escape(const std::string& s)
    {
        std::string out;

        for (char c : s) {
            if (c == '"' || c == '\\') {
                out += '\\';
            }

            out += c;
        }

        return out;
    }

    void write_metrics(const study& st, const std::vector<variant>& variants)
    {
        const recording& rec = st.rec;
        std::ofstream f(st.out / "metrics.json");
        f << "{\n  \"format\": \"edaa-replay-1\",\n  \"recording\": \"" << rec.name << "\",\n  \"frames\": " << rec.n
          << ",\n  \"warmup\": " << warmup << ",\n  \"margin\": " << margin << ",\n  \"size\": [" << rec.w << ", " << rec.h << "],\n";
        f << "  \"picks\": [";

        for (size_t i = 0; i < st.picks.size(); ++i) {
            f << (i ? ", " : "") << st.picks[i];
        }

        f << "],\n  \"burst\": {\"start\": " << st.burst_start << ", \"count\": " << burst_frames << ", \"rect\": [" << st.burst_x << ", "
          << st.burst_y << ", " << burst_w << ", " << burst_h << "]},\n  \"exposure\": [";

        for (uint32_t i = 0; i < rec.n; ++i) {
            f << (i ? ", " : "") << rec.exposure[i];
        }

        f << "],\n  \"on_foot\": " << ((rec.params[0].flags & edaa::recorder::flag_on_foot) ? "true" : "false") << ",\n  \"variants\": [\n";

        for (size_t k = 0; k < variants.size(); ++k) {
            const variant& v = variants[k];
            char head[512];
            std::snprintf(head, sizeof(head), "    {\"id\": \"%s\", \"label\": \"%s\", \"flicker\": %.6f, \"flicker_hot\": %.6f, \"sharpness\": %.6f, \"gpu_ms\": %.4f,\n",
                v.id.c_str(), escape(v.label).c_str(), mean_of(v.flicker_series), hot_flicker(v), mean_of(v.sharp_series),
                v.gpu_frames ? v.gpu_ms / v.gpu_frames : 0.0);
            f << head << "     \"flicker_series\": [";

            for (uint32_t i = 0; i < rec.n; ++i) {
                f << (i ? ", " : "") << std::lround(v.flicker_series[i] * 1e6) / 1e6;
            }

            f << "],\n     \"sharp_series\": [";

            for (uint32_t i = 0; i < rec.n; ++i) {
                f << (i ? ", " : "") << std::lround(v.sharp_series[i] * 1e6) / 1e6;
            }

            f << "]}" << (k + 1 < variants.size() ? "," : "") << "\n";
        }

        f << "  ]\n}\n";
    }
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3) {
        std::printf("edaa-replay <recording folder> <output folder> [run,run,...]\nruns:");

        for (const run_def& r : all_runs()) {
            std::printf(" %s", r.id.c_str());
        }

        std::printf("\n");
        return 2;
    }

    std::vector<std::string> wanted;
    std::vector<run_def> runs = all_runs();
    auto ascii = [](const wchar_t* w) {
        std::string out;

        for (; *w; ++w) {
            out += static_cast<char>(*w); // ids and settings are ASCII
        }

        return out;
    };

    // TAA experiments: --taa <id>=<key=value;...>, run in addition to the list
    // ("-" as the list: only the experiments).
    for (int i = 4; i < argc; i += 2) {
        if (std::wstring(argv[i]) != L"--taa" || i + 1 >= argc) {
            std::printf("unknown argument (--taa id=key=value;...)\n");
            return 2;
        }

        const std::string spec = ascii(argv[i + 1]);
        const size_t eq = spec.find('=');

        if (eq == std::string::npos) {
            std::printf("--taa id=key=value;...\n");
            return 2;
        }

        run_def r{ spec.substr(0, eq), "TAA " + spec.substr(eq + 1), kind::taa };
        r.taa = spec.substr(eq + 1);
        runs.push_back(r);
    }

    if (argc > 3) {
        std::wstring list = argv[3];
        std::wstringstream in(list);
        std::wstring item;

        while (std::getline(in, item, L',')) {
            std::string id;

            for (wchar_t c : item) {
                id += static_cast<char>(c); // run ids are ASCII
            }

            wanted.push_back(id);
        }
    }

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) {
        std::printf("WIC not available\n");
        return 1;
    }

    recording rec;
    const auto loading = std::chrono::steady_clock::now();

    if (!load(argv[1], rec)) {
        return 1;
    }

    std::printf("%s: %u frames, %ux%u crop of %ux%u, loaded in %.1f s\n", rec.name.c_str(), rec.n, rec.w, rec.h, rec.render_w, rec.render_h,
        std::chrono::duration<double>(std::chrono::steady_clock::now() - loading).count());

    if (rec.n < warmup + burst_frames + 2 || rec.w < burst_w + 2 * margin || rec.h < burst_h + 2 * margin) {
        std::printf("recording too short or crop too small\n");
        return 1;
    }

    study st{ rec, fs::path(argv[2]) / rec.name };
    fs::create_directories(st.out / "frames");
    fs::create_directories(st.out / "burst");
    fs::create_directories(st.out / "heat");

    for (double f : { 0.25, 0.5, 0.75, 1.0 }) {
        st.picks.push_back(std::min(rec.n - 1, std::max(warmup * 3, static_cast<uint32_t>(f * rec.n))));
    }

    st.burst_start = std::max(warmup * 2, rec.n / 2 - burst_frames / 2);
    st.burst_x = (rec.w - burst_w) / 2;
    st.burst_y = (rec.h - burst_h) / 2;

    gpu_state g;

    if (!init_gpu(g, rec.w, rec.h)) {
        std::printf("cannot create the D3D11 device or its resources\n");
        return 1;
    }

    std::vector<variant> variants;

    for (const run_def& run : runs) {
        if (!wanted.empty() && run.taa.empty() && std::find(wanted.begin(), wanted.end(), run.id) == wanted.end()) {
            continue;
        }

        const size_t before = variants.size();

        if (!play(g, st, run, variants)) {
            variants.resize(before); // a run that failed is left out
        }
    }

    write_metrics(st, variants);
    edaa::dlss::release(g.device.Get());
    std::printf("written to %s\n", st.out.string().c_str());
    return 0;
}
