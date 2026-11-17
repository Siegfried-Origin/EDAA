// Camera-reprojection motion vectors and debug visualisation.
// Conventions: docs/reverse-engineering.md section 1.2 (row-vector viewProj,
// camera-relative positions, reverse-Z with infinite far plane).

cbuffer motion_constants : register(b0)
{
    row_major float4x4 inv_view_proj; // inverse of the current unjittered c270-c273
    row_major float4x4 prev_view_proj; // previous frame c270-c273
    float4 camera_delta; // xyz: current camera position - previous (c275)
    float4 params; // xy: jitter in UV, z: near plane, w: vehicle mask distance (m)
    float4 render_size; // w, h, 1/w, 1/h
    float4 debug; // x: mode (1 motion, 2 mask, 3 depth, 4 history rejection, 5 objects, 6 temporal change, 7 HUD mask, 8 reactive mask, 9 ship auto), y: pixels for full colour,
                  // z: vehicle mask source (0 stencil bits, 1 distance), w: stencil bits of the player's vehicle
    float4 vehicle[3]; // rows of the vehicle motion: current -> previous camera-relative position (3x4)
    float4 vehicle_on; // x: 1 when the vehicle motion is known, else vehicle pixels get no motion,
                       // y: unused, z: 2 = aa_ship_steady (learned ship distance in ps_motion), else 0,
                       // w: aa_ship_steady ship distance (m, 0 = off; aa_ship_distance)
};

// Moving objects debug view (object_motion.cpp), up to 128 markers.
cbuffer object_constants : register(b1)
{
    float4 object_info; // x: marker count, yz: target size in pixels
    float4 object_markers[256]; // [2i] xy: position UV, zw: ghost offset UV; [2i+1] x: error px, y: kind (1 moving, 2 vehicle, 3 spinning)
};

Texture2D<float> scene_depth : register(t0);
Texture2D<float2> motion_tex : register(t1);
Texture2D<float> mask_tex : register(t2);
Texture2D<uint2> scene_stencil : register(t4); // .g = stencil
Texture2D<float4> history_tex : register(t3); // alpha: history rejection of the last TAA pass
Texture2D<float4> prev_history_tex : register(t5); // TAA output of the frame before
SamplerState point_sampler : register(s0);


void vs_fullscreen(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD)
{
    uv = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}


struct motion_output
{
    float2 motion : SV_Target0; // previous UV - current UV, jitter removed
    float mask : SV_Target1; // 1 = vehicle-attached pixel
};


