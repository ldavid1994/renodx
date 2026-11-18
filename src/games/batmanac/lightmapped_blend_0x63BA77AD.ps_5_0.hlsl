// Lightmapped surface 0x63BA77AD: the vertex color blends two normal maps and three texture layers and sets the
// specular exponent. The HDR tone mappers clamp it to 0..1: MSAA edge pixels of thin triangles extrapolate it, and a
// negative exponent blows single pixels far past white.

#include "./common.hlsli"

Texture2DArray<float4> t31 : register(t31);
Texture2D<float4> t8 : register(t8);
Texture2D<float4> t7 : register(t7);
Texture2D<float4> t6 : register(t6);
Texture2D<float4> t5 : register(t5);
Texture2D<float4> t4 : register(t4);
Texture2D<float4> t3 : register(t3);
Texture2D<float4> t2 : register(t2);
Texture2D<float4> t1 : register(t1);
Texture2D<float4> t0 : register(t0);
SamplerState s15_s : register(s15);
SamplerState s8_s : register(s8);
SamplerState s7_s : register(s7);
SamplerState s6_s : register(s6);
SamplerState s5_s : register(s5);
SamplerState s4_s : register(s4);
SamplerState s3_s : register(s3);
SamplerState s2_s : register(s2);
SamplerState s1_s : register(s1);
SamplerState s0_s : register(s0);
cbuffer cb10 : register(b10)
{
  float4 cb10[3];
}

cbuffer cb2 : register(b2)
{
  float4 cb2[5];
}

cbuffer cb0 : register(b0)
{
  float4 cb0[6];
}

// 3Dmigoto declarations
#define cmp -

