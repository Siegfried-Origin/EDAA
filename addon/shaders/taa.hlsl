// Temporal anti-aliasing resolve.
// Inputs: current HDR scene (jittered), history (previous resolve output),
// motion vectors (previous UV - current UV) and scene depth.
// History reprojection uses a Catmull-Rom filter, history rejection a 3x3
// YCoCg variance clip, and the blend is luminance weighted to tame HDR fireflies.

cbuffer taa_constants : register(b0)
{
    float4 render_size; // w, h, 1/w, 1/h
    float4 jitter; // xy: current jitter in UV, z: unjitter current sample (0/1), w: reconstruction filter (0/1)
    float4 blend; // x: history weight, y: history weight at high motion, z: reset (1 = no history), w: variance clip gamma
    float4 flicker; // x: anti-flicker on, y: minimum history weight, z: clip gamma for static pixels, w: soft off-screen on
    float4 hdr; // x: scale applied before the HDR compression (the scene is not exposed yet)
};

Texture2D<float3> scene_color : register(t0);
Texture2D<float4> history_tex : register(t1);
Texture2D<float2> motion_tex : register(t2);
Texture2D<float> scene_depth : register(t3);
SamplerState linear_clamp : register(s0);


float3 rgb_to_ycocg(float3 c)
{
    return float3(dot(c, float3(0.25, 0.5, 0.25)), dot(c, float3(0.5, 0, -0.5)), dot(c, float3(-0.25, 0.5, -0.25)));
}


float3 ycocg_to_rgb(float3 c)
{
    return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}


// Compress HDR before filtering so bright pixels do not dominate (reversible).
// The scene target holds values before the game's exposure, so they are
// scaled first: with too small a scale c/(1+c) stays linear and a bright thin
// edge still makes its dark neighbours ripple with the jitter.
float3 tonemap(float3 c)
{
    c *= hdr.x;
    return c / (1 + max(c.r, max(c.g, c.b)));
}


float3 untonemap(float3 c)
{
    return c / max(1e-4, 1 - max(c.r, max(c.g, c.b))) / hdr.x;
}


float3 load_current(int2 p)
{
    p = clamp(p, int2(0, 0), int2(render_size.xy) - 1);
    return rgb_to_ycocg(tonemap(scene_color.Load(int3(p, 0))));
}


// 5-tap Catmull-Rom history sample (bilinear taps, corners dropped).
float3 sample_history(float2 uv)
{
    const float2 pos = uv * render_size.xy;
    const float2 center = floor(pos - 0.5) + 0.5;
    const float2 f = pos - center;
    const float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    const float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    const float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    const float2 w3 = f * f * (-0.5 + 0.5 * f);
    const float2 w12 = w1 + w2;
    const float2 tc0 = (center - 1) * render_size.zw;
    const float2 tc3 = (center + 2) * render_size.zw;
    const float2 tc12 = (center + w2 / w12) * render_size.zw;

    float3 c = 0;
    c += history_tex.SampleLevel(linear_clamp, float2(tc12.x, tc0.y), 0).rgb * (w12.x * w0.y);
    c += history_tex.SampleLevel(linear_clamp, float2(tc0.x, tc12.y), 0).rgb * (w0.x * w12.y);
    c += history_tex.SampleLevel(linear_clamp, float2(tc12.x, tc12.y), 0).rgb * (w12.x * w12.y);
    c += history_tex.SampleLevel(linear_clamp, float2(tc3.x, tc12.y), 0).rgb * (w3.x * w12.y);
    c += history_tex.SampleLevel(linear_clamp, float2(tc12.x, tc3.y), 0).rgb * (w12.x * w3.y);
    const float weight = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return max(c / weight, 0);
}


// Moves 'h' towards the box centre until it is inside the box.
float3 clip_aabb(float3 h, float3 box_min, float3 box_max)
{
    const float3 center = 0.5 * (box_max + box_min);
    const float3 extent = 0.5 * (box_max - box_min) + 1e-5;
    const float3 v = h - center;
    const float3 units = abs(v / extent);
    const float m = max(units.x, max(units.y, units.z));
    return m > 1 ? center + v / m : h;
}


