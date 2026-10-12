#include "camera.hpp"

#include <cmath>

namespace edaa
{
    namespace
    {
        const float* r(const float* regs, int index)
        {
            return regs + index * 4;
        }


        bool near_zero(float v)
        {
            return std::fabs(v) < 1e-6f;
        }
    }


    const char* to_string(view_kind kind)
    {
        switch (kind) {
            case view_kind::perspective: return "perspective";
            case view_kind::orthographic: return "orthographic";
            default: return "none";
        }
    }


    camera_info decode_camera(const float* regs)
    {
        camera_info info;

        for (int i = 0; i < 3; ++i) {
            info.forward[i] = r(regs, reg::view_proj + i)[3];
            info.camera_pos[i] = r(regs, reg::camera_pos)[i];
        }

        // c273 = (0, 0, near, 0) for a reverse-Z infinite perspective,
        // (0, 0, z, 1) for an orthographic projection.
        const float* vp3 = r(regs, reg::view_proj + 3);

        if (!near_zero(vp3[0]) || !near_zero(vp3[1])) {
            info.reason = "c273 is not (0, 0, near, 0) or (0, 0, z, 1)";
            return info;
        }

        if (std::fabs(vp3[3] - 1.0f) < 1e-6f) {
            info.kind = view_kind::orthographic;
            info.reason = "c273.w = 1";
            return info;
        }

        if (!near_zero(vp3[3]) || vp3[2] <= 0.0f) {
            info.reason = "c273 is not (0, 0, near, 0) or (0, 0, z, 1)";
            return info;
        }

        // viewProj = rotation * P: x/y columns have lengths sx/sy, w column is unit.
        float len_x = 0, len_y = 0, len_w = 0;

        for (int i = 0; i < 3; ++i) {
            const float* row = r(regs, reg::view_proj + i);
            len_x += row[0] * row[0];
            len_y += row[1] * row[1];
            len_w += row[3] * row[3];
        }

        info.sx = std::sqrt(len_x);
        info.sy = std::sqrt(len_y);
        info.w_length = std::sqrt(len_w);
        info.near_plane = vp3[2];

        if (info.sx <= 0.0f || info.sy <= 0.0f || std::fabs(info.w_length - 1.0f) > 1e-3f) {
            info.reason = "c270-c272 is not rotation * projection";
            return info;
        }

        info.vfov_deg = 2.0f * std::atan(1.0f / info.sy) * 57.2957795f;
        info.aspect = info.sy / info.sx;
        info.kind = view_kind::perspective;
        info.reason = "ok";
        return info;
    }
}