void main(
  float4 v0 : COLOR0,
  float4 v1 : TEXCOORD0,
  float4 v2 : TEXCOORD1,
  float4 v3 : TEXCOORD6,
  float4 v4 : TEXCOORD7,
  float4 v5 : TEXCOORD5,
  float4 v6 : TEXCOORD12,
  float4 v7 : TEXCOORD13,
  float4 v8 : TEXCOORD14,
  float4 v9 : TEXCOORD15,
  float4 v10 : TEXCOORD16,
  float4 v11 : TEXCOORD17,
  float4 v12 : TEXCOORD18,
  float4 v13 : TEXCOORD19,
  float4 v14 : TEXCOORD20,
  float4 v15 : TEXCOORD21,
  float4 v16 : TEXCOORD22,
  float4 v17 : TEXCOORD23,
  float4 v18 : TEXCOORD24,
  float4 v19 : TEXCOORD25,
  float4 v20 : TEXCOORD26,
  float4 v21 : TEXCOORD27,
  float4 v22 : TEXCOORD28,
  uint v23 : SV_IsFrontFace0,
  out float4 o0 : SV_Target0,
  out float4 o1 : SV_Target1,
  out float4 o2 : SV_Target2)
{
  float4 r0,r1,r2,r3,r4,r5,r6,r7,r8;
  uint4 bitmask, uiDest;
  float4 fDest;

  if (RENODX_TONE_MAP_TYPE != renodx::draw::TONE_MAP_TYPE_VANILLA) {
    v0 = saturate(v0);
  }

  r0.x = cmp(0 != cb2[4].x);
  if (r0.x != 0) {
    r0.y = v5.w;
  }
  r0.x = r0.x ? r0.y : v5.w;
  r0.y = dot(v3.xyz, v3.xyz);
  r0.y = rsqrt(r0.y);
  r0.yzw = v3.xyz * r0.yyy;
  r1.xyz = t2.SampleBias(s2_s, v2.xy, 0).xyz;
  r1.xyz = r1.xyz * float3(2,2,2) + float3(-1,-1,-1);
  r2.xyz = t3.SampleBias(s3_s, v2.xy, 0).xyz;
  r2.xyz = r2.xyz * float3(2,2,2) + float3(-1,-1,-1);
  r2.xyz = r2.xyz + -r1.xyz;
  r1.xyz = v0.xxx * r2.xyz + r1.xyz;
  r1.w = dot(r1.xyz, r1.xyz);
  r1.w = rsqrt(r1.w);
  r1.xyz = r1.xyz * r1.www;
  r1.w = dot(r1.xyz, r0.yzw);
  r2.xyz = r1.xyz * r1.www;
  r0.yzw = r2.xyz * float3(2,2,2) + -r0.yzw;
  r2.xyz = float3(1,1,1) + -cb0[0].xyz;
  r3.xyz = t4.SampleBias(s4_s, v2.xy, 0).xyz;
  r4.xy = r0.yz * float2(0.5,0.5) + float2(0.5,0.5);
  r4.xyz = t5.SampleBias(s5_s, r4.xy, 0).xyz;
  r5.xyz = t6.SampleBias(s6_s, v2.xy, 0).xyz;
  r6.xyz = t7.SampleBias(s7_s, v2.xy, 0).xyz;
  r7.xyz = t8.SampleBias(s8_s, v2.xy, 0).xyz;
  r7.xyz = r7.xyz + -r6.xyz;
  r8.xyz = v0.xxx * r7.xyz + r6.xyz;
  r4.xyz = r4.xyz * r5.xyz + r8.xyz;
  r5.xyz = r8.xyz + -r4.xyz;
  r4.xyz = v0.zzz * r5.xyz + r4.xyz;
  r4.xyz = r4.xyz + -r3.xyz;
  r3.xyz = v0.yyy * r4.xyz + r3.xyz;
  r2.xyz = r3.xyz * r2.xyz;
  r2.xyz = r2.xyz * cb2[2].www + cb2[2].xyz;
  r3.x = saturate(dot(r1.yz, float2(0.816496611,0.577350259)));
  r3.y = saturate(dot(r1.xyz, float3(-0.707106769,-0.408248305,0.577350259)));
  r3.z = saturate(dot(r1.yzx, float3(-0.408248305,0.577350259,0.707106769)));
  r1.x = saturate(dot(r0.zw, float2(0.816496611,0.577350259)));
  r1.y = saturate(dot(r0.yzw, float3(-0.707106769,-0.408248305,0.577350259)));
  r1.z = saturate(dot(r0.zwy, float3(-0.408248305,0.577350259,0.707106769)));
  r0.yzw = r3.xyz * r3.xyz;
  r1.w = v0.x * 18 + 2;
  r1.xyz = log2(r1.xyz);
  r1.xyz = r1.www * r1.xyz;
  r1.xyz = exp2(r1.xyz);
  r3.xyz = t0.Sample(s0_s, v1.xy).xyz;
  r3.xyz = r3.xyz * r3.xyz;
  r3.xyz = cb0[4].xyz * r3.xyz;
  r4.xyz = t1.Sample(s1_s, v1.xy).xyz;
  r4.xyz = r4.xyz * r4.xyz;
  r4.xyz = cb0[5].xyz * r4.xyz;
  r0.y = dot(r4.xyz, r0.yzw);
  r5.xyz = v0.xyz * r7.xyz + r6.xyz;
  r5.xyz = r5.xyz * cb2[3].www + cb2[3].xyz;
  r0.z = dot(r4.xyz, r1.xyz);
  r1.xyz = r5.xyz * r0.zzz;
  r0.yzw = r0.yyy * r2.xyz + r1.xyz;
  r0.yzw = r0.yzw * r3.xyz + cb0[0].xyz;
  r0.yzw = r2.xyz * cb0[3].xyz + r0.yzw;
  r0.x = cb2[1].x / r0.x;
  o0.w = cb2[1].y + r0.x;
  r0.x = cmp(0 != cb10[0].x);
  if (r0.x != 0) {
    r1.xy = v7.xy / v7.ww;
    r0.x = min(0.999000013, v7.z);
    r1.z = 0;
    r1.x = t31.Sample(s15_s, r1.xyz).x;
    r0.x = r1.x + -r0.x;
    r0.x = saturate(r0.x * 60 + 1);
    r1.xy = v8.xy / v8.ww;
    r1.w = min(0.999000013, v8.z);
    r1.z = 1;
    r1.x = t31.Sample(s15_s, r1.xyz).x;
    r1.x = r1.x + -r1.w;
    r1.x = saturate(r1.x * 60 + 1);
    r0.x = r1.x + r0.x;
    r1.xy = v9.xy / v9.ww;
    r1.w = min(0.999000013, v9.z);
    r1.z = 2;
    r1.x = t31.Sample(s15_s, r1.xyz).x;
    r1.x = r1.x + -r1.w;
    r1.x = saturate(r1.x * 60 + 1);
    r0.x = r1.x + r0.x;
    r1.xy = v10.xy / v10.ww;
    r1.w = min(0.999000013, v10.z);
    r1.z = 3;
    r1.x = t31.Sample(s15_s, r1.xyz).x;
    r1.x = r1.x + -r1.w;
    r1.x = saturate(r1.x * 60 + 1);
    r0.x = r1.x + r0.x;
    r1.xy = v11.xy / v11.ww;
    r1.w = min(0.999000013, v11.z);
    r1.z = 4;
    r1.x = t31.Sample(s15_s, r1.xyz).x;
    r1.x = r1.x + -r1.w;
    r1.x = saturate(r1.x * 60 + 1);
    r0.x = r1.x + r0.x;
    r1.xy = v12.xy / v12.ww;
    r1.w = min(0.999000013, v12.z);
    r1.z = 5;
    r1.x = t31.Sample(s15_s, r1.xyz).x;
    r1.x = r1.x + -r1.w;
    r1.x = saturate(r1.x * 60 + 1);
    r0.x = r1.x + r0.x;
    r1.xy = v13.xy / v13.ww;
    r1.w = min(0.999000013, v13.z);
    r1.z = 6;
    r1.x = t31.Sample(s15_s, r1.xyz).x;
    r1.x = r1.x + -r1.w;
    r1.x = saturate(r1.x * 60 + 1);
    r0.x = r1.x + r0.x;
    r1.xy = v14.xy / v14.ww;
    r1.w = min(0.999000013, v14.z);
    r1.z = 7;
    r1.x = t31.Sample(s15_s, r1.xyz).x;
    r1.x = r1.x + -r1.w;
    r1.x = saturate(r1.x * 60 + 1);
    r0.x = r1.x + r0.x;
    r1.xy = v15.xy / v15.ww;
    r1.w = min(0.999000013, v15.z);
    r1.z = 8;
    r1.x = t31.Sample(s15_s, r1.xyz).x;
    r1.x = r1.x + -r1.w;
    r1.x = saturate(r1.x * 60 + 1);
    r0.x = r1.x + r0.x;
    r1.xy = v16.xy / v16.ww;
    r1.w = min(0.999000013, v16.z);
    r1.z = 9;
    r1.x = t31.Sample(s15_s, r1.xyz).x;
    r1.x = r1.x + -r1.w;
    r1.x = saturate(r1.x * 60 + 1);
    r0.x = r1.x + r0.x;
    r1.xy = v17.xy / v17.ww;
    r1.w = min(0.999000013, v17.z);
    r1.z = 10;
    r1.x = t31.Sample(s15_s, r1.xyz).x;
    r1.x = r1.x + -r1.w;
    r1.x = saturate(r1.x * 60 + 1);
    r0.x = r1.x + r0.x;
    r1.xy = v18.xy / v18.ww;
    r1.w = min(0.999000013, v18.z);
    r1.z = 11;
    r1.x = t31.Sample(s15_s, r1.xyz).x;
    r1.x = r1.x + -r1.w;
    r1.x = saturate(r1.x * 60 + 1);
    r0.x = r1.x + r0.x;
    r1.xy = v19.xy / v19.ww;
    r1.w = min(0.999000013, v19.z);
    r1.z = 12;
    r1.x = t31.Sample(s15_s, r1.xyz).x;
    r1.x = r1.x + -r1.w;
    r1.x = saturate(r1.x * 60 + 1);
    r0.x = r1.x + r0.x;
    r1.xy = v20.xy / v20.ww;
    r1.w = min(0.999000013, v20.z);
    r1.z = 13;
    r1.x = t31.Sample(s15_s, r1.xyz).x;
    r1.x = r1.x + -r1.w;
    r1.x = saturate(r1.x * 60 + 1);
    r0.x = r1.x + r0.x;
    r1.xy = v21.xy / v21.ww;
    r1.w = min(0.999000013, v21.z);
    r1.z = 14;
    r1.x = t31.Sample(s15_s, r1.xyz).x;
    r1.x = r1.x + -r1.w;
    r1.x = saturate(r1.x * 60 + 1);
    r0.x = r1.x + r0.x;
    r1.xy = v22.xy / v22.ww;
    r1.w = min(0.999000013, v22.z);
    r1.z = 15;
    r1.x = t31.Sample(s15_s, r1.xyz).x;
    r1.x = r1.x + -r1.w;
    r1.x = saturate(r1.x * 60 + 1);
    r0.x = r1.x + r0.x;
    r0.x = 0.0625 * r0.x;
    r1.x = dot(v6.xyz, v6.xyz);
    r1.x = sqrt(r1.x);
    r1.x = r1.x * cb10[2].y + cb10[2].z;
    r1.x = max(0, r1.x);
    r1.x = -r1.x * r1.x + 1;
    r1.x = max(0, r1.x);
    r1.x = cb10[2].x * r1.x + cb10[2].w;
    r1.x = abs(r1.x) * abs(r1.x);
    r1.yzw = float3(-1,-1,-1) + cb10[0].yzw;
    r1.xyz = r1.xxx * r1.yzw + float3(1,1,1);
    r0.x = r0.x * r0.x;
    r2.xyz = float3(1,1,1) + -r1.xyz;
    r1.xyz = r0.xxx * r2.xyz + r1.xyz;
    o0.xyz = r1.xyz * r0.yzw;
  } else {
    o0.xyz = r0.yzw;
  }
  o1.xyzw = float4(0,0,0,0);
  o2.xyzw = float4(0,0,0,0);
  return;
}
