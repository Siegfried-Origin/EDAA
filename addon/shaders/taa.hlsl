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
    float4 hdr; // x: scale applied before the HDR compression (the scene is not exposed yet),
                // y: jitter weighting strength (0 = off), z: history sharpening (0 = off),
                // w: jitter weighting normaliser (1 / mean weight over the jitter square)
    float4 display_size; // upscaling (ps_taau): w, h, 1/w, 1/h of the output
    float4 sharpen; // upscaling (ps_sharpen): x: sharpness 0..1
};

Texture2D<float3> scene_color : register(t0);
Texture2D<float4> history_tex : register(t1);
Texture2D<float2> motion_tex : register(t2);
Texture2D<float> scene_depth : register(t3);
Texture2D<float> hud_mask_tex : register(t4); // upscaling: 1 where the cockpit HUD was drawn this frame
Texture2D<float> vehicle_mask_tex : register(t5); // 1 on the player's vehicle (motion.hlsl)
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
// size: the history's w, h, 1/w, 1/h.
float3 sample_history(float2 uv, float4 size)
{
    const float2 pos = uv * size.xy;
    const float2 center = floor(pos - 0.5) + 0.5;
    const float2 f = pos - center;
    const float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    const float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    const float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    const float2 w3 = f * f * (-0.5 + 0.5 * f);
    const float2 w12 = w1 + w2;
    const float2 tc0 = (center - 1) * size.zw;
    const float2 tc3 = (center + 2) * size.zw;
    const float2 tc12 = (center + w2 / w12) * size.zw;

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

    // History follows the foreground; "moving" also takes this pixel's own
    // motion (world next to the cockpit, see ps_taau).
    const float2 motion = motion_tex.Load(int3(clamp(best, int2(0, 0), int2(render_size.xy) - 1), 0));
    const float motion_px = max(length(motion * render_size.xy), length(motion_tex.Load(int3(p, 0)) * render_size.xy));

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
        float3 sampled = tonemap(sample_history(history_uv, render_size));

        // History sharpening: resampling the history at a fraction of a pixel
        // blurs it a little every frame, which adds up while the camera turns.
        // Undo that blur (largest at half a pixel, none on still pixels) with
        // a small unsharp mask; the clip box below still bounds the result.
        if (hdr.z > 0) {
            const float2 f = frac(history_uv * render_size.xy - 0.5);
            const float blur = 2 * (f.x * (1 - f.x) + f.y * (1 - f.y)); // 0 on texel centres, 1 at half a texel
            const float2 d = render_size.zw;
            const float3 around = 0.25 * (tonemap(history_tex.SampleLevel(linear_clamp, history_uv + float2(d.x, 0), 0).rgb) +
                tonemap(history_tex.SampleLevel(linear_clamp, history_uv - float2(d.x, 0), 0).rgb) +
                tonemap(history_tex.SampleLevel(linear_clamp, history_uv + float2(0, d.y), 0).rgb) +
                tonemap(history_tex.SampleLevel(linear_clamp, history_uv - float2(0, d.y), 0).rgb));
            sampled = saturate(sampled + hdr.z * blur * (sampled - around));
        }

        const float3 raw = rgb_to_ycocg(sampled);
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

    // Jitter weighting: this frame's sample lies |jitter| pixels from the pixel
    // centre. A sample close to the centre adds more to the history, a far one
    // less (normalised so the average stays the same): the accumulated image
    // then holds less of the jitter footprint, which is what makes it soft.
    if (hdr.y > 0 && history_weight > 0 && history_weight < 1) {
        const float2 jitter_px2 = jitter.xy * render_size.xy;
        const float w = exp(-hdr.y * dot(jitter_px2, jitter_px2)) * hdr.w;
        history_weight = 1 - saturate((1 - history_weight) * w);
    }

    // Luminance-weighted blend (values are already compressed, so 1 - luma weighting).
    const float wc = (1 - history_weight) * (1 - current.x);
    const float wh = history_weight * (1 - history.x);
    const float3 result = (current * wc + history * wh) / max(wc + wh, 1e-5);
    // Clamp before inverting the compression (it diverges at 1): up to 1e5 / scale.
    return float4(untonemap(clamp(ycocg_to_rgb(result), 0, 0.99999)), rejection);
}


