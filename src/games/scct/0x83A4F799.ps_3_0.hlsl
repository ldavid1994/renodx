// Composite (0x83A4F799): the last draw of the 3D scene in normal view; night vision runs after it.
// The game adds the 8-bit bloom to the RGBA16F scene buffer and writes the 8-bit back buffer,
// which clips the sum at 1.0; the game has no tonemapping curve. Here the sum keeps
// its values above 1.0 and goes through ToneMapPass; Vanilla outputs the clipped sum as the game did.
//
// Original (fxc /dumpbin):
//   texld r0.xyz, v0.xyyy, s0          scene buffer
//   texld r1.xyz, v0.zwww, s1          bloom
//   if b0
//     texld r0.w, v0.xyyy, s5.x        mask
//     mad oC0.xyz, r0.xyzz, r0.w, r1.xyzz
//   else
//     add oC0.xyz, r1.xyzz, r0.xyzz
//   endif

#include "./shared.h"

sampler2D scene_buffer : register(s0);
sampler2D bloom : register(s1);
sampler2D mask : register(s5);
bool use_mask : register(b0);

float4 main(float4 texcoord : TEXCOORD0) : COLOR {
  float3 scene = tex2D(scene_buffer, texcoord.xy).rgb;
  const float3 bloom_color = tex2D(bloom, texcoord.zw).rgb;
  [branch] if (use_mask) {
    scene *= tex2D(mask, texcoord.xy).x;
  }
  // Gamma-encoded, like everything the game draws: it was shown on the display as written.
  // Bloom: 1 = vanilla.
  const float3 composite = scene + bloom_color * CUSTOM_BLOOM;

  float3 color;
  [branch] if (RENODX_TONE_MAP_TYPE == renodx::draw::TONE_MAP_TYPE_VANILLA) {
    // What vanilla showed: the sum clipped at white by the 8-bit back buffer.
    color = renodx::color::srgb::Decode(saturate(composite));
  }
  else {
    // The sum in linear light, before the 8-bit clip: everything the game drew past white.
    const float3 untonemapped = renodx::color::srgb::DecodeSafe(composite);
    // RenoDRT and None read exposure, highlights, shadows, contrast and saturation themselves, and
    // RenoDRT also Hue Shift (toward the hue of the game's per-channel clip) and Blowout.
    color = renodx::draw::ToneMapPass(untonemapped);
  }
  return float4(renodx::draw::RenderIntermediatePass(color), 1.f);
}
