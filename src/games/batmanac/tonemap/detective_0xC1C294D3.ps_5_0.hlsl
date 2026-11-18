// Detective-mode final pass 0xC1C294D3: saturate(lerp(scene * tint, color, fade)), then pow(cb0[2].x).
// The HDR tone mappers drop the saturate; under PsychoV-30, Hue Clip emulates its hue bend.

#include "../common.hlsli"

Texture2D<float4> t0 : register(t0);
SamplerState s0_s : register(s0);

cbuffer cb0 : register(b0) {
  float4 cb0[3];
}

void main(
    float2 v0: TEXCOORD0,
    out float4 o0: SV_Target0) {
  float4 scene = t0.Sample(s0_s, v0.xy);
  float3 untonemapped = lerp(scene.rgb * cb0[0].rgb, cb0[1].rgb, cb0[1].w);
  o0.w = scene.w;

  // cb0[2].x is the game's brightness slider, about 1 / 2.3 at its default, so the result decodes like the uber pass's.
  [branch]
  if (RENODX_TONE_MAP_TYPE == renodx::draw::TONE_MAP_TYPE_VANILLA) {
    o0.rgb = RenderEncoded(pow(saturate(untonemapped), cb0[2].x));
    return;
  }

  // max(0) keeps negatives out of pow, as the original's saturate did.
  o0.rgb = RenderEncoded(pow(ApplyHueClip(max(0, untonemapped)), cb0[2].x));
}
