// Night Vision HDR image (ps_3_0). With the toggle on, the add-on draws night vision with this and
// night_vision_vertex_shader in place of the game's 0x4ADA0B05 (ps_1_1), which tints the 8-bit
// brightness in alpha: (t0.a + t1.a + t2.a) * c0 + t3.a * c1, then * 2 * (1 + c2), with ps_1_x's
// [-1, 1] clamps on constants and every result, so the image stays at or below white. t0-t2 are the
// game's glows (256, 128 and 64 texels square), t3 the full-size brightness with the grain.
// This draws the same image, then adds light by how far the glow alone (t1, t2), weighted up, would
// take it past white, soft-limited. The glow is smooth, so blown-out areas get brighter without the
// surface detail the game's white hides; the light is in the image's own colour at white, rather
// than the tint, which keeps the near-white hue night vision clips to.

#include "./shared.h"
#include "./vision_hdr.hlsli"

sampler2D brightness_0 : register(s0);
sampler2D brightness_1 : register(s1);
sampler2D brightness_2 : register(s2);
sampler2D brightness_3 : register(s3);

float4 tint_brightness : register(c0);
float4 tint_extra : register(c1);
float4 tint_boost : register(c2);

// The glow's weight past white: at 1 it is what the game sums, which rarely reaches white alone.
static const float GLOW_STRENGTH = 2.f;
// The excess is added as a layer of light, fading in with the game's image from this much of white
// (gamma), so darker objects in front of a light keep their shape.
static const float LAYER_FROM = 0.33f;

float4 main(float2 texcoord_0 : TEXCOORD0, float2 texcoord_1 : TEXCOORD1,
            float2 texcoord_2 : TEXCOORD2, float2 texcoord_3 : TEXCOORD3,
            float2 pixel_position : VPOS) : COLOR {
  const float4 c0 = clamp(tint_brightness, -1.f, 1.f);
  const float4 c1 = clamp(tint_extra, -1.f, 1.f);
  const float4 c2 = clamp(tint_boost, -1.f, 1.f);

  float brightness = clamp(tex2D(brightness_0, texcoord_0).a + tex2D(brightness_1, texcoord_1).a, -1.f, 1.f);
  brightness = clamp(brightness + tex2D(brightness_2, texcoord_2).a, -1.f, 1.f);
  float4 color = clamp(brightness * c0, -1.f, 1.f);
  color = clamp(tex2D(brightness_3, texcoord_3).a * c1 + color, -1.f, 1.f);
  color = saturate((color * c2 + color) * 2.f);

  const float smooth_glow_brightness = CubicSample(brightness_1, texcoord_1, CUSTOM_NIGHT_VISION_GLOW_SIZES.xy).a
                                       + CubicSample(brightness_2, texcoord_2, CUSTOM_NIGHT_VISION_GLOW_SIZES.zw).a;
  const float3 glow = GLOW_STRENGTH * smooth_glow_brightness * c0.rgb * 2.f * (1.f + c2.rgb);
  const float image_peak = max(color.r, max(color.g, color.b));
  const float added = RolledOffExcess(max(glow.r, max(glow.g, glow.b)), CUSTOM_VISION_PEAK_RATIO)
                      * smoothstep(LAYER_FROM, 1.f, image_peak);
  color.rgb = AddLight(color.rgb, added, pixel_position);
  return color;
}
