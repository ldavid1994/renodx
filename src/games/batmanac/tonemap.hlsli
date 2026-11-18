#ifndef SRC_GAMES_BATMANAC_TONEMAP_HLSLI_
#define SRC_GAMES_BATMANAC_TONEMAP_HLSLI_

// Tonemap uber pass: DOF and bloom in linear, then pow(1 / 2.2), min(1) and a 16^3 LUT.
// The HDR tone mappers grade through the LUT by the max channel rolled off past BRIDGE_ANCHOR, then scale back.

#include "./common.hlsli"
#include "../../shaders/tonemap/c_infinity_rolloff.hlsl"

// Linear max channel where the bridge's roll-off into the LUT starts; below it the roll-off and its inverse are the
// identity. The roll-off is Musa's CInfinityRollOff.
#define BRIDGE_ANCHOR 0.375f

Texture2D<float4> t3 : register(t3);
Texture2D<float4> t2 : register(t2);
Texture2D<float4> t1 : register(t1);
Texture2D<float4> t0 : register(t0);
SamplerState s3_s : register(s3);
SamplerState s2_s : register(s2);
SamplerState s1_s : register(s1);
SamplerState s0_s : register(s0);

cbuffer cb2 : register(b2) {
  float4 cb2[2];
}

// Vanilla's 2D-strip addressing of the 16^3 LUT, encoded in and out; input must be in [0, 1].
float3 SampleLut(Texture2D<float4> lut, SamplerState lut_sampler, float3 encoded) {
  float blue_slice = floor(encoded.b * 14.9999f);
  float2 uv = float2(blue_slice * 0.0625f + encoded.r * 0.05859375f, encoded.g * 0.9375f);
  float3 lower = lut.Sample(lut_sampler, uv + float2(0.001953125f, 0.03125f)).rgb;
  float3 upper = lut.Sample(lut_sampler, uv + float2(0.064453125f, 0.03125f)).rgb;
  return lerp(lower, upper, encoded.b * 15.f - blue_slice);
}

// The HDR tone mappers drop garbage pixels; other negatives reach the DOF and the max(0) after the bloom, as in Vanilla.
float3 DropGarbage(float3 scene) {
  return (RENODX_TONE_MAP_TYPE != renodx::draw::TONE_MAP_TYPE_VANILLA && IsGarbageColor(scene)) ? 0.f : scene;
}

// Adds the bloom (a thresholded bright pass) in linear light, then grades and renders. max(0) keeps the scene's
// negatives out of the 2.2 encode.
float4 GradeAndRender(float3 untonemapped, float3 bloom, Texture2D<float4> lut, SamplerState lut_sampler) {
  untonemapped = max(0, untonemapped + bloom * CUSTOM_BLOOM);

  [branch]
  if (RENODX_TONE_MAP_TYPE == renodx::draw::TONE_MAP_TYPE_VANILLA) {
    float3 graded_encoded = SampleLut(lut, lut_sampler, min(1.f, renodx::color::gamma::Encode(untonemapped, 2.2f)));
    return float4(RenderEncoded(graded_encoded), dot(graded_encoded, float3(0.299f, 0.587f, 0.114f)));
  } else {
    // Rolled off and scaled back in linear; only the LUT's input and output are 2.2-encoded.
    float3 clipped = ApplyHueClip(untonemapped);
    float sdr_scale = renodx::tonemap::c_infinity_rolloff::ComputeMaxChannelScale(clipped, 1.f, BRIDGE_ANCHOR);
    float3 sdr = saturate(renodx::color::gamma::Encode(clipped * sdr_scale, 2.2f));
    float3 graded_sdr_linear = lerp(renodx::color::gamma::Decode(sdr, 2.2f),
                                    renodx::color::gamma::DecodeSafe(SampleLut(lut, lut_sampler, sdr), 2.2f),
                                    RENODX_COLOR_GRADE_STRENGTH);
    float3 graded_encoded = renodx::color::gamma::EncodeSafe(graded_sdr_linear / sdr_scale, 2.2f);

    return float4(RenderEncoded(graded_encoded), dot(min(1.f, graded_encoded), float3(0.299f, 0.587f, 0.114f)));
  }
}

// focus: focus distance, scale and exponent of the blur curve; blur_limits: near and far maximums.
// dof_offset: the blurred image's tap offset, in a different cb0 register per variant.
float4 TonemapUberPass(float2 scene_uv, float2 downsampled_uv, float4 focus, float4 blur_limits, float2 dof_offset) {
  // Depth of field: the larger of this pixel's own blur and the blurred image's, then a lerp toward the blurred image.
  // Depth buffer value (scene alpha) to view distance (cb2[1].zw), then the distance from the focus plane.
  float depth = t0.SampleLevel(s0_s, scene_uv, 0).w;
  float focus_distance = 1 / (depth * cb2[1].z - cb2[1].w) - focus.x;
  // Scaled and raised to a power, capped by the near or far maximum, doubled.
  float blur = pow(focus.y * abs(focus_distance), focus.z);
  float max_blur = (focus_distance < 0) ? blur_limits.x : blur_limits.y;
  float4 blurred = t1.Sample(s1_s, downsampled_uv + dof_offset * float2(2, -2));
  // The blurred image stores color and blur amount / 4.
  float dof_amount = saturate(max(2 * min(max_blur, blur), 4 * blurred.a) * CUSTOM_DOF);
  float3 scene = DropGarbage(t0.Sample(s0_s, scene_uv).rgb);
  return GradeAndRender(lerp(scene, blurred.rgb * 4.f, dof_amount), t2.Sample(s2_s, downsampled_uv).rgb, t3, s3_s);
}

#endif  // SRC_GAMES_BATMANAC_TONEMAP_HLSLI_
