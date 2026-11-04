#ifndef SRC_GAMES_SCCT_SHARED_H_
#define SRC_GAMES_SCCT_SHARED_H_

// Splinter Cell: Chaos Theory (Unreal Engine 2.5, D3D9, SM3.0 path).
//
// Must be 32bit aligned. 4 x float4.
// The composite replacement is ps_3_0, which has c0-c223; the game's own composite uses no float
// constants, so the injection lives in c50-c53 (constant_buffer_offset = 50 * 4 in addon.cpp).
// Tone Mapper values: Vanilla 0, None 1 and RenoDRT 3, the framework's types.
// Shadows at 100. RenoDRT lifts dark values by adding c * x^(2 - shadows); at 2 that is a flat floor
// under everything above black (blotches, near-black colours blown up), so the slider stops at 1.75.
#define CUSTOM_SHADOWS_MAX 1.75f

struct ShaderInjectData {
  float peak_white_nits;
  float diffuse_white_nits;
  float graphics_white_nits;
  float tone_map_type;

  float gamma_correction;
  float tone_map_hue_shift;  // RenoDRT: 0 = off, 1 = the hue of the game's per-channel clip
  float saturation;          // 1 = vanilla
  float bloom;               // 1 = vanilla

  float tone_map_exposure;    // 1 = vanilla
  float tone_map_highlights;  // 1 = neutral
  float tone_map_shadows;     // 1 = neutral
  float tone_map_contrast;    // 1 = neutral

  float swap_chain_output_preset;  // set by the add-on from Output Mode and HDR Format
  float tone_map_blowout;          // RenoDRT: highlight desaturation, 0 = off
  float padding0[2];
};

#ifdef __cplusplus

// The constants of the add-on's own draws (Night Vision HDR, EMF HDR, the scene scale) from c8, in
// float4 registers as the add-on uploads them. The shaders read them through draw_settings below:
// HLSL would lay these arrays out one element per register. Encoding is renodx::draw's
// (ENCODING_SRGB or ENCODING_GAMMA_2_2), the one the swap chain pass decodes the back buffer with.
struct NightVisionConstants {
  float glow_sizes[4];  // in texels, xy: s1, zw: s2
  float peak_ratio;     // Peak over Game Brightness
  float encoding;
  float padding0[2];
};

struct EmfConstants {
  float bump_matrix[4];  // BUMPENVMAT00, 01, 10, 11 of stage 3
  float peak_ratio;      // Peak over Game Brightness
  float encoding;
  float padding0[2];
  float level_sizes[4];  // in texels, per level (square)
};

struct SceneScaleConstants {
  float scale;  // Game over UI Brightness
  float encoding;
  float padding0[2];
};

#else

#if (__SHADER_TARGET_MAJOR == 3)

float4 shader_injection[4] : register(c50);

#define RENODX_PEAK_WHITE_NITS          shader_injection[0][0]
#define RENODX_DIFFUSE_WHITE_NITS       shader_injection[0][1]
#define RENODX_GRAPHICS_WHITE_NITS      shader_injection[0][2]
#define RENODX_TONE_MAP_TYPE            shader_injection[0][3]
#define RENODX_GAMMA_CORRECTION         shader_injection[1][0]
#define RENODX_TONE_MAP_HUE_SHIFT       shader_injection[1][1]
#define RENODX_TONE_MAP_SATURATION      shader_injection[1][2]
#define CUSTOM_BLOOM                    shader_injection[1][3]
#define RENODX_TONE_MAP_EXPOSURE        shader_injection[2][0]
#define RENODX_TONE_MAP_HIGHLIGHTS      shader_injection[2][1]
#define RENODX_TONE_MAP_SHADOWS         shader_injection[2][2]
#define RENODX_TONE_MAP_CONTRAST        shader_injection[2][3]
#define RENODX_SWAP_CHAIN_OUTPUT_PRESET shader_injection[3][0]
#define RENODX_TONE_MAP_BLOWOUT         shader_injection[3][1]

// The add-on's own draws (Night Vision HDR, EMF HDR, the scene scale): their constants, after the
// game's own (c0-c3), set for those draws only. Laid out as NightVisionConstants, EmfConstants and
// SceneScaleConstants.
float4 draw_settings[3] : register(c8);

#define CUSTOM_NIGHT_VISION_GLOW_SIZES draw_settings[0]    // in texels, xy: s1, zw: s2
#define CUSTOM_EMF_BUMP_MATRIX         draw_settings[0]    // BUMPENVMAT00, 01, 10, 11 of stage 3
#define CUSTOM_VISION_PEAK_RATIO       draw_settings[1].x  // Peak over Game Brightness
#define CUSTOM_VISION_ENCODING         draw_settings[1].y  // the swap chain's decoding
#define CUSTOM_EMF_LEVEL_SIZES         draw_settings[2]    // in texels, per level (square)
#define CUSTOM_SCENE_SCALE             draw_settings[0].x  // Game over UI Brightness
#define CUSTOM_SCENE_ENCODING          draw_settings[0].y  // the swap chain's decoding

#else

#if ((__SHADER_TARGET_MAJOR == 5 && __SHADER_TARGET_MINOR >= 1) || __SHADER_TARGET_MAJOR >= 6)
cbuffer shader_injection : register(b13, space50) {
  ShaderInjectData shader_injection : packoffset(c0);
}
#else
cbuffer shader_injection : register(b13) {
  ShaderInjectData shader_injection : packoffset(c0);
}
#endif

#define RENODX_PEAK_WHITE_NITS          shader_injection.peak_white_nits
#define RENODX_DIFFUSE_WHITE_NITS       shader_injection.diffuse_white_nits
#define RENODX_GRAPHICS_WHITE_NITS      shader_injection.graphics_white_nits
#define RENODX_TONE_MAP_TYPE            shader_injection.tone_map_type
#define RENODX_GAMMA_CORRECTION         shader_injection.gamma_correction
#define RENODX_TONE_MAP_HUE_SHIFT       shader_injection.tone_map_hue_shift
#define RENODX_TONE_MAP_SATURATION      shader_injection.saturation
#define CUSTOM_BLOOM                    shader_injection.bloom
#define RENODX_TONE_MAP_EXPOSURE        shader_injection.tone_map_exposure
#define RENODX_TONE_MAP_HIGHLIGHTS      shader_injection.tone_map_highlights
#define RENODX_TONE_MAP_SHADOWS         shader_injection.tone_map_shadows
#define RENODX_TONE_MAP_CONTRAST        shader_injection.tone_map_contrast
#define RENODX_SWAP_CHAIN_OUTPUT_PRESET shader_injection.swap_chain_output_preset
#define RENODX_TONE_MAP_BLOWOUT         shader_injection.tone_map_blowout

#endif

// Output: the HDR10 and SDR presets set their own encoding; scRGB uses no preset, so SwapChainPass
// keeps its defaults (scRGB, BT.709, no clamp or gamut compression).

#define RENODX_RENO_DRT_TONE_MAP_METHOD renodx::tonemap::renodrt::config::tone_map_method::REINHARD
// The composite leaves the scene at Game Brightness 1 (white at 1.0, as the game drew it), so night
// vision reads it as vanilla did; the scene scale pass brings the finished scene to Game Brightness
// against the HUD's UI Brightness before the first HUD draw.
#define RENODX_INTERMEDIATE_SCALING     1.f

#include "../../shaders/renodx.hlsl"

#endif  // __cplusplus

#endif  // SRC_GAMES_SCCT_SHARED_H_
