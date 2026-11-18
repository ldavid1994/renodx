/*
 * Copyright (C) 2026 RenoDX contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <d3d11.h>
#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <format>
#include <mutex>
#include <new>
#include <shared_mutex>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <include/reshade.hpp>

#include "../shared.h"

// State the DLAA passes share: the views the game thread queues for the render thread and their pairing, each
// moving draw's captured data, the frame's motion batches and the settings.
namespace batmanac::dlaa {

// UE3 FMatrix: row-major, row vectors (clip = position * matrix).
struct Matrix4 {
  std::array<float, 16> m = {};
};

struct ViewMatrices {
  Matrix4 view_matrix = {};
  Matrix4 projection_matrix = {};
  // Where the jitter moved this view's image, in pixels (x right, y down).
  std::array<float, 2> jitter_pixels = {};
};

// The game thread builds views ahead of the render thread, so they queue up in CalcSceneView; ViewPairing tells which
// queued view the frame being drawn used.
struct ViewQueue {
  std::shared_mutex mutex;
  std::deque<ViewMatrices> pending_views;
  std::uint32_t jitter_index = 0u;
  float render_width = 0.f;
  float render_height = 0.f;
};

struct ComputeTarget {
  reshade::api::resource resource = {0};
  reshade::api::resource_view srv = {0};
  reshade::api::resource_view uav = {0};
};

constexpr UINT VERTEX_CONSTANT_SLOTS = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;
constexpr UINT CAPTURED_VERTEX_BUFFER_SLOTS = 4u;

// What a moving draw keeps for its motion. A tessellated draw keeps copies of its vertex shader constant buffers (bones,
// object transform, view) and dynamic vertex buffers from its last two frames, to replay last frame's shape; any other
// draw streams its clip positions into one of two buffers each frame, so last frame's stay in the other.
struct CapturedConstants {
  std::array<std::array<ID3D11Buffer*, VERTEX_CONSTANT_SLOTS>, 2> buffers = {};
  std::array<std::array<ID3D11Buffer*, CAPTURED_VERTEX_BUFFER_SLOTS>, 2> vertex_buffers = {};
  // The offsets the copied vertex buffers were bound at: a ring buffer moves the mesh between frames.
  std::array<std::array<UINT, CAPTURED_VERTEX_BUFFER_SLOTS>, 2> vertex_offsets = {};
  // A tessellated draw's domain shader view buffer (b5), which projects its world positions.
  std::array<ID3D11Buffer*, 2> domain_views = {};
  std::uint32_t latest = 0u;
  std::array<ID3D11Buffer*, 2> positions = {};
  // The buffer that holds the newest positions.
  std::uint32_t latest_positions = 0u;
  // Last frame the draw was seen, last frame its copies were taken, and the frame its positions are from.
  std::uint64_t frame = UINT64_MAX;
  std::uint64_t copied_frame = UINT64_MAX;
  std::uint64_t positions_frame = UINT64_MAX;
};

static void ReleaseCapturedConstants(CapturedConstants* captured) {
  auto release = [](auto* buffer_sets) {
    for (auto& buffer_set : *buffer_sets) {
      for (ID3D11Buffer*& buffer : buffer_set) {
        if (buffer != nullptr) {
          buffer->Release();
        }
        buffer = nullptr;
      }
    }
  };
  release(&captured->buffers);
  release(&captured->vertex_buffers);
  for (ID3D11Buffer*& buffer : captured->domain_views) {
    if (buffer != nullptr) {
      buffer->Release();
    }
    buffer = nullptr;
  }
  for (ID3D11Buffer*& buffer : captured->positions) {
    if (buffer != nullptr) {
      buffer->Release();
    }
    buffer = nullptr;
  }
}

// A non-tessellated draw with motion: this frame's and last frame's streamed-out clip positions.
struct MotionObject {
  ID3D11Buffer* current = nullptr;
  ID3D11Buffer* previous = nullptr;
};

// Draws with motion that share a depth target, viewport and rasterizer state.
struct MotionBatch {
  ID3D11DepthStencilView* depth_view = nullptr;
  D3D11_VIEWPORT viewport = {};
  // The game's rasterizer state of the batch's draws without culling (the batch holds meshes of every cull mode): the
  // motion triangles must equal the depth those draws wrote, depth bias included.
  ID3D11RasterizerState* rasterizer_state = nullptr;
  D3D11_RASTERIZER_DESC rasterizer_desc = {};
  std::vector<MotionObject> objects;
};

// Render-thread state of the DLAA pass.
struct ResolveState {
  // The composite copied over a scene read through a view of the game's, and the debug view's output.
  std::array<ComputeTarget, 2> outputs = {};
  // Last frame the composite went through outputs[0] (a frame whose scene was not the bound render target).
  std::uint64_t composite_output_frame = UINT64_MAX;
  std::uint32_t width = 0u;
  std::uint32_t height = 0u;
  reshade::api::pipeline_layout layout = {0};
  reshade::api::pipeline prepare_pipeline = {0};
  reshade::api::pipeline composite_pipeline = {0};
  // While the scene is the bound render target, DLAA's output is drawn over it: a triangle covering the screen that writes
  // color only.
  ID3D11VertexShader* composite_vertex_shader = nullptr;
  ID3D11PixelShader* composite_pixel_shader = nullptr;
  ID3D11BlendState* composite_blend_state = nullptr;
  // Views of the scenes the input pass reads while they are unbound as render targets. A view keeps its scene alive, so
  // the cache stays small and goes with the outputs.
  std::unordered_map<ID3D11Resource*, ID3D11ShaderResourceView*> scene_views;
  ViewMatrices previous_view = {};
  std::uint64_t frame_index = 0u;
  std::uint64_t last_resolve_frame = UINT64_MAX;
  // Last frame that took its view from the queue: one resolve attempt per frame.
  std::uint64_t last_attempt_frame = UINT64_MAX;
  // Last frame that drew the pause menu's filter over the scene.
  std::uint64_t last_pause_filter_frame = UINT64_MAX;
  std::uint64_t last_logged_frame = 0u;
  // Diagnostics, logged once per 120 frames.
  std::uint32_t pass_draws = 0u;
  std::uint32_t resolves = 0u;
  std::uint32_t fog_resolves = 0u;
  // The render thread waited for the host's answer, in milliseconds.
  double host_wait_milliseconds = 0.0;
  double host_wait_max_milliseconds = 0.0;
  std::uint32_t empty_queue_reuses = 0u;
  std::uint32_t scene_skips = 0u;
  std::size_t max_queue_size = 0u;
  std::size_t max_present_queue_size = 0u;
  std::uint32_t motion_draws = 0u;
  std::uint32_t motion_fallbacks = 0u;
  std::uint32_t recorded_particle_draws = 0u;
  // Per-object motion of skinned draws: previous minus current NDC, cleared to CHARACTER_MOTION_UNKNOWN.
  ID3D11Texture2D* character_motion = nullptr;
  ID3D11RenderTargetView* character_motion_rtv = nullptr;
  ID3D11ShaderResourceView* character_motion_srv = nullptr;
  // Device depth of skinned draws with motion (0 elsewhere): their materials write other data into the scene alpha.
  ID3D11Texture2D* character_depth = nullptr;
  ID3D11RenderTargetView* character_depth_rtv = nullptr;
  ID3D11ShaderResourceView* character_depth_srv = nullptr;
  ID3D11VertexShader* motion_vertex_shader = nullptr;
  ID3D11PixelShader* motion_pixel_shader = nullptr;
  ID3D11InputLayout* motion_input_layout = nullptr;
  // A tessellated draw's streamed-out clip positions: this frame's, then the previous frame's.
  std::array<ID3D11Buffer*, 2> stream_out_positions = {};
  UINT stream_out_capacity = 0u;
  // The view buffer (b1) of the main view by frame parity, for taking each frame's jitter out of the motion.
  std::array<ID3D11Buffer*, 2> view_copies = {};
  std::array<std::uint64_t, 2> view_copy_frames = {UINT64_MAX, UINT64_MAX};
  // This frame's non-tessellated draws with motion, drawn in one pass before DLAA reads the motion targets (their
  // objects referenced).
  std::vector<MotionBatch> motion_batches;
  // The view buffers of this frame and the previous frame, from the frame's first batched draw.
  ID3D11Buffer* batch_view = nullptr;
  ID3D11Buffer* batch_previous_view = nullptr;
  // A read-only view of a batch's depth: the motion pass only tests depth, and a writable view would unbind the depth
  // wherever the game reads it as a texture at the time. Kept while the depth and its view description stay the same.
  ID3D11DepthStencilView* read_only_depth_view = nullptr;
  std::unordered_map<std::uint64_t, CapturedConstants> captured_constants;
  // How often each draw (vertex shader, buffers, range) was seen this frame: tells identical meshes apart.
  std::unordered_map<std::uint64_t, std::uint32_t> draw_occurrences;
  // Default-usage vertex buffers the CPU uploaded into this frame and the two before: cloth is rewritten every frame,
  // often alternating between two buffers; streamed geometry is uploaded once.
  std::unordered_set<std::uint64_t> uploaded_vertex_buffers;
  std::unordered_set<std::uint64_t> previous_uploaded_vertex_buffers;
  std::unordered_set<std::uint64_t> older_uploaded_vertex_buffers;
  // Full-resolution mask of skinned draws (characters, the cape), marked by drawing them a second time into it.
  ID3D11Texture2D* character_mask = nullptr;
  ID3D11RenderTargetView* character_mask_rtv = nullptr;
  ID3D11ShaderResourceView* character_mask_srv = nullptr;
  ID3D11PixelShader* character_mask_shader = nullptr;
  ID3D11DepthStencilState* character_mask_depth_state = nullptr;
  // Motion of tessellated skinned draws from their untessellated triangles, depth-tested among themselves.
  ID3D11Texture2D* tessellated_motion = nullptr;
  ID3D11RenderTargetView* tessellated_motion_rtv = nullptr;
  ID3D11ShaderResourceView* tessellated_motion_srv = nullptr;
  ID3D11Texture2D* tessellated_depth = nullptr;
  ID3D11DepthStencilView* tessellated_depth_dsv = nullptr;
  ID3D11ShaderResourceView* tessellated_depth_srv = nullptr;
  ID3D11DepthStencilState* tessellated_depth_state = nullptr;
  ID3D11VertexShader* motion_world_vertex_shader = nullptr;
  ID3D11PixelShader* tessellated_mask_shader = nullptr;
};

struct DlaaConstants {
  std::array<float, 16> view_to_previous_clip = {};
  std::array<float, 4> projection_terms = {};
  float character_mask_valid = 0.f;
};

constexpr std::uint32_t SCENE_DEPTH_TO_VIEW_DISTANCE_PS = 0x4FC5495Eu;
// The full-screen vertex shader (screen position and view ray) of the fog blended over the whole scene, shared by all
// its pixel shader variants: the translucent draws follow it.
constexpr std::uint32_t SCENE_FOG_VS = 0x355E3384u;
// The pause menu's full-screen filter over the scene, drawn before the first post pass.
constexpr std::uint32_t PAUSE_FILTER_PS = 0xEC4BCE15u;

// A vertex shader's bytecode until its first draw with motion creates the geometry shader that streams out its clip
// positions (null when that failed).
struct MotionVertexShader {
  std::vector<std::uint8_t> code;
  ID3D11GeometryShader* stream_out_shader = nullptr;
  // Skins with the bone palette (characters and their cloth); the others only move when the CPU rewrites their vertices.
  bool skinned = false;
};

// Vertex shaders by bytecode CRC32.
struct MotionVertexShaders {
  std::shared_mutex mutex;
  std::unordered_map<std::uint32_t, MotionVertexShader> shaders;
};
static MotionVertexShaders motion_vertex_shaders;

static ViewQueue view_queue;
static ResolveState resolve_state;

// Which queued view the GPU drew this frame: the game thread runs one or two views ahead and shifts after a hitch.
// The jitter the game rendered is read back from its view buffer (VS b1 of the frame's first draw into the main depth)
// two frames later and matched against the queued views' jitter, which tells the queue position to pair with from then
// on.
constexpr std::uint32_t VIEW_CANDIDATES = 3u;
struct ViewPairing {
  static constexpr std::uint32_t SLOTS = 4u;
  std::array<ID3D11Buffer*, SLOTS> staging = {};
  std::array<std::uint64_t, SLOTS> copied_frame = {UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX};
  // Jitter of the views at queue positions 0 (the oldest), 1 and 2 when the frame was resolved.
  std::array<std::array<std::array<float, 2>, VIEW_CANDIDATES>, SLOTS> candidate_jitter = {};
  std::array<std::uint32_t, SLOTS> candidate_count = {};
  std::array<std::uint32_t, SLOTS> used_position = {};
  std::array<bool, SLOTS> resolved = {};
  // Queue position paired with the frame being drawn.
  std::uint32_t position = 1u;
  // Diagnostics, logged once per 120 frames.
  std::uint32_t matches = 0u;
  std::uint32_t mismatches = 0u;
  std::uint32_t position_changes = 0u;
  std::uint32_t unread = 0u;
};
static ViewPairing view_pairing;
static ShaderInjectData* shader_injection = nullptr;
static float dlaa_enabled = 0.f;
// False once the game's GPU turned out not to be NVIDIA's: DLSS cannot run there, so the setting is hidden.
static bool dlaa_supported = true;
// True while DLAA resolved in the last two frames. The camera jitter and the texture LOD bias only help then; while it
// starts, after a failure or with MSAA on they would shake and sharpen the game's own image.
static std::atomic<bool> dlaa_resolving = false;
// NVSDK_NGX_DLSS_Hint_Render_Preset value: 0 default, 10 J, 11 K, 12 L, 13 M. K keeps thin geometry steady but averages
// away falling snow and animated lights; L keeps those and shimmers on railings and grates.
static float dlaa_preset = 11.f;
// Frames without the scene fog run DLAA before the first post pass instead: snow, rain and the main menu's dirt
// overlay are held back and replayed over its output. Off when the replay target cannot be created.
static bool particle_replay = true;

// Failures DLAA keeps running through, with less: each message is logged once per session.
struct LoggedWarnings {
  std::shared_mutex mutex;
  std::unordered_set<std::string_view> messages;
};

// message: a string literal, remembered by its text.
static void WarnOnce(std::string_view message, HRESULT result) {
  try {
    // Created by the first warning, where an allocation failure is caught.
    static LoggedWarnings logged_warnings;
    {
      const std::unique_lock lock(logged_warnings.mutex);
      if (logged_warnings.messages.contains(message)) return;
      logged_warnings.messages.insert(message);
    }
    reshade::log::message(reshade::log::level::warning,
                          std::format("batmanac DLAA: {} (0x{:08X})", message, static_cast<std::uint32_t>(result)).c_str());
  } catch (const std::bad_alloc&) {
    // No-op: out of memory the warning is dropped, so the caller's fallback still runs.
  }
}

}  // namespace batmanac::dlaa
