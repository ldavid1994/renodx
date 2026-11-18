// Masked flame sheet 0xE6F50BDA: 0x2A25E549 without the refracted scene. Emissive sheets raise a dimmer hot color
// to the cool color's luminance outside Vanilla.

#include "./common.hlsli"

Texture2D<float4> t1 : register(t1);
Texture2D<float4> t0 : register(t0);
SamplerState s1_s : register(s1);
SamplerState s0_s : register(s0);

cbuffer cb0 : register(b0) {
  float4 cb0[5];
}

void main(
    float4 v0: TEXCOORD10,
    float4 v1: TEXCOORD0,
    float4 v2: TEXCOORD1,
    float4 v3: TEXCOORD4,
    float4 v4: TEXCOORD6,
    float4 v5: TEXCOORD5,
    uint v6: SV_IsFrontFace0,
    out float4 o0: SV_Target0,
    out float4 o1: SV_Target1,
    out float4 o2: SV_Target2) {
  float3 density_texel = lerp(t0.SampleBias(s0_s, v1.xy, 0).xyz, t0.SampleBias(s0_s, v1.zw, 0).xyz, v0.x);
  float density = dot(density_texel, v2.xyz);
  float color_blend = min(1.f, exp2(cb0[4].x * log2(max(9.99999997e-007f, density))));
  float3 color = lerp(cb0[2].xyz, MatchHotLuminance(cb0[2].xyz, cb0[3].xyz), color_blend);
  color = cb0[4].y * color + cb0[1].xyz;

  o0.xyz = color * v3.w + v3.xyz;
  float mask = t1.SampleBias(s1_s, 8.f * v1.xy, 0).y;
  o0.w = mask * mask * saturate(v2.w * density);
  o1 = 0;
  o2 = 0;
}
