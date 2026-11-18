// LUT builder 0x3207DF59: the uber pass's 16^3 LUT strip (256x16) from the area's grade and the brightness slider.
// Its target is RGBA16F now: the HDR tone mappers keep the grade above 1, Vanilla clamps as the UNORM target did.

#include "./shared.h"

cbuffer cb0 : register(b0) {
  float4 cb0[9];
}

void main(
    float2 v0: TEXCOORD0,
    out float4 o0: SV_Target0) {
  float4 r0, r1;

  r0.xz = float2(-0.001953125, -0.03125) + v0.xy;
  r1.x = 16 * r0.x;
  r0.y = frac(r1.x);
  r0.w = -r0.y * 0.0625 + r0.x;
  r0.xyz = cb0[8].xxx * r0.yzw;
  r0.xyz = float3(1.06666672, 1.06666672, 1.06666672) * r0.xyz;
  r0.xyz = log2(r0.xyz);
  r0.xyz = float3(2.20000005, 2.20000005, 2.20000005) * r0.xyz;
  r0.xyz = exp2(r0.xyz);
  r0.xyz = saturate(-cb0[0].xyz + r0.xyz);
  r0.xyz = cb0[1].xyz * r0.xyz;
  r0.xyz = max(float3(9.99999994e-009, 9.99999994e-009, 9.99999994e-009), r0.xyz);
  r0.xyz = log2(r0.xyz);
  r0.xyz = cb0[2].xyz * r0.xyz;
  r0.xyz = exp2(r0.xyz);
  r0.w = dot(r0.xyz, cb0[3].xyz);
  r0.xyz = r0.xyz * cb0[0].www + r0.www;
  r0.xyz = r0.xyz * cb0[5].xyz + cb0[6].xyz;
  r0.xyz = cb0[4].xyz * r0.xyz;
  r0.xyz = log2(r0.xyz);
  r0.xyz = float3(0.454545468, 0.454545468, 0.454545468) * r0.xyz;
  r0.xyz = exp2(r0.xyz);
  r0.xyz = max(float3(9.99999994e-009, 9.99999994e-009, 9.99999994e-009), r0.xyz);
  r0.w = 9.99999994e-009;
  r0.xyzw = log2(r0.xyzw);
  r1.x = 2.20000005 * cb0[5].w;
  r0.xyzw = r1.xxxx * r0.xyzw;
  o0.xyzw = exp2(r0.xyzw);
  o0.rgb = (RENODX_TONE_MAP_TYPE == renodx::draw::TONE_MAP_TYPE_VANILLA) ? saturate(o0.rgb) : o0.rgb;
}