// Glint stabiliser (spec_stable), after DLAA / XeSS / FSR: the neural AA keeps
// thin glints sharp, so they still blink from frame to frame. Where this
// frame's output (t0) or the previous stabilised output (t1, moved along the
// motion vectors t2) is clearly brighter than this frame's 3x3 neighbourhood,
// the two are blended: the previous one, limited to the neighbourhood's range
// (no trails where the glint really moved away), counts up to 'strength'.
// Elsewhere the output passes through. Left alone: sky (t3 depth 0) and
// transparents (t4 reactive mask, unbound = 0). Writes the next history.
// sharpen.x: strength 0..1, sharpen.y: glint contrast (compressed HDR),
// sharpen.z: clip box in standard deviations; blend.z: reset.
float3 ps_spec_stable(float4 pos : SV_Position) : SV_Target
{
    const int2 p = int2(pos.xy);
    const int2 last = int2(render_size.xy) - 1;
    const float3 raw = scene_color.Load(int3(p, 0));
    const float3 c = tonemap(raw);
    const float lc = max(c.r, max(c.g, c.b));
    float3 m1 = 0, m2 = 0, box_min = 1e9, box_max = -1e9;
    float around = 0;

    // clang-format off
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            const float3 n = tonemap(scene_color.Load(int3(clamp(p + int2(x, y), int2(0, 0), last), 0)));
            m1 += n;
            m2 += n * n;
            box_min = min(box_min, n);
            box_max = max(box_max, n);
            around += x != 0 || y != 0 ? max(n.r, max(n.g, n.b)) : 0;
        }
    }
    // clang-format on

    around /= 8;
    const float2 history_uv = (pos.xy * render_size.zw) + motion_tex.Load(int3(p, 0));
    const bool gate = scene_depth.Load(int3(p, 0)) > 0 && hud_mask_tex.Load(int3(p, 0)) < 0.1;

    if (blend.z > 0 || !gate || sharpen.x <= 0 || any(history_uv < 0) || any(history_uv > 1)) {
        return raw;
    }

    const float3 prev = tonemap(history_tex.SampleLevel(linear_clamp, history_uv, 0).rgb);
    const float lp = max(prev.r, max(prev.g, prev.b));
    // Glint now, or a glint last frame that is gone now.
    const float t = max(sharpen.y, 1e-3);
    const float glint = saturate((max(lc, lp) - around - t) / t);

    if (glint <= 0) {
        return raw;
    }

    const float3 mean = m1 / 9;
    const float3 sigma = sqrt(max(m2 / 9 - mean * mean, 0));
    const float3 kept = clamp(prev, min(box_min, mean - sharpen.z * sigma), max(box_max, mean + sharpen.z * sigma));
    const float3 result = lerp(c, kept, sharpen.x * glint);
    return untonemap(clamp(result, 0, 0.99999));
}


