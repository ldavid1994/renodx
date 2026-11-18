#ifndef SRC_GAMES_BATMANAC_COMMON_HLSLI_
#define SRC_GAMES_BATMANAC_COMMON_HLSLI_

#include "./shared.h"

// Hue Clip (PsychoV-30): bends hue before the LUT lookup as the original's per-channel clip did. Preserves signs and
// magnitude endpoints, transfers only Neutwo's per-channel positions, not its whitening, then restores the luminance.
// Clip bend by ShortFuse.
float3 ApplyHueClip(float3 color) {
  if (RENODX_TONE_MAP_TYPE != CUSTOM_TONE_MAP_TYPE_PSYCHOV30) return color;
  const float3 magnitude = abs(color);
  const float3 reference = renodx::tonemap::neutwo::PerChannel(magnitude);
  const float reference_min = renodx::math::Min(reference);
  const float reference_range = renodx::math::Max(reference) - reference_min;
  const bool neutral = reference_range == 0.f;
  float3 transferred = renodx::math::Rescale(reference - reference_min, 0.f,
      renodx::math::Select(neutral, 1.f, reference_range),
      renodx::math::Min(magnitude), renodx::math::Max(magnitude));
  transferred = renodx::math::CopySign(transferred, color);
  transferred = renodx::math::Select(neutral, color, transferred);
  // Sky blue (blue > green > red) bends twice as far, toward cyan, as the original's clip turned it; fire and the
  // other channel orders keep the slider's strength.
  const bool sky_blue = color.b >= color.g && color.g >= color.r;
  float3 bent = lerp(color, transferred, saturate(CUSTOM_HUE_CLIP * (sky_blue ? 2.f : 1.f)));
  const float original_y = renodx::color::y::from::BT709(color);
  const float bent_y = renodx::color::y::from::BT709(bent);
  return (original_y > 0.f && bent_y > 0.f) ? bent * (original_y / bent_y) : color;
}

// Emissive flame sheets author their cool (edge) color above SDR white but their hot (core) color at it, darkening the
// core once nothing clips. Raises a dimmer hot color, hue kept, to the cool color's luminance.
float3 MatchHotLuminance(float3 cool_color, float3 hot_color) {
  if (RENODX_TONE_MAP_TYPE == renodx::draw::TONE_MAP_TYPE_VANILLA) return hot_color;
  const float cool_luminance = renodx::color::y::from::BT709(cool_color);
  const float hot_luminance = renodx::color::y::from::BT709(hot_color);
  if (cool_luminance <= 1.f || hot_luminance <= 0.f) return hot_color;
  return hot_color * max(1.f, cool_luminance / hot_luminance);
}

// Renders a pass's encoded output: decoded as sRGB, as Vanilla does; Gamma Correction then shows it as a 2.2 display
// would. PsychoV-30 and None tone map it. Clips nothing: the passes keep Vanilla's clip themselves.
float3 RenderEncoded(float3 encoded) {
  float3 color = renodx::color::srgb::DecodeSafe(encoded);
  [branch]
  if (RENODX_TONE_MAP_TYPE == CUSTOM_TONE_MAP_TYPE_PSYCHOV30) {
    float peak = max(1.f, RENODX_PEAK_WHITE_NITS / RENODX_DIFFUSE_WHITE_NITS);
    if (RENODX_GAMMA_CORRECTION == renodx::draw::GAMMA_CORRECTION_GAMMA_2_2) {
      peak = renodx::color::correct::Gamma(peak, true, 2.2f);
    }
    // Compression above PsychoV-30's own default of 1 keeps the toe nearer the identity below white; how near depends
    // on peak (the toe steepens as anchor / peak grows).
    color = renodx::tonemap::psychov::psychotm_test30(
        color, peak,
        RENODX_TONE_MAP_EXPOSURE, RENODX_TONE_MAP_HIGHLIGHTS, RENODX_TONE_MAP_SHADOWS, RENODX_TONE_MAP_CONTRAST,
        RENODX_TONE_MAP_SATURATION,
        1.f, 100.f, 1.f, 1.f, 0,  // placeholders: bleaching, clip point, hue restore, response power, white curve
        1.f,                      // cone response: the game has no tone curve around the anchor
        0.18f, 0.18f,             // input and output anchors: midgray
        1.f, 1,                   // gamut compression, BT.2020 target (HDR10 output only)
        1.f,                      // placeholder: adaptive normalization
        CUSTOM_PSYCHOV_COMPRESSION);
  } else if (RENODX_TONE_MAP_TYPE != renodx::draw::TONE_MAP_TYPE_VANILLA) {
    color = renodx::draw::ToneMapPass(color);
  }
  return renodx::draw::RenderIntermediatePass(color);
}

// Shader garbage: a channel far below 0 or infinite. Materials can write small negatives that the DOF lerp needs;
// garbage goes far beyond them.
bool IsGarbageColor(float3 color) {
  return any(color < -0.5f) || any(isinf(color));
}

#endif  // SRC_GAMES_BATMANAC_COMMON_HLSLI_