motion_output ps_motion(float4 pos : SV_Position)
{
    const float depth = scene_depth.Load(int3(pos.xy, 0));
    const float2 uv = pos.xy * render_size.zw - params.xy;
    const float2 ndc = float2(uv.x * 2 - 1, 1 - uv.y * 2);

    // Homogeneous camera-relative position; w = 0 for the sky (depth 0 = infinity).
    const float4 h = mul(float4(ndc, depth, 1), inv_view_proj);

    motion_output o;

    // Pixels of the player's vehicle move with the camera. The game tags them
    // in the stencil (bit 0x80: cockpit, SRV; not set on foot); the distance
    // rule (closer than params.w metres) is the fallback.
    if (debug.z < 0.5) {
        const uint bits = uint(debug.w);
        o.mask = (scene_stencil.Load(int3(pos.xy, 0)).g & bits) != 0 ? 1 : 0;

        // Some cockpit edges (a rim of 1-2 pixels along the canopy struts) are
        // drawn without the stencil bit. A pixel next to a vehicle pixel, at
        // about its distance (not the world behind), belongs to the vehicle too.
        if (o.mask == 0 && depth > 0) {
            const float view_z = params.z / depth;
            const int2 offsets[8] = { int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1), int2(2, 0), int2(-2, 0), int2(0, 2), int2(0, -2) };

            // clang-format off
            [unroll] for (int i = 0; i < 8; ++i) {
                const int3 q = int3(clamp(int2(pos.xy) + offsets[i], int2(0, 0), int2(render_size.xy) - 1), 0);
                const float d = scene_depth.Load(q);

                if ((scene_stencil.Load(q).g & bits) != 0 && d > 0 && view_z < params.z / d * 1.5 + 0.5) {
                    o.mask = 1;
                }
            }
            // clang-format on
        }
    }
    else {
        o.mask = (depth > 0 && params.z / depth < params.w) ? 1 : 0;
    }

    // aa_ship_steady: the same for the anti-aliasing (and every frame generation),
    // closer than vehicle_on.w metres.
    if (o.mask == 0 && vehicle_on.w > 0 && depth > 0 && params.z / depth < vehicle_on.w) {
        o.mask = 1;
    }

    // aa_ship_steady (vehicle_on.z = 2): the distance of the ship learned per
    // pixel (t5.y, ps_ship_auto), its largest value up to 2 pixels around (the
    // jitter moves the silhouette). Not farther than that is the ship; the world
    // behind an edge is farther and stays world (no flicker at the edges).
    if (o.mask == 0 && vehicle_on.z > 1.5 && depth > 0) {
        const float view_z = params.z / depth;
        float learned = 0;

        // clang-format off
        [unroll] for (int y = -2; y <= 2; ++y) {
            [unroll] for (int x = -2; x <= 2; ++x) {
                const int3 q = int3(clamp(int2(pos.xy) + int2(x, y), int2(0, 0), int2(render_size.xy) - 1), 0);
                learned = max(learned, prev_history_tex.Load(q).y);
            }
        }
        // clang-format on

        if (learned > 0 && view_z < learned * 1.03 + 0.5) {
            o.mask = 1;
        }
    }

    // Same world point relative to the previous camera position.
    const float4 prev_clip = mul(float4(h.xyz + camera_delta.xyz * h.w, h.w), prev_view_proj);
    float2 motion = 0;

    if (prev_clip.w > 0) {
        const float2 prev_ndc = prev_clip.xy / prev_clip.w;
        motion = float2(prev_ndc.x * 0.5 + 0.5, 0.5 - prev_ndc.y * 0.5) - uv;
    }

    // Vehicle pixels: the vehicle's own motion (head movements included), or
    // none when it is unknown (they mostly move with the camera).
    if (o.mask > 0.5) {
        motion = 0;

        if (vehicle_on.x > 0 && h.w > 0) {
            const float4 p = float4(h.xyz / h.w, 1);
            const float4 vehicle_clip = mul(float4(dot(vehicle[0], p), dot(vehicle[1], p), dot(vehicle[2], p), 1), prev_view_proj);

            if (vehicle_clip.w > 0) {
                const float2 prev_ndc = vehicle_clip.xy / vehicle_clip.w;
                motion = float2(prev_ndc.x * 0.5 + 0.5, 0.5 - prev_ndc.y * 0.5) - uv;
            }
        }
    }

    o.motion = motion;
    return o;
}


// Automatic ship detection (diagnosis, debug view "Ship auto"). The player's own
// ship moves with the camera, the world does not. Each pixel is reprojected both
// ways into the last frame (world: camera motion; ship: the vehicle motion, or no
// motion when it is unknown) and the depth found there is compared with the depth
// each prediction expects. Only a clear answer (one fits, the other does not)
// moves the score; otherwise (camera still, flat ground in level flight) it keeps
// the last value. Inputs: t0 depth, t2 last score, t5.xy last depth and learned
// ship distance (R32G32_FLOAT).
// Learned ship distance (aa_ship_steady, ps_motion): where a pixel clearly moved
// with the camera (and its score agrees), the ship is at least this far away
// there (view metres, at most 250). It forgets slowly (a few minutes), at once
// when something clearly in the world is closer, and is cleared with the score
// (switch off, on foot).
struct ship_auto_output
{
    float score : SV_Target0; // 1 = moves with the camera (own ship)
    float2 depth : SV_Target1; // x: this frame's depth, y: learned ship distance (m), for the next frame
};

// Relative depth error of the best match around the predicted position (3x3
// pixels): |expected / found - 1|, large when off screen or sky.
float ship_auto_error(float4 prev_clip, out float2 prev_uv)
{
    prev_uv = float2(-1, -1);

    if (prev_clip.w <= 0) {
        return 1e9;
    }

    const float3 ndc = prev_clip.xyz / prev_clip.w;
    prev_uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);

    if (any(prev_uv < 0) || any(prev_uv > 1) || ndc.z <= 0) {
        return 1e9;
    }

    const int2 c = int2(prev_uv * render_size.xy);
    float best = 1e9;

    // clang-format off
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            const int3 q = int3(clamp(c + int2(x, y), int2(0, 0), int2(render_size.xy) - 1), 0);
            const float found = prev_history_tex.Load(q).x;

            if (found > 0) {
                best = min(best, abs(ndc.z / found - 1));
            }
        }
    }
    // clang-format on

    return best;
}