// Temporal upscaling (Upscaling = EDAA TAA): the game's tonemapped image at
// render size (t0, LDR, jittered), the display-size history (t1), motion
// vectors and depth at render size (t2, t3) and the HUD mask (t4) give the
// display-size output. Input pixel q holds the scene at q + 0.5 - jitter (in
// input pixels). An output pixel looks at the 3x3 input samples around it:
// - weighted by their distance in input pixels: a soft estimate, used alone
//   where there is no history;
// - weighted by their distance in output pixels: a sharp estimate, and its
//   total weight says how much this frame adds to the history (a sample right
//   on the output pixel counts fully, a far one hardly). Over the jitter
//   sequence (8 x ratio^2 phases) every output pixel gets close samples, which
//   brings back the display resolution detail.
float4 ps_taau(float4 pos : SV_Position) : SV_Target
{
    const float2 uv = pos.xy * display_size.zw;
    const float2 in_pos = uv * render_size.xy;
    const int2 center = int2(floor(in_pos));
    const int2 last = int2(render_size.xy) - 1;
    // The cockpit HUD is drawn after the resolve point, without the jitter:
    // its pixels are filtered around their own centres (else the HUD shakes).
    const bool hud = hud_mask_tex.Load(int3(clamp(center, int2(0, 0), last), 0)) > 0.5;
    const float2 jitter_px = hud ? 0 : jitter.xy * render_size.xy;
    const float scale = display_size.x * render_size.z; // output pixels per input pixel

    float3 filtered = 0, sharp = 0, m1 = 0, m2 = 0, box_min = 1e9, box_max = -1e9;
    float filter_weight = 0, sharp_weight = 0, best_depth = -1;
    int2 best = clamp(center, int2(0, 0), last);

    // clang-format off
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            const int2 q = clamp(center + int2(x, y), int2(0, 0), last);
            const float3 c = rgb_to_ycocg(scene_color.Load(int3(q, 0)));
            const float2 d = float2(center + int2(x, y)) + 0.5 - jitter_px - in_pos; // input pixels
            const float d2 = dot(d, d);
            const float w = exp(-2.29 * d2); // ~Blackman-Harris, radius 1 input pixel
            filtered += c * w;
            filter_weight += w;
            const float ws = exp(-2.29 * d2 * scale * scale); // radius 1 output pixel
            sharp += c * ws;
            sharp_weight += ws;
            m1 += c;
            m2 += c * c;
            box_min = min(box_min, c);
            box_max = max(box_max, c);
            const float depth = scene_depth.Load(int3(q, 0));

            if (depth > best_depth) {
                best_depth = depth;
                best = q;
            }
        }
    }
    // clang-format on

    const float3 current = filtered / max(filter_weight, 1e-5);
    const float3 current_sharp = sharp_weight > 1e-4 ? sharp / sharp_weight : current;
    const float confidence = saturate(sharp_weight);
    // History follows the foreground (closest depth), but "moving" also looks at
    // this pixel's own motion: world pixels next to the cockpit take its zero
    // motion and would otherwise get the loose still-pixel clip (ghosting).
    const float2 motion = motion_tex.Load(int3(best, 0));
    const float2 own_motion = motion_tex.Load(int3(clamp(center, int2(0, 0), last), 0));
    const float motion_px = max(length(motion * display_size.xy), length(own_motion * display_size.xy)); // output pixels

    // Variance clip from the input samples. The 3x3 min/max changes with every
    // jitter phase: it limits moving pixels only (still ones would flicker).
    const float3 mean = m1 / 9;
    const float3 sigma = sqrt(max(m2 / 9 - mean * mean, 0));
    // The cockpit moves with the head and the ship's vibrations, which its
    // motion vectors follow only approximately: always the tight clip there.
    const bool vehicle = vehicle_mask_tex.Load(int3(clamp(center, int2(0, 0), last), 0)) > 0.5;
    const float moving = vehicle ? 1 : saturate(motion_px / 2);
    const float gamma = lerp(flicker.z, blend.w, moving);
    const float3 clip_min = lerp(mean - gamma * sigma, max(box_min, mean - gamma * sigma), moving);
    const float3 clip_max = lerp(mean + gamma * sigma, min(box_max, mean + gamma * sigma), moving);

    const float2 history_uv = uv + motion;
    float history_weight = blend.x;

    if (blend.z > 0 || any(history_uv < 0) || any(history_uv > 1) || hud) {
        history_weight = 0; // no history, just entered the screen, or cockpit HUD drawn this frame
    }

    history_weight = lerp(history_weight, min(history_weight, blend.y), saturate(motion_px / 32));

    float3 history = current;

    if (history_weight > 0) {
        history = clip_aabb(rgb_to_ycocg(sample_history(history_uv, display_size)), clip_min, clip_max);

        if (flicker.x > 0) {
            const float diff = abs(current.x - history.x) / max(max(current.x, history.x), 0.2);
            const float agreement = 1 - saturate(diff);
            history_weight = lerp(min(flicker.y, history_weight), history_weight, agreement * agreement);
        }
    }

    // This frame's share: full without history (soft estimate), else the sharp
    // estimate scaled by how close the samples fell.
    const float alpha = history_weight > 0 ? (1 - history_weight) * confidence : 1;
    const float3 added = history_weight > 0 ? current_sharp : current;
    // Luminance-weighted blend (the input is already tonemapped: 1 / (1 + luma)).
    const float wc = alpha / (1 + added.x);
    const float wh = (1 - alpha) / (1 + history.x);
    const float3 result = (added * wc + history * wh) / max(wc + wh, 1e-5);
    return float4(saturate(ycocg_to_rgb(result)), 1);
}


