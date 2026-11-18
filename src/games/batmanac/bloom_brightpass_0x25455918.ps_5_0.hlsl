// Bloom bright pass 0x25455918: five taps of the DOF blur target, scaled, thresholded and squared, capped at 8.
// The blur target is float now, so the taps are capped at 1 here as its UNORM format did (else sparks bloom red).

Texture2D<float4> t0 : register(t0);
SamplerState s0_s : register(s0);

cbuffer cb0 : register(b0) {
  float4 cb0[2];
}

void main(
    float4 v0: TEXCOORD0,
    float4 v1: TEXCOORD1,
    float2 v2: TEXCOORD2,
    out float4 o0: SV_Target0) {
  float4 r0 = saturate(t0.Sample(s0_s, v0.xy))
              + saturate(t0.Sample(s0_s, v0.zw))
              + saturate(t0.Sample(s0_s, v1.xy))
              + saturate(t0.Sample(s0_s, v1.zw))
              + saturate(t0.Sample(s0_s, v2.xy));
  r0.xyz = cb0[1].xyz * r0.xyz;
  o0.w = 0.2f * r0.w;
  r0.xyz = min(8.f, 0.8f * r0.xyz);
  r0.xyz = max(0, cb0[1].www + r0.xyz);
  o0.xyz = min(8.f, r0.xyz * r0.xyz);
}
