// CPU time spent in EDAA's event handlers, developer builds only (overlay:
// "EDAA CPU time"). EDAA_PROFILE(slot) times the rest of the enclosing scope.
// Scopes nest: the draw hooks include the passes they run.

#pragma once

#include <atomic>
#include <cstdint>
#include <Windows.h>

namespace edaa::profile
{
    enum slot : int
    {
        draws, // draw / draw_indexed hooks (resolve and upscale passes included)
        binds, // render target, pipeline and viewport binds
        maps, // buffer Map/Unmap (camera, instance records)
        passes, // resolve, upscale and HUD mask passes
        present, // Present (camera, objects, debug view, capture)
        count
    };

    inline std::atomic<int64_t> ticks[count] = {};


    struct scope
    {
        slot which;
        int64_t start;


        explicit scope(slot s) : which(s)
        {
            LARGE_INTEGER t;
            QueryPerformanceCounter(&t);
            start = t.QuadPart;
        }


        ~scope()
        {
            LARGE_INTEGER t;
            QueryPerformanceCounter(&t);
            ticks[which].fetch_add(t.QuadPart - start, std::memory_order_relaxed);
        }
    };


    // Milliseconds per slot since the last call, and resets (at Present).
    inline void take(float out[count])
    {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);

        for (int i = 0; i < count; ++i) {
            out[i] = static_cast<float>(static_cast<double>(ticks[i].exchange(0, std::memory_order_relaxed)) * 1000.0 / static_cast<double>(f.QuadPart));
        }
    }
}

#if EDAA_DEVELOPER
#define EDAA_PROFILE(s) const edaa::profile::scope edaa_profile_scope(edaa::profile::s)
#else
#define EDAA_PROFILE(s) ((void)0)
#endif