// Upscaling output: the TAAU history (t1) with contrast-adaptive sharpening
// (AMD CAS style): each pixel subtracts a share of its 4 neighbours, less where
// the local contrast is already high (no halos), into the game target.
float3 ps_sharpen(float4 pos : SV_Position) : SV_Target
{
    const int2 p = int2(pos.xy);
    const int2 last = int2(display_size.xy) - 1;
    const float3 c = history_tex.Load(int3(p, 0)).rgb;

    if (sharpen.x <= 0) {
        return c;
    }

    const float3 n = history_tex.Load(int3(clamp(p + int2(0, -1), int2(0, 0), last), 0)).rgb;
    const float3 s = history_tex.Load(int3(clamp(p + int2(0, 1), int2(0, 0), last), 0)).rgb;
    const float3 w = history_tex.Load(int3(clamp(p + int2(-1, 0), int2(0, 0), last), 0)).rgb;
    const float3 e = history_tex.Load(int3(clamp(p + int2(1, 0), int2(0, 0), last), 0)).rgb;
    const float3 lo = min(c, min(min(n, s), min(w, e)));
    const float3 hi = max(c, max(max(n, s), max(w, e)));
    const float3 amount = sqrt(saturate(min(lo, 1 - hi) / max(hi, 1e-4)));
    const float3 weight = -amount / lerp(8.0, 5.0, sharpen.x);
    return saturate((c + (n + s + w + e) * weight) / (1 + 4 * weight));
}


// HUD mask for upscaling: 1 where the scene changed a lot between the snapshot
// (t1: at the resolve point, taken again after each fullscreen pass such as
// the helmet visor) and now (t0): the cockpit HUD and holograms. A pixel counts
// only when what was added is at least half as bright as what was under it,
// so faint glows do not cost it its history. Dilated by one pixel to cover the
// edges once upscaled.
struct hud_mask_output
{
    float changed : SV_Target0; // 1 where the HUD changed the pixel or a neighbour (frame generation, TAA upscaling)
    float graded : SV_Target1; // 0..1: how much the HUD makes up the pixel (reactive mask of the upscalers)
};

hud_mask_output ps_hud_mask(float4 pos : SV_Position)
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

    // Graded, this pixel only: the HUD's share of the pixel. Bright text and lines
    // (share near 1) keep no history in the upscaler; the faint tint of a panel's
    // background (share just above the 1/3 of 'changed') keeps it, so the scene
    // behind stays anti-aliased instead of flickering inside the panel rectangle.
    const int3 c = int3(pos.xy, 0);
    const float3 before = history_tex.Load(c).rgb;
    const float3 diff = abs(scene_color.Load(c) - before);
    const float added = max(diff.r, max(diff.g, diff.b));
    const float under = max(before.r, max(before.g, before.b));
    const float share = added / (added + under + 1e-4);

    hud_mask_output o;
    o.changed = changed;
    o.graded = added > 0.004 ? smoothstep(0.34, 0.75, share) : 0;
    return o;
}


