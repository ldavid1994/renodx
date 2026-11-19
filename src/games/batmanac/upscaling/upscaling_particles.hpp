/*
 * Copyright (C) 2026 RenoDX contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <d3d11.h>
#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <include/reshade.hpp>

#include "../../../utils/state.hpp"
#include "./upscaling_state.hpp"

// Snow, rain and the main menu's dirt overlay: draws held back from before the upscaler and replayed after it.
namespace batmanac::upscaling {

constexpr UINT RECORDED_VERTEX_BUFFER_SLOTS = 4u;
constexpr UINT RECORDED_RESOURCE_SLOTS = 32u;
constexpr UINT RECORDED_SAMPLER_SLOTS = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT;

// The game's state at a held-back particle draw. Dynamic buffers are copied, since the game rewrites them before the
// replay; everything else is referenced until the replay.
struct RecordedDraw {
  // Copies owned by this slot, reused from frame to frame.
  std::array<ID3D11Buffer*, RECORDED_VERTEX_BUFFER_SLOTS> vertex_buffer_copies = {};
  ID3D11Buffer* index_buffer_copy = nullptr;
  std::array<ID3D11Buffer*, VERTEX_CONSTANT_SLOTS> vertex_constant_copies = {};
  std::array<ID3D11Buffer*, VERTEX_CONSTANT_SLOTS> pixel_constant_copies = {};

  ID3D11InputLayout* input_layout = nullptr;
  D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
  std::array<ID3D11Buffer*, RECORDED_VERTEX_BUFFER_SLOTS> vertex_buffers = {};
  std::array<UINT, RECORDED_VERTEX_BUFFER_SLOTS> strides = {};
  std::array<UINT, RECORDED_VERTEX_BUFFER_SLOTS> offsets = {};
  ID3D11Buffer* index_buffer = nullptr;
  DXGI_FORMAT index_format = DXGI_FORMAT_UNKNOWN;
  UINT index_offset = 0u;
  ID3D11VertexShader* vertex_shader = nullptr;
  ID3D11GeometryShader* geometry_shader = nullptr;
  ID3D11PixelShader* pixel_shader = nullptr;
  std::array<ID3D11Buffer*, VERTEX_CONSTANT_SLOTS> vertex_constants = {};
  std::array<ID3D11Buffer*, VERTEX_CONSTANT_SLOTS> pixel_constants = {};
  std::array<ID3D11ShaderResourceView*, RECORDED_RESOURCE_SLOTS> pixel_resources = {};
  std::array<ID3D11SamplerState*, RECORDED_SAMPLER_SLOTS> pixel_samplers = {};
  ID3D11RasterizerState* rasterizer_state = nullptr;
  D3D11_VIEWPORT viewport = {};
  D3D11_RECT scissor = {};
  ID3D11BlendState* blend_state = nullptr;
  std::array<float, 4> blend_factor = {};
  UINT sample_mask = 0u;
  ID3D11DepthStencilState* depth_state = nullptr;
  UINT stencil_ref = 0u;
  ID3D11RenderTargetView* render_target = nullptr;
  ID3D11DepthStencilView* depth_view = nullptr;
  bool indexed = false;
  UINT count = 0u;
  UINT instance_count = 0u;
  UINT first = 0u;
  INT base_vertex = 0;
  UINT first_instance = 0u;
};

// Particle draws held back until the upscaler's output is over the scene. Render thread only.
struct ParticleRecorder {
  std::vector<RecordedDraw> draws;
  std::size_t count = 0u;
  // The particles drew into the scene target, but the first post pass reads a copy of it, which the upscaler writes: the
  // replay draws into that copy.
  ID3D11RenderTargetView* post_input_rtv = nullptr;
  ID3D11Resource* post_input = nullptr;
};
static ParticleRecorder particle_recorder;

static void ReleaseRecordedReferences(RecordedDraw* draw) {
  auto release = [](auto** object) {
    if (*object != nullptr) {
      (*object)->Release();
      *object = nullptr;
    }
  };
  release(&draw->input_layout);
  for (ID3D11Buffer*& buffer : draw->vertex_buffers) {
    release(&buffer);
  }
  release(&draw->index_buffer);
  release(&draw->vertex_shader);
  release(&draw->geometry_shader);
  release(&draw->pixel_shader);
  for (ID3D11Buffer*& buffer : draw->vertex_constants) {
    release(&buffer);
  }
  for (ID3D11Buffer*& buffer : draw->pixel_constants) {
    release(&buffer);
  }
  for (ID3D11ShaderResourceView*& view : draw->pixel_resources) {
    release(&view);
  }
  for (ID3D11SamplerState*& sampler : draw->pixel_samplers) {
    release(&sampler);
  }
  release(&draw->rasterizer_state);
  release(&draw->blend_state);
  release(&draw->depth_state);
  release(&draw->render_target);
  release(&draw->depth_view);
}

// Drops held-back draws that were not replayed and, on teardown, the slots' buffer copies.
static void ClearRecordedDraws(bool release_copies) {
  for (std::size_t i = 0u; i < particle_recorder.count; ++i) {
    ReleaseRecordedReferences(&particle_recorder.draws[i]);
  }
  particle_recorder.count = 0u;
  if (!release_copies) return;
  if (particle_recorder.post_input_rtv != nullptr) {
    particle_recorder.post_input_rtv->Release();
    particle_recorder.post_input_rtv = nullptr;
  }
  particle_recorder.post_input = nullptr;
  for (RecordedDraw& draw : particle_recorder.draws) {
    for (ID3D11Buffer* copy : draw.vertex_buffer_copies) {
      if (copy != nullptr) {
        copy->Release();
      }
    }
    for (ID3D11Buffer* copy : draw.vertex_constant_copies) {
      if (copy != nullptr) {
        copy->Release();
      }
    }
    for (ID3D11Buffer* copy : draw.pixel_constant_copies) {
      if (copy != nullptr) {
        copy->Release();
      }
    }
    if (draw.index_buffer_copy != nullptr) {
      draw.index_buffer_copy->Release();
    }
  }
  particle_recorder.draws.clear();
}

// Pixel shaders of draws that change in place with no motion vectors, so DLSS would average them away: the falling snow
// and rain mesh, the distant rain curtain and the main menu's scrolling dirt overlay.
constexpr std::array<std::uint32_t, 3> REPLAYED_PIXEL_SHADERS = {0x19F9FBC2u, 0x03DE17F0u, 0x71D465C2u};

// One of those draws (blending on, no depth writes) into the main scene target before the upscaler pass is held back and
// replayed over the upscaler's output.
// Only while the previous frame resolved, so the replay can expect this frame's upscaler output. True when the draw was
// recorded, so the original is skipped.
static bool RecordParticleDraw(reshade::api::command_list* cmd_list, bool indexed, UINT count, UINT instance_count, UINT first,
                               INT base_vertex, UINT first_instance) {
  if (aa_enabled == 0.f || !particle_replay || resolve_state.last_resolve_frame == UINT64_MAX
      || resolve_state.last_resolve_frame + 1u != resolve_state.frame_index) return false;
  // In the pause menu the filter is built from the scene before the replay: draws replayed later would sit on top.
  if (resolve_state.last_pause_filter_frame != UINT64_MAX && resolve_state.last_pause_filter_frame + 1u >= resolve_state.frame_index) return false;
  const std::uint32_t pixel_shader_hash = renodx::utils::state::GetCurrentShaderHash(cmd_list, reshade::api::pipeline_stage::pixel_shader);
  if (std::ranges::find(REPLAYED_PIXEL_SHADERS, pixel_shader_hash) == REPLAYED_PIXEL_SHADERS.end()) return false;

  auto* context = reinterpret_cast<ID3D11DeviceContext*>(cmd_list->get_native());
  RecordedDraw recorded = {};
  context->OMGetBlendState(&recorded.blend_state, recorded.blend_factor.data(), &recorded.sample_mask);
  context->OMGetDepthStencilState(&recorded.depth_state, &recorded.stencil_ref);
  context->OMGetRenderTargets(1u, &recorded.render_target, &recorded.depth_view);
  bool qualifies = recorded.blend_state != nullptr && recorded.render_target != nullptr;
  if (qualifies) {
    D3D11_BLEND_DESC blend_desc = {};
    recorded.blend_state->GetDesc(&blend_desc);
    qualifies = blend_desc.RenderTarget[0].BlendEnable != FALSE;
  }
  if (qualifies) {
    // A null state is the default, which tests and writes depth. Soft particles read the scene's depth from a texture
    // instead and often draw with no depth test or buffer.
    bool writes_depth = recorded.depth_view != nullptr;
    if (recorded.depth_state != nullptr) {
      D3D11_DEPTH_STENCIL_DESC depth_desc = {};
      recorded.depth_state->GetDesc(&depth_desc);
      writes_depth = depth_desc.DepthEnable != FALSE && depth_desc.DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ZERO;
    }
    qualifies = !writes_depth;
  }
  if (qualifies) {
    ID3D11Resource* scene_resource = nullptr;
    recorded.render_target->GetResource(&scene_resource);
    D3D11_TEXTURE2D_DESC scene_desc = {};
    static_cast<ID3D11Texture2D*>(scene_resource)->GetDesc(&scene_desc);
    scene_resource->Release();
    qualifies = static_cast<float>(scene_desc.Width) == view_queue.render_width && static_cast<float>(scene_desc.Height) == view_queue.render_height
                && scene_desc.SampleDesc.Count == 1u
                && (scene_desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT || scene_desc.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS);
  }
  if (qualifies) {
    ID3D11HullShader* hull_shader = nullptr;
    context->HSGetShader(&hull_shader, nullptr, nullptr);
    qualifies = hull_shader == nullptr;
    if (hull_shader != nullptr) {
      hull_shader->Release();
    }
  }
  if (!qualifies) {
    ReleaseRecordedReferences(&recorded);
    return false;
  }

  if (particle_recorder.count == particle_recorder.draws.size()) {
    particle_recorder.draws.emplace_back();
  }
  RecordedDraw& slot = particle_recorder.draws[particle_recorder.count];
  // The slot keeps its buffer copies; everything else comes from this draw.
  recorded.vertex_buffer_copies = slot.vertex_buffer_copies;
  recorded.index_buffer_copy = slot.index_buffer_copy;
  recorded.vertex_constant_copies = slot.vertex_constant_copies;
  recorded.pixel_constant_copies = slot.pixel_constant_copies;

  ID3D11Device* device = nullptr;
  context->GetDevice(&device);
  // Copies all of source into the slot's buffer (grown as needed) and returns it, referenced for the replay; null when
  // it cannot be created.
  auto copy_buffer = [&](ID3D11Buffer* source, UINT bind_flags, ID3D11Buffer** copy) -> ID3D11Buffer* {
    D3D11_BUFFER_DESC source_desc = {};
    source->GetDesc(&source_desc);
    const UINT size = source_desc.ByteWidth;
    D3D11_BUFFER_DESC copy_desc = {};
    if (*copy != nullptr) {
      (*copy)->GetDesc(&copy_desc);
    }
    if (*copy == nullptr || copy_desc.ByteWidth < size) {
      if (*copy != nullptr) {
        (*copy)->Release();
        *copy = nullptr;
      }
      const D3D11_BUFFER_DESC desc = {.ByteWidth = (size + 15u) & ~15u, .Usage = D3D11_USAGE_DEFAULT, .BindFlags = bind_flags};
      if (const HRESULT result = device->CreateBuffer(&desc, nullptr, copy); FAILED(result)) {
        WarnOnce("cannot copy a held-back particle draw's buffers; it is drawn in place, before the upscaler", result);
        return nullptr;
      }
    }
    const D3D11_BOX box = {.left = 0u, .top = 0u, .front = 0u, .right = size, .bottom = 1u, .back = 1u};
    context->CopySubresourceRegion(*copy, 0u, 0u, 0u, 0u, source, 0u, &box);
    (*copy)->AddRef();
    return *copy;
  };
  auto is_dynamic = [](ID3D11Buffer* buffer) {
    D3D11_BUFFER_DESC desc = {};
    buffer->GetDesc(&desc);
    return desc.Usage == D3D11_USAGE_DYNAMIC;
  };

  context->IAGetInputLayout(&recorded.input_layout);
  context->IAGetPrimitiveTopology(&recorded.topology);
  context->VSGetShader(&recorded.vertex_shader, nullptr, nullptr);
  context->GSGetShader(&recorded.geometry_shader, nullptr, nullptr);
  context->PSGetShader(&recorded.pixel_shader, nullptr, nullptr);
  context->PSGetShaderResources(0u, RECORDED_RESOURCE_SLOTS, recorded.pixel_resources.data());
  context->PSGetSamplers(0u, RECORDED_SAMPLER_SLOTS, recorded.pixel_samplers.data());
  context->RSGetState(&recorded.rasterizer_state);
  UINT viewport_count = 1u;
  context->RSGetViewports(&viewport_count, &recorded.viewport);
  UINT scissor_count = 1u;
  context->RSGetScissorRects(&scissor_count, &recorded.scissor);
  recorded.indexed = indexed;
  recorded.count = count;
  recorded.instance_count = instance_count;
  recorded.first = first;
  recorded.base_vertex = base_vertex;
  recorded.first_instance = first_instance;

  bool copied = true;
  // Constants are small and rewritten between draws: always copied.
  std::array<ID3D11Buffer*, VERTEX_CONSTANT_SLOTS> game_constants = {};
  context->VSGetConstantBuffers(0u, VERTEX_CONSTANT_SLOTS, game_constants.data());
  for (UINT stage = 0u; stage < 2u; ++stage) {
    if (stage == 1u) {
      context->PSGetConstantBuffers(0u, VERTEX_CONSTANT_SLOTS, game_constants.data());
    }
    auto& bound = (stage == 0u ? recorded.vertex_constants : recorded.pixel_constants);
    auto& copies = (stage == 0u ? recorded.vertex_constant_copies : recorded.pixel_constant_copies);
    for (UINT i = 0u; i < VERTEX_CONSTANT_SLOTS; ++i) {
      if (game_constants[i] == nullptr) continue;
      bound[i] = copy_buffer(game_constants[i], D3D11_BIND_CONSTANT_BUFFER, &copies[i]);
      copied = copied && bound[i] != nullptr;
      game_constants[i]->Release();
      game_constants[i] = nullptr;
    }
  }

  // Dynamic vertex and index buffers are copied whole, so the replay keeps the game's offsets and draw arguments.
  context->IAGetVertexBuffers(0u, RECORDED_VERTEX_BUFFER_SLOTS, recorded.vertex_buffers.data(), recorded.strides.data(), recorded.offsets.data());
  for (UINT i = 0u; i < RECORDED_VERTEX_BUFFER_SLOTS; ++i) {
    ID3D11Buffer* game_buffer = recorded.vertex_buffers[i];
    if (game_buffer == nullptr || !is_dynamic(game_buffer)) continue;
    recorded.vertex_buffers[i] = copy_buffer(game_buffer, D3D11_BIND_VERTEX_BUFFER, &recorded.vertex_buffer_copies[i]);
    copied = copied && recorded.vertex_buffers[i] != nullptr;
    game_buffer->Release();
  }
  if (indexed) {
    context->IAGetIndexBuffer(&recorded.index_buffer, &recorded.index_format, &recorded.index_offset);
    if (recorded.index_buffer != nullptr && is_dynamic(recorded.index_buffer)) {
      ID3D11Buffer* game_buffer = recorded.index_buffer;
      recorded.index_buffer = copy_buffer(game_buffer, D3D11_BIND_INDEX_BUFFER, &recorded.index_buffer_copy);
      copied = copied && recorded.index_buffer != nullptr;
      game_buffer->Release();
    }
  }
  device->Release();

  if (!copied) {
    // Keep the copies made so far in the slot; draw the particle normally.
    slot.vertex_buffer_copies = recorded.vertex_buffer_copies;
    slot.index_buffer_copy = recorded.index_buffer_copy;
    slot.vertex_constant_copies = recorded.vertex_constant_copies;
    slot.pixel_constant_copies = recorded.pixel_constant_copies;
    ReleaseRecordedReferences(&recorded);
    return false;
  }
  slot = recorded;
  ++particle_recorder.count;
  ++resolve_state.recorded_particle_draws;
  return true;
}

}  // namespace batmanac::upscaling
