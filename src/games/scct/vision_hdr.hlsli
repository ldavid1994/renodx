// Shared by Night Vision HDR and EMF HDR: light past white added to a vision pass's encoded image,
// worked out in linear light, eased in, rolled off toward Peak and dithered.

#ifndef SRC_GAMES_SCCT_VISION_HDR_HLSLI_
#define SRC_GAMES_SCCT_VISION_HDR_HLSLI_

// The excess eases in over this much either side of white (linear), so its onset has no crease.
static const float EXCESS_KNEE = 0.25f;
// R2 low-discrepancy dither (Roberts) over the pixel position against HDR10 banding: peak to peak
// DITHER_STRENGTH of the colour (linear), full once the layer adds 1 / DITHER_ONSET of white.
static const float2 DITHER_PATTERN = float2(0.7548776662f, 0.5698402910f);
static const float DITHER_STRENGTH = 0.018f;
static const float DITHER_ONSET = 16.f;

// Cubic B-spline upsampling from 4 bilinear taps (Sigg and Hadwiger, GPU Gems 2, ch. 20). The
// game's glows are tiny, and the excess amplifies the creases their bilinear upsampling leaves at
// every texel edge.
float4 CubicSample(sampler2D glow, float2 texcoord, float2 size) {
  const float2 position = texcoord * size - 0.5f;
  const float2 f = frac(position);
  const float2 f2 = f * f;
  const float2 f3 = f2 * f;
  const float2 w0 = (1.f - 3.f * f + 3.f * f2 - f3) / 6.f;
  const float2 w1 = (4.f - 6.f * f2 + 3.f * f3) / 6.f;
  const float2 w3 = f3 / 6.f;
  const float2 first_weight = w0 + w1;
  const float2 corner = position - f;
  const float2 first_tap = (corner - 0.5f + w1 / first_weight) / size;
  const float2 second_tap = (corner + 1.5f + w3 / (1.f - first_weight)) / size;
  return lerp(lerp(tex2D(glow, second_tap), tex2D(glow, float2(first_tap.x, second_tap.y)), first_weight.x),
              lerp(tex2D(glow, float2(second_tap.x, first_tap.y)), tex2D(glow, first_tap), first_weight.x),
              first_weight.y);
}

// The image is the game's encoded output, decoded and encoded as the swap chain decodes it
// (CUSTOM_VISION_ENCODING: sRGB or 2.2 for Gamma Correction), so the light past white lands where
// the display shows it and Peak holds.

// The light (linear, in units of white) that an encoded peak puts past white: eased in with a
// soft knee, then at most what Peak leaves above white. x / (1 + x / limit) approaches that limit
// without reaching it, so the light rolls off toward Peak instead of clipping flat there.
float RolledOffExcess(float peak, float peak_ratio) {
  const float past_white = renodx::draw::DecodeColor(max(0.f, peak).xxx, CUSTOM_VISION_ENCODING).x - 1.f;
  float excess = max(0.f, past_white);
  if (abs(past_white) < EXCESS_KNEE) {
    excess = (past_white + EXCESS_KNEE) * (past_white + EXCESS_KNEE) / (4.f * EXCESS_KNEE);
  }
  const float limit = max(1e-3f, peak_ratio - 1.f);
  return excess / (1.f + excess / limit);
}

// The image with `added` (linear) on top as a layer of light in its own colour at white, which
// keeps the hue the game clips to, dithered as the layer grows.
float3 AddLight(float3 color, float added, float2 pixel_position) {
  const float image_peak = max(color.r, max(color.g, color.b));
  const float3 image_white = renodx::draw::DecodeColor(color / max(image_peak, 1e-4f), CUSTOM_VISION_ENCODING);
  const float dither = frac(dot(pixel_position, DITHER_PATTERN)) - 0.5f;
  const float3 linear_color = (renodx::draw::DecodeColor(color, CUSTOM_VISION_ENCODING) + added * image_white)
                              * (1.f + dither * DITHER_STRENGTH * saturate(added * DITHER_ONSET));
  return renodx::draw::EncodeColor(max(0.f, linear_color), CUSTOM_VISION_ENCODING);
}

#endif  // SRC_GAMES_SCCT_VISION_HDR_HLSLI_
