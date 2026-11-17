// GPU timings of EDAA's passes (see gpu_profiler.hpp).

#include "gpu_profiler.hpp"
#include "reshade_includes.hpp"

#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace edaa::gpu_profiler
{
    namespace
    {
        constexpr int ring = 6; // frame sets in flight
        constexpr int max_pairs = 8; // begin/end pairs per section and frame
        constexpr double overlay_window_s = 2.0;
        constexpr double log_window_s = 10.0;

        struct frame_set
        {
            ComPtr<ID3D11Query> disjoint, start, end;
            ComPtr<ID3D11Query> ts[count][max_pairs][2];
            int used[count] = {};
            bool open = false, pending = false;
            char scene = '-';
        };

        struct sample
        {
            float frame = 0;
            float sec[count] = {};
            float edaa = 0;
        };

        // Mean and P99 of a list of samples.
        struct window
        {
            std::vector<sample> samples;
            LARGE_INTEGER start = {};
            char scene = '-';
        };

        std::mutex mutex;
        ID3D11Device* device = nullptr; // the queries hold their own references
        frame_set sets[ring];
        int current = 0;
        bool active = false; // a set is open: sections are timed
        uint32_t skipped = 0;
        window overlay_window, log_window;
        results published;
        char scene_marker = '-';


        float p99_of(std::vector<float>& v)
        {
            if (v.empty()) {
                return 0.0f;
            }

            const size_t k = std::min(v.size() - 1, static_cast<size_t>(static_cast<double>(v.size()) * 0.99));
            std::nth_element(v.begin(), v.begin() + static_cast<ptrdiff_t>(k), v.end());
            return v[k];
        }


        results summarise(const std::vector<sample>& samples)
        {
            results r;
            r.frames = static_cast<uint32_t>(samples.size());
            r.skipped = skipped;

            if (samples.empty()) {
                return r;
            }

            std::vector<float> values(samples.size());
            auto stat = [&](auto get, float& mean, float& p99) {
                double sum = 0;

                for (size_t i = 0; i < samples.size(); ++i) {
                    values[i] = get(samples[i]);
                    sum += values[i];
                }

                mean = static_cast<float>(sum / static_cast<double>(samples.size()));
                p99 = p99_of(values);
            };

            stat([](const sample& x) { return x.frame; }, r.frame_mean, r.frame_p99);
            stat([](const sample& x) { return x.edaa; }, r.edaa_mean, r.edaa_p99);

            for (int s = 0; s < count; ++s) {
                stat([s](const sample& x) { return x.sec[s]; }, r.mean[s], r.p99[s]);
            }

            r.elite_mean = std::max(0.0f, r.frame_mean - r.edaa_mean);
            r.valid = true;
            return r;
        }


        void log_results(const window& w)
        {
            if (w.samples.size() < 30) {
                return; // a scene change right after the last line
            }

            const results r = summarise(w.samples);
            char line[512];
            snprintf(line, sizeof(line),
                "EDAA GPU: scene %c | frame %.2f ms (p99 %.2f) | motion %.2f | fg_in %.2f | hud %.2f | dlss %.2f (p99 %.2f) | taa %.2f | fg_out %.2f | reactive %.2f | "
                "edaa %.2f (p99 %.2f) | elite %.2f | %u frames, %u skipped",
                w.scene, r.frame_mean, r.frame_p99, r.mean[motion], r.mean[fg_in], r.mean[hud], r.mean[dlss], r.p99[dlss], r.mean[taa],
                r.mean[fg_out], r.mean[reactive], r.edaa_mean, r.edaa_p99, r.elite_mean, r.frames, r.skipped);
            reshade::log::message(reshade::log::level::info, line);
        }


        void reset_windows(char scene)
        {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            overlay_window = {};
            log_window = {};
            overlay_window.start = log_window.start = now;
            overlay_window.scene = log_window.scene = scene;
        }


        void drop_queries()
        {
            for (frame_set& f : sets) {
                f = frame_set();
            }

            device = nullptr;
            active = false;
            current = 0;
        }


        bool create_queries(ID3D11Device* d)
        {
            drop_queries();
            D3D11_QUERY_DESC disjoint_desc = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
            D3D11_QUERY_DESC ts_desc = { D3D11_QUERY_TIMESTAMP, 0 };

            for (frame_set& f : sets) {
                if (FAILED(d->CreateQuery(&disjoint_desc, &f.disjoint)) || FAILED(d->CreateQuery(&ts_desc, &f.start)) ||
                    FAILED(d->CreateQuery(&ts_desc, &f.end))) {
                    drop_queries();
                    return false;
                }

                for (auto& section_pairs : f.ts) {
                    for (auto& pair : section_pairs) {
                        if (FAILED(d->CreateQuery(&ts_desc, &pair[0])) || FAILED(d->CreateQuery(&ts_desc, &pair[1]))) {
                            drop_queries();
                            return false;
                        }
                    }
                }
            }

            device = d;
            return true;
        }


        bool ready(ID3D11DeviceContext* ctx, ID3D11Query* q, UINT64& value)
        {
            return ctx->GetData(q, &value, sizeof(value), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
        }


        // Results of a closed set: false while the GPU has not reached its end.
        bool read(ID3D11DeviceContext* ctx, frame_set& f, sample& out, bool& usable)
        {
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};

            if (ctx->GetData(f.disjoint.Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
                return false;
            }

            UINT64 t0, t1;

            if (!ready(ctx, f.start.Get(), t0) || !ready(ctx, f.end.Get(), t1)) {
                return false;
            }

            usable = !dj.Disjoint && dj.Frequency != 0 && t1 >= t0;
            const double to_ms = usable ? 1000.0 / static_cast<double>(dj.Frequency) : 0.0;
            out = {};
            out.frame = static_cast<float>(static_cast<double>(t1 - t0) * to_ms);

            for (int s = 0; s < count; ++s) {
                double sum = 0;

                for (int i = 0; i < f.used[s]; ++i) {
                    UINT64 a, b;

                    if (!ready(ctx, f.ts[s][i][0].Get(), a) || !ready(ctx, f.ts[s][i][1].Get(), b)) {
                        return false;
                    }

                    if (b > a) {
                        sum += static_cast<double>(b - a) * to_ms;
                    }
                }

                out.sec[s] = static_cast<float>(sum);
                out.edaa += out.sec[s];
            }

            return true;
        }


        void add_sample(const sample& x, char scene)
        {
            if (scene != log_window.scene) {
                log_results(log_window);
                reset_windows(scene);
            }

            overlay_window.samples.push_back(x);
            log_window.samples.push_back(x);
        }


        void close_windows()
        {
            LARGE_INTEGER now, frequency;
            QueryPerformanceCounter(&now);
            QueryPerformanceFrequency(&frequency);
            auto seconds = [&](const window& w) {
                return static_cast<double>(now.QuadPart - w.start.QuadPart) / static_cast<double>(frequency.QuadPart);
            };

            if (seconds(overlay_window) >= overlay_window_s) {
                if (!overlay_window.samples.empty()) {
                    published = summarise(overlay_window.samples);
                }

                overlay_window.samples.clear();
                overlay_window.start = now;
            }

            if (seconds(log_window) >= log_window_s) {
                log_results(log_window);
                log_window.samples.clear();
                log_window.start = now;
            }
        }
    }


    scope::scope(ID3D11DeviceContext* c, section s) : ctx(c), which(s), open(false)
    {
        const std::lock_guard<std::mutex> lock(mutex);

        if (!active || ctx == nullptr) {
            return;
        }

        frame_set& f = sets[current];

        if (f.used[which] >= max_pairs) {
            return;
        }

        ctx->End(f.ts[which][f.used[which]][0].Get());
        open = true;
    }


    scope::~scope()
    {
        if (!open) {
            return;
        }

        const std::lock_guard<std::mutex> lock(mutex);

        if (!active) {
            return; // switched off in between: the set is gone
        }

        frame_set& f = sets[current];
        ctx->End(f.ts[which][f.used[which]][1].Get());
        ++f.used[which];
    }


    void frame(ID3D11DeviceContext* ctx, bool enabled)
    {
        const std::lock_guard<std::mutex> lock(mutex);

        if (!enabled || ctx == nullptr) {
            if (device != nullptr) {
                drop_queries();
                published = {};
            }

            return;
        }

        ComPtr<ID3D11Device> d;
        ctx->GetDevice(&d);

        if (d.Get() != device) {
            if (!create_queries(d.Get())) {
                return;
            }

            skipped = 0;
            published = {};
            reset_windows(scene_marker);
        }

        // Close the frame in progress.
        frame_set& cur = sets[current];

        if (cur.open) {
            ctx->End(cur.end.Get());
            ctx->End(cur.disjoint.Get());
            cur.open = false;
            cur.pending = true;
        }

        // Read the finished ones, oldest first; stop at the first not ready.
        for (int i = 1; i <= ring; ++i) {
            frame_set& f = sets[(current + i) % ring];

            if (!f.pending) {
                continue;
            }

            sample x;
            bool usable = false;

            if (!read(ctx, f, x, usable)) {
                break;
            }

            f.pending = false;

            if (usable) {
                add_sample(x, f.scene);
            }
            else {
                ++skipped;
            }
        }

        // Open the next one: a set still pending is dropped.
        current = (current + 1) % ring;
        frame_set& next = sets[current];

        if (next.pending) {
            next.pending = false;
            ++skipped;
        }

        std::fill(std::begin(next.used), std::end(next.used), 0);
        next.scene = scene_marker;
        ctx->Begin(next.disjoint.Get());
        ctx->End(next.start.Get());
        next.open = true;
        active = true;
        close_windows();
    }


    results last()
    {
        const std::lock_guard<std::mutex> lock(mutex);
        return published;
    }


    const char* section_name(section s)
    {
        static const char* const names[count] = { "motion", "fg_in", "hud", "dlss", "taa", "fg_out", "reactive" };
        return s >= 0 && s < count ? names[s] : "?";
    }


    void next_scene()
    {
        char scene;
        {
            const std::lock_guard<std::mutex> lock(mutex);
            scene_marker = scene_marker == '-' ? 'A' : scene_marker == 'E' ? '-' : static_cast<char>(scene_marker + 1);
            scene = scene_marker;
        }
        char line[64];
        snprintf(line, sizeof(line), "EDAA GPU: scene %c", scene);
        reshade::log::message(reshade::log::level::info, line);
    }


    char scene()
    {
        const std::lock_guard<std::mutex> lock(mutex);
        return scene_marker;
    }


    void release()
    {
        const std::lock_guard<std::mutex> lock(mutex);
        drop_queries();
        published = {};
    }
}
