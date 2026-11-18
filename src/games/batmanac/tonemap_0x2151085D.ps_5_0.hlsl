// Tonemap uber pass 0x2151085D, motion blur on: motion blur's constants come before the DOF tap offset in cb0.

#include "./tonemap.hlsli"

cbuffer cb0 : register(b0) {
  float4 cb0[44];
}

void main(
    float4 v0: TEXCOORD0,
    float2 v1: TEXCOORD1,
    out float4 o0: SV_Target0) {
  o0 = TonemapUberPass(v0.zw, v1, cb0[8], cb0[9], cb0[43].xy);
}
