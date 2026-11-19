/*
 * Copyright (C) 2026 RenoDX contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef SRC_GAMES_BATMANAC_UPSCALING_HOST_PROTOCOL_H_
#define SRC_GAMES_BATMANAC_UPSCALING_HOST_PROTOCOL_H_

#include <cstdint>

// Shared between the add-on (32-bit game process) and the 64-bit upscaler host: NVIDIA DLSS only ships for x64.
// Fixed-width fields only, so both bitnesses see the same layout.
namespace batmanac::upscaling {

inline constexpr std::uint32_t PROTOCOL_VERSION = 5u;

enum class HostState : std::uint32_t {
  STARTING = 0u,
  READY = 1u,
  FAILED = 2u,
};

// What evaluates the frame: NVIDIA DLAA on the host's D3D11 device, or AMD FSR on a D3D12 device it makes when asked.
enum class Upscaler : std::uint32_t {
  DLSS = 0u,
  FSR = 1u,
};

enum class SharedTexture : std::uint32_t {
  COLOR = 0u,
  DEPTH = 1u,
  MOTION_VECTORS = 2u,
  BIAS_CURRENT_COLOR_MASK = 3u,
  OUTPUT = 4u,
};
inline constexpr std::uint32_t SHARED_TEXTURE_COUNT = 5u;
inline constexpr const wchar_t* SHARED_TEXTURE_NAMES[SHARED_TEXTURE_COUNT] = {L"color", L"depth", L"motion", L"bias", L"output"};

// The textures are ordered by a shared fence: the add-on signals a request's value (odd) once its inputs are written, the
// host waits for it on its GPU, evaluates and signals the value after it (even), which the add-on's GPU waits for before
// reading the output.
struct SharedBlock {
  std::uint32_t protocol_version;
  // Written by the add-on.
  std::uint32_t adapter_luid_low;
  std::int32_t adapter_luid_high;
  // Bumped when the shared textures are recreated; part of their names.
  std::uint32_t texture_generation;
  std::uint32_t width;
  std::uint32_t height;
  // Bumped once per frame to evaluate.
  std::uint32_t frame;
  // Where this frame's jitter moved the image, in pixels (x right, y down).
  float jitter_x;
  float jitter_y;
  float motion_vector_scale_x;
  float motion_vector_scale_y;
  std::uint32_t reset;
  std::uint32_t preset;
  // Set with each texture generation.
  Upscaler upscaler;
  // FSR's camera: the near plane in view units (the depth has an infinite far plane) and the vertical field of view in
  // radians.
  float camera_near;
  float camera_vertical_fov;
  // FSR's RCAS sharpening pass: 1 on, 0 off; its strength from 0 (lightest) to 1.
  std::uint32_t sharpening;
  float sharpness;
  // The fence's handle in the host's process (duplicated there with each texture generation).
  std::uint64_t fence_handle;
  // The value the add-on signalled for this frame.
  std::uint64_t request_value;
  // Written by the host.
  HostState host_state;
  std::int32_t host_error;
  std::uint32_t completed_frame;
  // 1 while DLSS can run, set before the host is ready; 0 once it cannot (no DLSS on this GPU or driver, or its feature
  // failed): DLSS requests are then answered unevaluated and the add-on switches to FSR.
  std::uint32_t dlss_available;
  // The value the host signalled after this frame's work, failed or not.
  std::uint64_t done_value;
  // What evaluates the current texture generation and its version, null-terminated; set before its first answer.
  char upscaler_status[64];
};

// Kernel object names; the game's process id and, for textures, the generation complete them.
inline constexpr const wchar_t* SHARED_BLOCK_NAME = L"Local\\batmanac_upscaling_block_";
inline constexpr const wchar_t* REQUEST_EVENT_NAME = L"Local\\batmanac_upscaling_request_";
inline constexpr const wchar_t* DONE_EVENT_NAME = L"Local\\batmanac_upscaling_done_";
inline constexpr const wchar_t* SHARED_FENCE_NAME_PREFIX = L"Local\\batmanac_upscaling_fence_";
inline constexpr const wchar_t* SHARED_TEXTURE_NAME_PREFIX = L"Local\\batmanac_upscaling_";
inline constexpr const wchar_t* HOST_EXECUTABLE = L"renodx-batmanac-dlaa-host64.exe";

}  // namespace batmanac::upscaling

#endif  // SRC_GAMES_BATMANAC_UPSCALING_HOST_PROTOCOL_H_
