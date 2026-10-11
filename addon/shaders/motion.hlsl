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
	float4 debug; // x: mode (1 motion, 2 mask, 3 depth, 4 history rejection, 5 objects), y: pixels for full colour,
	              // z: vehicle mask source (0 stencil bits, 1 distance), w: stencil bits of the player's vehicle
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
	if (debug.z < 0.5)
		o.mask = (scene_stencil.Load(int3(pos.xy, 0)).g & uint(debug.w)) != 0 ? 1 : 0;
	else
		o.mask = (depth > 0 && params.z / depth < params.w) ? 1 : 0;

	// Same world point relative to the previous camera position.
	const float4 prev_clip = mul(float4(h.xyz + camera_delta.xyz * h.w, h.w), prev_view_proj);
	float2 motion = 0;
	if (prev_clip.w > 0)
	{
		const float2 prev_ndc = prev_clip.xy / prev_clip.w;
		motion = float2(prev_ndc.x * 0.5 + 0.5, 0.5 - prev_ndc.y * 0.5) - uv;
	}
	// Vehicle-attached pixels move with the camera: zero camera-relative motion.
	o.motion = o.mask > 0 ? 0 : motion;
	return o;
}

float4 ps_debug(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	const int2 px = int2(uv * render_size.xy);
	const float depth = scene_depth.Load(int3(px, 0));
	const float view_z = depth > 0 ? params.z / depth : 1e9;
	const float depth_shade = saturate(1 - log2(max(view_z, 1)) / 17); // 1 m .. 128 km

	if (debug.x == 1)
	{
		// Grey = no motion, red/green = horizontal/vertical motion, saturates at debug.y pixels.
		const float2 m = motion_tex.Load(int3(px, 0)) * render_size.xy / debug.y;
		return float4(saturate(0.5 + 0.5 * m), 0.5, 1);
	}
	if (debug.x == 2)
	{
		const float mask = mask_tex.Load(int3(px, 0));
		return float4(lerp(depth_shade.xxx * 0.5, float3(1, 0.3, 0.1), mask), 1);
	}
	if (debug.x == 5)
	{
		// Grey scene; ring per moving object (yellow -> red as the camera
		// reprojection error reaches debug.y pixels, cyan: player's vehicle)
		// and a line towards where its history ghost would be.
		float3 colour = depth_shade.xxx * 0.5;
		const float2 p = uv * object_info.yz;
		for (int i = 0; i < int(object_info.x); ++i)
		{
			const float4 m = object_markers[2 * i];
			const float4 e = object_markers[2 * i + 1];
			const float2 a = m.xy * object_info.yz;
			const float2 ab = m.zw * object_info.yz;
			const float t = saturate(dot(p - a, ab) / max(dot(ab, ab), 1e-6));
			const bool on_line = length(p - a - ab * t) < 1.0;
			const bool on_ring = abs(length(p - a) - 8) < 1.5;
			if (e.y > 2.5)
			{
				// Spinning in place (asteroids): small green dot, to check rings land on objects.
				if (length(p - a) < 3)
					colour = float3(0.2, 1, 0.3);
			}
			else if (on_line || on_ring)
				colour = e.y > 1.5 ? float3(0.1, 0.8, 1) : lerp(float3(1, 0.9, 0.1), float3(1, 0.1, 0.1), saturate(e.x / debug.y));
		}
		return float4(colour, 1);
	}
	if (debug.x == 4)
	{
		// Grey scene, red where the TAA clipped the reprojected history.
		const float rejection = history_tex.Load(int3(px, 0)).a;
		return float4(lerp(depth_shade.xxx * 0.5, float3(1, 0.1, 0.1), rejection), 1);
	}
	return float4(depth_shade.xxx, 1);
}
