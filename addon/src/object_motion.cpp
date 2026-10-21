// Per-object motion, step 1: analysis only (developer builds).
//
// Instanced models get their transform from the structured buffer t33
// (336-byte records, docs/reverse-engineering.md section 2.1), written by the
// CPU with a single Map/Unmap at the start of each frame. The game keeps no
// previous transform and reorders the records every frame, so:
//   1. at Unmap, copy the first 32 bytes of every record (bone base, scale,
//      packed rotation, position);
//   2. at Present, match each record to one of the previous frame: same
//      position = still (or spinning if the rotation changed), otherwise the
//      nearest previous record of the same kind and scale within 32 m
//      (checked offline on the cockpit and SRV capture pairs);
//   3. for moving records on screen, measure how far camera reprojection
//      misses them: history is fetched where the camera alone would have put
//      the point, the object really was elsewhere -> ghost offset in pixels.
// Results go to the overlay and the "Moving objects" debug view.
//
// Used in every build: the motion of the player's vehicle (vehicle_motion).
// Cockpit and SRV parts are rigid with the hull, so matching the records near
// the camera between two frames gives one rigid transform. Applied to the
// vehicle pixels (stencil mask), it gives them their true motion, head
// movements included, instead of none.

#include "profile.hpp"
#include "events.hpp"
#include "state.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <smmintrin.h>
#include <unordered_map>
#include <vector>
#include <Windows.h>

using namespace reshade::api;

namespace edaa
{
    namespace
    {
        constexpr uint32_t record_stride = 336;
        constexpr float cell_size = 32.0f; // metres: matching radius for moving records

        // First 32 bytes of a t33 record.
        struct record
        {
            uint32_t base; // bone/sub-object base index (0 = rigid)
            float scale;
            uint64_t rotation; // packed quaternion, 4 x 16 bit
            float pos[3]; // local world frame
            uint32_t flags;
        };
        static_assert(sizeof(record) == 32);

        std::atomic<bool> enabled{ false }; // records read (vehicle motion or analysis)
        std::mutex records_mutex; // protects everything below
        std::unordered_map<uint64_t, void*> mapped; // t33-like buffers waiting for Unmap
        std::vector<record> pending; // records written this frame
        size_t pending_count = 0; // records in the buffer they come from (the largest one is kept)
        // Vehicle motion only needs the records around the camera: reading the whole
        // write-combined buffer every frame cost several ms. All of them for the analysis.
        bool keep_all = false;
        float near_center[3] = {};
        float near_radius = 0;
        uint32_t pending_maps = 0;
        float pending_read_ms = 0;
        std::vector<record> previous; // records of the previous frame


