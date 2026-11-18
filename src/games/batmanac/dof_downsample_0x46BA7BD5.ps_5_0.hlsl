// DOF blur downsample 0x46BA7BD5: half-res scene / 4, blur amount from depth in alpha, into an RGBA16F target.
// Vanilla stores pixels capped as the UNORM target did; the HDR tone mappers drop garbage pixels.

#include "./common.hlsli"

Texture2D<float4> t0 : register(t0);
SamplerState s0_s : register(s0);

cbuffer cb0 : register(b0) {
  float4 cb0[3];
}

void main(
    float2 v0: TEXCOORD0,
    out float4 o0: SV_Target0) {
  float4 scene = t0.Sample(s0_s, v0.xy);

  // max(0) drops NaN. The blurs average this target, so capping here reproduces the original's blur exactly;
  // the HDR tone mappers keep the float range for valid pixels.
  float3 downsampled = 0.25f * max(0, scene.rgb);
  if (RENODX_TONE_MAP_TYPE == renodx::draw::TONE_MAP_TYPE_VANILLA) {
    o0.rgb = saturate(downsampled);
  } else {
    o0.rgb = IsGarbageColor(scene.rgb) ? 0.f : downsampled;
  }

  // Nearest depth of this texel and the ones 2.5 texels right and below (cb0[2].xy is the texel size).
  float depth = min(min(scene.w, t0.Sample(s0_s, v0.xy + cb0[2].xy * float2(2.5f, 0)).w),
                    t0.Sample(s0_s, v0.xy + cb0[2].xy * float2(0, 2.5f)).w);

  // Circle of confusion: distance from the focus plane (cb0[0].x) scaled and raised to a power, capped by the near
  // or far maximum, signed (the original's lt / lt / iadd on all-ones masks is sign(), 0 at the focus plane).
  float focus_distance = depth - cb0[0].x;
  float blur = pow(abs(cb0[0].y * focus_distance), cb0[0].z);
  float max_blur = (focus_distance < 0) ? cb0[1].x : cb0[1].y;
  float signed_blur = saturate(min(max_blur, blur)) * sign(focus_distance);

  // Stored as 0.5 +/- half the blur, / 4 like the color.
  o0.a = 0.25f * (signed_blur * 0.5f + 0.5f);
}