ship_auto_output ps_ship_auto(float4 pos : SV_Position)
{
    const int3 p = int3(pos.xy, 0);
    const float depth = scene_depth.Load(p);
    const float last = mask_tex.Load(p);
    const float learned = prev_history_tex.Load(p).y * 0.99997;
    ship_auto_output o;
    o.depth = float2(depth, learned);
    o.score = last;

    if (depth <= 0) {
        o.score = 0; // sky
        return o;
    }

    const float view_z = params.z / depth;

    const float2 uv = pos.xy * render_size.zw - params.xy;
    const float2 ndc = float2(uv.x * 2 - 1, 1 - uv.y * 2);
    const float4 h = mul(float4(ndc, depth, 1), inv_view_proj);

    if (h.w <= 0) {
        return o;
    }

    float2 world_uv, ship_uv;
    const float world_error = ship_auto_error(mul(float4(h.xyz + camera_delta.xyz * h.w, h.w), prev_view_proj), world_uv);
    float ship_error;

    if (vehicle_on.x > 0) {
        const float4 v = float4(h.xyz / h.w, 1);
        ship_error = ship_auto_error(mul(float4(dot(vehicle[0], v), dot(vehicle[1], v), dot(vehicle[2], v), 1), prev_view_proj), ship_uv);
    }
    else {
        // Unknown vehicle motion: the ship stands still on screen.
        ship_uv = uv;
        const float found = prev_history_tex.Load(p).x;
        ship_error = found > 0 ? abs(depth / found - 1) : 1e9;
    }

    // Both predictions (nearly) the same: no information.
    if (length((world_uv - ship_uv) * render_size.xy) < 2) {
        return o;
    }

    const float tolerance = 0.02; // 2 % of the depth
    const float rate = 0.08;

    if (ship_error < tolerance && world_error > 4 * tolerance) {
        o.score = lerp(last, 1, rate);

        if (o.score > 0.5 && view_z < 250) {
            o.depth.y = max(learned, view_z);
        }
    }
    else if (world_error < tolerance && ship_error > 4 * tolerance) {
        o.score = lerp(last, 0, rate);

        if (view_z < learned * 1.03 + 0.5) {
            o.depth.y = 0; // the world is closer than the learned ship: wrong here
        }
    }

    return o;
}


// Cockpit HUD panels (hud_mask on t2, render size): they hang in the cockpit but
// write no depth, so ps_motion gave them the motion of the world behind. Here they
// get the vehicle's motion at about 1.5 m (head turns: the distance hardly matters).
float2 ps_hud_motion(float4 pos : SV_Position) : SV_Target
{
    clip(mask_tex.Load(int3(pos.xy, 0)) - 0.5);
    const float2 uv = pos.xy * render_size.zw - params.xy;
    const float2 ndc = float2(uv.x * 2 - 1, 1 - uv.y * 2);
    const float4 h = mul(float4(ndc, params.z / 1.5, 1), inv_view_proj);
    float2 motion = 0;

    if (vehicle_on.x > 0 && h.w > 0) {
        const float4 p = float4(h.xyz / h.w, 1);
        const float4 vehicle_clip = mul(float4(dot(vehicle[0], p), dot(vehicle[1], p), dot(vehicle[2], p), 1), prev_view_proj);

        if (vehicle_clip.w > 0) {
            const float2 prev_ndc = vehicle_clip.xy / vehicle_clip.w;
            motion = float2(prev_ndc.x * 0.5 + 0.5, 0.5 - prev_ndc.y * 0.5) - uv;
        }
    }

    return motion;
}