        // Milliseconds since boot (double: a float has ~8 ms resolution there).
        double now_ms()
        {
            LARGE_INTEGER t, f;
            QueryPerformanceCounter(&t);
            QueryPerformanceFrequency(&f);
            return static_cast<double>(t.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
        }


        // The mapping is write-combined memory: plain reads are very slow, SSE4.1
        // streaming loads are the fast way to read it back. The position (second
        // 16 bytes) is read first: far records are skipped unless keep_all (records lock held).
        void copy_records(const uint8_t* src, size_t count, std::vector<record>& out)
        {
            out.clear();
            out.reserve(keep_all ? count : 256);
            const bool aligned = (reinterpret_cast<uintptr_t>(src) & 15) == 0;
            const float r2 = near_radius * near_radius;

            for (size_t i = 0; i < count; ++i) {
                const uint8_t* p = src + i * record_stride;
                record r;

                if (aligned) {
                    const __m128i b = _mm_stream_load_si128(reinterpret_cast<const __m128i*>(p + 16));
                    _mm_storeu_si128(reinterpret_cast<__m128i*>(&r) + 1, b);
                }
                else {
                    std::memcpy(reinterpret_cast<uint8_t*>(&r) + 16, p + 16, 16);
                }

                if (!keep_all) {
                    const float dx = r.pos[0] - near_center[0], dy = r.pos[1] - near_center[1], dz = r.pos[2] - near_center[2];

                    if (!(dx * dx + dy * dy + dz * dz < r2)) {
                        continue;
                    }
                }

                if (aligned) {
                    const __m128i a = _mm_stream_load_si128(reinterpret_cast<const __m128i*>(p));
                    _mm_storeu_si128(reinterpret_cast<__m128i*>(&r), a);
                }
                else {
                    std::memcpy(&r, p, 16);
                }

                out.push_back(r);
            }
        }


        bool is_instance_buffer(device* device, resource resource)
        {
            const resource_desc desc = device->get_resource_desc(resource);
            return desc.type == resource_type::buffer && desc.buffer.structured.stride == record_stride &&
                desc.buffer.size >= record_stride;
        }


        void on_map_buffer_region(device* device, resource resource, uint64_t offset, uint64_t, map_access access, void** data)
        {
            EDAA_PROFILE(maps);

            if (!is_game_device(device)) {
                return;
            }

            if (!enabled.load() || data == nullptr || *data == nullptr || offset != 0 || access == map_access::read_only ||
                !is_instance_buffer(device, resource)) {
                return;
            }

            const std::lock_guard<std::mutex> lock(records_mutex);
            mapped[resource.handle] = *data;
        }


        void on_unmap_buffer_region(device* device, resource resource)
        {
            EDAA_PROFILE(maps);

            if (!is_game_device(device)) {
                return;
            }

            const uint8_t* data = nullptr;
            {
                const std::lock_guard<std::mutex> lock(records_mutex);
                const auto it = mapped.find(resource.handle);

                if (it == mapped.end()) {
                    return;
                }

                data = static_cast<const uint8_t*>(it->second);
                mapped.erase(it);
            }

            // No device call with the records lock held (lock order, see frame_tracking.cpp).
            // Called before the native Unmap: the mapping is still valid.
            const size_t count = static_cast<size_t>(device->get_resource_desc(resource).buffer.size / record_stride);
            const std::lock_guard<std::mutex> lock(records_mutex);
            const double start = now_ms();

            if (count >= pending_count) { // several such buffers: keep the largest
                pending_count = count;
                copy_records(data, count, pending);
            }
            pending_read_ms += static_cast<float>(now_ms() - start);
            pending_maps++;
        }


        bool valid(const record& r)
        {
            if (!(r.scale > 0.0f && r.scale < 1e6f)) {
                return false;
            }

            for (float v : r.pos) {
                if (!std::isfinite(v) || std::fabs(v) > 1e7f) {
                    return false;
                }
            }

            return true;
        }


        using position_key = std::array<uint32_t, 3>;


        position_key key_of(const record& r)
        {
            position_key k;
            std::memcpy(k.data(), r.pos, sizeof(k));
            return k;
        }


        int64_t cell_of(const float pos[3], int dx = 0, int dy = 0, int dz = 0)
        {
            const int64_t x = static_cast<int64_t>(std::floor(pos[0] / cell_size)) + dx;
            const int64_t y = static_cast<int64_t>(std::floor(pos[1] / cell_size)) + dy;
            const int64_t z = static_cast<int64_t>(std::floor(pos[2] / cell_size)) + dz;
            return (x & 0x1FFFFF) | ((y & 0x1FFFFF) << 21) | ((z & 0x1FFFFF) << 42);
        }


        // Row-vector viewProj (c270-c273) applied to a camera-relative point -> UV; false if behind.
        bool project(const camera_frame& c, const float world[3], float uv[2])
        {
            const float p[3] = { world[0] - c.position[0], world[1] - c.position[1], world[2] - c.position[2] };
            float clip[4];

            for (int j = 0; j < 4; ++j) {
                clip[j] = p[0] * c.view_proj[j] + p[1] * c.view_proj[4 + j] + p[2] * c.view_proj[8 + j] + c.view_proj[12 + j];
            }

            if (clip[3] <= 1e-6f) {
                return false;
            }

            uv[0] = clip[0] / clip[3] * 0.5f + 0.5f;
            uv[1] = 0.5f - clip[1] / clip[3] * 0.5f;
            return true;
        }


        float distance(const float a[3], const float b[3])
        {
            const float d[3] = { a[0] - b[0], a[1] - b[1], a[2] - b[2] };
            return std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        }


        // Matches 'cur' against 'prev' and fills the stats and markers (state locked).
        void match(addon_state& s, const std::vector<record>& cur, const std::vector<record>& prev)
        {
            object_stats& st = s.objects;

            // Previous records by exact position.
            std::vector<std::pair<position_key, uint32_t>> by_position;
            by_position.reserve(prev.size());

            for (uint32_t i = 0; i < prev.size(); ++i) {
                if (valid(prev[i])) {
                    by_position.emplace_back(key_of(prev[i]), i);
                }
            }

            std::sort(by_position.begin(), by_position.end());

            // Matching is many-to-one: the game often writes several records for
            // one object (one per pass), and their number varies between frames.
            std::vector<uint32_t> unmatched;
            std::vector<object_marker> spinning; // a few, to check in the debug view that rings land on objects

            for (uint32_t i = 0; i < cur.size(); ++i) {
                if (!valid(cur[i])) {
                    continue;
                }

                st.records++;
                const auto it = std::lower_bound(by_position.begin(), by_position.end(), std::make_pair(key_of(cur[i]), 0u));

                if (it != by_position.end() && it->first == key_of(cur[i])) {
                    if (prev[it->second].rotation == cur[i].rotation) {
                        st.still++;
                    }
                    else {
                        st.spinning++;
                        float uv[2];

                        if (spinning.size() < 32 && project(s.camera, cur[i].pos, uv) && uv[0] >= 0 && uv[0] <= 1 && uv[1] >= 0 && uv[1] <= 1) {
                            object_marker m;
                            m.x = uv[0];
                            m.y = uv[1];
                            m.kind = object_marker::spinning;
                            spinning.push_back(m);
                        }
                    }
                }
                else {
                    unmatched.push_back(i);
                }
            }

            // Previous records by grid cell.
            std::vector<std::pair<int64_t, uint32_t>> by_cell;

            for (uint32_t i = 0; i < prev.size(); ++i) {
                if (valid(prev[i])) {
                    by_cell.emplace_back(cell_of(prev[i].pos), i);
                }
            }

            std::sort(by_cell.begin(), by_cell.end());

            float cam_delta[3], cam_speed = 0;

            for (int k = 0; k < 3; ++k) {
                cam_delta[k] = s.camera.position[k] - s.prev_camera.position[k];
                cam_speed += cam_delta[k] * cam_delta[k];
            }

            cam_speed = std::sqrt(cam_speed);

            std::vector<object_marker> markers;
            double error_sum = 0;

            for (const uint32_t i : unmatched) {
                const record& r = cur[i];
                int best = -1;
                float best_d = cell_size;

                for (int dz = -1; dz <= 1; ++dz) {
                    for (int dy = -1; dy <= 1; ++dy) {
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int64_t cell = cell_of(r.pos, dx, dy, dz);

                            for (auto it = std::lower_bound(by_cell.begin(), by_cell.end(), std::make_pair(cell, 0u));
                                it != by_cell.end() && it->first == cell; ++it) {
                                const record& p = prev[it->second];

                                // Same kind (rigid or skinned: bone indices are reallocated) and scale.
                                if ((p.base == 0) != (r.base == 0) || std::fabs(p.scale - r.scale) > 1e-3f * r.scale) {
                                    continue;
                                }

                                const float d = distance(p.pos, r.pos);

                                if (d < best_d) {
                                    best_d = d;
                                    best = static_cast<int>(it->second);
                                }
                            }
                        }
                    }
                }

                if (best < 0) {
                    st.unmatched++;
                    continue;
                }

                const record& p = prev[best];

                // Close to the camera and, if the camera moves, moving about like it:
                // part of the player's vehicle (cockpit, SRV; the stencil mask covers
                // them). No vehicle on foot: people walking nearby are real motion.
                float rel = 0;

                for (int k = 0; k < 3; ++k) {
                    const float d = (r.pos[k] - p.pos[k]) - cam_delta[k];
                    rel += d * d;
                }

                const bool vehicle = !s.on_foot && distance(r.pos, s.camera.position) < 20.0f &&
                    (cam_speed <= 0.01f || std::sqrt(rel) < cam_speed + 1.0f);
                (vehicle ? st.vehicle : st.moving)++;

                // Ghost offset: where camera reprojection fetches history minus where the object was.
                float uv[2], reprojected[2], truth[2];

                if (!project(s.camera, r.pos, uv) || uv[0] < 0 || uv[0] > 1 || uv[1] < 0 || uv[1] > 1 ||
                    !project(s.prev_camera, r.pos, reprojected) || !project(s.prev_camera, p.pos, truth)) {
                    continue;
                }

                object_marker m;
                m.x = uv[0];
                m.y = uv[1];
                m.offset_x = truth[0] - reprojected[0];
                m.offset_y = truth[1] - reprojected[1];
                const float ex = m.offset_x * static_cast<float>(s.render_w), ey = m.offset_y * static_cast<float>(s.render_h);
                m.error_px = std::sqrt(ex * ex + ey * ey);
                m.kind = vehicle ? object_marker::vehicle : object_marker::moving;
                markers.push_back(m);

                if (!vehicle) {
                    st.visible_moving++;
                    error_sum += m.error_px;
                    st.max_error_px = std::max(st.max_error_px, m.error_px);
                }
            }

            if (st.visible_moving > 0) {
                st.mean_error_px = static_cast<float>(error_sum / st.visible_moving);
            }

            // Debug view: the largest errors first, then spinning objects in the free slots.
            const size_t n = std::min<size_t>(markers.size(), addon_state::max_markers);
            std::partial_sort(markers.begin(), markers.begin() + n, markers.end(),
                [](const object_marker& a, const object_marker& b) { return a.error_px > b.error_px; });
            markers.resize(n);

            for (size_t i = 0; i < spinning.size() && markers.size() < addon_state::max_markers; ++i) {
                markers.push_back(spinning[i]);
            }

            std::copy(markers.begin(), markers.end(), s.markers);
            s.marker_count = static_cast<int>(markers.size());
        }
    }


