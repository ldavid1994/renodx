/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 *
 * RenoDX HDR mod for Tom Clancy's Splinter Cell: Chaos Theory (Steam, D3D9, 32-bit).
 *
 * HDR output goes through RenoDX's DX11 device proxy, which gives the back buffer an RGBA16F
 * clone. The composite (0x83A4F799) is replaced so the scene keeps its values above 1.0 and is
 * display-mapped; the HUD draws onto the clone after it and the swap chain pass shows
 * it at UI Brightness. Copies to and from the back buffer are done on its clone (copy redirect),
 * so the vision passes read the real image.
 */

#define ImTextureID ImU64

#define RENODX_MODS_SWAPCHAIN_VERSION 2

#include <d3d9.h>

#include <deps/imgui/imgui.h>
#include <include/reshade.hpp>

#include <embed/shaders.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include "../../mods/shader.hpp"
#include "../../mods/swapchain.hpp"
#include "../../utils/date.hpp"
#include "../../utils/settings.hpp"
#include "./shared.h"

namespace {

// Replaced game shaders: the composite (0x83A4F799).
renodx::mods::shader::CustomShaders custom_shaders = {
    __ALL_CUSTOM_SHADERS,
};

ShaderInjectData shader_injection;
float night_vision_hdr = 0.f;
float emf_hdr = 0.f;

constexpr float TONE_MAP_VANILLA = 0.f;
constexpr float TONE_MAP_NONE = 1.f;
constexpr float TONE_MAP_RENO_DRT = 3.f;

constexpr float OUTPUT_MODE_SDR = 0.f;
constexpr float OUTPUT_MODE_HDR = 1.f;
constexpr float HDR_FORMAT_HDR10 = 0.f;

// renodx::draw::SWAP_CHAIN_OUTPUT_PRESET_* (src/shaders/draw.hlsl).
constexpr float SWAP_CHAIN_OUTPUT_PRESET_NONE = -1.f;
constexpr float SWAP_CHAIN_OUTPUT_PRESET_SDR = 0.f;
constexpr float SWAP_CHAIN_OUTPUT_PRESET_HDR10 = 1.f;

renodx::utils::settings::Setting* output_mode_setting = nullptr;
renodx::utils::settings::Setting* hdr_format_setting = nullptr;
renodx::utils::settings::Setting* tone_map_type_setting = nullptr;
renodx::utils::settings::Setting* peak_nits_setting = nullptr;
renodx::utils::settings::Setting* game_nits_setting = nullptr;
renodx::utils::settings::Setting* ui_nits_setting = nullptr;
renodx::utils::settings::Setting* gamma_correction_setting = nullptr;

bool IsToneMapped() {
  return shader_injection.tone_map_type > TONE_MAP_VANILLA;
}

// Hue Correction: RenoDRT and PsychoV-30; None does no tone mapping and keeps its colour.
bool UsesHueCorrection() {
  return IsToneMapped() && shader_injection.tone_map_type != TONE_MAP_NONE;
}

bool IsHDR() {
  return output_mode_setting->GetValue() != OUTPUT_MODE_SDR;
}

// Tone Mapper, Peak, Game and UI Brightness and Gamma Correction have no binding: UpdateOutput
// writes them, and overrides them in SDR.
renodx::utils::settings::Settings settings = {
    output_mode_setting = new renodx::utils::settings::Setting{
        .key = "OutputMode",
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = OUTPUT_MODE_HDR,
        .can_reset = false,
        .label = "Output Mode",
        .section = "Display Output",
        .tooltip = "SDR: the game's picture for an SDR display (or HDR off in Windows).\n"
                   "HDR: HDR output in the HDR Format below.\n"
                   "Starts on SDR or HDR to match Windows' HDR setting.",
        .labels = {"SDR", "HDR"},
        .is_global = true,
    },
    hdr_format_setting = new renodx::utils::settings::Setting{
        .key = "HDRFormat",
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .can_reset = false,
        .label = "HDR Format",
        .section = "Display Output",
        .tooltip = "HDR10: 10-bit BT.2020 PQ.\n"
                   "scRGB: 16-bit float BT.709, for capture or overlay tools that need it.\n"
                   "Restart the game to apply.",
        .labels = {"HDR10", "scRGB"},
        .is_global = true,
    },
    tone_map_type_setting = new renodx::utils::settings::Setting{
        .key = "ToneMapType",
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        // Psycho-V: closest to vanilla below paper white and in highlight colour.
        .default_value = 3.f,
        .can_reset = true,
        .label = "Tone Mapper",
        .section = "Tone Mapping",
        .tooltip = "Vanilla: the game's picture, clipped at white as in the original.\n"
                   "None: no highlight roll-off (clips at peak).\n"
                   "RenoDRT: highlights above white are kept and rolled off toward peak.\n"
                   "PsychoV-30: perceptual tone mapper; keeps hue and purity as highlights compress.",
        // Menu index -> type: framework type 2 (ACES) isn't offered.
        .labels = {"Vanilla", "None", "RenoDRT", "PsychoV-30"},
        .is_enabled = &IsHDR,
        .parse = [](float value) {
          if (value >= 3.f) return CUSTOM_TONE_MAP_TYPE_PSYCHO_V;
          if (value >= 2.f) return TONE_MAP_RENO_DRT;
          return value;
        },
    },
    peak_nits_setting = new renodx::utils::settings::Setting{
        .key = "ToneMapPeakNits",
        .default_value = 1000.f,
        .label = "Peak Brightness",
        .section = "Tone Mapping",
        .tooltip = "Sets the value of peak white in nits",
        .min = 48.f,
        .max = 4000.f,
        .is_enabled = &IsToneMapped,
    },
    game_nits_setting = new renodx::utils::settings::Setting{
        .key = "ToneMapGameNits",
        .default_value = 203.f,
        .label = "Game Brightness",
        .section = "Tone Mapping",
        .tooltip = "Sets the value of 100% white in nits",
        .min = 48.f,
        .max = 500.f,
        .is_enabled = &IsHDR,
    },
    ui_nits_setting = new renodx::utils::settings::Setting{
        .key = "ToneMapUINits",
        .default_value = 203.f,
        .label = "UI Brightness",
        .section = "Tone Mapping",
        .tooltip = "Sets the brightness of UI and HUD elements in nits",
        .min = 48.f,
        .max = 500.f,
        .is_enabled = &IsHDR,
    },
    gamma_correction_setting = new renodx::utils::settings::Setting{
        .key = "GammaCorrection",
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 1.f,
        .label = "Gamma Correction",
        .section = "Tone Mapping",
        .tooltip = "How the game's SDR image is decoded.\n"
                   "2.2 matches how the game looks on a typical SDR display.",
        .labels = {"sRGB", "2.2"},
        .is_enabled = &IsHDR,
    },
    new renodx::utils::settings::Setting{
        .key = "ToneMapHueCorrection",
        .binding = &shader_injection.hue_correction,
        .default_value = 100.f,
        .label = "Hue Correction",
        .section = "Tone Mapping",
        .tooltip = "Shifts HDR highlights toward the hue vanilla showed.\n"
                   "Vanilla clipped each colour channel at white, so bright lamps and lights turned\n"
                   "whiter. Only colours that went past white are shifted.\n"
                   "0% = keep the computed colour, 100% = vanilla hue.",
        .max = 100.f,
        .is_enabled = &UsesHueCorrection,
        .parse = [](float value) { return value * 0.01f; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeExposure",
        .binding = &shader_injection.tone_map_exposure,
        .default_value = 1.f,
        .label = "Exposure",
        .section = "Color Grading",
        .tooltip = "Brightness of the 3D scene before tone mapping (not the HUD). 1.00 = vanilla.",
        .max = 2.f,
        .format = "%.2f",
        .is_enabled = &IsToneMapped,
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeHighlights",
        .binding = &shader_injection.tone_map_highlights,
        .default_value = 50.f,
        .label = "Highlights",
        .section = "Color Grading",
        .tooltip = "Brightness of the bright parts of the scene. 50 = vanilla.",
        .max = 100.f,
        .is_enabled = &IsToneMapped,
        .parse = [](float value) { return value * 0.02f; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeShadows",
        .binding = &shader_injection.tone_map_shadows,
        .default_value = 50.f,
        .label = "Shadows",
        .section = "Color Grading",
        .tooltip = "Brightness of the dark parts of the scene. 50 = vanilla.",
        .max = 100.f,
        .is_enabled = &IsToneMapped,
        // 0 = 0, 50 = 1, 100 = CUSTOM_SHADOWS_MAX instead of 2 (see shared.h).
        .parse = [](float value) {
          if (value <= 50.f) return value * 0.02f;
          return std::lerp(1.f, CUSTOM_SHADOWS_MAX, (value - 50.f) * 0.02f);
        },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeContrast",
        .binding = &shader_injection.tone_map_contrast,
        .default_value = 50.f,
        .label = "Contrast",
        .section = "Color Grading",
        .tooltip = "Contrast of the 3D scene. 50 = vanilla.",
        .max = 100.f,
        .is_enabled = &IsToneMapped,
        .parse = [](float value) { return value * 0.02f; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeSaturation",
        .binding = &shader_injection.saturation,
        .default_value = 50.f,
        .label = "Saturation",
        .section = "Color Grading",
        .tooltip = "Colour saturation of the 3D scene (not the HUD).\n"
                   "0 = greyscale, 50 = vanilla, 100 = twice vanilla.",
        .max = 100.f,
        .is_enabled = &IsToneMapped,
        .parse = [](float value) { return value * 0.02f; },
    },
    new renodx::utils::settings::Setting{
        .key = "FxBloom",
        .binding = &shader_injection.bloom,
        .default_value = 50.f,
        .label = "Bloom",
        .section = "Effects",
        .tooltip = "Strength of the game's glow.\n"
                   "0 = off, 50 = vanilla, 100 = twice vanilla.",
        .max = 100.f,
        .parse = [](float value) { return value * 0.02f; },
    },
    new renodx::utils::settings::Setting{
        .key = "FxNightVisionHdr",
        .binding = &night_vision_hdr,
        .value_type = renodx::utils::settings::SettingValueType::BOOLEAN,
        .default_value = 0.f,
        .label = "Night Vision HDR",
        .section = "Effects",
        .tooltip = "Night vision's blown-out areas go past white, raised by the game's own glow,\n"
                   "rolling off toward Peak Brightness. Off = the game's night vision.",
        .is_enabled = &IsHDR,
    },
    new renodx::utils::settings::Setting{
        .key = "FxEmfHdr",
        .binding = &emf_hdr,
        .value_type = renodx::utils::settings::SettingValueType::BOOLEAN,
        .default_value = 0.f,
        .label = "EMF HDR",
        .section = "Effects",
        .tooltip = "EMF vision's glows go past white where the game's own glow adds up past it,\n"
                   "rolling off toward Peak Brightness. Off = the game's EMF vision.",
        .is_enabled = &IsHDR,
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::TEXT,
        .label = "- The game's own brightness and gamma options have no effect; use Game Brightness and Gamma Correction.",
        .section = "About",
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::BUTTON,
        .label = "Reset",
        .section = "About",
        .group = "button-line-1",
        .on_change = []() {
          for (auto* setting : settings) {
            if (setting->key.empty()) continue;
            if (!setting->can_reset) continue;
            renodx::utils::settings::UpdateSetting(setting->key, setting->default_value);
          }
        },
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::BUTTON,
        .label = "Discord",
        .section = "About",
        .group = "button-line-1",
        .tint = 0x5865F2,
        .on_change = []() {
          renodx::utils::platform::LaunchURL("https://discord.gg/", "F6AUTeWJHM");
        },
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::BUTTON,
        .label = "Github",
        .section = "About",
        .group = "button-line-1",
        .on_change = []() {
          renodx::utils::platform::LaunchURL("https://github.com/clshortfuse/renodx");
        },
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::BUTTON,
        .label = "ShortFuse's Ko-Fi",
        .section = "About",
        .group = "button-line-1",
        .tint = 0xFF5A16,
        .on_change = []() {
          renodx::utils::platform::LaunchURL("https://ko-fi.com/", "shortfuse");
        },
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::TEXT,
        .label = "Version: 1.4.2 (" + std::string(renodx::utils::date::ISO_DATE) + ")",
        .section = "About",
    },
};

void OnPresetOff() {
  renodx::utils::settings::UpdateSetting("ToneMapType", TONE_MAP_VANILLA);
  renodx::utils::settings::UpdateSetting("ToneMapPeakNits", 203.f);
  renodx::utils::settings::UpdateSetting("ToneMapGameNits", 203.f);
  renodx::utils::settings::UpdateSetting("ToneMapUINits", 203.f);
  renodx::utils::settings::UpdateSetting("GammaCorrection", 1.f);
  renodx::utils::settings::UpdateSetting("ToneMapHueCorrection", 0.f);
  renodx::utils::settings::UpdateSetting("ColorGradeExposure", 1.f);
  renodx::utils::settings::UpdateSetting("ColorGradeHighlights", 50.f);
  renodx::utils::settings::UpdateSetting("ColorGradeShadows", 50.f);
  renodx::utils::settings::UpdateSetting("ColorGradeContrast", 50.f);
  renodx::utils::settings::UpdateSetting("ColorGradeSaturation", 50.f);
  renodx::utils::settings::UpdateSetting("FxBloom", 50.f);
  renodx::utils::settings::UpdateSetting("FxNightVisionHdr", 0.f);
  renodx::utils::settings::UpdateSetting("FxEmfHdr", 0.f);
}

// Psycho-V settings that put it on vanilla below white (vanilla is the identity there): per Peak /
// Game Brightness ratio, the shadows that match 0.2-10 nits and the highlights that match 50-203
// nits, at Gamma Correction 2.2. Fitted on the GPU with psychotm_test30 on a grey ramp;
// midtones then stay within 0.3-2.6%. Interpolated in log(ratio), clamped at the ends.
struct PsychoMatch {
  float ratio = 1.f;
  float shadows = 1.f;
  float highlights = 1.f;
};
constexpr PsychoMatch PSYCHO_MATCH[] = {
    {.ratio = 1.25f, .shadows = 1.6964f, .highlights = 1.2922f},
    {.ratio = 1.5f, .shadows = 1.6415f, .highlights = 1.2185f},
    {.ratio = 2.f, .shadows = 1.5608f, .highlights = 1.1446f},
    {.ratio = 2.5f, .shadows = 1.5026f, .highlights = 1.1077f},
    {.ratio = 3.f, .shadows = 1.4575f, .highlights = 1.0855f},
    {.ratio = 4.f, .shadows = 1.3908f, .highlights = 1.0603f},
    {.ratio = 5.f, .shadows = 1.3427f, .highlights = 1.0464f},
    {.ratio = 6.f, .shadows = 1.3060f, .highlights = 1.0376f},
    {.ratio = 8.f, .shadows = 1.2526f, .highlights = 1.0271f},
    {.ratio = 10.f, .shadows = 1.2155f, .highlights = 1.0211f},
    {.ratio = 15.f, .shadows = 1.1575f, .highlights = 1.0135f},
    {.ratio = 20.f, .shadows = 1.1238f, .highlights = 1.0098f},
    {.ratio = 30.f, .shadows = 1.0863f, .highlights = 1.0063f},
    {.ratio = 50.f, .shadows = 1.0531f, .highlights = 1.0036f},
};

// Writes what the shaders read from Output Mode, on the HDR Format chosen at startup, and the
// Psycho-V match for the resulting Peak / Game Brightness. Run every game frame, so slider changes,
// presets and Reset all reach the shaders through it.
void UpdateOutput() {
  const bool is_hdr10 = (renodx::mods::swapchain::target_format == reshade::api::format::r10g10b10a2_unorm);
  if (IsHDR()) {
    shader_injection.swap_chain_output_preset = (is_hdr10 ? SWAP_CHAIN_OUTPUT_PRESET_HDR10 : SWAP_CHAIN_OUTPUT_PRESET_NONE);
    shader_injection.tone_map_type = tone_map_type_setting->parse(tone_map_type_setting->GetValue());
    // Peak never below Game Brightness: the tone mappers can't map white to a lower peak.
    shader_injection.peak_white_nits = (std::max)(peak_nits_setting->GetValue(), game_nits_setting->GetValue());
    shader_injection.diffuse_white_nits = game_nits_setting->GetValue();
    shader_injection.graphics_white_nits = ui_nits_setting->GetValue();
    shader_injection.gamma_correction = gamma_correction_setting->GetValue();
  } else {
    // SDR: the Vanilla picture, with white at 1.0 in the sRGB swap chain, or at 80 nits (scRGB 1.0)
    // in the scRGB one, peak at white, and no gamma correction: the display decodes the game's image
    // itself, as in vanilla.
    const float sdr_white_nits = (is_hdr10 ? 1.f : 80.f);
    shader_injection.swap_chain_output_preset = (is_hdr10 ? SWAP_CHAIN_OUTPUT_PRESET_SDR : SWAP_CHAIN_OUTPUT_PRESET_NONE);
    shader_injection.tone_map_type = TONE_MAP_VANILLA;
    shader_injection.peak_white_nits = sdr_white_nits;
    shader_injection.diffuse_white_nits = sdr_white_nits;
    shader_injection.graphics_white_nits = sdr_white_nits;
    shader_injection.gamma_correction = 0.f;
  }

  // The match only depends on the peak the composite passes PsychoV-30. It was fitted at Gamma
  // Correction 2.2, where that peak is pre-corrected (srgb::Decode(ratio^(1 / 2.2))); with sRGB it is
  // the ratio itself, so look up the ratio whose pre-corrected peak equals it.
  float match_ratio = shader_injection.peak_white_nits / (std::max)(shader_injection.diffuse_white_nits, 1.f);
  if (shader_injection.gamma_correction == 0.f) {
    match_ratio = std::pow(1.055f * std::pow(match_ratio, 1.f / 2.4f) - 0.055f, 2.2f);
  }
  const auto* first = std::begin(PSYCHO_MATCH);
  const auto* last = std::end(PSYCHO_MATCH) - 1;
  match_ratio = std::clamp(match_ratio, first->ratio, last->ratio);
  const auto* upper = first + 1;
  while (upper != last && upper->ratio < match_ratio) {
    ++upper;
  }
  const auto* lower = upper - 1;
  const float t = std::log(match_ratio / lower->ratio) / std::log(upper->ratio / lower->ratio);
  shader_injection.psycho_shadows_match = std::lerp(lower->shadows, upper->shadows, t);
  shader_injection.psycho_highlights_match = std::lerp(lower->highlights, upper->highlights, t);
}

// The proxy's DXGI swap chain and the colour space last set on it, kept on Output Mode in OnPresent.
reshade::api::swapchain* proxy_swapchain = nullptr;
reshade::api::color_space proxy_color_space = reshade::api::color_space::unknown;

bool fired_on_init_swapchain = false;

void OnInitSwapchain(reshade::api::swapchain* swapchain, bool resize) {
  if (!renodx::utils::swapchain::IsDXGI(swapchain)) return;
  // A new or resized proxy swap chain starts in the HDR Format's colour space: set it again.
  proxy_color_space = reshade::api::color_space::unknown;
  if (fired_on_init_swapchain) return;

  // Output Mode starts on what Windows shows: SDR when its HDR is off or unsupported. It can still be
  // switched during the session.
  const auto display_info = renodx::utils::swapchain::GetDisplayInfo(swapchain);
  const bool windows_hdr = (display_info.hdr_supported && display_info.hdr_enabled);
  output_mode_setting->Set(windows_hdr ? OUTPUT_MODE_HDR : OUTPUT_MODE_SDR);
  peak_nits_setting->default_value = std::round(display_info.peak_nits);

  fired_on_init_swapchain = true;
}

// The render target the game last bound, kept across presents, for the orphaned clone in OnPresent.
reshade::api::resource_view last_render_target = {0u};

// ---------------------------------------------------------------------------
// Replaced draws: game draws that run with the add-on's own shaders (night vision's image, EMF's
// image and EMF's warp). D3D9 state stays set for later draws, so the game's values of the state a
// replaced draw sets are put back once it has drawn. A state block recorded from that draw's own
// binds holds exactly that state: it captures the game's values before the draw and applies them
// after it. The block holds references to what it captured, game textures in D3DPOOL_DEFAULT among
// them, so it is released before a device reset (OnDestroySwapchain) and recorded again after it.
// ---------------------------------------------------------------------------

struct ReplacedDraw {
  IDirect3DDevice9* device = nullptr;
  IDirect3DVertexShader9* vertex_shader = nullptr;  // null keeps the game's vertex shader
  IDirect3DPixelShader9* pixel_shader = nullptr;
  IDirect3DStateBlock9* game_state = nullptr;
  bool replaced = false;  // the draw runs with the add-on's state, to be put back once it has drawn
};
ReplacedDraw night_vision_draw;
ReplacedDraw emf_image_draw;
ReplacedDraw emf_warp_draw;
constexpr ReplacedDraw* REPLACED_DRAWS[] = {&night_vision_draw, &emf_image_draw, &emf_warp_draw};

void ReleaseReplacedDraw(ReplacedDraw* draw) {
  if (draw->vertex_shader != nullptr) {
    draw->vertex_shader->Release();
  }
  if (draw->pixel_shader != nullptr) {
    draw->pixel_shader->Release();
  }
  if (draw->game_state != nullptr) {
    draw->game_state->Release();
  }
  *draw = {};
}

// Returns whether the draw's shaders exist on this device; an empty vertex shader keeps the game's.
// A failed creation is kept, so it isn't retried every frame on the same device.
bool CreateReplacedDraw(IDirect3DDevice9* native_device, std::span<const std::uint8_t> vertex_shader,
                        std::span<const std::uint8_t> pixel_shader, const char* name, ReplacedDraw* draw) {
  if (draw->device != native_device) {
    ReleaseReplacedDraw(draw);
    draw->device = native_device;
    // The pixel shader is only created once the vertex shader exists (or the game's is kept).
    const bool vertex_shader_ready =
        vertex_shader.empty()
        || SUCCEEDED(native_device->CreateVertexShader(reinterpret_cast<const DWORD*>(vertex_shader.data()),
                                                       &draw->vertex_shader));
    const bool shaders_ready =
        vertex_shader_ready
        && SUCCEEDED(native_device->CreatePixelShader(reinterpret_cast<const DWORD*>(pixel_shader.data()),
                                                      &draw->pixel_shader));
    if (!shaders_ready) {
      reshade::log::message(reshade::log::level::warning, (std::string("[scct] ") + name + ": could not create its shaders").c_str());
      ReleaseReplacedDraw(draw);
      draw->device = native_device;
    }
  }
  return draw->pixel_shader != nullptr;
}

// Sets the add-on's state for the draw with bind, after capturing the game's values of it. On the
// first draw on a device (or after a reset) bind is first recorded into the state block, which only
// stores the calls; it must set the same states on every draw. If recording fails, the draw is left
// to the game's shaders on this device, as when its shaders can't be created.
template <typename Bind>
void ReplaceDraw(IDirect3DDevice9* native_device, const Bind& bind, ReplacedDraw* draw) {
  if (draw->game_state == nullptr) {
    if (FAILED(native_device->BeginStateBlock())) return;
    bind();
    if (FAILED(native_device->EndStateBlock(&draw->game_state))) {
      reshade::log::message(reshade::log::level::warning, "[scct] could not record a replaced draw's state block");
      ReleaseReplacedDraw(draw);
      draw->device = native_device;
      return;
    }
  }
  // Still replaced when the last draw's restore never ran: the device holds the add-on's state, and
  // the block still holds the game's.
  if (!draw->replaced) {
    draw->game_state->Capture();
  }
  bind();
  draw->replaced = true;
}

void RestoreReplacedDraw(ReplacedDraw* draw) {
  if (!draw->replaced) return;
  draw->game_state->Apply();
  draw->replaced = false;
}

// ---------------------------------------------------------------------------
// Night Vision HDR. Night vision is built from the composite's 8-bit copy and clamped at
// every step, so it stays at or below white. With the toggle on, its image draw (0x4ADA0B05) is a
// replaced draw with night_vision_hdr_pixel_shader, which raises what it clipped (see the shader).
// That is a ps_3_0, which only runs with a vs_3_0, so the draw also gets night_vision_vertex_shader,
// a port of the game's. With the toggle off, or if the shaders can't be created, the game's own
// shaders draw it.
// ---------------------------------------------------------------------------

// Night vision's and EMF's ps_3_0 read their constants from c8 (vision_settings in shared.h).
constexpr UINT VISION_FIRST_CONSTANT_REGISTER = 8;
static_assert(sizeof(NightVisionConstants) % (4 * sizeof(float)) == 0,
              "NightVisionConstants must fill whole float4 registers");
constexpr UINT NIGHT_VISION_CONSTANT_REGISTER_COUNT = sizeof(NightVisionConstants) / (4 * sizeof(float));
// The glows the shader upsamples; its cubic upsampling is built on bilinear taps.
constexpr DWORD NIGHT_VISION_GLOW_STAGES[] = {1, 2};

bool OnNightVisionImageDraw(reshade::api::command_list* cmd_list) {
  if (night_vision_hdr == 0.f || !IsHDR()) return true;
  auto* native_device = reinterpret_cast<IDirect3DDevice9*>(cmd_list->get_device()->get_native());
  if (!CreateReplacedDraw(native_device, __night_vision_vertex_shader, __night_vision_hdr_pixel_shader,
                          "Night Vision HDR", &night_vision_draw)) {
    return true;
  }

  NightVisionConstants constants = {
      .glow_sizes = {1.f, 1.f, 1.f, 1.f},
      .peak_ratio = shader_injection.peak_white_nits / (std::max)(shader_injection.diffuse_white_nits, 1.f),
  };
  for (size_t index = 0; index < std::size(NIGHT_VISION_GLOW_STAGES); ++index) {
    IDirect3DBaseTexture9* glow = nullptr;
    if (SUCCEEDED(native_device->GetTexture(NIGHT_VISION_GLOW_STAGES[index], &glow)) && glow != nullptr) {
      D3DSURFACE_DESC glow_desc = {};
      if (glow->GetType() == D3DRTYPE_TEXTURE
          && SUCCEEDED(static_cast<IDirect3DTexture9*>(glow)->GetLevelDesc(0, &glow_desc))) {
        constants.glow_sizes[2 * index] = static_cast<float>(glow_desc.Width);
        constants.glow_sizes[(2 * index) + 1] = static_cast<float>(glow_desc.Height);
      }
      glow->Release();
    }
  }
  ReplaceDraw(
      native_device,
      [&]() {
        for (const DWORD stage : NIGHT_VISION_GLOW_STAGES) {
          native_device->SetSamplerState(stage, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
          native_device->SetSamplerState(stage, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        }
        native_device->SetPixelShaderConstantF(VISION_FIRST_CONSTANT_REGISTER,
                                               reinterpret_cast<const float*>(&constants),
                                               NIGHT_VISION_CONSTANT_REGISTER_COUNT);
        native_device->SetVertexShader(night_vision_draw.vertex_shader);
        native_device->SetPixelShader(night_vision_draw.pixel_shader);
      },
      &night_vision_draw);
  return true;
}

void OnNightVisionImageDrawn(reshade::api::command_list* cmd_list) {
  RestoreReplacedDraw(&night_vision_draw);
}

// ---------------------------------------------------------------------------
// EMF HDR. EMF's image is the game's 8-bit, 256-texel weighted sum of its glow levels
// (0xCDFC48BC), so it clips at white. With the toggle on, the draw that shows it (0x7CE55E3E,
// texbem) is a replaced draw with emf_hdr_pixel_shader and emf_vertex_shader, which sum the levels
// again without the clip and add what went past white. The levels are the textures the sum drew
// from, held with a reference until the next present (or a swap chain or device teardown), so a
// level the game recreates in between is never bound freed.
// ---------------------------------------------------------------------------

static_assert(sizeof(EmfConstants) % (4 * sizeof(float)) == 0,
              "EmfConstants must fill whole float4 registers");
constexpr UINT EMF_CONSTANT_REGISTER_COUNT = sizeof(EmfConstants) / (4 * sizeof(float));
constexpr DWORD EMF_LEVEL_COUNT = 4;
// The game's draw uses stages 0-3; the shader reads the levels from the four after them.
constexpr DWORD EMF_FIRST_LEVEL_STAGE = 4;
constexpr D3DSAMPLERSTATETYPE EMF_LEVEL_SAMPLER_STATES[] = {D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER, D3DSAMP_ADDRESSU,
                                                            D3DSAMP_ADDRESSV};
// Bilinear, for the shader's cubic upsampling, and clamped, as the game samples them.
constexpr DWORD EMF_LEVEL_SAMPLER_VALUES[] = {D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP};

IDirect3DBaseTexture9* emf_levels[EMF_LEVEL_COUNT] = {};
bool emf_image_drawn = false;  // EMF HDR has drawn this frame's image

void ReleaseEmfLevels() {
  for (auto*& level : emf_levels) {
    if (level != nullptr) {
      level->Release();
      level = nullptr;
    }
  }
}

bool OnEmfLevelsSumDraw(reshade::api::command_list* cmd_list) {
  if (emf_hdr == 0.f || !IsHDR()) return true;
  auto* native_device = reinterpret_cast<IDirect3DDevice9*>(cmd_list->get_device()->get_native());
  ReleaseEmfLevels();
  for (DWORD level = 0; level < EMF_LEVEL_COUNT; ++level) {
    // On failure D3D9 leaves the slot null, which OnEmfImageDraw checks.
    native_device->GetTexture(level, &emf_levels[level]);
  }
  return true;
}

bool OnEmfImageDraw(reshade::api::command_list* cmd_list) {
  if (emf_hdr == 0.f || !IsHDR()) return true;
  if (std::ranges::find(emf_levels, nullptr) != std::end(emf_levels)) return true;
  auto* native_device = reinterpret_cast<IDirect3DDevice9*>(cmd_list->get_device()->get_native());
  if (!CreateReplacedDraw(native_device, __emf_vertex_shader, __emf_hdr_pixel_shader, "EMF HDR", &emf_image_draw)) {
    return true;
  }

  EmfConstants constants = {
      .peak_ratio = shader_injection.peak_white_nits / (std::max)(shader_injection.diffuse_white_nits, 1.f),
  };
  constexpr D3DTEXTURESTAGESTATETYPE BUMP_MATRIX_STATES[] = {D3DTSS_BUMPENVMAT00, D3DTSS_BUMPENVMAT01,
                                                             D3DTSS_BUMPENVMAT10, D3DTSS_BUMPENVMAT11};
  for (size_t index = 0; index < std::size(BUMP_MATRIX_STATES); ++index) {
    DWORD entry = 0;
    native_device->GetTextureStageState(3, BUMP_MATRIX_STATES[index], &entry);
    constants.bump_matrix[index] = std::bit_cast<float>(entry);
  }
  for (DWORD level = 0; level < EMF_LEVEL_COUNT; ++level) {
    D3DSURFACE_DESC level_desc = {};
    if (emf_levels[level]->GetType() == D3DRTYPE_TEXTURE
        && SUCCEEDED(static_cast<IDirect3DTexture9*>(emf_levels[level])->GetLevelDesc(0, &level_desc))) {
      constants.level_sizes[level] = static_cast<float>(level_desc.Width);
    }
  }
  ReplaceDraw(
      native_device,
      [&]() {
        for (DWORD level = 0; level < EMF_LEVEL_COUNT; ++level) {
          const DWORD stage = EMF_FIRST_LEVEL_STAGE + level;
          for (size_t index = 0; index < std::size(EMF_LEVEL_SAMPLER_STATES); ++index) {
            native_device->SetSamplerState(stage, EMF_LEVEL_SAMPLER_STATES[index], EMF_LEVEL_SAMPLER_VALUES[index]);
          }
          native_device->SetTexture(stage, emf_levels[level]);
        }
        native_device->SetPixelShaderConstantF(VISION_FIRST_CONSTANT_REGISTER,
                                               reinterpret_cast<const float*>(&constants),
                                               EMF_CONSTANT_REGISTER_COUNT);
        native_device->SetVertexShader(emf_image_draw.vertex_shader);
        native_device->SetPixelShader(emf_image_draw.pixel_shader);
      },
      &emf_image_draw);
  return true;
}

void OnEmfImageDrawn(reshade::api::command_list* cmd_list) {
  if (emf_image_draw.replaced) {
    emf_image_drawn = true;
  }
  RestoreReplacedDraw(&emf_image_draw);
}

// After EMF's image, the game copies the screen to an 8-bit texture and draws it back through a
// grid that warps it (0x2E3398C1), which would clip it at white again. Once EMF HDR has drawn the
// image, that draw is a replaced draw that reads a 16-bit copy of the screen instead, with
// emf_warp_pixel_shader and the game's vertex shader. The copy is in D3DPOOL_DEFAULT, so it is
// released before a device reset (OnDestroySwapchain).
IDirect3DTexture9* emf_warp_screen_copy = nullptr;

void ReleaseEmfWarpCopy() {
  if (emf_warp_screen_copy != nullptr) {
    emf_warp_screen_copy->Release();
    emf_warp_screen_copy = nullptr;
  }
}

bool OnEmfWarpDraw(reshade::api::command_list* cmd_list) {
  if (!emf_image_drawn) return true;
  auto* native_device = reinterpret_cast<IDirect3DDevice9*>(cmd_list->get_device()->get_native());
  if (emf_warp_draw.device != native_device) {
    // The copy belongs to the old device.
    ReleaseEmfWarpCopy();
  }
  if (!CreateReplacedDraw(native_device, {}, __emf_warp_pixel_shader, "EMF HDR warp", &emf_warp_draw)) {
    return true;
  }
  IDirect3DSurface9* render_target = nullptr;
  if (FAILED(native_device->GetRenderTarget(0, &render_target)) || render_target == nullptr) return true;
  D3DSURFACE_DESC target_desc = {};
  render_target->GetDesc(&target_desc);
  // Only the full-screen redraw from the screen's copy; the shader's other draws are left alone.
  IDirect3DBaseTexture9* screen = nullptr;
  native_device->GetTexture(0, &screen);
  D3DSURFACE_DESC screen_desc = {};
  const bool is_screen_redraw = screen != nullptr && screen->GetType() == D3DRTYPE_TEXTURE
                                && SUCCEEDED(static_cast<IDirect3DTexture9*>(screen)->GetLevelDesc(0, &screen_desc))
                                && screen_desc.Width == target_desc.Width && screen_desc.Height == target_desc.Height;
  if (screen != nullptr) {
    screen->Release();
  }
  if (emf_warp_screen_copy != nullptr) {
    D3DSURFACE_DESC copy_desc = {};
    emf_warp_screen_copy->GetLevelDesc(0, &copy_desc);
    if (copy_desc.Width != target_desc.Width || copy_desc.Height != target_desc.Height) {
      ReleaseEmfWarpCopy();
    }
  }
  if (is_screen_redraw && emf_warp_screen_copy == nullptr
      && FAILED(native_device->CreateTexture(target_desc.Width, target_desc.Height, 1, D3DUSAGE_RENDERTARGET,
                                             D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &emf_warp_screen_copy, nullptr))) {
    reshade::log::message(reshade::log::level::warning, "[scct] EMF HDR: could not create the screen copy");
    emf_warp_screen_copy = nullptr;
  }
  IDirect3DSurface9* copy_surface = nullptr;
  if (is_screen_redraw && emf_warp_screen_copy != nullptr) {
    emf_warp_screen_copy->GetSurfaceLevel(0, &copy_surface);
  }
  if (copy_surface == nullptr) {
    render_target->Release();
    return true;
  }
  native_device->StretchRect(render_target, nullptr, copy_surface, nullptr, D3DTEXF_NONE);
  copy_surface->Release();
  render_target->Release();

  ReplaceDraw(
      native_device,
      [&]() {
        native_device->SetTexture(0, emf_warp_screen_copy);
        native_device->SetPixelShader(emf_warp_draw.pixel_shader);
      },
      &emf_warp_draw);
  return true;
}

void OnEmfWarpDrawn(reshade::api::command_list* cmd_list) {
  RestoreReplacedDraw(&emf_warp_draw);
}

void OnDestroySwapchain(reshade::api::swapchain* swapchain, bool resize) {
  if (swapchain->get_device()->get_api() != reshade::api::device_api::d3d9) return;
  ReleaseEmfLevels();
  ReleaseEmfWarpCopy();
  for (auto* draw : REPLACED_DRAWS) {
    if (draw->game_state != nullptr) {
      draw->game_state->Release();
      draw->game_state = nullptr;
    }
    draw->replaced = false;
  }
}

// ---------------------------------------------------------------------------
// Old D3D9 devices. Exiting from gameplay to the main menu, or loading a save, sometimes leaks the
// game's D3D9 device: it is never destroyed, so the proxy stays bound to it and the new device's
// frames never reach the screen (frozen picture). When a D3D9 device starts while an older one is
// still alive, the proxy's own teardown is run for the old one; it binds to the new one at its next
// present. Whatever the old device still holds stays leaked.
// ---------------------------------------------------------------------------

std::vector<reshade::api::device*> live_d3d9_devices;

void OnInitDevice(reshade::api::device* device) {
  if (device->get_api() != reshade::api::device_api::d3d9) return;
  for (auto* old_device : live_d3d9_devices) {
    if (old_device == device) continue;
    if (renodx::utils::device_proxy::proxied_device_reshade == old_device) {
      reshade::log::message(reshade::log::level::warning, "[scct] old D3D9 device still alive: releasing the proxy from it");
      renodx::utils::device_proxy::OnDestroyDevice(old_device);
    }
  }
  std::erase_if(live_d3d9_devices, [device](const reshade::api::device* tracked) { return tracked != device; });
  live_d3d9_devices.push_back(device);
}

void OnDestroyDevice(reshade::api::device* device) {
  if (device->get_api() != reshade::api::device_api::d3d9) return;
  last_render_target = {0u};
  ReleaseEmfLevels();
  auto* native_device = reinterpret_cast<IDirect3DDevice9*>(device->get_native());
  if (emf_warp_draw.device == native_device) {
    ReleaseEmfWarpCopy();
  }
  for (auto* draw : REPLACED_DRAWS) {
    if (draw->device == native_device) {
      ReleaseReplacedDraw(draw);
    }
  }
  std::erase_if(live_d3d9_devices, [device](const reshade::api::device* tracked) { return tracked == device; });
}

// ---------------------------------------------------------------------------
// Copy redirect. With the DX11 proxy, draws to the back buffer go to its RGBA16F clone, but the
// game's StretchRect copies from it (night vision, thermal and EMF copy the finished image to
// process it) would still read the 8-bit original. Copies whose source or destination has an
// active clone are done on the clone.
// ---------------------------------------------------------------------------

bool Copy(reshade::api::command_list* cmd_list,
          reshade::api::resource source, uint32_t source_subresource, const reshade::api::subresource_box* source_box,
          reshade::api::resource dest, uint32_t dest_subresource, const reshade::api::subresource_box* dest_box,
          D3DTEXTUREFILTERTYPE filter) {
  auto* device = cmd_list->get_device();
  if (device->get_api() != reshade::api::device_api::d3d9) return false;

  auto active_clone = [](reshade::api::resource resource) {
    reshade::api::resource clone = {0u};
    if (resource.handle == 0u) return clone;
    renodx::utils::resource::GetResourceInfo(resource, [&](const renodx::utils::resource::ResourceInfo& info) {
      if (info.clone_enabled && info.clone.handle != 0u) {
        clone = info.clone;
      }
    });
    return clone;
  };
  // Returns an AddRef'd surface for a D3D9 surface or texture handle, or nullptr.
  auto get_surface = [](reshade::api::resource resource, uint32_t subresource) -> IDirect3DSurface9* {
    auto* native_resource = reinterpret_cast<IDirect3DResource9*>(resource.handle);
    if (native_resource == nullptr) return nullptr;
    switch (native_resource->GetType()) {
      case D3DRTYPE_SURFACE: {
        auto* surface = static_cast<IDirect3DSurface9*>(native_resource);
        surface->AddRef();
        return surface;
      }
      case D3DRTYPE_TEXTURE: {
        IDirect3DSurface9* surface = nullptr;
        if (FAILED(static_cast<IDirect3DTexture9*>(native_resource)->GetSurfaceLevel(subresource, &surface))) return nullptr;
        return surface;
      }
      default:
        return nullptr;
    }
  };
  auto to_rect = [](const reshade::api::subresource_box& box) {
    return RECT{
        .left = static_cast<LONG>(box.left),
        .top = static_cast<LONG>(box.top),
        .right = static_cast<LONG>(box.right),
        .bottom = static_cast<LONG>(box.bottom),
    };
  };

  const auto source_clone = active_clone(source);
  const auto dest_clone = active_clone(dest);
  if (source_clone.handle == 0u && dest_clone.handle == 0u) return false;

  auto* source_surface = get_surface((source_clone.handle != 0u) ? source_clone : source, source_subresource);
  auto* dest_surface = get_surface((dest_clone.handle != 0u) ? dest_clone : dest, dest_subresource);
  bool handled = false;
  if (source_surface != nullptr && dest_surface != nullptr) {
    const RECT source_rect = ((source_box != nullptr) ? to_rect(*source_box) : RECT{});
    const RECT dest_rect = ((dest_box != nullptr) ? to_rect(*dest_box) : RECT{});
    auto* native_device = reinterpret_cast<IDirect3DDevice9*>(device->get_native());
    handled = SUCCEEDED(native_device->StretchRect(
        source_surface, (source_box != nullptr) ? &source_rect : nullptr,
        dest_surface, (dest_box != nullptr) ? &dest_rect : nullptr,
        filter));
    if (!handled) {
      reshade::log::message(reshade::log::level::warning, "[scct] copy redirect: StretchRect failed");
    }
  }
  if (source_surface != nullptr) {
    source_surface->Release();
  }
  if (dest_surface != nullptr) {
    dest_surface->Release();
  }
  return handled;  // true: the original 8-bit copy is skipped
}

bool OnCopyTextureRegion(reshade::api::command_list* cmd_list, reshade::api::resource source, uint32_t source_subresource,
                         const reshade::api::subresource_box* source_box, reshade::api::resource dest, uint32_t dest_subresource,
                         const reshade::api::subresource_box* dest_box, reshade::api::filter_mode filter) {
  D3DTEXTUREFILTERTYPE d3d_filter = D3DTEXF_NONE;
  switch (filter) {
    case reshade::api::filter_mode::min_mag_mip_point:
      d3d_filter = D3DTEXF_POINT;
      break;
    case reshade::api::filter_mode::min_mag_mip_linear:
    case reshade::api::filter_mode::min_mag_linear_mip_point:
      d3d_filter = D3DTEXF_LINEAR;
      break;
    default:
      break;
  }
  return Copy(cmd_list, source, source_subresource, source_box, dest, dest_subresource, dest_box, d3d_filter);
}

bool OnCopyResource(reshade::api::command_list* cmd_list, reshade::api::resource source, reshade::api::resource dest) {
  return Copy(cmd_list, source, 0, nullptr, dest, 0, nullptr, D3DTEXF_NONE);
}

void OnBindRenderTargets(reshade::api::command_list* cmd_list, uint32_t count,
                         const reshade::api::resource_view* rtvs, reshade::api::resource_view dsv) {
  if (count > 0 && cmd_list->get_device()->get_api() == reshade::api::device_api::d3d9) {
    last_render_target = rtvs[0];
  }
}

void OnPresent(reshade::api::command_queue* queue, reshade::api::swapchain* swapchain, const reshade::api::rect* source_rect,
               const reshade::api::rect* dest_rect, uint32_t dirty_rect_count, const reshade::api::rect* dirty_rects) {
  auto* device = queue->get_device();
  // The DX11 proxy presents too. Its swap chain's colour space follows Output Mode: HDR10 or sRGB
  // for SDR on an HDR10 swap chain; an scRGB swap chain stays scRGB.
  if (device->get_api() != reshade::api::device_api::d3d9) {
    reshade::api::color_space color_space = reshade::api::color_space::extended_srgb_linear;
    if (renodx::mods::swapchain::target_format == reshade::api::format::r10g10b10a2_unorm) {
      color_space = (IsHDR() ? reshade::api::color_space::hdr10_st2084 : reshade::api::color_space::srgb_nonlinear);
    }
    if (swapchain != proxy_swapchain || color_space != proxy_color_space) {
      renodx::utils::swapchain::ChangeColorSpace(swapchain, color_space);
      proxy_swapchain = swapchain;
      proxy_color_space = color_space;
    }
    return;
  }

  ReleaseEmfLevels();
  emf_image_drawn = false;
  UpdateOutput();

  // Orphaned back buffer clone: the game keeps drawing into the proxy's first clone of
  // the back buffer after the proxy replaced it. The full-size RGBA16F target it last bound, if it
  // is not the current clone, is copied into the current one.
  const auto drawn_view = last_render_target;
  if (drawn_view.handle != 0u) {
    const auto back_buffer = swapchain->get_current_back_buffer();
    reshade::api::resource clone = {0u};
    renodx::utils::resource::GetResourceInfo(back_buffer, [&](const renodx::utils::resource::ResourceInfo& info) {
      if (info.clone_enabled) {
        clone = info.clone;
      }
    });
    const auto drawn = device->get_resource_from_view(drawn_view);
    const auto drawn_desc = device->get_resource_desc(drawn);
    const auto back_buffer_desc = device->get_resource_desc(back_buffer);
    const bool orphaned = clone.handle != 0u && drawn.handle != back_buffer.handle && drawn.handle != clone.handle
                          && drawn_desc.texture.format == reshade::api::format::r16g16b16a16_float
                          && drawn_desc.texture.width == back_buffer_desc.texture.width
                          && drawn_desc.texture.height == back_buffer_desc.texture.height;
    auto* clone_resource = reinterpret_cast<IDirect3DResource9*>(clone.handle);
    // A D3D9 render target view is the surface itself; the clone is a texture.
    IDirect3DSurface9* dest_surface = nullptr;
    if (orphaned && clone_resource->GetType() == D3DRTYPE_TEXTURE) {
      static_cast<IDirect3DTexture9*>(clone_resource)->GetSurfaceLevel(0, &dest_surface);
    }
    if (dest_surface != nullptr) {
      auto* native_device = reinterpret_cast<IDirect3DDevice9*>(device->get_native());
      const bool copied = SUCCEEDED(native_device->StretchRect(
          reinterpret_cast<IDirect3DSurface9*>(drawn_view.handle), nullptr, dest_surface, nullptr, D3DTEXF_NONE));
      dest_surface->Release();
      static bool logged = false;
      if (!logged) {
        std::stringstream s;
        s << "[scct] orphaned back buffer clone 0x" << std::hex << drawn.handle << " -> current clone 0x" << clone.handle
          << std::dec << (copied ? ": copied at present" : ": StretchRect failed");
        reshade::log::message(copied ? reshade::log::level::info : reshade::log::level::warning, s.str().c_str());
        logged = true;
      }
    }
  }
}

}  // namespace

extern "C" __declspec(dllexport) constexpr const char* NAME = "RenoDX";
extern "C" __declspec(dllexport) constexpr const char* DESCRIPTION = "RenoDX Splinter Cell: Chaos Theory";

BOOL APIENTRY DllMain(HMODULE h_module, DWORD fdw_reason, LPVOID lpv_reserved) {
  switch (fdw_reason) {
    case DLL_PROCESS_ATTACH:
      if (!reshade::register_addon(h_module)) return FALSE;

      renodx::mods::shader::force_pipeline_cloning = true;
      renodx::mods::shader::expected_constant_buffer_space = 50;
      renodx::mods::shader::expected_constant_buffer_index = 13;
      // ps_3_0 composite: the injection goes in c50-c53 (see shared.h).
      renodx::mods::shader::constant_buffer_offset = 50 * 4;

      renodx::mods::swapchain::expected_constant_buffer_index = 13;
      renodx::mods::swapchain::expected_constant_buffer_space = 50;
      // The proxy cannot use exclusive fullscreen: borderless window sized to the monitor.
      renodx::mods::swapchain::force_borderless = true;
      renodx::mods::swapchain::prevent_full_screen = false;
      renodx::mods::swapchain::force_screen_tearing = false;
      renodx::mods::swapchain::use_resource_cloning = true;
      // The proxy sets the HDR Format's colour space on its swap chain, and OnPresent keeps it on
      // Output Mode; the game's D3D9 swap chain stays SDR.
      renodx::mods::swapchain::set_color_space = false;
      renodx::mods::swapchain::use_device_proxy = true;

      renodx::mods::swapchain::swap_chain_proxy_vertex_shader = __swap_chain_proxy_vertex_shader_dx11;
      renodx::mods::swapchain::swap_chain_proxy_pixel_shader = __swap_chain_proxy_pixel_shader_dx11;

      // No render target upgrades: the proxy clones the back buffer, the scene buffer is
      // already RGBA16F, and the bloom chain and the scene and vision copies stay 8-bit as in vanilla.

      // Not replaced: hooked only, for Night Vision HDR to swap in its own shaders.
      custom_shaders[0x4ADA0B05u] = {
          .crc32 = 0x4ADA0B05u,
          .on_draw = &OnNightVisionImageDraw,
          .on_drawn = &OnNightVisionImageDrawn,
      };
      // Hooked only, for EMF HDR: the sum of EMF's glow levels, the draw that shows it, and the
      // warp that redraws the screen after it.
      custom_shaders[0xCDFC48BCu] = {
          .crc32 = 0xCDFC48BCu,
          .on_draw = &OnEmfLevelsSumDraw,
      };
      custom_shaders[0x7CE55E3Eu] = {
          .crc32 = 0x7CE55E3Eu,
          .on_draw = &OnEmfImageDraw,
          .on_drawn = &OnEmfImageDrawn,
      };
      custom_shaders[0x2E3398C1u] = {
          .crc32 = 0x2E3398C1u,
          .on_draw = &OnEmfWarpDraw,
          .on_drawn = &OnEmfWarpDrawn,
      };

      reshade::register_event<reshade::addon_event::init_swapchain>(OnInitSwapchain);
      reshade::register_event<reshade::addon_event::destroy_swapchain>(OnDestroySwapchain);
      reshade::register_event<reshade::addon_event::init_device>(OnInitDevice);
      reshade::register_event<reshade::addon_event::destroy_device>(OnDestroyDevice);
      reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(OnBindRenderTargets);
      reshade::register_event<reshade::addon_event::copy_texture_region>(OnCopyTextureRegion);
      reshade::register_event<reshade::addon_event::copy_resource>(OnCopyResource);
      reshade::register_event<reshade::addon_event::present>(OnPresent);
      break;
    case DLL_PROCESS_DETACH:
      reshade::unregister_event<reshade::addon_event::present>(OnPresent);
      reshade::unregister_event<reshade::addon_event::copy_resource>(OnCopyResource);
      reshade::unregister_event<reshade::addon_event::copy_texture_region>(OnCopyTextureRegion);
      reshade::unregister_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(OnBindRenderTargets);
      reshade::unregister_event<reshade::addon_event::destroy_device>(OnDestroyDevice);
      reshade::unregister_event<reshade::addon_event::init_device>(OnInitDevice);
      reshade::unregister_event<reshade::addon_event::init_swapchain>(OnInitSwapchain);
      reshade::unregister_event<reshade::addon_event::destroy_swapchain>(OnDestroySwapchain);
      reshade::unregister_addon(h_module);
      break;
  }

  renodx::utils::settings::Use(fdw_reason, &settings, &OnPresetOff);
  if (fdw_reason == DLL_PROCESS_ATTACH) {
    // The proxy's swap chain format is fixed when it is created: apply the saved HDR Format before.
    renodx::mods::swapchain::SetUseHDR10(hdr_format_setting->GetValue() == HDR_FORMAT_HDR10);
    UpdateOutput();
  }
  renodx::mods::swapchain::Use(fdw_reason, &shader_injection);
  renodx::mods::shader::Use(fdw_reason, custom_shaders, &shader_injection);

  return TRUE;
}
