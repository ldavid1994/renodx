/*
 * Copyright (C) 2026 RenoDX contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <d3d11.h>
#include <Windows.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <mutex>
#include <shared_mutex>
#include <vector>

#include <embed/shaders.h>
#include <include/reshade.hpp>

#include "../../utils/state.hpp"
#include "./dlaa_host.hpp"
#include "./dlaa_state.hpp"
#include "./shared.h"

// Per-object motion: moving draws stream out their positions; one pass per frame draws the batched motion before
// DLAA reads it, and a tessellated draw draws its own.
namespace batmanac::dlaa {

// Forgets the batched motion: drawn, or dropped with a frame that never reached DLAA.
static void ResetMotionBatch() {
  for (IUnknown* object : std::initializer_list<IUnknown*>{resolve_state.batch_view, resolve_state.batch_previous_view}) {
    if (object != nullptr) {
      object->Release();
    }
  }
  resolve_state.batch_view = nullptr;
  resolve_state.batch_previous_view = nullptr;
  for (const MotionBatch& batch : resolve_state.motion_batches) {
    batch.depth_view->Release();
    if (batch.rasterizer_state != nullptr) {
      batch.rasterizer_state->Release();
    }
    for (const MotionObject& object : batch.objects) {
      object.current->Release();
      object.previous->Release();
    }
  }
  resolve_state.motion_batches.clear();
}

// Draws the frame's batched motion into the mask, motion and depth targets wherever a streamed-out triangle is the
// depth's surface, then restores the game's state. Drawn last, a later draw in front of a moving one hides its motion.
static void DrawBatchedMotion(ID3D11DeviceContext* context) {
  if (resolve_state.motion_batches.empty()) return;
  std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> saved_render_targets = {};
  ID3D11DepthStencilView* saved_depth_view = nullptr;
  context->OMGetRenderTargets(static_cast<UINT>(saved_render_targets.size()), saved_render_targets.data(), &saved_depth_view);
  ID3D11DepthStencilState* saved_depth_state = nullptr;
  UINT saved_stencil_ref = 0u;
  context->OMGetDepthStencilState(&saved_depth_state, &saved_stencil_ref);
  ID3D11BlendState* saved_blend_state = nullptr;
  std::array<float, 4> saved_blend_factor = {};
  UINT saved_sample_mask = 0u;
  context->OMGetBlendState(&saved_blend_state, saved_blend_factor.data(), &saved_sample_mask);
  ID3D11RasterizerState* saved_rasterizer_state = nullptr;
  context->RSGetState(&saved_rasterizer_state);
  std::array<D3D11_VIEWPORT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> saved_viewports = {};
  UINT saved_viewport_count = static_cast<UINT>(saved_viewports.size());
  context->RSGetViewports(&saved_viewport_count, saved_viewports.data());
  ID3D11InputLayout* saved_input_layout = nullptr;
  context->IAGetInputLayout(&saved_input_layout);
  std::array<ID3D11Buffer*, 2> saved_vertex_buffers = {};
  std::array<UINT, 2> saved_strides = {};
  std::array<UINT, 2> saved_offsets = {};
  context->IAGetVertexBuffers(0u, 2u, saved_vertex_buffers.data(), saved_strides.data(), saved_offsets.data());
  D3D11_PRIMITIVE_TOPOLOGY saved_topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
  context->IAGetPrimitiveTopology(&saved_topology);
  ID3D11VertexShader* saved_vertex_shader = nullptr;
  context->VSGetShader(&saved_vertex_shader, nullptr, nullptr);
  ID3D11HullShader* saved_hull_shader = nullptr;
  context->HSGetShader(&saved_hull_shader, nullptr, nullptr);
  ID3D11DomainShader* saved_domain_shader = nullptr;
  context->DSGetShader(&saved_domain_shader, nullptr, nullptr);
  ID3D11GeometryShader* saved_geometry_shader = nullptr;
  context->GSGetShader(&saved_geometry_shader, nullptr, nullptr);
  ID3D11PixelShader* saved_pixel_shader = nullptr;
  context->PSGetShader(&saved_pixel_shader, nullptr, nullptr);
  ID3D11Buffer* saved_constants_b1 = nullptr;
  context->VSGetConstantBuffers(1u, 1u, &saved_constants_b1);
  ID3D11Buffer* saved_constants_b11 = nullptr;
  context->VSGetConstantBuffers(11u, 1u, &saved_constants_b11);

  context->OMSetDepthStencilState(resolve_state.character_mask_depth_state, 0u);
  context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
  constexpr std::array<UINT, 2> POSITION_STRIDES = {sizeof(float) * 4u, sizeof(float) * 4u};
  constexpr std::array<UINT, 2> POSITION_OFFSETS = {0u, 0u};
  context->IASetInputLayout(resolve_state.motion_input_layout);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context->VSSetShader(resolve_state.motion_vertex_shader, nullptr, 0u);
  context->HSSetShader(nullptr, nullptr, 0u);
  context->DSSetShader(nullptr, nullptr, 0u);
  context->GSSetShader(nullptr, nullptr, 0u);
  context->PSSetShader(resolve_state.motion_pixel_shader, nullptr, 0u);
  context->VSSetConstantBuffers(1u, 1u, &resolve_state.batch_view);
  context->VSSetConstantBuffers(11u, 1u, &resolve_state.batch_previous_view);
  const std::array<ID3D11RenderTargetView*, 3> character_targets = {resolve_state.character_mask_rtv, resolve_state.character_motion_rtv, resolve_state.character_depth_rtv};
  ID3D11DepthStencilView* bound_depth_view = nullptr;
  for (const MotionBatch& batch : resolve_state.motion_batches) {
    if (batch.depth_view != bound_depth_view) {
      ID3D11Resource* depth_resource = nullptr;
      batch.depth_view->GetResource(&depth_resource);
      D3D11_DEPTH_STENCIL_VIEW_DESC read_only_desc = {};
      batch.depth_view->GetDesc(&read_only_desc);
      read_only_desc.Flags = D3D11_DSV_READ_ONLY_DEPTH;
      if (read_only_desc.Format == DXGI_FORMAT_D24_UNORM_S8_UINT || read_only_desc.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT) {
        read_only_desc.Flags |= D3D11_DSV_READ_ONLY_STENCIL;
      }
      if (resolve_state.read_only_depth_view != nullptr) {
        ID3D11Resource* kept_resource = nullptr;
        resolve_state.read_only_depth_view->GetResource(&kept_resource);
        D3D11_DEPTH_STENCIL_VIEW_DESC kept_desc = {};
        resolve_state.read_only_depth_view->GetDesc(&kept_desc);
        if (kept_resource != depth_resource || std::memcmp(&kept_desc, &read_only_desc, sizeof(kept_desc)) != 0) {
          resolve_state.read_only_depth_view->Release();
          resolve_state.read_only_depth_view = nullptr;
        }
        kept_resource->Release();
      }
      if (resolve_state.read_only_depth_view == nullptr) {
        ID3D11Device* device = nullptr;
        context->GetDevice(&device);
        if (const HRESULT result = device->CreateDepthStencilView(depth_resource, &read_only_desc, &resolve_state.read_only_depth_view);
            FAILED(result)) {
          WarnOnce("cannot create a read-only depth view; moving objects drawn into that depth keep the camera's motion", result);
        }
        device->Release();
      }
      depth_resource->Release();
      // Binding the game's writable view instead would unbind the depth wherever the game reads it as a texture.
      if (resolve_state.read_only_depth_view == nullptr) continue;
      context->OMSetRenderTargets(static_cast<UINT>(character_targets.size()), character_targets.data(), resolve_state.read_only_depth_view);
      bound_depth_view = batch.depth_view;
    }
    context->RSSetState(batch.rasterizer_state);
    context->RSSetViewports(1u, &batch.viewport);
    for (const MotionObject& object : batch.objects) {
      const std::array<ID3D11Buffer*, 2> positions = {object.current, object.previous};
      context->IASetVertexBuffers(0u, 2u, positions.data(), POSITION_STRIDES.data(), POSITION_OFFSETS.data());
      // The vertex count is what this frame's stream-out wrote into the buffer in slot 0.
      context->DrawAuto();
    }
  }

  context->OMSetRenderTargets(static_cast<UINT>(saved_render_targets.size()), saved_render_targets.data(), saved_depth_view);
  context->OMSetDepthStencilState(saved_depth_state, saved_stencil_ref);
  context->OMSetBlendState(saved_blend_state, saved_blend_factor.data(), saved_sample_mask);
  context->RSSetState(saved_rasterizer_state);
  context->RSSetViewports(saved_viewport_count, saved_viewports.data());
  context->IASetInputLayout(saved_input_layout);
  context->IASetVertexBuffers(0u, 2u, saved_vertex_buffers.data(), saved_strides.data(), saved_offsets.data());
  context->IASetPrimitiveTopology(saved_topology);
  context->VSSetShader(saved_vertex_shader, nullptr, 0u);
  context->HSSetShader(saved_hull_shader, nullptr, 0u);
  context->DSSetShader(saved_domain_shader, nullptr, 0u);
  context->GSSetShader(saved_geometry_shader, nullptr, 0u);
  context->PSSetShader(saved_pixel_shader, nullptr, 0u);
  context->VSSetConstantBuffers(1u, 1u, &saved_constants_b1);
  context->VSSetConstantBuffers(11u, 1u, &saved_constants_b11);
  for (IUnknown* saved : std::initializer_list<IUnknown*>{saved_depth_view, saved_depth_state, saved_blend_state, saved_rasterizer_state, saved_input_layout,
                                                          saved_vertex_buffers[0], saved_vertex_buffers[1], saved_vertex_shader, saved_hull_shader,
                                                          saved_domain_shader, saved_geometry_shader, saved_pixel_shader, saved_constants_b1, saved_constants_b11}) {
    if (saved != nullptr) {
      saved->Release();
    }
  }
  for (ID3D11RenderTargetView* render_target : saved_render_targets) {
    if (render_target != nullptr) {
      render_target->Release();
    }
  }
  ResetMotionBatch();
}

static void ReleaseCharacterTargets() {
  auto release = [](auto** object) {
    if (*object != nullptr) {
      (*object)->Release();
      *object = nullptr;
    }
  };
  release(&resolve_state.character_mask_depth_state);
  release(&resolve_state.character_mask_shader);
  release(&resolve_state.character_mask_srv);
  release(&resolve_state.character_mask_rtv);
  release(&resolve_state.character_mask);
  release(&resolve_state.character_motion_srv);
  release(&resolve_state.character_motion_rtv);
  release(&resolve_state.character_motion);
  release(&resolve_state.character_depth_srv);
  release(&resolve_state.character_depth_rtv);
  release(&resolve_state.character_depth);
  release(&resolve_state.motion_vertex_shader);
  release(&resolve_state.motion_pixel_shader);
  release(&resolve_state.motion_input_layout);
  release(&resolve_state.stream_out_positions[0]);
  release(&resolve_state.stream_out_positions[1]);
  release(&resolve_state.tessellated_motion_srv);
  release(&resolve_state.tessellated_motion_rtv);
  release(&resolve_state.tessellated_motion);
  release(&resolve_state.tessellated_depth_dsv);
  release(&resolve_state.tessellated_depth_srv);
  release(&resolve_state.tessellated_depth);
  release(&resolve_state.tessellated_depth_state);
  release(&resolve_state.motion_world_vertex_shader);
  release(&resolve_state.tessellated_mask_shader);
  release(&resolve_state.read_only_depth_view);
  release(&resolve_state.view_copies[0]);
  release(&resolve_state.view_copies[1]);
  resolve_state.view_copy_frames = {UINT64_MAX, UINT64_MAX};
  resolve_state.stream_out_capacity = 0u;
  ResetMotionBatch();
}

// Issues a skinned or CPU-animated draw into the main depth, then streams out its clip positions: kept for the next
// frame and, with the previous frame's, added to the motion batch. A tessellated draw instead replays last frame's
// constants and vertices for its motion. Without a previous frame only its mask is drawn. True when it issued the draw,
// so the original is skipped.
// draw_range: index or vertex count, instance count, first index or vertex, base vertex.
template <typename IssueDraw>
static bool MarkCharacter(reshade::api::command_list* cmd_list, const std::array<std::uint32_t, 4>& draw_range, const IssueDraw& issue_draw) {
  // Skinned draws after DLAA ran this frame (translucent hair, for example) have nothing to hand it.
  if (dlaa_enabled == 0.f || resolve_state.last_resolve_frame == resolve_state.frame_index) return false;
  const std::uint32_t vertex_shader = renodx::utils::state::GetCurrentShaderHash(cmd_list, reshade::api::pipeline_stage::vertex_shader);
  bool skinned = false;
  ID3D11GeometryShader* stream_out_shader = nullptr;
  bool stream_out_pending = false;
  {
    const std::shared_lock lock(motion_vertex_shaders.mutex);
    const auto found = motion_vertex_shaders.shaders.find(vertex_shader);
    if (found == motion_vertex_shaders.shaders.end()) return false;
    const auto& [hash, shader] = *found;
    skinned = shader.skinned;
    stream_out_shader = shader.stream_out_shader;
    stream_out_pending = !shader.code.empty();
  }
  auto rewritten_every_frame = [](ID3D11Buffer* buffer) {
    const auto handle = reinterpret_cast<std::uint64_t>(buffer);
    return resolve_state.uploaded_vertex_buffers.contains(handle)
           && (resolve_state.previous_uploaded_vertex_buffers.contains(handle) || resolve_state.older_uploaded_vertex_buffers.contains(handle));
  };

  auto* context = reinterpret_cast<ID3D11DeviceContext*>(cmd_list->get_native());
  if (!skinned) {
    std::array<ID3D11Buffer*, CAPTURED_VERTEX_BUFFER_SLOTS> vertex_buffers = {};
    context->IAGetVertexBuffers(0u, CAPTURED_VERTEX_BUFFER_SLOTS, vertex_buffers.data(), nullptr, nullptr);
    bool uploaded = false;
    for (ID3D11Buffer* buffer : vertex_buffers) {
      if (buffer != nullptr) {
        uploaded = uploaded || rewritten_every_frame(buffer);
        buffer->Release();
      }
    }
    if (!uploaded) return false;
    // A draw that writes neither color nor depth leaves nothing in the image: the occlusion tests' boxes, rebuilt every
    // frame in a rewritten buffer, would each get a mask draw. A depth-only pass of a moving mesh still writes depth.
    ID3D11PixelShader* pixel_shader = nullptr;
    context->PSGetShader(&pixel_shader, nullptr, nullptr);
    bool writes_color = pixel_shader != nullptr;
    if (pixel_shader != nullptr) {
      pixel_shader->Release();
    }
    ID3D11BlendState* blend_state = nullptr;
    context->OMGetBlendState(&blend_state, nullptr, nullptr);
    if (writes_color && blend_state != nullptr) {
      D3D11_BLEND_DESC blend_desc = {};
      blend_state->GetDesc(&blend_desc);
      const std::size_t blended_targets = (blend_desc.IndependentBlendEnable ? std::size(blend_desc.RenderTarget) : 1u);
      writes_color = std::any_of(blend_desc.RenderTarget, blend_desc.RenderTarget + blended_targets,
                                 [](const D3D11_RENDER_TARGET_BLEND_DESC& target) { return target.RenderTargetWriteMask != 0u; });
    }
    if (blend_state != nullptr) {
      blend_state->Release();
    }
    ID3D11DepthStencilState* depth_state = nullptr;
    context->OMGetDepthStencilState(&depth_state, nullptr);
    // A null state is the default, which tests and writes depth.
    D3D11_DEPTH_STENCIL_DESC depth_state_desc = {.DepthEnable = TRUE, .DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL};
    if (depth_state != nullptr) {
      depth_state->GetDesc(&depth_state_desc);
      depth_state->Release();
    }
    const bool writes_depth = depth_state_desc.DepthEnable != FALSE && depth_state_desc.DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ZERO;
    if (!writes_color && !writes_depth) return false;
  }
  HRESULT stream_out_result = S_OK;
  if (stream_out_pending) {
    const std::unique_lock lock(motion_vertex_shaders.mutex);
    MotionVertexShader& shader = motion_vertex_shaders.shaders[vertex_shader];
    if (!shader.code.empty()) {
      ID3D11Device* device = nullptr;
      context->GetDevice(&device);
      // A vertex shader's bytecode can stand in for a geometry shader that only streams out its outputs: the clip
      // position, or the world position a vertex shader that feeds tessellation hands the domain shader (semantics
      // ignore case).
      constexpr UINT POSITION_STRIDE = sizeof(float) * 4u;
      for (const char* semantic : {"SV_Position", "VS_To_DS_Position"}) {
        const D3D11_SO_DECLARATION_ENTRY position_entry = {.Stream = 0u, .SemanticName = semantic, .SemanticIndex = 0u, .StartComponent = 0u, .ComponentCount = 4u, .OutputSlot = 0u};
        stream_out_result = device->CreateGeometryShaderWithStreamOutput(shader.code.data(), shader.code.size(), &position_entry, 1u, &POSITION_STRIDE, 1u,
                                                                         D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &shader.stream_out_shader);
        if (SUCCEEDED(stream_out_result)) break;
      }
      device->Release();
      shader.code = std::vector<std::uint8_t>();
    }
    stream_out_shader = shader.stream_out_shader;
  }
  if (FAILED(stream_out_result)) {
    WarnOnce("a moving mesh's vertex shader cannot stream out its positions; its draws keep the camera's motion", stream_out_result);
  }
  // Without its motion, a CPU-animated mesh keeps the camera motion of static geometry.
  if (!skinned && stream_out_shader == nullptr) return false;
  std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> saved_render_targets = {};
  ID3D11DepthStencilView* depth_view = nullptr;
  context->OMGetRenderTargets(static_cast<UINT>(saved_render_targets.size()), saved_render_targets.data(), &depth_view);
  auto release_render_targets = [&]() {
    for (ID3D11RenderTargetView* render_target : saved_render_targets) {
      if (render_target != nullptr) {
        render_target->Release();
      }
    }
    if (depth_view != nullptr) {
      depth_view->Release();
    }
  };
  if (depth_view == nullptr) {
    release_render_targets();
    return false;
  }
  ID3D11Resource* depth_resource = nullptr;
  depth_view->GetResource(&depth_resource);
  D3D11_TEXTURE2D_DESC depth_desc = {};
  static_cast<ID3D11Texture2D*>(depth_resource)->GetDesc(&depth_desc);
  depth_resource->Release();
  // Only the main view's depth: shadow maps, the velocity pass and other targets have other sizes.
  if (static_cast<float>(depth_desc.Width) != view_queue.render_width || static_cast<float>(depth_desc.Height) != view_queue.render_height
      || depth_desc.SampleDesc.Count != 1u) {
    release_render_targets();
    return false;
  }

  // The character targets follow the main depth: a resolution change without a new device recreates them.
  if (resolve_state.character_mask != nullptr) {
    D3D11_TEXTURE2D_DESC mask_desc = {};
    resolve_state.character_mask->GetDesc(&mask_desc);
    if (mask_desc.Width != depth_desc.Width || mask_desc.Height != depth_desc.Height) {
      ReleaseCharacterTargets();
    }
  }
  if (resolve_state.character_mask == nullptr) {
    ID3D11Device* device = nullptr;
    context->GetDevice(&device);
    D3D11_TEXTURE2D_DESC mask_desc = {
        .Width = depth_desc.Width,
        .Height = depth_desc.Height,
        .MipLevels = 1u,
        .ArraySize = 1u,
        .Format = DXGI_FORMAT_R8_UNORM,
        .SampleDesc = {.Count = 1u},
        .Usage = D3D11_USAGE_DEFAULT,
        .BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
    };
    D3D11_TEXTURE2D_DESC motion_desc = mask_desc;
    // Full precision: in NDC, half floats step by about half a pixel once the camera pans.
    motion_desc.Format = DXGI_FORMAT_R32G32_FLOAT;
    D3D11_TEXTURE2D_DESC character_depth_desc = mask_desc;
    character_depth_desc.Format = DXGI_FORMAT_R32_FLOAT;
    const D3D11_DEPTH_STENCIL_DESC depth_state_desc = {
        .DepthEnable = TRUE,
        .DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO,
        .DepthFunc = D3D11_COMPARISON_EQUAL,
    };
    // Drawn as depth, read by the DLAA input pass as the tessellated surfaces' device depth.
    D3D11_TEXTURE2D_DESC tessellated_depth_desc = mask_desc;
    tessellated_depth_desc.Format = DXGI_FORMAT_R32_TYPELESS;
    tessellated_depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    const D3D11_DEPTH_STENCIL_VIEW_DESC tessellated_dsv_desc = {.Format = DXGI_FORMAT_D32_FLOAT, .ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D};
    const D3D11_SHADER_RESOURCE_VIEW_DESC tessellated_srv_desc = {
        .Format = DXGI_FORMAT_R32_FLOAT, .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D, .Texture2D = {.MipLevels = 1u}};
    // The game's depth buffer is inverted: nearer is greater.
    const D3D11_DEPTH_STENCIL_DESC tessellated_depth_state_desc = {
        .DepthEnable = TRUE,
        .DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL,
        .DepthFunc = D3D11_COMPARISON_GREATER,
    };
    const std::array<D3D11_INPUT_ELEMENT_DESC, 2> motion_input_elements = {{
        {.SemanticName = "CURRENT", .Format = DXGI_FORMAT_R32G32B32A32_FLOAT, .InputSlot = 0u, .InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA},
        {.SemanticName = "PREVIOUS", .Format = DXGI_FORMAT_R32G32B32A32_FLOAT, .InputSlot = 1u, .InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA},
    }};
    const bool created = SUCCEEDED(device->CreateTexture2D(&mask_desc, nullptr, &resolve_state.character_mask))
                         && SUCCEEDED(device->CreateRenderTargetView(resolve_state.character_mask, nullptr, &resolve_state.character_mask_rtv))
                         && SUCCEEDED(device->CreateShaderResourceView(resolve_state.character_mask, nullptr, &resolve_state.character_mask_srv))
                         && SUCCEEDED(device->CreatePixelShader(__dlaa_character_mask.data(), __dlaa_character_mask.size(), nullptr, &resolve_state.character_mask_shader))
                         && SUCCEEDED(device->CreateDepthStencilState(&depth_state_desc, &resolve_state.character_mask_depth_state))
                         && SUCCEEDED(device->CreateTexture2D(&motion_desc, nullptr, &resolve_state.character_motion))
                         && SUCCEEDED(device->CreateRenderTargetView(resolve_state.character_motion, nullptr, &resolve_state.character_motion_rtv))
                         && SUCCEEDED(device->CreateShaderResourceView(resolve_state.character_motion, nullptr, &resolve_state.character_motion_srv))
                         && SUCCEEDED(device->CreateTexture2D(&character_depth_desc, nullptr, &resolve_state.character_depth))
                         && SUCCEEDED(device->CreateRenderTargetView(resolve_state.character_depth, nullptr, &resolve_state.character_depth_rtv))
                         && SUCCEEDED(device->CreateShaderResourceView(resolve_state.character_depth, nullptr, &resolve_state.character_depth_srv))
                         && SUCCEEDED(device->CreateVertexShader(__dlaa_motion_vertex.data(), __dlaa_motion_vertex.size(), nullptr, &resolve_state.motion_vertex_shader))
                         && SUCCEEDED(device->CreatePixelShader(__dlaa_motion_pixel.data(), __dlaa_motion_pixel.size(), nullptr, &resolve_state.motion_pixel_shader))
                         && SUCCEEDED(device->CreateInputLayout(motion_input_elements.data(), static_cast<UINT>(motion_input_elements.size()),
                                                                __dlaa_motion_vertex.data(), __dlaa_motion_vertex.size(), &resolve_state.motion_input_layout))
                         && SUCCEEDED(device->CreateTexture2D(&motion_desc, nullptr, &resolve_state.tessellated_motion))
                         && SUCCEEDED(device->CreateRenderTargetView(resolve_state.tessellated_motion, nullptr, &resolve_state.tessellated_motion_rtv))
                         && SUCCEEDED(device->CreateShaderResourceView(resolve_state.tessellated_motion, nullptr, &resolve_state.tessellated_motion_srv))
                         && SUCCEEDED(device->CreateTexture2D(&tessellated_depth_desc, nullptr, &resolve_state.tessellated_depth))
                         && SUCCEEDED(device->CreateDepthStencilView(resolve_state.tessellated_depth, &tessellated_dsv_desc, &resolve_state.tessellated_depth_dsv))
                         && SUCCEEDED(device->CreateShaderResourceView(resolve_state.tessellated_depth, &tessellated_srv_desc, &resolve_state.tessellated_depth_srv))
                         && SUCCEEDED(device->CreateDepthStencilState(&tessellated_depth_state_desc, &resolve_state.tessellated_depth_state))
                         && SUCCEEDED(device->CreateVertexShader(__dlaa_motion_world_vertex.data(), __dlaa_motion_world_vertex.size(), nullptr, &resolve_state.motion_world_vertex_shader))
                         && SUCCEEDED(device->CreatePixelShader(__dlaa_tessellated_mask.data(), __dlaa_tessellated_mask.size(), nullptr, &resolve_state.tessellated_mask_shader));
    device->Release();
    if (!created) {
      ReleaseCharacterTargets();
      FailDlaa("cannot create the character targets");
      release_render_targets();
      return false;
    }
    constexpr std::array<float, 4> CLEAR_COLOR = {0.f, 0.f, 0.f, 0.f};
    context->ClearRenderTargetView(resolve_state.character_mask_rtv, CLEAR_COLOR.data());
    context->ClearRenderTargetView(resolve_state.character_depth_rtv, CLEAR_COLOR.data());
    constexpr std::array<float, 4> MOTION_CLEAR_COLOR = {CHARACTER_MOTION_UNKNOWN, CHARACTER_MOTION_UNKNOWN, 0.f, 0.f};
    context->ClearRenderTargetView(resolve_state.character_motion_rtv, MOTION_CLEAR_COLOR.data());
    context->ClearRenderTargetView(resolve_state.tessellated_motion_rtv, MOTION_CLEAR_COLOR.data());
    context->ClearDepthStencilView(resolve_state.tessellated_depth_dsv, D3D11_CLEAR_DEPTH, 0.f, 0u);
  }

  issue_draw();

  // Stream-out sees each triangle once as a list: a list (or a patch list of three control points, streamed out before
  // tessellation) keeps its count, a strip of n indices yields n - 2 triangles.
  D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
  context->IAGetPrimitiveTopology(&topology);
  UINT streamed_vertices = 0u;
  if (topology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST || topology == D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST) {
    streamed_vertices = draw_range[0];
  } else if (topology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP && draw_range[0] > 2u) {
    streamed_vertices = (draw_range[0] - 2u) * 3u;
  }
  ID3D11HullShader* hull_shader = nullptr;
  context->HSGetShader(&hull_shader, nullptr, nullptr);
  ID3D11DomainShader* domain_shader = nullptr;
  context->DSGetShader(&domain_shader, nullptr, nullptr);
  const bool tessellated = hull_shader != nullptr;
  // Drawing the vertex shader's outputs as plain triangles: the stream-out and the motion pass of a tessellated draw.
  auto set_tessellation = [&](bool enabled) {
    if (!tessellated) return;
    context->HSSetShader(enabled ? hull_shader : nullptr, nullptr, 0u);
    context->DSSetShader(enabled ? domain_shader : nullptr, nullptr, 0u);
    context->IASetPrimitiveTopology(enabled ? topology : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  };

  bool has_motion = false;
  // Last frame's copy of the view buffer (b1, or the domain shader's b5), for the motion pass; a tessellated draw also
  // needs this frame's.
  ID3D11Buffer* previous_view_constants = nullptr;
  ID3D11Buffer* current_domain_view = nullptr;
  if (tessellated) {
    context->DSGetConstantBuffers(5u, 1u, &current_domain_view);
  }
  if (stream_out_shader != nullptr && draw_range[1] == 1u && streamed_vertices != 0u && (!tessellated || current_domain_view != nullptr)) {
    ID3D11Buffer* index_buffer = nullptr;
    DXGI_FORMAT index_format = DXGI_FORMAT_UNKNOWN;
    UINT index_offset = 0u;
    context->IAGetIndexBuffer(&index_buffer, &index_format, &index_offset);
    // FNV-1a over what identifies the draw's mesh, not its vertex buffers, which cloth alternates between frames; the
    // occurrence counter then separates meshes drawn more than once.
    std::uint64_t draw_key = 14695981039346656037ull;
    auto mix = [&](std::uint64_t value) {
      draw_key = (draw_key ^ value) * 1099511628211ull;
    };
    for (std::uint64_t value : {static_cast<std::uint64_t>(vertex_shader), reinterpret_cast<std::uint64_t>(index_buffer), static_cast<std::uint64_t>(index_offset),
                                static_cast<std::uint64_t>(draw_range[0]), static_cast<std::uint64_t>(draw_range[2]), static_cast<std::uint64_t>(draw_range[3])}) {
      mix(value);
    }
    if (index_buffer != nullptr) {
      index_buffer->Release();
    }
    mix(resolve_state.draw_occurrences[draw_key]++);

    ID3D11Device* device = nullptr;
    context->GetDevice(&device);
    std::array<ID3D11Buffer*, VERTEX_CONSTANT_SLOTS> current_constants = {};
    context->VSGetConstantBuffers(0u, VERTEX_CONSTANT_SLOTS, current_constants.data());
    std::array<ID3D11Buffer*, CAPTURED_VERTEX_BUFFER_SLOTS> current_vertex_buffers = {};
    std::array<UINT, CAPTURED_VERTEX_BUFFER_SLOTS> vertex_strides = {};
    std::array<UINT, CAPTURED_VERTEX_BUFFER_SLOTS> vertex_offsets = {};
    context->IAGetVertexBuffers(0u, CAPTURED_VERTEX_BUFFER_SLOTS, current_vertex_buffers.data(), vertex_strides.data(), vertex_offsets.data());
    CapturedConstants& captured = resolve_state.captured_constants[draw_key];
    const std::uint32_t previous_set = captured.latest;
    const std::uint32_t current_set = 1u - captured.latest;
    // A draw keeps its data from its second frame in a row and gets motion from its third: draws seen only once, such as
    // those reading a ring buffer at new offsets every frame, keep nothing.
    const bool seen_last_frame = captured.frame != UINT64_MAX && captured.frame + 1u == resolve_state.frame_index;
    const UINT required_bytes = streamed_vertices * static_cast<UINT>(sizeof(float) * 4u);
    bool has_previous = false;
    // Dynamic and CPU-rewritten vertex buffers hold this frame's shape: a tessellated draw's previous pass reads last
    // frame's copy.
    std::array<ID3D11Buffer*, CAPTURED_VERTEX_BUFFER_SLOTS> previous_vertex_buffers = current_vertex_buffers;
    std::array<UINT, CAPTURED_VERTEX_BUFFER_SLOTS> previous_vertex_offsets = vertex_offsets;
    if (tessellated) {
      has_previous = captured.copied_frame != UINT64_MAX && captured.copied_frame + 1u == resolve_state.frame_index
                     && captured.domain_views[previous_set] != nullptr;
      bool copied = true;
      // Copies a game buffer into same-sized copy (recreated when the size changed); a null source clears the copy.
      auto copy_buffer = [&](ID3D11Buffer* source, UINT bind_flags, ID3D11Buffer** copy) {
        if (!seen_last_frame) return;
        if (source == nullptr) {
          if (*copy != nullptr) {
            (*copy)->Release();
          }
          *copy = nullptr;
          return;
        }
        D3D11_BUFFER_DESC game_desc = {};
        source->GetDesc(&game_desc);
        if (*copy != nullptr) {
          D3D11_BUFFER_DESC copy_desc = {};
          (*copy)->GetDesc(&copy_desc);
          if (copy_desc.ByteWidth != game_desc.ByteWidth) {
            (*copy)->Release();
            *copy = nullptr;
          }
        }
        if (*copy == nullptr) {
          const D3D11_BUFFER_DESC copy_desc = {.ByteWidth = game_desc.ByteWidth, .Usage = D3D11_USAGE_DEFAULT, .BindFlags = bind_flags};
          if (const HRESULT result = device->CreateBuffer(&copy_desc, nullptr, copy); FAILED(result)) {
            WarnOnce("cannot copy a tessellated mesh's constants or vertices; it keeps the camera's motion next frame", result);
            copied = false;
            return;
          }
        }
        context->CopyResource(*copy, source);
      };
      for (UINT slot = 0u; slot < VERTEX_CONSTANT_SLOTS; ++slot) {
        copy_buffer(current_constants[slot], D3D11_BIND_CONSTANT_BUFFER, &captured.buffers[current_set][slot]);
      }
      copy_buffer(current_domain_view, D3D11_BIND_CONSTANT_BUFFER, &captured.domain_views[current_set]);
      for (UINT slot = 0u; slot < CAPTURED_VERTEX_BUFFER_SLOTS; ++slot) {
        ID3D11Buffer* dynamic_buffer = nullptr;
        if (current_vertex_buffers[slot] != nullptr) {
          D3D11_BUFFER_DESC desc = {};
          current_vertex_buffers[slot]->GetDesc(&desc);
          if (desc.Usage == D3D11_USAGE_DYNAMIC || rewritten_every_frame(current_vertex_buffers[slot])) {
            dynamic_buffer = current_vertex_buffers[slot];
          }
        }
        copy_buffer(dynamic_buffer, D3D11_BIND_VERTEX_BUFFER, &captured.vertex_buffers[current_set][slot]);
        captured.vertex_offsets[current_set][slot] = vertex_offsets[slot];
        if (captured.vertex_buffers[previous_set][slot] != nullptr && captured.vertex_buffers[current_set][slot] != nullptr) {
          previous_vertex_buffers[slot] = captured.vertex_buffers[previous_set][slot];
          previous_vertex_offsets[slot] = captured.vertex_offsets[previous_set][slot];
        }
      }
      if (seen_last_frame) {
        captured.latest = current_set;
        captured.copied_frame = (copied ? resolve_state.frame_index : UINT64_MAX);
      }
    } else {
      // The view buffer once per frame; motion also needs last frame's.
      const std::uint32_t view_slot = static_cast<std::uint32_t>(resolve_state.frame_index & 1u);
      if (resolve_state.view_copy_frames[view_slot] != resolve_state.frame_index && current_constants[1] != nullptr) {
        D3D11_BUFFER_DESC view_desc = {};
        current_constants[1]->GetDesc(&view_desc);
        ID3D11Buffer*& view_copy = resolve_state.view_copies[view_slot];
        if (view_copy != nullptr) {
          D3D11_BUFFER_DESC copy_desc = {};
          view_copy->GetDesc(&copy_desc);
          if (copy_desc.ByteWidth != view_desc.ByteWidth) {
            view_copy->Release();
            view_copy = nullptr;
          }
        }
        if (view_copy == nullptr) {
          const D3D11_BUFFER_DESC copy_desc = {.ByteWidth = view_desc.ByteWidth, .Usage = D3D11_USAGE_DEFAULT, .BindFlags = D3D11_BIND_CONSTANT_BUFFER};
          if (const HRESULT result = device->CreateBuffer(&copy_desc, nullptr, &view_copy); FAILED(result)) {
            WarnOnce("cannot create the view buffer copy; non-tessellated moving draws keep the camera's motion until two frames in a row get a copy", result);
          }
        }
        if (view_copy != nullptr) {
          context->CopyResource(view_copy, current_constants[1]);
          resolve_state.view_copy_frames[view_slot] = resolve_state.frame_index;
        }
      }
      ID3D11Buffer* previous_positions = captured.positions[captured.latest_positions];
      D3D11_BUFFER_DESC positions_desc = {};
      if (previous_positions != nullptr) {
        previous_positions->GetDesc(&positions_desc);
      }
      has_previous = previous_positions != nullptr && captured.positions_frame + 1u == resolve_state.frame_index && positions_desc.ByteWidth == required_bytes
                     && resolve_state.view_copy_frames[view_slot] == resolve_state.frame_index
                     && resolve_state.view_copy_frames[1u - view_slot] + 1u == resolve_state.frame_index;
    }
    captured.frame = resolve_state.frame_index;

    D3D11_VIEWPORT viewport = {};
    UINT viewport_count = 1u;
    context->RSGetViewports(&viewport_count, &viewport);
    D3D11_RASTERIZER_DESC motion_rasterizer_desc = {.FillMode = D3D11_FILL_SOLID, .CullMode = D3D11_CULL_BACK, .DepthClipEnable = TRUE};
    ID3D11RasterizerState* game_rasterizer_state = nullptr;
    context->RSGetState(&game_rasterizer_state);
    if (game_rasterizer_state != nullptr) {
      game_rasterizer_state->GetDesc(&motion_rasterizer_desc);
      game_rasterizer_state->Release();
    }
    motion_rasterizer_desc.CullMode = D3D11_CULL_NONE;
    motion_rasterizer_desc.ScissorEnable = FALSE;
    if (tessellated && has_previous && required_bytes > resolve_state.stream_out_capacity) {
      for (ID3D11Buffer*& positions : resolve_state.stream_out_positions) {
        if (positions != nullptr) {
          positions->Release();
        }
        positions = nullptr;
      }
      resolve_state.stream_out_capacity = 0u;
      const D3D11_BUFFER_DESC positions_desc = {
          .ByteWidth = std::max(required_bytes, 8u << 20u), .Usage = D3D11_USAGE_DEFAULT, .BindFlags = D3D11_BIND_STREAM_OUTPUT | D3D11_BIND_VERTEX_BUFFER};
      HRESULT positions_result = device->CreateBuffer(&positions_desc, nullptr, &resolve_state.stream_out_positions[0]);
      if (SUCCEEDED(positions_result)) {
        positions_result = device->CreateBuffer(&positions_desc, nullptr, &resolve_state.stream_out_positions[1]);
      }
      if (SUCCEEDED(positions_result)) {
        resolve_state.stream_out_capacity = positions_desc.ByteWidth;
      } else {
        WarnOnce("cannot create the tessellated stream-out buffers; tessellated meshes keep the camera's motion", positions_result);
      }
    }
    // A draw seen last frame streams this frame's positions into the buffer that does not hold last frame's.
    ID3D11Buffer** current_positions = &captured.positions[1u - captured.latest_positions];
    if (!tessellated && seen_last_frame) {
      D3D11_BUFFER_DESC positions_desc = {};
      if (*current_positions != nullptr) {
        (*current_positions)->GetDesc(&positions_desc);
      }
      if (*current_positions != nullptr && positions_desc.ByteWidth != required_bytes) {
        (*current_positions)->Release();
        *current_positions = nullptr;
      }
      if (*current_positions == nullptr) {
        const D3D11_BUFFER_DESC kept_desc = {
            .ByteWidth = required_bytes, .Usage = D3D11_USAGE_DEFAULT, .BindFlags = D3D11_BIND_STREAM_OUTPUT | D3D11_BIND_VERTEX_BUFFER};
        if (const HRESULT result = device->CreateBuffer(&kept_desc, nullptr, current_positions); FAILED(result)) {
          WarnOnce("cannot create a moving object's position buffer; it keeps the camera's motion", result);
        }
      }
    }

    const bool streams = (tessellated ? has_previous && required_bytes <= resolve_state.stream_out_capacity
                                      : seen_last_frame && *current_positions != nullptr);
    if (streams) {
      ID3D11GeometryShader* saved_geometry_shader = nullptr;
      context->GSGetShader(&saved_geometry_shader, nullptr, nullptr);
      std::array<ID3D11Buffer*, D3D11_SO_BUFFER_SLOT_COUNT> saved_stream_out = {};
      context->SOGetTargets(D3D11_SO_BUFFER_SLOT_COUNT, saved_stream_out.data());
      context->GSSetShader(stream_out_shader, nullptr, 0u);
      set_tessellation(false);
      if (tessellated) {
        constexpr UINT STREAM_OUT_OFFSET = 0u;
        context->VSSetConstantBuffers(0u, VERTEX_CONSTANT_SLOTS, captured.buffers[previous_set].data());
        context->IASetVertexBuffers(0u, CAPTURED_VERTEX_BUFFER_SLOTS, previous_vertex_buffers.data(), vertex_strides.data(), previous_vertex_offsets.data());
        context->SOSetTargets(1u, &resolve_state.stream_out_positions[1], &STREAM_OUT_OFFSET);
        issue_draw();
        context->IASetVertexBuffers(0u, CAPTURED_VERTEX_BUFFER_SLOTS, current_vertex_buffers.data(), vertex_strides.data(), vertex_offsets.data());
        context->VSSetConstantBuffers(0u, VERTEX_CONSTANT_SLOTS, current_constants.data());
        context->SOSetTargets(1u, &resolve_state.stream_out_positions[0], &STREAM_OUT_OFFSET);
        issue_draw();
      } else {
        constexpr UINT STREAM_OUT_OFFSET = 0u;
        context->SOSetTargets(1u, current_positions, &STREAM_OUT_OFFSET);
        issue_draw();
      }
      // An offset of -1 appends where each restored target left off.
      constexpr std::array<UINT, D3D11_SO_BUFFER_SLOT_COUNT> APPEND_OFFSETS = {UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX};
      context->SOSetTargets(D3D11_SO_BUFFER_SLOT_COUNT, saved_stream_out.data(), APPEND_OFFSETS.data());
      context->GSSetShader(saved_geometry_shader, nullptr, 0u);
      set_tessellation(true);
      for (IUnknown* saved : std::initializer_list<IUnknown*>{saved_geometry_shader, saved_stream_out[0], saved_stream_out[1], saved_stream_out[2], saved_stream_out[3]}) {
        if (saved != nullptr) {
          saved->Release();
        }
      }
      if (!tessellated) {
        // This frame's positions become next frame's previous ones.
        captured.latest_positions = 1u - captured.latest_positions;
        captured.positions_frame = resolve_state.frame_index;
      }
      if (has_previous) {
        has_motion = true;
        previous_view_constants = captured.domain_views[previous_set];
        if (!tessellated) {
          if (resolve_state.motion_batches.empty()) {
            const std::uint32_t view_slot = static_cast<std::uint32_t>(resolve_state.frame_index & 1u);
            resolve_state.batch_view = resolve_state.view_copies[view_slot];
            resolve_state.batch_previous_view = resolve_state.view_copies[1u - view_slot];
            for (IUnknown* object : std::initializer_list<IUnknown*>{resolve_state.batch_view, resolve_state.batch_previous_view}) {
              if (object != nullptr) {
                object->AddRef();
              }
            }
          }
          auto batch = std::find_if(resolve_state.motion_batches.begin(), resolve_state.motion_batches.end(), [&](const MotionBatch& candidate) {
            return candidate.depth_view == depth_view && std::memcmp(&candidate.viewport, &viewport, sizeof(viewport)) == 0
                   && std::memcmp(&candidate.rasterizer_desc, &motion_rasterizer_desc, sizeof(motion_rasterizer_desc)) == 0;
          });
          if (batch == resolve_state.motion_batches.end()) {
            MotionBatch& added = resolve_state.motion_batches.emplace_back(
                MotionBatch{.depth_view = depth_view, .viewport = viewport, .rasterizer_desc = motion_rasterizer_desc});
            depth_view->AddRef();
            if (const HRESULT result = device->CreateRasterizerState(&motion_rasterizer_desc, &added.rasterizer_state); FAILED(result)) {
              WarnOnce("cannot create a motion batch's rasterizer state; its motion is drawn with the default rasterizer state (back-face culling, no depth bias)", result);
            }
            batch = std::prev(resolve_state.motion_batches.end());
          }
          const MotionObject& object = batch->objects.emplace_back(
              MotionObject{.current = captured.positions[captured.latest_positions], .previous = captured.positions[1u - captured.latest_positions]});
          object.current->AddRef();
          object.previous->AddRef();
          ++resolve_state.motion_draws;
        }
      }
    }
    device->Release();
    for (ID3D11Buffer* buffer : current_constants) {
      if (buffer != nullptr) {
        buffer->Release();
      }
    }
    for (ID3D11Buffer* buffer : current_vertex_buffers) {
      if (buffer != nullptr) {
        buffer->Release();
      }
    }
  }

  // Batched draws with motion are done; the others draw their mask (and a tessellated draw its motion) here.
  if (has_motion && !tessellated) {
    for (IUnknown* saved : std::initializer_list<IUnknown*>{hull_shader, domain_shader, current_domain_view}) {
      if (saved != nullptr) {
        saved->Release();
      }
    }
    release_render_targets();
    return true;
  }
  ID3D11DepthStencilState* saved_depth_state = nullptr;
  UINT saved_stencil_ref = 0u;
  context->OMGetDepthStencilState(&saved_depth_state, &saved_stencil_ref);
  ID3D11BlendState* saved_blend_state = nullptr;
  std::array<float, 4> saved_blend_factor = {};
  UINT saved_sample_mask = 0u;
  context->OMGetBlendState(&saved_blend_state, saved_blend_factor.data(), &saved_sample_mask);
  ID3D11PixelShader* saved_pixel_shader = nullptr;
  context->PSGetShader(&saved_pixel_shader, nullptr, nullptr);
  context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
  const std::array<ID3D11RenderTargetView*, 3> character_targets = {resolve_state.character_mask_rtv, resolve_state.character_motion_rtv, resolve_state.character_depth_rtv};
  if (has_motion) {
    // The untessellated triangles miss the tessellated surface's depth: their motion goes to its own target, nearest
    // triangle first, and the real surface is marked below.
    const std::array<ID3D11RenderTargetView*, 3> tessellated_targets = {nullptr, resolve_state.tessellated_motion_rtv, nullptr};
    context->OMSetRenderTargets(static_cast<UINT>(tessellated_targets.size()), tessellated_targets.data(), resolve_state.tessellated_depth_dsv);
    context->OMSetDepthStencilState(resolve_state.tessellated_depth_state, 0u);
    ID3D11InputLayout* saved_input_layout = nullptr;
    context->IAGetInputLayout(&saved_input_layout);
    std::array<ID3D11Buffer*, 2> saved_vertex_buffers = {};
    std::array<UINT, 2> saved_strides = {};
    std::array<UINT, 2> saved_offsets = {};
    context->IAGetVertexBuffers(0u, 2u, saved_vertex_buffers.data(), saved_strides.data(), saved_offsets.data());
    ID3D11VertexShader* saved_vertex_shader = nullptr;
    context->VSGetShader(&saved_vertex_shader, nullptr, nullptr);
    ID3D11GeometryShader* saved_geometry_shader = nullptr;
    context->GSGetShader(&saved_geometry_shader, nullptr, nullptr);
    ID3D11Buffer* saved_constants_b1 = nullptr;
    context->VSGetConstantBuffers(1u, 1u, &saved_constants_b1);
    ID3D11Buffer* saved_constants_b11 = nullptr;
    context->VSGetConstantBuffers(11u, 1u, &saved_constants_b11);
    context->VSSetConstantBuffers(11u, 1u, &previous_view_constants);
    context->VSSetConstantBuffers(1u, 1u, &current_domain_view);
    set_tessellation(false);

    constexpr std::array<UINT, 2> POSITION_STRIDES = {sizeof(float) * 4u, sizeof(float) * 4u};
    constexpr std::array<UINT, 2> POSITION_OFFSETS = {0u, 0u};
    context->IASetInputLayout(resolve_state.motion_input_layout);
    context->IASetVertexBuffers(0u, 2u, resolve_state.stream_out_positions.data(), POSITION_STRIDES.data(), POSITION_OFFSETS.data());
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(resolve_state.motion_world_vertex_shader, nullptr, 0u);
    // A geometry shader of the game's draw would take the motion vertex shader's outputs.
    context->GSSetShader(nullptr, nullptr, 0u);
    context->PSSetShader(resolve_state.motion_pixel_shader, nullptr, 0u);
    // The vertex count is what the current frame's stream-out wrote into the buffer in slot 0.
    context->DrawAuto();

    context->IASetInputLayout(saved_input_layout);
    context->IASetVertexBuffers(0u, 2u, saved_vertex_buffers.data(), saved_strides.data(), saved_offsets.data());
    context->IASetPrimitiveTopology(topology);
    set_tessellation(true);
    context->VSSetShader(saved_vertex_shader, nullptr, 0u);
    context->GSSetShader(saved_geometry_shader, nullptr, 0u);
    context->VSSetConstantBuffers(1u, 1u, &saved_constants_b1);
    context->VSSetConstantBuffers(11u, 1u, &saved_constants_b11);
    for (IUnknown* saved : std::initializer_list<IUnknown*>{saved_input_layout, saved_vertex_buffers[0], saved_vertex_buffers[1], saved_vertex_shader,
                                                            saved_geometry_shader, saved_constants_b1, saved_constants_b11}) {
      if (saved != nullptr) {
        saved->Release();
      }
    }
    ++resolve_state.motion_draws;
  }
  context->OMSetRenderTargets(static_cast<UINT>(character_targets.size()), character_targets.data(), depth_view);
  context->OMSetDepthStencilState(resolve_state.character_mask_depth_state, 0u);
  context->PSSetShader((has_motion ? resolve_state.tessellated_mask_shader : resolve_state.character_mask_shader), nullptr, 0u);
  issue_draw();
  if (!has_motion) {
    ++resolve_state.motion_fallbacks;
  }

  context->OMSetRenderTargets(static_cast<UINT>(saved_render_targets.size()), saved_render_targets.data(), depth_view);
  context->OMSetDepthStencilState(saved_depth_state, saved_stencil_ref);
  context->OMSetBlendState(saved_blend_state, saved_blend_factor.data(), saved_sample_mask);
  context->PSSetShader(saved_pixel_shader, nullptr, 0u);
  for (IUnknown* saved : std::initializer_list<IUnknown*>{saved_depth_state, saved_blend_state, saved_pixel_shader, hull_shader, domain_shader, current_domain_view}) {
    if (saved != nullptr) {
      saved->Release();
    }
  }
  release_render_targets();
  return true;
}

}  // namespace batmanac::dlaa