float4 ps_debug(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    const int2 px = int2(uv * render_size.xy);
    const float depth = scene_depth.Load(int3(px, 0));
    const float view_z = depth > 0 ? params.z / depth : 1e9;
    const float depth_shade = saturate(1 - log2(max(view_z, 1)) / 17); // 1 m .. 128 km

    if (debug.x == 1) {
        // Grey = no motion, red/green = horizontal/vertical motion, saturates at debug.y pixels.
        const float2 m = motion_tex.Load(int3(px, 0)) * render_size.xy / debug.y;
        return float4(saturate(0.5 + 0.5 * m), 0.5, 1);
    }

    if (debug.x == 2 || debug.x == 7 || debug.x == 8) {
        // 2: vehicle mask (orange), 7: HUD mask of the upscaling (cyan), both bound to t2.
        const float mask = mask_tex.Load(int3(px, 0));
        return float4(lerp(depth_shade.xxx * 0.5, debug.x == 2 ? float3(1, 0.3, 0.1) : debug.x == 8 ? float3(1, 0.85, 0.1) : float3(0.1, 0.9, 1), mask), 1);
    }

    if (debug.x == 9) {
        // Ship auto: green = detected as own ship (t2 score), orange = game stencil
        // only, yellow = both, grey = world (t3.r: vehicle mask).
        const float score = mask_tex.Load(int3(px, 0));
        const bool stencil = history_tex.Load(int3(px, 0)).r > 0.5;

        if (stencil) {
            return float4(score > 0.5 ? float3(1, 0.9, 0.1) : float3(1, 0.3, 0.1), 1);
        }

        return float4(lerp(depth_shade.xxx * 0.5, float3(0.1, 1, 0.2), score), 1);
    }

    if (debug.x == 5) {
        // Grey scene; ring per moving object (yellow -> red as the camera
        // reprojection error reaches debug.y pixels, cyan: player's vehicle)
        // and a line towards where its history ghost would be.
        float3 colour = depth_shade.xxx * 0.5;
        const float2 p = uv * object_info.yz;

        for (int i = 0; i < int(object_info.x); ++i) {
            const float4 m = object_markers[2 * i];
            const float4 e = object_markers[2 * i + 1];
            const float2 a = m.xy * object_info.yz;
            const float2 ab = m.zw * object_info.yz;
            const float t = saturate(dot(p - a, ab) / max(dot(ab, ab), 1e-6));
            const bool on_line = length(p - a - ab * t) < 1.0;
            const bool on_ring = abs(length(p - a) - 8) < 1.5;

            if (e.y > 2.5) {
                // Spinning in place (asteroids): small green dot, to check rings land on objects.
                if (length(p - a) < 3) {
                    colour = float3(0.2, 1, 0.3);
                }
            }
            else if (on_line || on_ring) {
                colour = e.y > 1.5 ? float3(0.1, 0.8, 1) : lerp(float3(1, 0.9, 0.1), float3(1, 0.1, 0.1), saturate(e.x / debug.y));
            }
        }

        return float4(colour, 1);
    }

    if (debug.x == 6) {
        // Grey scene; red where the TAA output changed since the last frame
        // (relative luminance change, full red at 10%). With the camera still,
        // red pixels are what flickers in the TAA output itself; flicker on
        // screen without red here comes from after the resolve.
        const float3 luma = float3(0.2126, 0.7152, 0.0722);
        const float a = dot(history_tex.Load(int3(px, 0)).rgb, luma);
        const float b = dot(prev_history_tex.Load(int3(px, 0)).rgb, luma);
        const float change = abs(a - b) / max(max(a, b), 0.05);
        return float4(lerp(depth_shade.xxx * 0.5, float3(1, 0.1, 0.1), saturate(change * 10)), 1);
    }

    if (debug.x == 4) {
        // Grey scene, red where the TAA clipped the reprojected history.
        const float rejection = history_tex.Load(int3(px, 0)).a;
        return float4(lerp(depth_shade.xxx * 0.5, float3(1, 0.1, 0.1), rejection), 1);
    }

    return float4(depth_shade.xxx, 1);
}


// Elite's exposure for DLAA (dlss_exposure_mode 2, exposure.hpp): the formula of
// the tonemap pixel shader (380822531cf1f3d4) on the adapted luminance its vertex
// shader reads (first texel of t0, last frame) with its cb2 (b2, copies of both).
// The scene times this is what Elite's tonemap curve gets.
cbuffer tonemap_constants : register(b2)
{
    float4 tm[16];
};

cbuffer exposure_constants : register(b3)
{
    float4 exposure_params; // x: scale (dlss_exposure_scale), y: fallback (dlss_exposure)
};

float ps_exposure(float4 pos : SV_Position) : SV_Target
{
    const float lum = scene_depth.Load(int3(0, 0, 0));
    const float bias = (tm[8].z != 0 ? 1.4 : 0) + tm[8].x;
    const float x = log2(8 * lum) - bias - 3;
    const float w2 = 2 * tm[7].w;
    const float d = log2(exp2(x * tm[8].y) + 1) * 0.30103 + w2;
    const float curve = exp2(log2(w2 / d) * tm[7].w);
    const float exposure = (tm[7].z * 1.03 - curve) / lum * exp2(-tm[7].y);
    const float result = tm[2].x * exposure * tm[6].y * exposure_params.x;

    // NaN, no luminance yet or nonsense: the fixed value.
    return lum > 0 && result > 1e-6 && result < 1e6 ? result : exposure_params.y;
}