float4 ps_taa(float4 pos : SV_Position) : SV_Target
{
    const int2 p = int2(pos.xy);
    const float2 uv = pos.xy * render_size.zw;

    // 3x3 neighbourhood statistics; also find the closest depth (reverse-Z: largest)
    // to take the motion of the foreground at silhouettes.
    // Reconstruction filter: the current sample is a Gaussian-weighted sum of
    // the 3x3 neighbours around the unjittered pixel centre. Neighbour q holds
    // the scene at q - jitter, so its distance to the centre is q - jitter - p.
    // Thin bright details then fade between jitter phases instead of popping.
    const float2 jitter_px = jitter.xy * render_size.xy;
    float3 filtered = 0;
    float filter_weight = 0;
    float3 m1 = 0, m2 = 0, box_min = 1e9, box_max = -1e9;
    float best_depth = -1;
    int2 best = p;

    // clang-format off: it puts the brace of an attributed loop on its own line.
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            const int2 q = p + int2(x, y);
            const float3 c = load_current(q);
            const float2 offset = float2(x, y) - jitter_px;
            const float w = exp(-2.29 * dot(offset, offset)); // ~Blackman-Harris, radius 1 px
            filtered += c * w;
            filter_weight += w;
            m1 += c;
            m2 += c * c;
            box_min = min(box_min, c);
            box_max = max(box_max, c);
            const float d = scene_depth.Load(int3(clamp(q, int2(0, 0), int2(render_size.xy) - 1), 0));

            if (d > best_depth) {
                best_depth = d;
                best = q;
            }
        }
    }
    // clang-format on

    const float2 motion = motion_tex.Load(int3(clamp(best, int2(0, 0), int2(render_size.xy) - 1), 0));
    const float motion_px = length(motion * render_size.xy);

    // Variance clip box. Still pixels get a larger box (static clip gamma): thin
    // bright details that come and go with the jitter are then not clipped
    // every other frame (flicker); moving pixels keep the tight box (ghosting).
    const float3 mean = m1 / 9;
    const float3 sigma = sqrt(max(m2 / 9 - mean * mean, 0));
    const float gamma = lerp(flicker.z, blend.w, saturate(motion_px / 2));
    const float3 clip_min = max(box_min, mean - gamma * sigma);
    const float3 clip_max = min(box_max, mean + gamma * sigma);

    float3 current = load_current(p);

    if (jitter.w > 0) {
        current = filtered / filter_weight;
    }
    else if (jitter.z > 0) {
        current = rgb_to_ycocg(tonemap(scene_color.SampleLevel(linear_clamp, uv + jitter.xy, 0)));
    }

    float2 history_uv = uv + motion;

    float history_weight = blend.x;

    if (any(history_uv < 0) || any(history_uv > 1)) {
        // Just entered the screen: either no history, or (soft mode) the nearest
        // edge pixel of the history, still limited by the clip box.
        history_weight = flicker.w > 0 ? 0.5 * history_weight : 0;
        history_uv = saturate(history_uv);
    }

    if (blend.z > 0) {
        history_weight = 0;
    }

    // Less history for fast motion (less ghosting, more aliasing while moving).
    history_weight = lerp(history_weight, min(history_weight, blend.y), saturate(motion_px / 32));

    float3 history = current;
    float rejection = 1; // for the debug view: 0 = history fully kept, 1 = discarded

    if (history_weight > 0) {
        const float3 raw = rgb_to_ycocg(tonemap(sample_history(history_uv)));
        history = clip_aabb(raw, clip_min, clip_max);
        rejection = saturate(length(raw - history) * 8);

        // Anti-flicker (Lottes): trust the history more when it agrees with the
        // current sample, less when they differ in luminance.
        if (flicker.x > 0) {
            const float diff = abs(current.x - history.x) / max(max(current.x, history.x), 0.2);
            const float agreement = 1 - saturate(diff);
            history_weight = lerp(min(flicker.y, history_weight), history_weight, agreement * agreement);
        }
    }

    // Luminance-weighted blend (values are already compressed, so 1 - luma weighting).
    const float wc = (1 - history_weight) * (1 - current.x);
    const float wh = history_weight * (1 - history.x);
    const float3 result = (current * wc + history * wh) / max(wc + wh, 1e-5);
    // Clamp before inverting the compression (it diverges at 1): up to 1e5 / scale.
    return float4(untonemap(clamp(ycocg_to_rgb(result), 0, 0.99999)), rejection);
}


// HUD mask for upscaling: 1 where the scene changed a lot between the snapshot
// (t1: at the resolve point, taken again after each fullscreen pass such as
// the helmet visor) and now (t0): the cockpit HUD and holograms. A pixel counts
// only when what was added is at least half as bright as what was under it,
// so faint glows do not cost it its history. Dilated by one pixel to cover the
// edges once upscaled.
float ps_hud_mask(float4 pos : SV_Position) : SV_Target
{
    float changed = 0;

    // clang-format off
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            // Out of bounds loads return 0 in both: no change.
            const int3 p = int3(int2(pos.xy) + int2(x, y), 0);
            const float3 before = history_tex.Load(p).rgb;
            const float3 diff = abs(scene_color.Load(p) - before);
            const float added = max(diff.r, max(diff.g, diff.b));
            const float under = max(before.r, max(before.g, before.b));
            changed = max(changed, added > 0.5 * under + 1e-4 ? 1.0 : 0.0);
        }
    }
    // clang-format on

    return changed;
}


// Copies the resolved image back into the game's scene target.
float3 ps_copy(float4 pos : SV_Position) : SV_Target
{
    return history_tex.Load(int3(pos.xy, 0)).rgb;
}
