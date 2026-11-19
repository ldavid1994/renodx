/*
 * Copyright (C) 2024 Carlos Lopez
 * SPDX-License-Identifier: MIT
 */

#define ImTextureID ImU64

#define DEBUG_LEVEL_0

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string>

#include <deps/imgui/imgui.h>
#include <include/reshade.hpp>

#include <embed/shaders.h>

#include "../../mods/shader.hpp"
#include "../../mods/swapchain.hpp"
#include "../../utils/data.hpp"
#include "../../utils/date.hpp"
#include "../../utils/platform.hpp"
#include "../../utils/resource.hpp"
#include "../../utils/resource_upgrade.hpp"
#include "../../utils/settings.hpp"
#include "../../utils/swapchain.hpp"
#include "./shared.h"
#include "./upscaling/upscaling.hpp"

namespace {

renodx::mods::shader::CustomShaders custom_shaders = {
    __ALL_CUSTOM_SHADERS,
};

ShaderInjectData shader_injection;

float current_settings_mode = 0;

// renodx::draw::TONE_MAP_TYPE_VANILLA, which has no C++ counterpart.
constexpr float TONE_MAP_TYPE_VANILLA = 0.f;

bool IsToneMapped() { return shader_injection.tone_map_type != TONE_MAP_TYPE_VANILLA; }

// Hue Clip and Compression apply to PsychoV-30 only.
bool IsPsychoV() { return shader_injection.tone_map_type == CUSTOM_TONE_MAP_TYPE_PSYCHOV30; }

auto* peak_nits_setting = new renodx::utils::settings::Setting{
    .key = "ToneMapPeakNits",
    .binding = &shader_injection.peak_white_nits,
    .default_value = 1000.f,
    .label = "Peak Brightness",
    .section = "Tone Mapping",
    .tooltip =
        "Sets the value of peak white in nits.\n"
        "Defaults to the display's peak when Windows HDR is on.",
    .min = 48.f,
    .max = 4000.f,
};

// UE3 needs a borderless flip-model swap chain for HDR. Global settings, applied in DllMain
// before the game creates its swap chain.
auto* force_borderless_setting = new renodx::utils::settings::Setting{
    .key = "SwapChainForceBorderless",
    .value_type = renodx::utils::settings::SettingValueType::INTEGER,
    .default_value = 1.f,
    .label = "Force Borderless",
    .section = "Display Output",
    .tooltip = "Forces fullscreen to be borderless for proper HDR",
    .labels = {"Disabled", "Enabled"},
    .on_change_value = [](float previous, float current) { renodx::mods::swapchain::force_borderless = (current == 1.f); },
    .is_global = true,
    .is_visible = []() { return current_settings_mode >= 1; },
};

auto* prevent_full_screen_setting = new renodx::utils::settings::Setting{
    .key = "SwapChainPreventFullscreen",
    .value_type = renodx::utils::settings::SettingValueType::INTEGER,
    .default_value = 1.f,
    .label = "Prevent Fullscreen",
    .section = "Display Output",
    .tooltip = "Prevent exclusive fullscreen for proper HDR",
    .labels = {"Disabled", "Enabled"},
    .on_change_value = [](float previous, float current) { renodx::mods::swapchain::prevent_full_screen = (current == 1.f); },
    .is_global = true,
    .is_visible = []() { return current_settings_mode >= 1; },
};

renodx::utils::settings::Settings settings = {
    new renodx::utils::settings::Setting{
        .key = "SettingsMode",
        .binding = &current_settings_mode,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 0.f,
        .can_reset = false,
        .label = "Settings Mode",
        .labels = {"Simple", "Advanced"},
        .is_global = true,
    },
    new renodx::utils::settings::Setting{
        .key = "ToneMapType",
        .binding = &shader_injection.tone_map_type,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 2.f,
        .can_reset = true,
        .label = "Tone Mapper",
        .section = "Tone Mapping",
        .tooltip = "Sets the tone mapper type",
        // Menu index -> type: Vanilla and None are renodx::draw's; PsychoV-30 is the mod's own type 4.
        .labels = {"Vanilla", "None", "PsychoV-30"},
        .parse = [](float value) {
          if (value >= 2.f) return CUSTOM_TONE_MAP_TYPE_PSYCHOV30;
          return value;
        },
    },
    peak_nits_setting,
    new renodx::utils::settings::Setting{
        .key = "ToneMapGameNits",
        .binding = &shader_injection.diffuse_white_nits,
        .default_value = 203.f,
        .label = "Game Brightness",
        .section = "Tone Mapping",
        .tooltip = "Sets the value of 100% white in nits",
        .min = 48.f,
        .max = 500.f,
    },
    new renodx::utils::settings::Setting{
        .key = "ToneMapUINits",
        .binding = &shader_injection.graphics_white_nits,
        .default_value = 203.f,
        .label = "UI Brightness",
        .section = "Tone Mapping",
        .tooltip = "Sets the brightness of UI and HUD elements in nits",
        .min = 48.f,
        .max = 500.f,
    },
    new renodx::utils::settings::Setting{
        .key = "GammaCorrection",
        .binding = &shader_injection.gamma_correction,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 1.f,
        .label = "Gamma Correction",
        .section = "Tone Mapping",
        .tooltip = "Emulates a display EOTF.",
        .labels = {"Off", "2.2"},
    },
    new renodx::utils::settings::Setting{
        .key = "FxHueClip",
        .binding = &shader_injection.custom_hue_clip,
        .default_value = 50.f,
        .label = "Hue Clip",
        .section = "Tone Mapping",
        .tooltip = "Emulates the vanilla hue clip effect.",
        .max = 100.f,
        .parse = [](float value) { return value * 0.01f; },
        .is_visible = &IsPsychoV,
    },
    new renodx::utils::settings::Setting{
        .key = "PsychoVCompression",
        .binding = &shader_injection.custom_psychov_compression,
        .default_value = 2.f,
        .label = "Compression",
        .section = "Tone Mapping",
        .tooltip = "Controls PsychoV-30's finite-response compression power.\n"
                   "0 selects automatic compression; positive values select a manual power.",
        .max = 2.f,
        .format = "%.2f",
        .is_visible = &IsPsychoV,
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeExposure",
        .binding = &shader_injection.tone_map_exposure,
        .default_value = 1.f,
        .label = "Exposure",
        .section = "Color Grading",
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
        .max = 100.f,
        .is_enabled = &IsToneMapped,
        .parse = [](float value) { return value * 0.02f; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeContrast",
        .binding = &shader_injection.tone_map_contrast,
        .default_value = 50.f,
        .label = "Contrast",
        .section = "Color Grading",
        .max = 100.f,
        .is_enabled = &IsToneMapped,
        .parse = [](float value) { return value * 0.02f; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeSaturation",
        .binding = &shader_injection.tone_map_saturation,
        .default_value = 50.f,
        .label = "Saturation",
        .section = "Color Grading",
        .max = 100.f,
        .is_enabled = &IsToneMapped,
        .parse = [](float value) { return value * 0.02f; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeScene",
        .binding = &shader_injection.color_grade_strength,
        .default_value = 100.f,
        .label = "Scene Grading",
        .section = "Color Grading",
        .tooltip = "Scene grading as applied by the game",
        .max = 100.f,
        .is_enabled = &IsToneMapped,
        .parse = [](float value) { return value * 0.01f; },
    },
    new renodx::utils::settings::Setting{
        .key = "FxBloom",
        .binding = &shader_injection.custom_bloom,
        .default_value = 50.f,
        .label = "Bloom",
        .section = "Effects",
        .tooltip = "Controls the strength of the game's bloom.\n"
                   "0 disables bloom; 50 preserves the vanilla strength.",
        .max = 100.f,
        .parse = [](float value) { return value * 0.02f; },
    },
    new renodx::utils::settings::Setting{
        .key = "FxDoF",
        .binding = &shader_injection.custom_dof,
        .default_value = 50.f,
        .label = "Depth of Field",
        .section = "Effects",
        .tooltip = "Controls the strength of the game's depth of field.\n"
                   "0 disables depth of field; 50 preserves the vanilla strength.",
        .max = 100.f,
        .parse = [](float value) { return value * 0.02f; },
    },
    force_borderless_setting,
    prevent_full_screen_setting,
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::BUTTON,
        .label = "Reset All",
        .section = "Options",
        .group = "button-line-1",
        .on_change = []() { renodx::utils::settings::ResetSettings(); },
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::BUTTON,
        .label = "RenoDX Discord",
        .section = "Links",
        .group = "button-line-2",
        .tint = 0x5865F2,
        .on_change = []() { renodx::utils::platform::LaunchURL("https://discord.gg/", "Ce9bQHQrSV"); },
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::BUTTON,
        .label = "HDR Den Discord",
        .section = "Links",
        .group = "button-line-2",
        .tint = 0x5865F2,
        .on_change = []() { renodx::utils::platform::LaunchURL("https://discord.gg/", "5WZXDpmbpP"); },
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::BUTTON,
        .label = "More Mods",
        .section = "Links",
        .group = "button-line-2",
        .tint = 0x2B3137,
        .on_change = []() { renodx::utils::platform::LaunchURL("https://github.com/clshortfuse/renodx/wiki/Mods"); },
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::BUTTON,
        .label = "Github",
        .section = "Links",
        .group = "button-line-2",
        .tint = 0x2B3137,
        .on_change = []() { renodx::utils::platform::LaunchURL("https://github.com/clshortfuse/renodx"); },
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::BUTTON,
        .label = "ShortFuse's Ko-Fi",
        .section = "Links",
        .group = "button-line-3",
        .tint = 0xFF5A16,
        .on_change = []() { renodx::utils::platform::LaunchURL("https://ko-fi.com/shortfuse"); },
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::BUTTON,
        .label = "Musa's Ko-Fi",
        .section = "Links",
        .group = "button-line-3",
        .tint = 0xFF5A16,
        .on_change = []() { renodx::utils::platform::LaunchURL("https://ko-fi.com/musaqh"); },
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::TEXT,
        .label = "- PsychoV-30 tone mapper by ShortFuse\n"
                 "- Special thanks to ShortFuse and Musa",
        .section = "About",
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::TEXT,
        .label = std::string("Build: ") + renodx::utils::date::ISO_DATE_TIME,
        .section = "About",
    },
};

void OnPresetOff() {
  renodx::utils::settings::UpdateSettings({
      {"ToneMapType", 0.f},
      {"ToneMapPeakNits", 203.f},
      {"ToneMapGameNits", 203.f},
      {"ToneMapUINits", 203.f},
      {"GammaCorrection", 1.f},
      {"ColorGradeExposure", 1.f},
      {"ColorGradeHighlights", 50.f},
      {"ColorGradeShadows", 50.f},
      {"ColorGradeContrast", 50.f},
      {"ColorGradeSaturation", 50.f},
      {"FxHueClip", 50.f},
      {"PsychoVCompression", 2.f},
      {"ColorGradeScene", 100.f},
      {"FxBloom", 50.f},
      {"FxDoF", 50.f},
  });
}

constexpr const char* DOF_BLUR_UPGRADE_NAME = "DofBlur";
constexpr const char* UE3_GAME_WINDOW_CLASS = "LaunchUnrealUWindowsClient";

bool fired_on_init_swapchain = false;

void OnInitSwapchain(reshade::api::swapchain* swapchain, bool resize) {
  // The DOF blur targets are a quarter of the game window's back buffer plus 2, recreated on resize.
  auto* device = swapchain->get_device();
  const bool is_game_window =
      (renodx::utils::platform::GetWindowClassName(static_cast<HWND>(swapchain->get_hwnd())) == UE3_GAME_WINDOW_CLASS);
  if (auto* data = renodx::utils::data::Get<renodx::utils::resource::upgrade::DeviceData>(device);
      is_game_window && data != nullptr) {
    const auto back_buffer_desc = device->get_resource_desc(swapchain->get_current_back_buffer());
    const std::unique_lock lock(data->mutex);
    for (auto& upgrade_info : data->upgrade_infos) {
      if (upgrade_info.name != DOF_BLUR_UPGRADE_NAME) continue;
      upgrade_info.dimensions = {
          .width = static_cast<int16_t>((back_buffer_desc.texture.width / 4) + 2),
          .height = static_cast<int16_t>((back_buffer_desc.texture.height / 4) + 2),
          .depth = renodx::utils::resource::ResourceUpgradeInfo::ANY,
      };
    }
  }

  // Peak Brightness defaults to the display's peak, read once from the first DXGI swap chain. A Peak still at the
  // old default follows it, so a fresh install starts on the display.
  if (fired_on_init_swapchain) return;
  if (!renodx::utils::swapchain::IsDXGI(swapchain)) return;

  const float display_peak = renodx::utils::swapchain::GetPeakNits(swapchain).value_or(1000.f);
  const bool peak_was_default = (peak_nits_setting->GetValue() == peak_nits_setting->default_value);
  peak_nits_setting->default_value =
      std::clamp(std::round(display_peak), peak_nits_setting->min, peak_nits_setting->max);
  if (peak_was_default) {
    renodx::utils::settings::UpdateSetting(peak_nits_setting->key, peak_nits_setting->default_value);
  }

  fired_on_init_swapchain = true;
}

// UE3 creates its game window with WS_EX_TOOLWINDOW and clears the style only after CreateSwapChain, so
// mods::swapchain would skip the game's one swap chain as a tool window. Clearing it
// first does what the game does moments later. Registered before mods::swapchain so it runs first.
// Remove when mods::swapchain stops rejecting this window.
bool OnCreateSwapchain(reshade::api::device_api api, reshade::api::swapchain_desc& desc, void* hwnd) {
  auto* window = static_cast<HWND>(hwnd);
  if (!renodx::utils::platform::IsToolWindow(window)) return false;
  if (renodx::utils::platform::GetWindowClassName(window) != UE3_GAME_WINDOW_CLASS) return false;

  SetWindowLongPtr(window, GWL_EXSTYLE, GetWindowLongPtr(window, GWL_EXSTYLE) & ~WS_EX_TOOLWINDOW);
  reshade::log::message(reshade::log::level::info, "batmanac: cleared WS_EX_TOOLWINDOW on the UE3 game window before CreateSwapChain");
  return false;
}

bool initialized = false;

}  // namespace

extern "C" __declspec(dllexport) constexpr const char* NAME = "RenoDX";
extern "C" __declspec(dllexport) constexpr const char* DESCRIPTION = "RenoDX for Batman: Arkham City";

BOOL APIENTRY DllMain(HMODULE h_module, DWORD fdw_reason, LPVOID lpv_reserved) {
  switch (fdw_reason) {
    case DLL_PROCESS_ATTACH:
      if (!reshade::register_addon(h_module)) return FALSE;
      reshade::register_event<reshade::addon_event::create_swapchain>(OnCreateSwapchain);

      if (!initialized) {
        renodx::mods::shader::force_pipeline_cloning = true;
        renodx::mods::shader::expected_constant_buffer_space = 50;
        renodx::mods::shader::expected_constant_buffer_index = 13;
        renodx::mods::shader::allow_multiple_push_constants = true;

        renodx::mods::swapchain::expected_constant_buffer_index = 13;
        renodx::mods::swapchain::expected_constant_buffer_space = 50;
        renodx::mods::swapchain::use_resource_cloning = true;
        renodx::mods::swapchain::SetUseHDR10(true);
        renodx::mods::swapchain::swap_chain_proxy_shaders = {
            {
                reshade::api::device_api::d3d11,
                {
                    .vertex_shader = __swap_chain_proxy_vertex_shader_dx11,
                    .pixel_shader = __swap_chain_proxy_pixel_shader_dx11,
                },
            },
        };
        // The DOF blur targets store the scene / 4 in UNORM, clipping blurred lights at 4 linear.
        // Their size is set from the game window's back buffer in OnInitSwapchain; until then only a 1x1 target of that
        // format matches.
        renodx::mods::swapchain::resource_upgrade_infos.push_back({
            .old_format = reshade::api::format::r16g16b16a16_unorm,
            .new_format = reshade::api::format::r16g16b16a16_float,
            .dimensions = {.width = 1, .height = 1, .depth = renodx::utils::resource::ResourceUpgradeInfo::ANY},
            .usage_include = reshade::api::resource_usage::render_target,
            .name = DOF_BLUR_UPGRADE_NAME,
        });
        // The LUT builder's 256x16 strip: UNORM clamped the grade at 1 and stored it in 8 bits.
        renodx::mods::swapchain::resource_upgrade_infos.push_back({
            .old_format = reshade::api::format::r8g8b8a8_unorm,
            .new_format = reshade::api::format::r16g16b16a16_float,
            .dimensions = {.width = 256, .height = 16, .depth = renodx::utils::resource::ResourceUpgradeInfo::ANY},
            .usage_include = reshade::api::resource_usage::render_target,
        });
        // FXAA's input, written by the tonemap pass instead of the back buffer; FXAA only blends it, so HDR survives.
        // With MSAA the game allocates a multisampled twin that resolves into it (both unused); unequal formats crash.
        renodx::mods::swapchain::resource_upgrade_infos.push_back({
            .old_format = reshade::api::format::r8g8b8a8_unorm,
            .new_format = reshade::api::format::r16g16b16a16_float,
            .dimensions = {.width = renodx::utils::resource::ResourceUpgradeInfo::BACK_BUFFER,
                           .height = renodx::utils::resource::ResourceUpgradeInfo::BACK_BUFFER},
            .usage_include = reshade::api::resource_usage::render_target,
        });

        force_borderless_setting->ReadFromConfig(renodx::utils::settings::global_name);
        renodx::mods::swapchain::force_borderless = (force_borderless_setting->GetValue() == 1.f);
        prevent_full_screen_setting->ReadFromConfig(renodx::utils::settings::global_name);
        renodx::mods::swapchain::prevent_full_screen = (prevent_full_screen_setting->GetValue() == 1.f);

        settings.push_back(new renodx::utils::settings::Setting{
            .key = batmanac::upscaling::AA_SETTING_KEY,
            .binding = &batmanac::upscaling::aa_enabled,
            .value_type = renodx::utils::settings::SettingValueType::INTEGER,
            .default_value = 0.f,
            .label = "Anti-Aliasing",
            .section = "Anti-Aliasing",
            .tooltip =
                "Temporal anti-aliasing at native resolution, by the method below. Needs anti-aliasing off in the game's\n"
                "launcher (no MSAA, no FXAA). With MSAA on, or if it cannot start, it turns itself off and the game's image\n"
                "stays as it is.",
            .labels = {"Off", "On"},
            // Greyed out after a failure: the choice stays as saved, and the status line below says why it is off.
            .is_enabled = []() { return !batmanac::upscaling::host_link.failed; },
            // Every write (overlay, preset, Reset All) keeps anti-aliasing off while a failure stands; the saved choice stays.
            .parse = [](float value) { return (batmanac::upscaling::host_link.failed ? 0.f : value); },
        });
        settings.push_back(new renodx::utils::settings::Setting{
            .key = "DLAAMethod",
            .binding = &batmanac::upscaling::upscaler,
            .value_type = renodx::utils::settings::SettingValueType::INTEGER,
            .default_value = 0.f,
            .label = "Upscaling Method",
            .section = "Anti-Aliasing",
            .tooltip =
                "DLAA: NVIDIA DLAA (an NVIDIA RTX GPU and nvngx_dlss.dll next to the add-on).\n"
                "FSR: AMD FSR at native resolution (FSR 4 on GPUs that support it, FSR 3.1 elsewhere).\n"
                "Needs amd_fidelityfx_loader_dx12.dll and amd_fidelityfx_upscaler_dx12.dll next to the helper.",
            .labels = {"DLAA", "FSR"},
            .is_visible = []() { return batmanac::upscaling::aa_enabled != 0.f && batmanac::upscaling::dlss_available; },
        });
        // Where DLAA cannot run, the method is FSR and shown without a choice.
        settings.push_back(new renodx::utils::settings::Setting{
            .value_type = renodx::utils::settings::SettingValueType::LABEL,
            .label = "Upscaling Method",
            .section = "Anti-Aliasing",
            .tooltip = "DLAA needs an NVIDIA RTX GPU and nvngx_dlss.dll next to the add-on; ReShade.log says why it is not offered.",
            .labels = {"FSR"},
            .is_visible = []() { return batmanac::upscaling::aa_enabled != 0.f && !batmanac::upscaling::dlss_available; },
        });
        settings.push_back(new renodx::utils::settings::Setting{
            .key = "FSRSharpening",
            .binding = &batmanac::upscaling::fsr_sharpening,
            .value_type = renodx::utils::settings::SettingValueType::INTEGER,
            .default_value = 1.f,
            .label = "FSR Sharpening",
            .section = "Anti-Aliasing",
            .tooltip = "FSR's own sharpening pass (RCAS).",
            .labels = {"Off", "On"},
            .is_visible = []() { return batmanac::upscaling::aa_enabled != 0.f && (batmanac::upscaling::upscaler == 1.f || !batmanac::upscaling::dlss_available); },
        });
        settings.push_back(new renodx::utils::settings::Setting{
            .key = "FSRSharpness",
            .binding = &batmanac::upscaling::fsr_sharpness,
            .default_value = 80.f,
            .label = "FSR Sharpness",
            .section = "Anti-Aliasing",
            .tooltip = "Strength of FSR's sharpening, from its lightest (0) to its strongest (100).",
            .max = 100.f,
            .parse = [](float value) { return value * 0.01f; },
            .is_visible = []() { return batmanac::upscaling::aa_enabled != 0.f && (batmanac::upscaling::upscaler == 1.f || !batmanac::upscaling::dlss_available) && batmanac::upscaling::fsr_sharpening != 0.f; },
        });
        settings.push_back(new renodx::utils::settings::Setting{
            .key = "DLAAPreset",
            .binding = &batmanac::upscaling::dlss_preset,
            .value_type = renodx::utils::settings::SettingValueType::INTEGER,
            .default_value = 2.f,
            .label = "DLAA Preset",
            .section = "Anti-Aliasing",
            .tooltip =
                "K: steadiest on railings and grates, but falling snow and animated lights fade.\n"
                "L: keeps snow and animated lights, with more shimmer on thin geometry.",
            .labels = {"Default", "J", "K", "L", "M"},
            // Menu index -> NVSDK_NGX_DLSS_Hint_Render_Preset: J is 10, then one per letter.
            .parse = [](float value) { return (value == 0.f ? 0.f : value + 9.f); },
            .is_visible = []() { return batmanac::upscaling::aa_enabled != 0.f && batmanac::upscaling::upscaler == 0.f && batmanac::upscaling::dlss_available; },
        });
        settings.push_back(new renodx::utils::settings::Setting{
            .value_type = renodx::utils::settings::SettingValueType::CUSTOM,
            .label = "Upscaler Status",
            .section = "Anti-Aliasing",
            .on_draw = []() {
              std::string status;
              {
                const std::shared_lock lock(batmanac::upscaling::upscaler_status.mutex);
                status = batmanac::upscaling::upscaler_status.text;
              }
              ImGui::TextDisabled("%s", (status.empty() ? "Starting" : status.c_str()));
              return false; },
            // Also after a failure, which turns anti-aliasing off and names the reason.
            .is_visible = []() { return batmanac::upscaling::aa_enabled != 0.f || batmanac::upscaling::host_link.failed; },
        });
#ifndef NDEBUG
        settings.push_back(new renodx::utils::settings::Setting{
            .key = "DLAADebugView",
            .binding = &shader_injection.custom_dlaa_debug_view,
            .value_type = renodx::utils::settings::SettingValueType::INTEGER,
            .default_value = 0.f,
            .label = "DLAA Debug View",
            .section = "Anti-Aliasing",
            .labels = {"Off", "Motion Vectors", "Character Motion"},
            .is_visible = []() { return batmanac::upscaling::aa_enabled != 0.f; },
        });
#endif

        initialized = true;
      }

      reshade::register_event<reshade::addon_event::init_swapchain>(OnInitSwapchain);
      break;
    case DLL_PROCESS_DETACH:
      reshade::unregister_event<reshade::addon_event::init_swapchain>(OnInitSwapchain);
      reshade::unregister_event<reshade::addon_event::create_swapchain>(OnCreateSwapchain);
      reshade::unregister_addon(h_module);
      break;
  }

  renodx::utils::settings::Use(fdw_reason, &settings, &OnPresetOff);
  renodx::mods::swapchain::Use(fdw_reason, &shader_injection);
  renodx::mods::shader::Use(fdw_reason, custom_shaders, &shader_injection);
  batmanac::upscaling::Use(fdw_reason, &shader_injection);

  return TRUE;
}