    void analyse_objects(addon_state& s)
    {
        const bool on = s.config.object_analysis || s.config.debug == static_cast<int>(debug_view::objects);
        enabled = on || (s.config.vehicle_motion && !s.on_foot);

        std::vector<record> cur;
        uint32_t maps;
        float read_ms;
        {
            const std::lock_guard<std::mutex> lock(records_mutex);
            // Next frame: records within reach of the vehicle around this camera.
            keep_all = on;
            std::memcpy(near_center, s.camera.position, sizeof(near_center));
            near_radius = 25.0f + 4.0f * s.camera_speed;
            pending_count = 0;
            cur.swap(pending);
            maps = pending_maps;
            read_ms = pending_read_ms;
            pending_maps = 0;
            pending_read_ms = 0;
        }

        s.objects = {};
        s.marker_count = 0;

        if (!on) {
            const std::lock_guard<std::mutex> lock(records_mutex);
            previous.swap(cur); // kept for vehicle_motion
            return;
        }

        s.objects.buffer_maps = maps;
        s.objects.read_ms = read_ms;

        std::vector<record> prev;
        {
            const std::lock_guard<std::mutex> lock(records_mutex);
            prev.swap(previous);
        }
        const double start = now_ms();

        if (!cur.empty() && !prev.empty() && s.camera.valid && s.prev_camera.valid && !s.map_mode && !s.screenshot_mode) {
            match(s, cur, prev);
        }

        s.objects.match_ms = static_cast<float>(now_ms() - start);

        const std::lock_guard<std::mutex> lock(records_mutex);
        previous.swap(cur);
    }


