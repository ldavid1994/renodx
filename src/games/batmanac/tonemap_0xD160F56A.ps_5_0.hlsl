// Tonemap uber pass 0xD160F56A, motion blur off.

#include "./tonemap.hlsli"

cbuffer cb0 : register(b0) {
  float4 cb0[12];
}

void main(
    float4 v0: TEXCOORD0,
    float2 v1: TEXCOORD1,
    out float4 o0: SV_Target0) {
  o0 = TonemapUberPass(v0.zw, v1, cb0[8], cb0[9], cb0[11].xy);
}
