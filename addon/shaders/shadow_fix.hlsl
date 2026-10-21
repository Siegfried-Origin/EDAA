// Jittered copy of a constant buffer holding a view ray (shadow_fix.cpp).
// Two layouts, chosen by shift.w:
//   0: sun shadow mask pixel shader, ray = ndc.x * c[1] + ndc.y * c[2] + c[4]:
//      c[4].xyz += shift.x * c[1].xyz + shift.y * c[2].xyz (shift in NDC);
//   1: tiled lighting compute shaders, ray.i = dot(c[10 + i].xyw, (u, v, 1)):
//      c[10..12].w += shift.x * c.x + shift.y * c.y (shift in UV);
//   2: ambient occlusion compute shader, view.xy = (uv * c[4].xy + c[4].zw) * z:
//      c[4].zw += shift * c[4].xy (shift in UV).
// The depth was rendered with the viewport jitter; the shifted ray points at
// the jittered sample instead of the pixel centre.

cbuffer shift_constants : register(b0)
{
    float4 shift; // xy: ray shift, z: float4 registers to copy, w: layout
};

ByteAddressBuffer source : register(t0); // the game's constant buffer, copied
RWByteAddressBuffer target : register(u0);


// clang-format off
[numthreads(64, 1, 1)]
void cs_shadow_constants(uint i : SV_DispatchThreadID)
{
    if (i >= uint(shift.z)) {
        return;
    }

    float4 v = asfloat(source.Load4(i * 16));

    if (shift.w < 0.5) {
        if (i == 4) {
            v.xyz += shift.x * asfloat(source.Load3(16)) + shift.y * asfloat(source.Load3(32));
        }
    }
    else if (shift.w < 1.5) {
        if (i >= 10 && i <= 12) {
            v.w += shift.x * v.x + shift.y * v.y;
        }
    }
    else if (i == 4) {
        v.zw += shift.xy * v.xy;
    }

    target.Store4(i * 16, asuint(v));
}
// clang-format on