    namespace
    {
        // Unit quaternion (x, y, z, w) of a packed record rotation -> row-major 3x3 matrix.
        void rotation_matrix(uint64_t packed, double m[9])
        {
            double q[4];

            for (int i = 0; i < 4; ++i) {
                q[i] = static_cast<double>((packed >> (16 * i)) & 0xFFFF) / 32768.0 - 1.0;
            }

            const double n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
            const double x = q[0] / n, y = q[1] / n, z = q[2] / n, w = q[3] / n;
            const double r[9] = { 1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w),
                2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
                2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y) };
            std::memcpy(m, r, sizeof(r));
        }


        // Point of a record's model, 'd' metres along its axes, camera-relative.
        std::array<double, 3> model_point(const record& r, const double rot[9], const double d[3], const float camera[3])
        {
            std::array<double, 3> out;

            for (int i = 0; i < 3; ++i) {
                out[i] = r.pos[i] - static_cast<double>(camera[i]) + rot[i * 3] * d[0] + rot[i * 3 + 1] * d[1] + rot[i * 3 + 2] * d[2];
            }

            return out;
        }


        // Largest eigenvalue's eigenvector of a symmetric 4x4 matrix (cyclic Jacobi).
        void largest_eigenvector(double a[4][4], double out[4])
        {
            double v[4][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } };

            for (int sweep = 0; sweep < 50; ++sweep) {
                double off = 0;

                for (int p = 0; p < 4; ++p) {
                    for (int q = p + 1; q < 4; ++q) {
                        off += a[p][q] * a[p][q];
                    }
                }

                if (off < 1e-30) {
                    break;
                }

                for (int p = 0; p < 4; ++p) {
                    for (int q = p + 1; q < 4; ++q) {
                        if (std::fabs(a[p][q]) < 1e-300) {
                            continue;
                        }

                        const double theta = (a[q][q] - a[p][p]) / (2 * a[p][q]);
                        const double tn = (theta >= 0 ? 1.0 : -1.0) / (std::fabs(theta) + std::sqrt(theta * theta + 1));
                        const double c = 1 / std::sqrt(tn * tn + 1), s = tn * c;

                        for (int k = 0; k < 4; ++k) {
                            const double akp = a[k][p], akq = a[k][q];
                            a[k][p] = c * akp - s * akq;
                            a[k][q] = s * akp + c * akq;
                        }

                        for (int k = 0; k < 4; ++k) {
                            const double apk = a[p][k], aqk = a[q][k];
                            a[p][k] = c * apk - s * aqk;
                            a[q][k] = s * apk + c * aqk;
                        }

                        for (int k = 0; k < 4; ++k) {
                            const double vkp = v[k][p], vkq = v[k][q];
                            v[k][p] = c * vkp - s * vkq;
                            v[k][q] = s * vkp + c * vkq;
                        }
                    }
                }
            }

            int best = 0;

            for (int i = 1; i < 4; ++i) {
                best = a[i][i] > a[best][best] ? i : best;
            }

            for (int i = 0; i < 4; ++i) {
                out[i] = v[i][best];
            }
        }


        // Least-squares rigid motion b = R a + t over point pairs (Horn 1987,
        // closed form with quaternions). R row-major.
        void fit_rigid(const std::vector<std::array<double, 3>>& a, const std::vector<std::array<double, 3>>& b, double r[9], double t[3])
        {
            double ca[3] = {}, cb[3] = {};
            const double n = static_cast<double>(a.size());

            for (size_t i = 0; i < a.size(); ++i) {
                for (int k = 0; k < 3; ++k) {
                    ca[k] += a[i][k] / n;
                    cb[k] += b[i][k] / n;
                }
            }

            double s[3][3] = {}; // sum of (a - ca)(b - cb)^T

            for (size_t i = 0; i < a.size(); ++i) {
                for (int j = 0; j < 3; ++j) {
                    for (int k = 0; k < 3; ++k) {
                        s[j][k] += (a[i][j] - ca[j]) * (b[i][k] - cb[k]);
                    }
                }
            }

            double m[4][4] = {
                { s[0][0] + s[1][1] + s[2][2], s[1][2] - s[2][1], s[2][0] - s[0][2], s[0][1] - s[1][0] },
                { s[1][2] - s[2][1], s[0][0] - s[1][1] - s[2][2], s[0][1] + s[1][0], s[2][0] + s[0][2] },
                { s[2][0] - s[0][2], s[0][1] + s[1][0], -s[0][0] + s[1][1] - s[2][2], s[1][2] + s[2][1] },
                { s[0][1] - s[1][0], s[2][0] + s[0][2], s[1][2] + s[2][1], -s[0][0] - s[1][1] + s[2][2] },
            };
            double q[4]; // w, x, y, z
            largest_eigenvector(m, q);
            const double w = q[0], x = q[1], y = q[2], z = q[3];
            const double rot[9] = { w * w + x * x - y * y - z * z, 2 * (x * y - w * z), 2 * (x * z + w * y),
                2 * (x * y + w * z), w * w - x * x + y * y - z * z, 2 * (y * z - w * x),
                2 * (x * z - w * y), 2 * (y * z + w * x), w * w - x * x - y * y + z * z };
            std::memcpy(r, rot, sizeof(rot));

            for (int i = 0; i < 3; ++i) {
                t[i] = cb[i] - (r[i * 3] * ca[0] + r[i * 3 + 1] * ca[1] + r[i * 3 + 2] * ca[2]);
            }
        }
    }


    bool vehicle_motion(const camera_frame& cur_cam, const camera_frame& prev_cam, float out[12])
    {
        // This frame's records (written at the start of the frame) and the previous
        // frame's, used in place: render thread, the lock only guards against Unmap.
        const std::lock_guard<std::mutex> lock(records_mutex);
        const std::vector<record>& cur = pending;
        const std::vector<record>& prev = previous;

        if (cur.empty() || prev.empty() || !cur_cam.valid || !prev_cam.valid) {
            return false;
        }

        double cam_delta[3];

        for (int k = 0; k < 3; ++k) {
            cam_delta[k] = static_cast<double>(cur_cam.position[k]) - prev_cam.position[k];
        }

        // Records near the camera, matched to the previous one at the same place
        // relative to the camera (the vehicle carries both), same kind and scale.
        std::vector<const record*> near_prev;

        for (const record& p : prev) {
            if (valid(p) && distance(p.pos, prev_cam.position) < 20.0f) {
                near_prev.push_back(&p);
            }
        }

        // Each matched record gives 7 point pairs: its origin and points 0.5 m
        // along its axes (so its rotation counts), camera-relative, current ->
        // previous. The vehicle's parts do not move exactly alike (vibrations
        // shake them by a few tenths of a degree each): the motion closest to
        // all of them (least squares) is used, after dropping the outliers
        // (animated parts, wrong matches).
        constexpr double offsets[7][3] = { { 0, 0, 0 }, { 0.5, 0, 0 }, { -0.5, 0, 0 }, { 0, 0.5, 0 }, { 0, -0.5, 0 }, { 0, 0, 0.5 }, { 0, 0, -0.5 } };
        std::vector<std::array<double, 3>> from, to;

        for (const record& c : cur) {
            if (!valid(c) || distance(c.pos, cur_cam.position) >= 20.0f || from.size() >= 256 * 7) {
                continue;
            }

            const record* best = nullptr;
            double best_d = 0.25; // metres, once the camera motion is removed

            for (const record* p : near_prev) {
                if ((p->base == 0) != (c.base == 0) || std::fabs(p->scale - c.scale) > 1e-3f * c.scale) {
                    continue;
                }

                double d2 = 0;

                for (int k = 0; k < 3; ++k) {
                    const double d = c.pos[k] - p->pos[k] - cam_delta[k];
                    d2 += d * d;
                }

                if (std::sqrt(d2) < best_d) {
                    best_d = std::sqrt(d2);
                    best = p;
                }
            }

            if (best == nullptr) {
                continue;
            }

            double rc[9], rp[9];
            rotation_matrix(c.rotation, rc);
            rotation_matrix(best->rotation, rp);

            for (const auto& d : offsets) {
                from.push_back(model_point(c, rc, d, cur_cam.position));
                to.push_back(model_point(*best, rp, d, prev_cam.position));
            }
        }

        const size_t matched = from.size() / 7;

        if (matched < 3) {
            return false;
        }

        double r[9], t[3];
        fit_rigid(from, to, r, t);

        // Outliers: records whose points miss the fit by more than 3 times the
        // median miss (at least 2 mm). Twice, then the fit on what remains.
        for (int pass = 0; pass < 2; ++pass) {
            std::vector<double> miss(from.size() / 7, 0.0);

            for (size_t i = 0; i < from.size(); ++i) {
                double e2 = 0;

                for (int k = 0; k < 3; ++k) {
                    const double e = r[k * 3] * from[i][0] + r[k * 3 + 1] * from[i][1] + r[k * 3 + 2] * from[i][2] + t[k] - to[i][k];
                    e2 += e * e;
                }

                miss[i / 7] = std::max(miss[i / 7], std::sqrt(e2));
            }

            std::vector<double> sorted = miss;
            std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
            const double limit = std::max(0.002, 3 * sorted[sorted.size() / 2]);
            std::vector<std::array<double, 3>> kept_from, kept_to;

            for (size_t i = 0; i < from.size(); ++i) {
                if (miss[i / 7] <= limit) {
                    kept_from.push_back(from[i]);
                    kept_to.push_back(to[i]);
                }
            }

            if (kept_from.size() == from.size()) {
                break;
            }

            // Most records must agree: else this is not one vehicle.
            if (kept_from.size() / 7 < 3 || kept_from.size() * 2 < from.size()) {
                return false;
            }

            from.swap(kept_from);
            to.swap(kept_to);
            fit_rigid(from, to, r, t);
        }

        // Current camera-relative position -> previous camera-relative position.
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                out[i * 4 + j] = static_cast<float>(r[i * 3 + j]);
            }

            out[i * 4 + 3] = static_cast<float>(t[i]);
        }

        return true;
    }


    void register_object_events()
    {
        reshade::register_event<reshade::addon_event::map_buffer_region>(on_map_buffer_region);
        reshade::register_event<reshade::addon_event::unmap_buffer_region>(on_unmap_buffer_region);
    }


    void unregister_object_events()
    {
        reshade::unregister_event<reshade::addon_event::map_buffer_region>(on_map_buffer_region);
        reshade::unregister_event<reshade::addon_event::unmap_buffer_region>(on_unmap_buffer_region);
    }
}
