#ifndef SRC_GAMES_BATMANAC_SHARED_H_
#define SRC_GAMES_BATMANAC_SHARED_H_

// Must be 32bit aligned
// Should be 4x32
struct ShaderInjectData {
  float peak_white_nits;
  float diffuse_white_nits;
  float graphics_white_nits;
  float color_grade_strength;
  float tone_map_type;
  float tone_map_exposure;
  float tone_map_highlights;
  float tone_map_shadows;
  float tone_map_contrast;
  float tone_map_saturation;
  float gamma_correction;
  float custom_bloom;
  float custom_dof;
  float custom_hue_clip;
  float custom_psychov_compression;
  float custom_dlaa_debug_view;
};

#define CUSTOM_TONE_MAP_TYPE_PSYCHOV30 4.f
// Clear value of the character motion texture where no motion was captured.
#define CHARACTER_MOTION_UNKNOWN 32768.f
// Value of the character motion texture where a tessellated skinned draw is the surface: read the tessellated motion.
#define CHARACTER_MOTION_TESSELLATED -32768.f

#ifndef __cplusplus
#if ((__SHADER_TARGET_MAJOR == 5 && __SHADER_TARGET_MINOR >= 1) || __SHADER_TARGET_MAJOR >= 6)
cbuffer shader_injection : register(b13, space50) {
#elif (__SHADER_TARGET_MAJOR < 5) || ((__SHADER_TARGET_MAJOR == 5) && (__SHADER_TARGET_MINOR < 1))
cbuffer shader_injection : register(b13) {
#endif
  ShaderInjectData shader_injection : packoffset(c0);
}

#define RENODX_TONE_MAP_TYPE        shader_injection.tone_map_type
#define RENODX_PEAK_WHITE_NITS      shader_injection.peak_white_nits
#define RENODX_DIFFUSE_WHITE_NITS   shader_injection.diffuse_white_nits
#define RENODX_GRAPHICS_WHITE_NITS  shader_injection.graphics_white_nits
#define RENODX_GAMMA_CORRECTION     shader_injection.gamma_correction
#define RENODX_TONE_MAP_EXPOSURE    shader_injection.tone_map_exposure
#define RENODX_TONE_MAP_HIGHLIGHTS  shader_injection.tone_map_highlights
#define RENODX_TONE_MAP_SHADOWS     shader_injection.tone_map_shadows
#define RENODX_TONE_MAP_CONTRAST    shader_injection.tone_map_contrast
#define RENODX_TONE_MAP_SATURATION  shader_injection.tone_map_saturation
#define RENODX_COLOR_GRADE_STRENGTH shader_injection.color_grade_strength
#define CUSTOM_BLOOM                shader_injection.custom_bloom
#define CUSTOM_DOF                  shader_injection.custom_dof
#define CUSTOM_HUE_CLIP             shader_injection.custom_hue_clip
#define CUSTOM_PSYCHOV_COMPRESSION  shader_injection.custom_psychov_compression
#define CUSTOM_DLAA_DEBUG_VIEW      shader_injection.custom_dlaa_debug_view

// No peak clamp in the tone map pass (the swap chain proxy scales down colors whose largest channel exceeds Peak
// Brightness); the rest are draw.hlsl's defaults.
#define RENODX_TONE_MAP_CLAMP_PEAK             renodx::color::convert::COLOR_SPACE_NONE
#define RENODX_SWAP_CHAIN_CLAMP_COLOR_SPACE    renodx::color::convert::COLOR_SPACE_BT2020
#define RENODX_SWAP_CHAIN_ENCODING             renodx::draw::ENCODING_PQ
#define RENODX_SWAP_CHAIN_ENCODING_COLOR_SPACE renodx::color::convert::COLOR_SPACE_BT2020

#include "../../shaders/renodx.hlsl"

#endif

#endif  // SRC_GAMES_BATMANAC_SHARED_H_