// Reactive mask for DLSS / XeSS (their "bias current colour" input): the share
// of the pixel the transparents (particles, glass, glows) changed since the
// snapshot taken right before the first of them (t1), times the strength
// (sharpen.y). Shares below the threshold (sharpen.z: thin haze, light shafts
// over the whole room) count as nothing. 1 = the upscaler uses the current frame.
float ps_reactive(float4 pos : SV_Position) : SV_Target
{
    const int3 p = int3(pos.xy, 0);
    const float3 now = scene_color.Load(p);
    const float3 before = history_tex.Load(p).rgb;
    const float3 diff = abs(now - before);
    const float changed = max(diff.r, max(diff.g, diff.b));
    const float level = max(max(now.r, max(now.g, now.b)), max(before.r, max(before.g, before.b)));
    const float share = changed / max(level, 1e-4);
    return saturate(sharpen.y * saturate((share - sharpen.z) / max(1 - sharpen.z, 1e-3)));
}


// Frame generation: the motion vectors with the cockpit HUD standing still
// (the HUD keeps its place on screen while the scene moves behind it).
float2 ps_fg_motion(float4 pos : SV_Position) : SV_Target
{
    const int3 p = int3(pos.xy, 0);
    return hud_mask_tex.Load(p) > 0.5 ? float2(0, 0) : motion_tex.Load(p);
}


// Frame generation with hud_motion: the HUD already has the cockpit's motion.
float2 ps_fg_motion_keep(float4 pos : SV_Position) : SV_Target
{
    return motion_tex.Load(int3(pos.xy, 0));
}


// Copies the resolved image back into the game's scene target.
float3 ps_copy(float4 pos : SV_Position) : SV_Target
{
    return history_tex.Load(int3(pos.xy, 0)).rgb;
}


// DLAA output (t1, HDR before the game's exposure) sharpened into the game's
// scene target: the same contrast-adaptive weights as ps_sharpen, computed on
// the HDR compressed by tonemap() (hdr.x) so bright stars and engines do not
// dominate, and the result limited to the 4 neighbours' range (like AMD RCAS)
// so edges get steeper without bright or dark rims. DLSS keeps its own
// history, so the sharpened copy is never fed back. sharpen.x: 0..1.
float3 ps_sharpen_hdr(float4 pos : SV_Position) : SV_Target
{
    const int2 p = int2(pos.xy);
    const int2 last = int2(render_size.xy) - 1;
    const float3 raw = history_tex.Load(int3(p, 0)).rgb;

    if (sharpen.x <= 0) {
        return raw;
    }

    const float3 c = tonemap(raw);
    const float3 n = tonemap(history_tex.Load(int3(clamp(p + int2(0, -1), int2(0, 0), last), 0)).rgb);
    const float3 s = tonemap(history_tex.Load(int3(clamp(p + int2(0, 1), int2(0, 0), last), 0)).rgb);
    const float3 w = tonemap(history_tex.Load(int3(clamp(p + int2(-1, 0), int2(0, 0), last), 0)).rgb);
    const float3 e = tonemap(history_tex.Load(int3(clamp(p + int2(1, 0), int2(0, 0), last), 0)).rgb);
    const float3 lo4 = min(min(n, s), min(w, e));
    const float3 hi4 = max(max(n, s), max(w, e));
    const float3 lo = min(c, lo4);
    const float3 hi = max(c, hi4);
    const float3 amount = sqrt(saturate(min(lo, 1 - hi) / max(hi, 1e-4)));
    const float3 weight = -amount / lerp(8.0, 5.0, sharpen.x);
    const float3 sharp = (c + (n + s + w + e) * weight) / (1 + 4 * weight);
    // At most as dark or as bright as the darkest/brightest neighbour or the pixel itself.
    return untonemap(clamp(sharp, lo, min(hi, 0.999)));
}
