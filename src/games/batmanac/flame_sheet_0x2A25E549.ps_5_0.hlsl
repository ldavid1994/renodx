// Masked flame sheet 0x2A25E549: as 0x8E23494B, with a tiled mask texture squared into alpha. Emissive sheets raise
// a dimmer hot color to the cool color's luminance outside Vanilla.

#include "./common.hlsli"

Texture2D<float4> t3 : register(t3);
Texture2D<float4> t2 : register(t2);
Texture2D<float4> t1 : register(t1);
Texture2D<float4> t0 : register(t0);
SamplerState s3_s : register(s3);
SamplerState s2_s : register(s2);
SamplerState s1_s : register(s1);
SamplerState s0_s : register(s0);

cbuffer cb2 : register(b2) {
  float4 cb2[5];
}

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
  float3 screen_position = v5.xyw;
  if (cb2[4].x != 0) {
    float2 depth_scale_bias = t0.Sample(s1_s, float2(0.0625f, 0)).xy;
    if (v5.w != 1.f) {
      screen_position.x = depth_scale_bias.x * (v5.w - depth_scale_bias.y) + v5.x;
    }
  }
  float2 scene_uv = (screen_position.xy / screen_position.z) * cb2[0].xy + cb2[0].wz;
  float3 refracted_scene = t1.Sample(s0_s, scene_uv).xyz;

  float3 density_texel = lerp(t2.SampleBias(s2_s, v1.xy, 0).xyz, t2.SampleBias(s2_s, v1.zw, 0).xyz, v0.x);
  float density = dot(density_texel, v2.xyz);
  float color_blend = min(1.f, exp2(cb0[4].x * log2(max(9.99999997e-007f, density))));
  float3 color = lerp(cb0[2].xyz, MatchHotLuminance(cb0[2].xyz, cb0[3].xyz), color_blend) + refracted_scene;
  color = cb0[4].y * color + cb0[1].xyz;

  float mask = t3.SampleBias(s3_s, 8.f * v1.xy, 0).y;
  o0.w = mask * mask * saturate(v2.w * density);
  o0.xyz = color * v3.w + v3.xyz;
  o1 = 0;
  o2 = 0;
}
