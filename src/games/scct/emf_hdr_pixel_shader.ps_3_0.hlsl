// EMF HDR image (ps_3_0). With the toggle on, the add-on draws EMF with this and emf_vertex_shader
// in place of the game's 0x7CE55E3E (ps_1_1), which is `texbem`: it shifts its lookup of EMF's
// image (t3) by the wobble texture (t0, V8U8) through the bump-environment matrix, and shows it.
// That image is the game's 8-bit, 256-texel weighted sum of its EMF glow levels (0xCDFC48BC: the
// 256, 128, 64 and 32 texel levels, weighted 0.5, 1, 1, 0.5), so the sum clips at white.
// This draws the same image, then adds light by how far the levels' own sum, smoothed, goes past
// white, soft-limited toward Peak. Smoothed because past white the finer levels' surface detail
// shows as blotches the clip used to hide.

#include "./shared.h"
#include "./vision_hdr.hlsli"

sampler2D wobble : register(s0);
sampler2D image : register(s3);
sampler2D level_0 : register(s4);
sampler2D level_1 : register(s5);
sampler2D level_2 : register(s6);
sampler2D level_3 : register(s7);

float4 level_weights[4] : register(c0);  // the game's, still set from its sum of the levels

static const float SMOOTHING_WEIGHTS[4] = {0.125f, 0.375f, 0.375f, 0.125f};

float4 main(float2 texcoord_0 : TEXCOORD0, float2 texcoord_3 : TEXCOORD3, float2 pixel_position : VPOS) : COLOR {
  // texbem: u += M00 * du + M10 * dv, v += M01 * du + M11 * dv.
  const float2 offset = tex2D(wobble, texcoord_0).rg;
  const float2 texcoord = texcoord_3 + offset.x * CUSTOM_EMF_BUMP_MATRIX.xy + offset.y * CUSTOM_EMF_BUMP_MATRIX.zw;
  float4 color = saturate(tex2D(image, texcoord));

  // The finer levels carry the detail of the glowing surfaces, which past white shows as a cloudy
  // mottle the clip used to hide: they are smoothed to the coarsest level's scale first, with a
  // B-spline (1, 3, 3, 1) grid of 4 x 4 bilinear taps one 64-texel level texel apart.
  float3 fine_sum = 0.f;
  [unroll] for (int y = 0; y < 4; ++y) {
    [unroll] for (int x = 0; x < 4; ++x) {
      const float2 tap = texcoord + (float2(x, y) - 1.5f) / CUSTOM_EMF_LEVEL_SIZES.z;
      const float weight = SMOOTHING_WEIGHTS[x] * SMOOTHING_WEIGHTS[y];
      fine_sum += weight * (tex2D(level_0, tap).rgb * level_weights[0].rgb + tex2D(level_1, tap).rgb * level_weights[1].rgb + tex2D(level_2, tap).rgb * level_weights[2].rgb);
    }
  }
  const float3 levels_sum = fine_sum + CubicSample(level_3, texcoord, CUSTOM_EMF_LEVEL_SIZES.ww).rgb * level_weights[3].rgb;
  const float image_peak = max(color.r, max(color.g, color.b));
  const float added = RolledOffExcess(max(levels_sum.r, max(levels_sum.g, levels_sum.b)), CUSTOM_VISION_PEAK_RATIO)
                      * saturate(image_peak * 2.f - 1.f);
  color.rgb = AddLight(color.rgb, added, pixel_position);
  return color;
}
