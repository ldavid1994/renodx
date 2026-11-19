/*
 * Copyright (C) 2026 RenoDX contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <d3d11.h>
#include <dxgi.h>
#include <Windows.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>

#include <embed/shaders.h>
#include <include/reshade.hpp>

#include "../../../utils/detour.hpp"
#include "../../../utils/hash.hpp"
#include "../../../utils/settings.hpp"
#include "../../../utils/state.hpp"
#include "./host_link.hpp"
#include "./upscaling_motion.hpp"
#include "./upscaling_particles.hpp"
#include "./upscaling_state.hpp"
#include "../shared.h"

// Temporal anti-aliasing (NVIDIA DLAA or AMD FSR at native resolution) for a game without it. The player view's projection matrix is jittered where
// ULocalPlayer::CalcSceneView hands it to the FSceneView constructor: every matrix the renderer derives from it
// (view-projection, its inverse, screen to shadow) stays consistent. Before the first post pass, which reads the scene
// with 1 - device depth in alpha, compute passes write DLSS's inputs, the 64-bit host runs the upscaler and its output replaces
// the scene.
namespace batmanac::upscaling {

static void* scene_view_trampoline = nullptr;
// The game's device: the first one created while none is tracked. Other devices leave the upscaler alone.
static reshade::api::device* game_device = nullptr;

static void __cdecl OnCalcSceneView(Matrix4* view_matrix, Matrix4* projection_matrix) {
  const std::unique_lock lock(view_queue.mutex);
  ViewMatrices& view = view_queue.pending_views.emplace_back(ViewMatrices{.view_matrix = *view_matrix, .projection_matrix = *projection_matrix});
  while (view_queue.pending_views.size() > 4u) {
    view_queue.pending_views.pop_front();
  }

  if (aa_enabled == 0.f || !aa_resolving.load() || view_queue.render_width == 0.f || view_queue.render_height == 0.f) return;

  auto halton = [](std::uint32_t index, std::uint32_t base) {
    float fraction = 1.f;
    float result = 0.f;
    while (index > 0u) {
      fraction /= static_cast<float>(base);
      result += fraction * static_cast<float>(index % base);
      index /= base;
    }
    return result;
  };
  // DLSS recommends a long Halton sequence for DLAA.
  constexpr std::uint32_t PHASE_COUNT = 32u;
  const std::uint32_t sample = (view_queue.jitter_index++ % PHASE_COUNT) + 1u;
  const float jitter_x_pixels = halton(sample, 2u) - 0.5f;
  const float jitter_y_pixels = halton(sample, 3u) - 0.5f;
  view.jitter_pixels = {jitter_x_pixels, jitter_y_pixels};
  // M[2][3] is 1, so clip w is view depth: adding to M[2][0..1] shifts the image by a constant NDC offset.
  // NDC y points up, pixel rows go down.
  projection_matrix->m[8] += 2.f * jitter_x_pixels / view_queue.render_width;
  projection_matrix->m[9] -= 2.f * jitter_y_pixels / view_queue.render_height;
}

// Patched over CalcSceneView's `push 8; push 0x710` before `appMalloc(sizeof(FSceneView))`. There, esp points
// at the function's locals: ViewMatrix at +0x114 and ProjectionMatrix at +0x194. pushad and pushfd add 36 bytes.
__declspec(naked) static void SceneViewAllocationStub() {
  __asm {
    pushad
    pushfd
    lea eax, [esp + 36 + 0x194]
    push eax
    lea eax, [esp + 40 + 0x114]
    push eax
    call OnCalcSceneView
    add esp, 8
    popfd
    popad
    jmp scene_view_trampoline
  }
}

static void DestroyOutputs(reshade::api::device* device) {
  for (auto& item : resolve_state.outputs) {
    if (item.srv.handle != 0u) {
      device->destroy_resource_view(item.srv);
    }
    if (item.uav.handle != 0u) {
      device->destroy_resource_view(item.uav);
    }
    if (item.resource.handle != 0u) {
      device->destroy_resource(item.resource);
    }
    item = {};
  }
  for (auto& [scene, view] : resolve_state.scene_views) {
    view->Release();
  }
  resolve_state.scene_views.clear();
  resolve_state.width = 0u;
  resolve_state.height = 0u;
}

constexpr UINT UPSCALING_SHADER_RESOURCES = 6u;

// scene_srv: a view of the scene, or null when the scene is the bound render target (it is then unbound and read through
// a cached view).
static bool Resolve(reshade::api::command_list* cmd_list, reshade::api::resource scene, reshade::api::resource_view scene_srv) {
  DrawBatchedMotion(reinterpret_cast<ID3D11DeviceContext*>(cmd_list->get_native()));
  auto* device = cmd_list->get_device();
  const reshade::api::resource_desc scene_desc = device->get_resource_desc(scene);
  // With MSAA on, the full-screen scene and the main depth are multisampled: the upscaler would only run later on the game's
  // resolved copy, without the characters' own motion, on top of MSAA's cost. It turns itself off instead.
  if (scene_desc.texture.samples != 1u && static_cast<float>(scene_desc.texture.width) == view_queue.render_width
      && static_cast<float>(scene_desc.texture.height) == view_queue.render_height) {
    TurnOffAntiAliasing("the scene is multisampled; turn MSAA off in the game's launcher to use anti-aliasing");
    return false;
  }
  if (scene_desc.texture.samples != 1u || scene_desc.texture.format != reshade::api::format::r16g16b16a16_float) {
    ++resolve_state.scene_skips;
    return false;
  }

  ViewMatrices current_view;
  std::array<std::array<float, 2>, VIEW_CANDIDATES> candidate_jitter = {};
  std::uint32_t candidate_count = 0u;
  std::uint32_t used_position = 0u;
  resolve_state.last_attempt_frame = resolve_state.frame_index;
  {
    const std::unique_lock lock(view_queue.mutex);
    resolve_state.max_queue_size = std::max(resolve_state.max_queue_size, view_queue.pending_views.size());
    if (view_queue.pending_views.empty()) {
      // The game drew a frame without building a new view: the camera has not moved.
      ++resolve_state.empty_queue_reuses;
      current_view = resolve_state.previous_view;
    } else {
      candidate_count = std::min<std::uint32_t>(static_cast<std::uint32_t>(view_queue.pending_views.size()), VIEW_CANDIDATES);
      for (std::uint32_t i = 0u; i < candidate_count; ++i) {
        candidate_jitter[i] = view_queue.pending_views[i].jitter_pixels;
      }
      used_position = std::min(view_pairing.position, candidate_count - 1u);
      current_view = view_queue.pending_views[used_position];
      view_queue.pending_views.pop_front();
    }
  }

  if (resolve_state.layout.handle == 0u) {
    const std::array<reshade::api::pipeline_layout_param, 4> params = {
        reshade::api::pipeline_layout_param(reshade::api::descriptor_range{
            .dx_register_index = 0u, .count = UPSCALING_SHADER_RESOURCES, .visibility = reshade::api::shader_stage::compute, .type = reshade::api::descriptor_type::shader_resource_view}),
        reshade::api::pipeline_layout_param(reshade::api::descriptor_range{
            .dx_register_index = 0u, .count = 5u, .visibility = reshade::api::shader_stage::compute, .type = reshade::api::descriptor_type::unordered_access_view}),
        reshade::api::pipeline_layout_param(reshade::api::constant_range{
            .dx_register_index = 0u, .count = sizeof(UpscalingConstants) / sizeof(std::uint32_t), .visibility = reshade::api::shader_stage::compute}),
        reshade::api::pipeline_layout_param(reshade::api::constant_range{
            .dx_register_index = 13u, .count = sizeof(ShaderInjectData) / sizeof(std::uint32_t), .visibility = reshade::api::shader_stage::compute}),
    };
    auto create_compute_pipeline = [&](std::span<const std::uint8_t> code, reshade::api::pipeline* pipeline) {
      const reshade::api::shader_desc compute_shader = {.code = code.data(), .code_size = code.size()};
      const reshade::api::pipeline_subobject subobject = {
          .type = reshade::api::pipeline_subobject_type::compute_shader, .count = 1u, .data = const_cast<reshade::api::shader_desc*>(&compute_shader)};
      return device->create_pipeline(resolve_state.layout, 1u, &subobject, pipeline);
    };
    if (!device->create_pipeline_layout(static_cast<std::uint32_t>(params.size()), params.data(), &resolve_state.layout)
        || !create_compute_pipeline(__upscaling_prepare, &resolve_state.prepare_pipeline)
        || !create_compute_pipeline(__upscaling_composite, &resolve_state.composite_pipeline)) {
      TurnOffAntiAliasing("cannot create the compute pipelines");
      return false;
    }
  }

  bool history_valid = resolve_state.last_resolve_frame + 1u == resolve_state.frame_index;
  if (resolve_state.width != scene_desc.texture.width || resolve_state.height != scene_desc.texture.height) {
    DestroyOutputs(device);
    const reshade::api::resource_desc output_desc(
        scene_desc.texture.width, scene_desc.texture.height, 1u, 1u, scene_desc.texture.format, 1u,
        reshade::api::memory_heap::gpu_only,
        reshade::api::resource_usage::shader_resource | reshade::api::resource_usage::unordered_access | reshade::api::resource_usage::copy_source
            | reshade::api::resource_usage::copy_dest);
    for (auto& item : resolve_state.outputs) {
      if (!device->create_resource(output_desc, nullptr, reshade::api::resource_usage::shader_resource, &item.resource)
          || !device->create_resource_view(item.resource, reshade::api::resource_usage::shader_resource,
                                           reshade::api::resource_view_desc(scene_desc.texture.format), &item.srv)
          || !device->create_resource_view(item.resource, reshade::api::resource_usage::unordered_access,
                                           reshade::api::resource_view_desc(scene_desc.texture.format), &item.uav)) {
        DestroyOutputs(device);
        TurnOffAntiAliasing("cannot create the output textures");
        return false;
      }
    }
    resolve_state.width = scene_desc.texture.width;
    resolve_state.height = scene_desc.texture.height;
    history_valid = false;
  }

  using Matrix4d = std::array<double, 16>;
  auto multiply = [](const auto& a, const auto& b) {
    Matrix4d result = {};
    for (int row = 0; row < 4; ++row) {
      for (int column = 0; column < 4; ++column) {
        for (int k = 0; k < 4; ++k) {
          result[row * 4 + column] += static_cast<double>(a[row * 4 + k]) * static_cast<double>(b[k * 4 + column]);
        }
      }
    }
    return result;
  };
  // The shader rebuilds each pixel as a camera-relative point of the current view and maps it with
  // inverse(view) * previous view * previous projection (unjittered, in double): only the camera's relative motion
  // enters the float math, so distant surfaces keep precise motion.
  const Matrix4d previous_view_projection = multiply(resolve_state.previous_view.view_matrix.m, resolve_state.previous_view.projection_matrix.m);
  const std::array<float, 16> current_projection = current_view.projection_matrix.m;
  resolve_state.previous_view = current_view;

  // Gauss-Jordan inverse with partial pivoting.
  Matrix4d inverse_current = {};
  {
    Matrix4d work = {};
    for (int i = 0; i < 16; ++i) work[i] = static_cast<double>(current_view.view_matrix.m[i]);
    for (int i = 0; i < 4; ++i) inverse_current[i * 4 + i] = 1.0;
    for (int column = 0; column < 4; ++column) {
      int pivot = column;
      for (int row = column + 1; row < 4; ++row) {
        if (std::abs(work[row * 4 + column]) > std::abs(work[pivot * 4 + column])) {
          pivot = row;
        }
      }
      if (std::abs(work[pivot * 4 + column]) < 1e-12) return false;
      for (int k = 0; k < 4; ++k) {
        std::swap(work[column * 4 + k], work[pivot * 4 + k]);
        std::swap(inverse_current[column * 4 + k], inverse_current[pivot * 4 + k]);
      }
      const double scale = 1.0 / work[column * 4 + column];
      for (int k = 0; k < 4; ++k) {
        work[column * 4 + k] *= scale;
        inverse_current[column * 4 + k] *= scale;
      }
      for (int row = 0; row < 4; ++row) {
        if (row == column) continue;
        const double factor = work[row * 4 + column];
        for (int k = 0; k < 4; ++k) {
          work[row * 4 + k] -= factor * work[column * 4 + k];
          inverse_current[row * 4 + k] -= factor * inverse_current[column * 4 + k];
        }
      }
    }
  }
  const Matrix4d view_to_previous = multiply(inverse_current, previous_view_projection);
  // 1/P[0][0], 1/P[1][1], 1 - P[2][2], 1/P[3][2]: with the scene alpha (1 - device depth), a pixel's view point is
  // (ndc.x / P[0][0], ndc.y / P[1][1], 1, (1 - P[2][2] - alpha) / P[3][2]) up to scale.
  const std::array<float, 4> projection_terms = {
      1.f / current_projection[0], 1.f / current_projection[5], 1.f - current_projection[10], 1.f / current_projection[14]};
  // Screen-center motion at a mid-range depth (scene alpha 0.01): a camera cut moves it by a large part of the screen, or
  // behind the previous camera.
  const std::array<double, 4> center_point = {0.0, 0.0, 1.0, (static_cast<double>(projection_terms[2]) - 0.01) * projection_terms[3]};
  std::array<double, 4> previous_center_clip = {};
  for (int column = 0; column < 4; ++column) {
    for (int k = 0; k < 4; ++k) previous_center_clip[column] += center_point[k] * view_to_previous[k * 4 + column];
  }
  const double center_motion_x = previous_center_clip[0] / previous_center_clip[3] * 0.5 * resolve_state.width;
  const double center_motion_y = previous_center_clip[1] / previous_center_clip[3] * 0.5 * resolve_state.height;
  const double center_motion_pixels = std::sqrt(center_motion_x * center_motion_x + center_motion_y * center_motion_y);
  if (!(previous_center_clip[3] > 0.0) || !(center_motion_pixels < 0.25 * resolve_state.width)) {
    history_valid = false;
  }

  UpscalingConstants constants = {
      .projection_terms = projection_terms,
      // The mask exists once a skinned draw was marked; it is cleared after every present.
      .character_mask_valid = (resolve_state.character_mask_srv != nullptr ? 1.f : 0.f),
  };
  for (int i = 0; i < 16; ++i) {
    constants.view_to_previous_clip[i] = static_cast<float>(view_to_previous[i]);
  }

  auto* context = reinterpret_cast<ID3D11DeviceContext*>(cmd_list->get_native());
  // Without a view of the game's, the scene is the bound render target: the input pass reads it through a view of its own
  // while it is unbound, and the upscaler's output is drawn back over it.
  const bool scene_is_target = (scene_srv.handle == 0u);
  if (scene_is_target) {
    auto* scene_resource = reinterpret_cast<ID3D11Resource*>(scene.handle);
    if (resolve_state.scene_views.size() >= 4u && !resolve_state.scene_views.contains(scene_resource)) {
      for (auto& [cached_scene, view] : resolve_state.scene_views) {
        view->Release();
      }
      resolve_state.scene_views.clear();
    }
    ID3D11ShaderResourceView*& scene_view = resolve_state.scene_views[scene_resource];
    if (scene_view == nullptr || resolve_state.composite_blend_state == nullptr) {
      ID3D11Device* native_device = nullptr;
      context->GetDevice(&native_device);
      const D3D11_SHADER_RESOURCE_VIEW_DESC scene_view_desc = {
          .Format = DXGI_FORMAT_R16G16B16A16_FLOAT, .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D, .Texture2D = {.MipLevels = 1u}};
      D3D11_BLEND_DESC blend_desc = {};
      blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
      const bool created = (scene_view != nullptr || SUCCEEDED(native_device->CreateShaderResourceView(scene_resource, &scene_view_desc, &scene_view)))
                           && (resolve_state.composite_vertex_shader != nullptr
                               || SUCCEEDED(native_device->CreateVertexShader(__upscaling_composite_vertex.data(), __upscaling_composite_vertex.size(), nullptr, &resolve_state.composite_vertex_shader)))
                           && (resolve_state.composite_pixel_shader != nullptr
                               || SUCCEEDED(native_device->CreatePixelShader(__upscaling_composite_pixel.data(), __upscaling_composite_pixel.size(), nullptr, &resolve_state.composite_pixel_shader)))
                           && (resolve_state.composite_blend_state != nullptr || SUCCEEDED(native_device->CreateBlendState(&blend_desc, &resolve_state.composite_blend_state)));
      native_device->Release();
      if (!created) {
        if (scene_view != nullptr) {
          scene_view->Release();
        }
        resolve_state.scene_views.erase(scene_resource);
        TurnOffAntiAliasing("cannot create the composite draw");
        return false;
      }
    }
    scene_srv = reshade::api::resource_view{reinterpret_cast<std::uintptr_t>(scene_view)};
  }
  if (!StartHost(context, scene_desc.texture.width, scene_desc.texture.height)) return false;

  // The dispatches run inside the game's draw: save the compute bindings they overwrite and put them back.
  ID3D11ComputeShader* saved_shader = nullptr;
  context->CSGetShader(&saved_shader, nullptr, nullptr);
  std::array<ID3D11ShaderResourceView*, UPSCALING_SHADER_RESOURCES> saved_srvs = {};
  context->CSGetShaderResources(0u, static_cast<UINT>(saved_srvs.size()), saved_srvs.data());
  std::array<ID3D11UnorderedAccessView*, 5> saved_uavs = {};
  context->CSGetUnorderedAccessViews(0u, static_cast<UINT>(saved_uavs.size()), saved_uavs.data());
  ID3D11Buffer* saved_constants_b0 = nullptr;
  context->CSGetConstantBuffers(0u, 1u, &saved_constants_b0);
  ID3D11Buffer* saved_constants_b13 = nullptr;
  context->CSGetConstantBuffers(13u, 1u, &saved_constants_b13);

  const ComputeTarget& composite_output = resolve_state.outputs[0];
  const ComputeTarget& debug_output = resolve_state.outputs[1];
  auto native_view = [](IUnknown* view) { return reshade::api::resource_view{reinterpret_cast<std::uintptr_t>(view)}; };
  auto dispatch = [&](reshade::api::pipeline pipeline, const std::array<reshade::api::resource_view, UPSCALING_SHADER_RESOURCES>& shader_resources,
                      const std::array<reshade::api::resource_view, 5>& unordered_accesses) {
    cmd_list->bind_pipeline(reshade::api::pipeline_stage::all_compute, pipeline);
    cmd_list->push_descriptors(reshade::api::shader_stage::compute, resolve_state.layout, 0u,
                               {.count = UPSCALING_SHADER_RESOURCES, .type = reshade::api::descriptor_type::shader_resource_view, .descriptors = shader_resources.data()});
    cmd_list->push_descriptors(reshade::api::shader_stage::compute, resolve_state.layout, 1u,
                               {.count = 5u, .type = reshade::api::descriptor_type::unordered_access_view, .descriptors = unordered_accesses.data()});
    cmd_list->push_constants(reshade::api::shader_stage::compute, resolve_state.layout, 2u, 0u,
                             sizeof(UpscalingConstants) / sizeof(std::uint32_t), &constants);
    cmd_list->push_constants(reshade::api::shader_stage::compute, resolve_state.layout, 3u, 0u,
                             sizeof(ShaderInjectData) / sizeof(std::uint32_t), shader_injection);
    cmd_list->dispatch((resolve_state.width + 7u) / 8u, (resolve_state.height + 7u) / 8u, 1u);
    // The host reads and writes the shared textures: they must not stay bound here.
    const std::array<ID3D11ShaderResourceView*, UPSCALING_SHADER_RESOURCES> no_shader_resources = {};
    const std::array<ID3D11UnorderedAccessView*, 5> no_unordered_accesses = {};
    context->CSSetShaderResources(0u, static_cast<UINT>(no_shader_resources.size()), no_shader_resources.data());
    context->CSSetUnorderedAccessViews(0u, static_cast<UINT>(no_unordered_accesses.size()), no_unordered_accesses.data(), nullptr);
  };

  if (const std::uint32_t check_slot = static_cast<std::uint32_t>(resolve_state.frame_index % ViewPairing::SLOTS);
      view_pairing.copied_frame[check_slot] == resolve_state.frame_index) {
    view_pairing.candidate_jitter[check_slot] = candidate_jitter;
    view_pairing.candidate_count[check_slot] = candidate_count;
    view_pairing.used_position[check_slot] = used_position;
    view_pairing.resolved[check_slot] = true;
  }
  std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> saved_render_targets = {};
  ID3D11DepthStencilView* saved_depth_view = nullptr;
  if (scene_is_target) {
    context->OMGetRenderTargets(static_cast<UINT>(saved_render_targets.size()), saved_render_targets.data(), &saved_depth_view);
    context->OMSetRenderTargets(0u, nullptr, nullptr);
  }
  const bool resolved = RunUpscaler(current_view, history_valid, [&]() {
    dispatch(resolve_state.prepare_pipeline,
             {scene_srv, native_view(resolve_state.character_mask_srv), native_view(resolve_state.character_motion_srv),
              native_view(resolve_state.character_depth_srv), native_view(resolve_state.tessellated_motion_srv),
              native_view(resolve_state.tessellated_depth_srv)},
             {native_view(host_link.depth_uav), native_view(host_link.motion_vector_uav), native_view(host_link.bias_mask_uav),
              native_view(host_link.color_uav), debug_output.uav}); }, [&]() {
    if (!scene_is_target) {
      dispatch(resolve_state.composite_pipeline,
               {scene_srv, native_view(host_link.output_srv), reshade::api::resource_view{0}, reshade::api::resource_view{0}, reshade::api::resource_view{0},
                reshade::api::resource_view{0}},
               {composite_output.uav, reshade::api::resource_view{0}, reshade::api::resource_view{0}, reshade::api::resource_view{0}, reshade::api::resource_view{0}});
      return;
    }
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
    ID3D11ShaderResourceView* saved_pixel_resource = nullptr;
    context->PSGetShaderResources(0u, 1u, &saved_pixel_resource);

    context->OMSetRenderTargets(1u, &saved_render_targets[0], nullptr);
    context->OMSetBlendState(resolve_state.composite_blend_state, nullptr, 0xFFFFFFFFu);
    context->RSSetState(nullptr);
    const D3D11_VIEWPORT viewport = {.Width = static_cast<float>(resolve_state.width), .Height = static_cast<float>(resolve_state.height), .MaxDepth = 1.f};
    context->RSSetViewports(1u, &viewport);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(resolve_state.composite_vertex_shader, nullptr, 0u);
    context->HSSetShader(nullptr, nullptr, 0u);
    context->DSSetShader(nullptr, nullptr, 0u);
    context->GSSetShader(nullptr, nullptr, 0u);
    context->PSSetShader(resolve_state.composite_pixel_shader, nullptr, 0u);
    context->PSSetShaderResources(0u, 1u, &host_link.output_srv);
    context->Draw(3u, 0u);

    context->PSSetShaderResources(0u, 1u, &saved_pixel_resource);
    context->OMSetBlendState(saved_blend_state, saved_blend_factor.data(), saved_sample_mask);
    context->RSSetState(saved_rasterizer_state);
    context->RSSetViewports(saved_viewport_count, saved_viewports.data());
    context->IASetInputLayout(saved_input_layout);
    context->IASetPrimitiveTopology(saved_topology);
    context->VSSetShader(saved_vertex_shader, nullptr, 0u);
    context->HSSetShader(saved_hull_shader, nullptr, 0u);
    context->DSSetShader(saved_domain_shader, nullptr, 0u);
    context->GSSetShader(saved_geometry_shader, nullptr, 0u);
    context->PSSetShader(saved_pixel_shader, nullptr, 0u);
    for (IUnknown* saved : std::initializer_list<IUnknown*>{saved_blend_state, saved_rasterizer_state, saved_input_layout, saved_vertex_shader, saved_hull_shader,
                                                            saved_domain_shader, saved_geometry_shader, saved_pixel_shader, saved_pixel_resource}) {
      if (saved != nullptr) {
        saved->Release();
      }
    } });
  if (scene_is_target) {
    context->OMSetRenderTargets(static_cast<UINT>(saved_render_targets.size()), saved_render_targets.data(), saved_depth_view);
    for (ID3D11RenderTargetView* render_target : saved_render_targets) {
      if (render_target != nullptr) {
        render_target->Release();
      }
    }
    if (saved_depth_view != nullptr) {
      saved_depth_view->Release();
    }
  }

  context->CSSetShader(saved_shader, nullptr, 0u);
  context->CSSetShaderResources(0u, static_cast<UINT>(saved_srvs.size()), saved_srvs.data());
  const std::array<UINT, 5> keep_counters = {UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX};
  context->CSSetUnorderedAccessViews(0u, static_cast<UINT>(saved_uavs.size()), saved_uavs.data(), keep_counters.data());
  context->CSSetConstantBuffers(0u, 1u, &saved_constants_b0);
  context->CSSetConstantBuffers(13u, 1u, &saved_constants_b13);
  for (IUnknown* saved : std::initializer_list<IUnknown*>{saved_shader, saved_uavs[0], saved_uavs[1], saved_uavs[2], saved_uavs[3], saved_uavs[4], saved_constants_b0, saved_constants_b13}) {
    if (saved != nullptr) {
      saved->Release();
    }
  }
  for (IUnknown* saved : saved_srvs) {
    if (saved != nullptr) {
      saved->Release();
    }
  }
  if (!resolved) return false;

  if (!scene_is_target) {
    resolve_state.composite_output_frame = resolve_state.frame_index;
  }
  if (shader_injection->custom_dlaa_debug_view != 0.f) {
    cmd_list->copy_resource(debug_output.resource, scene);
  } else if (!scene_is_target) {
    cmd_list->copy_resource(composite_output.resource, scene);
  }
  resolve_state.last_resolve_frame = resolve_state.frame_index;
  ++resolve_state.resolves;
  return true;
}

template <typename IssueDraw>
static bool OnDrawCall(reshade::api::command_list* cmd_list, const IssueDraw& issue_draw) {
  if (aa_enabled == 0.f && particle_recorder.count == 0u) return false;
  const std::uint32_t pixel_shader = renodx::utils::state::GetCurrentShaderHash(cmd_list, reshade::api::pipeline_stage::pixel_shader);
  if (pixel_shader == PAUSE_FILTER_PS) {
    resolve_state.last_pause_filter_frame = resolve_state.frame_index;
    return false;
  }
  auto* context = reinterpret_cast<ID3D11DeviceContext*>(cmd_list->get_native());
  if (renodx::utils::state::GetCurrentShaderHash(cmd_list, reshade::api::pipeline_stage::vertex_shader) == SCENE_FOG_VS) {
    if (aa_enabled == 0.f || resolve_state.last_attempt_frame == resolve_state.frame_index) return false;
    ID3D11RenderTargetView* scene_rtv = nullptr;
    context->OMGetRenderTargets(1u, &scene_rtv, nullptr);
    if (scene_rtv == nullptr) return false;
    // The scene stays bound as the render target, which keeps it alive.
    ID3D11Resource* scene = nullptr;
    scene_rtv->GetResource(&scene);
    scene->Release();
    scene_rtv->Release();
    issue_draw();
    if (Resolve(cmd_list, reshade::api::resource{reinterpret_cast<std::uintptr_t>(scene)}, reshade::api::resource_view{0u})) {
      ++resolve_state.fog_resolves;
    }
    return true;
  }
  if (pixel_shader != SCENE_DEPTH_TO_VIEW_DISTANCE_PS) return false;
  if (aa_enabled != 0.f) {
    ++resolve_state.pass_draws;
    if (resolve_state.last_attempt_frame != resolve_state.frame_index) {
      if (const reshade::api::resource_view scene_srv = renodx::utils::state::GetBoundResourceView(
              cmd_list, reshade::api::shader_stage::pixel, reshade::api::descriptor_type::shader_resource_view, 0u);
          scene_srv.handle != 0u) {
        Resolve(cmd_list, cmd_list->get_device()->get_resource_from_view(scene_srv), scene_srv);
      }
    }
  }
  if (particle_recorder.count == 0u) return false;

  // Replays the held-back particle draws into this pass's input, then restores the draw's state. Soft particles read
  // The upscaler's composite (same scene, alpha kept, whether or not a debug view was shown) in place of the texture they now
  // draw into; without this frame's composite they read nothing.
  ID3D11ShaderResourceView* post_input_snapshot = nullptr;
  if (resolve_state.composite_output_frame == resolve_state.frame_index) {
    post_input_snapshot = reinterpret_cast<ID3D11ShaderResourceView*>(resolve_state.outputs[0].srv.handle);
  }

  ID3D11InputLayout* saved_input_layout = nullptr;
  context->IAGetInputLayout(&saved_input_layout);
  D3D11_PRIMITIVE_TOPOLOGY saved_topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
  context->IAGetPrimitiveTopology(&saved_topology);
  std::array<ID3D11Buffer*, RECORDED_VERTEX_BUFFER_SLOTS> saved_vertex_buffers = {};
  std::array<UINT, RECORDED_VERTEX_BUFFER_SLOTS> saved_strides = {};
  std::array<UINT, RECORDED_VERTEX_BUFFER_SLOTS> saved_offsets = {};
  context->IAGetVertexBuffers(0u, RECORDED_VERTEX_BUFFER_SLOTS, saved_vertex_buffers.data(), saved_strides.data(), saved_offsets.data());
  ID3D11Buffer* saved_index_buffer = nullptr;
  DXGI_FORMAT saved_index_format = DXGI_FORMAT_UNKNOWN;
  UINT saved_index_offset = 0u;
  context->IAGetIndexBuffer(&saved_index_buffer, &saved_index_format, &saved_index_offset);
  ID3D11VertexShader* saved_vertex_shader = nullptr;
  context->VSGetShader(&saved_vertex_shader, nullptr, nullptr);
  ID3D11GeometryShader* saved_geometry_shader = nullptr;
  context->GSGetShader(&saved_geometry_shader, nullptr, nullptr);
  ID3D11PixelShader* saved_pixel_shader = nullptr;
  context->PSGetShader(&saved_pixel_shader, nullptr, nullptr);
  std::array<ID3D11Buffer*, VERTEX_CONSTANT_SLOTS> saved_vertex_constants = {};
  context->VSGetConstantBuffers(0u, VERTEX_CONSTANT_SLOTS, saved_vertex_constants.data());
  std::array<ID3D11Buffer*, VERTEX_CONSTANT_SLOTS> saved_pixel_constants = {};
  context->PSGetConstantBuffers(0u, VERTEX_CONSTANT_SLOTS, saved_pixel_constants.data());
  std::array<ID3D11ShaderResourceView*, RECORDED_RESOURCE_SLOTS> saved_pixel_resources = {};
  context->PSGetShaderResources(0u, RECORDED_RESOURCE_SLOTS, saved_pixel_resources.data());
  std::array<ID3D11SamplerState*, RECORDED_SAMPLER_SLOTS> saved_pixel_samplers = {};
  context->PSGetSamplers(0u, RECORDED_SAMPLER_SLOTS, saved_pixel_samplers.data());
  ID3D11RasterizerState* saved_rasterizer_state = nullptr;
  context->RSGetState(&saved_rasterizer_state);
  std::array<D3D11_VIEWPORT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> saved_viewports = {};
  UINT saved_viewport_count = static_cast<UINT>(saved_viewports.size());
  context->RSGetViewports(&saved_viewport_count, saved_viewports.data());
  std::array<D3D11_RECT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> saved_scissors = {};
  UINT saved_scissor_count = static_cast<UINT>(saved_scissors.size());
  context->RSGetScissorRects(&saved_scissor_count, saved_scissors.data());
  std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> saved_render_targets = {};
  ID3D11DepthStencilView* saved_depth_view = nullptr;
  context->OMGetRenderTargets(static_cast<UINT>(saved_render_targets.size()), saved_render_targets.data(), &saved_depth_view);
  ID3D11BlendState* saved_blend_state = nullptr;
  std::array<float, 4> saved_blend_factor = {};
  UINT saved_sample_mask = 0u;
  context->OMGetBlendState(&saved_blend_state, saved_blend_factor.data(), &saved_sample_mask);
  ID3D11DepthStencilState* saved_depth_state = nullptr;
  UINT saved_stencil_ref = 0u;
  context->OMGetDepthStencilState(&saved_depth_state, &saved_stencil_ref);

  ID3D11Resource* post_input = nullptr;
  if (saved_pixel_resources[0] != nullptr) {
    saved_pixel_resources[0]->GetResource(&post_input);
    post_input->Release();
  }
  if (post_input != nullptr && post_input != particle_recorder.post_input) {
    if (particle_recorder.post_input_rtv != nullptr) {
      particle_recorder.post_input_rtv->Release();
      particle_recorder.post_input_rtv = nullptr;
    }
    ID3D11Device* device = nullptr;
    context->GetDevice(&device);
    if (SUCCEEDED(device->CreateRenderTargetView(post_input, nullptr, &particle_recorder.post_input_rtv))) {
      particle_recorder.post_input = post_input;
    } else {
      reshade::log::message(reshade::log::level::error, "batmanac upscaling: cannot draw into the post pass input; particles are drawn normally from the next frame");
      particle_replay = false;
      particle_recorder.post_input = nullptr;
    }
    device->Release();
  }
  const bool replays = post_input != nullptr && post_input == particle_recorder.post_input;

  for (std::size_t i = 0u; i < particle_recorder.count && replays; ++i) {
    RecordedDraw& draw = particle_recorder.draws[i];
    std::array<ID3D11ShaderResourceView*, RECORDED_RESOURCE_SLOTS> pixel_resources = draw.pixel_resources;
    for (ID3D11ShaderResourceView*& view : pixel_resources) {
      if (view == nullptr) continue;
      ID3D11Resource* resource = nullptr;
      view->GetResource(&resource);
      resource->Release();
      if (resource == post_input) {
        view = post_input_snapshot;
      }
    }
    // Render target first: binding the post pass input as a target unbinds it from the pixel shader inputs.
    context->OMSetRenderTargets(1u, &particle_recorder.post_input_rtv, draw.depth_view);
    context->OMSetBlendState(draw.blend_state, draw.blend_factor.data(), draw.sample_mask);
    context->OMSetDepthStencilState(draw.depth_state, draw.stencil_ref);
    context->IASetInputLayout(draw.input_layout);
    context->IASetPrimitiveTopology(draw.topology);
    context->IASetVertexBuffers(0u, RECORDED_VERTEX_BUFFER_SLOTS, draw.vertex_buffers.data(), draw.strides.data(), draw.offsets.data());
    context->IASetIndexBuffer(draw.index_buffer, draw.index_format, draw.index_offset);
    context->VSSetShader(draw.vertex_shader, nullptr, 0u);
    context->GSSetShader(draw.geometry_shader, nullptr, 0u);
    context->PSSetShader(draw.pixel_shader, nullptr, 0u);
    context->VSSetConstantBuffers(0u, VERTEX_CONSTANT_SLOTS, draw.vertex_constants.data());
    context->PSSetConstantBuffers(0u, VERTEX_CONSTANT_SLOTS, draw.pixel_constants.data());
    context->PSSetShaderResources(0u, RECORDED_RESOURCE_SLOTS, pixel_resources.data());
    context->PSSetSamplers(0u, RECORDED_SAMPLER_SLOTS, draw.pixel_samplers.data());
    context->RSSetState(draw.rasterizer_state);
    context->RSSetViewports(1u, &draw.viewport);
    context->RSSetScissorRects(1u, &draw.scissor);
    if (draw.indexed) {
      context->DrawIndexedInstanced(draw.count, draw.instance_count, draw.first, draw.base_vertex, draw.first_instance);
    } else {
      context->DrawInstanced(draw.count, draw.instance_count, draw.first, draw.first_instance);
    }
  }
  ClearRecordedDraws(false);

  // Render targets first again, so the restored pixel shader inputs (the scene) stay bound.
  context->OMSetRenderTargets(static_cast<UINT>(saved_render_targets.size()), saved_render_targets.data(), saved_depth_view);
  context->OMSetBlendState(saved_blend_state, saved_blend_factor.data(), saved_sample_mask);
  context->OMSetDepthStencilState(saved_depth_state, saved_stencil_ref);
  context->IASetInputLayout(saved_input_layout);
  context->IASetPrimitiveTopology(saved_topology);
  context->IASetVertexBuffers(0u, RECORDED_VERTEX_BUFFER_SLOTS, saved_vertex_buffers.data(), saved_strides.data(), saved_offsets.data());
  context->IASetIndexBuffer(saved_index_buffer, saved_index_format, saved_index_offset);
  context->VSSetShader(saved_vertex_shader, nullptr, 0u);
  context->GSSetShader(saved_geometry_shader, nullptr, 0u);
  context->PSSetShader(saved_pixel_shader, nullptr, 0u);
  context->VSSetConstantBuffers(0u, VERTEX_CONSTANT_SLOTS, saved_vertex_constants.data());
  context->PSSetConstantBuffers(0u, VERTEX_CONSTANT_SLOTS, saved_pixel_constants.data());
  context->PSSetShaderResources(0u, RECORDED_RESOURCE_SLOTS, saved_pixel_resources.data());
  context->PSSetSamplers(0u, RECORDED_SAMPLER_SLOTS, saved_pixel_samplers.data());
  context->RSSetState(saved_rasterizer_state);
  context->RSSetViewports(saved_viewport_count, saved_viewports.data());
  context->RSSetScissorRects(saved_scissor_count, saved_scissors.data());

  auto release = [](IUnknown* object) {
    if (object != nullptr) {
      object->Release();
    }
  };
  release(saved_input_layout);
  for (IUnknown* object : saved_vertex_buffers) {
    release(object);
  }
  release(saved_index_buffer);
  release(saved_vertex_shader);
  release(saved_geometry_shader);
  release(saved_pixel_shader);
  for (IUnknown* object : saved_vertex_constants) {
    release(object);
  }
  for (IUnknown* object : saved_pixel_constants) {
    release(object);
  }
  for (IUnknown* object : saved_pixel_resources) {
    release(object);
  }
  for (IUnknown* object : saved_pixel_samplers) {
    release(object);
  }
  release(saved_rasterizer_state);
  for (IUnknown* object : saved_render_targets) {
    release(object);
  }
  release(saved_depth_view);
  release(saved_blend_state);
  release(saved_depth_state);
  return false;
}

// Texture LOD bias of -1 while the upscaler runs, DLSS's recommendation at native resolution. Every mipmapped sampler gets a
// biased twin at creation, bound after the game binds pixel samplers, so toggling anti-aliasing applies at once.
struct BiasedSamplers {
  std::shared_mutex mutex;
  std::unordered_map<std::uint64_t, ID3D11SamplerState*> twins;
};
static BiasedSamplers biased_samplers;

static void OnInitSampler(reshade::api::device* device, const reshade::api::sampler_desc& desc, reshade::api::sampler sampler) {
  auto* game_sampler = reinterpret_cast<ID3D11SamplerState*>(sampler.handle);
  if (game_sampler == nullptr) return;
  D3D11_SAMPLER_DESC sampler_desc = {};
  game_sampler->GetDesc(&sampler_desc);
  // Only samplers with linear mip filtering (bit 0; anisotropic filters set it too): point-mip samplers are the post passes'.
  if ((sampler_desc.Filter & 0x1) == 0 || sampler_desc.MaxLOD <= 0.f) return;
  ID3D11Device* native_device = nullptr;
  game_sampler->GetDevice(&native_device);
  D3D11_SAMPLER_DESC twin_desc = sampler_desc;
  twin_desc.MipLODBias = sampler_desc.MipLODBias - 1.f;
  ID3D11SamplerState* twin_sampler = nullptr;
  const HRESULT twin_result = native_device->CreateSamplerState(&twin_desc, &twin_sampler);
  native_device->Release();
  if (FAILED(twin_result)) {
    WarnOnce("cannot create a biased sampler; the sampler keeps its earlier mapping (the game's mip bias if it has none)", twin_result);
    return;
  }
  // D3D11 hands out the same object for identical descriptions, so the same game sampler can arrive again; its earlier
  // twin reference is given back. Releases happen outside the lock: a twin can be another game sampler, and its last
  // release runs this add-on's destroy callback.
  ID3D11SamplerState* replaced_twin = nullptr;
  {
    const std::unique_lock lock(biased_samplers.mutex);
    ID3D11SamplerState*& stored_twin = biased_samplers.twins[sampler.handle];
    replaced_twin = stored_twin;
    stored_twin = twin_sampler;
  }
  if (replaced_twin != nullptr) {
    replaced_twin->Release();
  }
}

static void OnDestroySampler(reshade::api::device* device, reshade::api::sampler sampler) {
  ID3D11SamplerState* released_twin = nullptr;
  {
    const std::unique_lock lock(biased_samplers.mutex);
    if (auto twin = biased_samplers.twins.find(sampler.handle); twin != biased_samplers.twins.end()) {
      const auto& [game_sampler, twin_sampler] = *twin;
      released_twin = twin_sampler;
      biased_samplers.twins.erase(twin);
    }
  }
  if (released_twin != nullptr) {
    released_twin->Release();
  }
}

static void OnPushDescriptors(
    reshade::api::command_list* cmd_list,
    reshade::api::shader_stage stages,
    reshade::api::pipeline_layout layout,
    uint32_t layout_param,
    const reshade::api::descriptor_table_update& update) {
  if (aa_enabled == 0.f || !aa_resolving.load() || update.type != reshade::api::descriptor_type::sampler
      || (stages & reshade::api::shader_stage::pixel) != reshade::api::shader_stage::pixel || update.count == 0u
      || update.count > D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) return;
  const auto* game_samplers = static_cast<const reshade::api::sampler*>(update.descriptors);
  std::array<ID3D11SamplerState*, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT> bound = {};
  bool replaced = false;
  {
    const std::shared_lock lock(biased_samplers.mutex);
    for (std::uint32_t i = 0u; i < update.count; ++i) {
      bound[i] = reinterpret_cast<ID3D11SamplerState*>(game_samplers[i].handle);
      if (auto twin = biased_samplers.twins.find(game_samplers[i].handle); twin != biased_samplers.twins.end()) {
        const auto& [game_sampler, twin_sampler] = *twin;
        bound[i] = twin_sampler;
        replaced = true;
      }
    }
  }
  if (!replaced) return;
  reinterpret_cast<ID3D11DeviceContext*>(cmd_list->get_native())->PSSetSamplers(update.binding + update.array_offset, update.count, bound.data());
}

// Copies the view constant buffer (VS b1) of the frame's first draw into the main depth before the upscaler pass into the
// frame's staging slot, for the view pairing readback.
static void CaptureViewBuffer(ID3D11DeviceContext* context) {
  const std::uint32_t slot = static_cast<std::uint32_t>(resolve_state.frame_index % ViewPairing::SLOTS);
  if (aa_enabled == 0.f || view_pairing.copied_frame[slot] == resolve_state.frame_index
      || resolve_state.last_resolve_frame == resolve_state.frame_index) return;
  ID3D11DepthStencilView* depth_view = nullptr;
  context->OMGetRenderTargets(0u, nullptr, &depth_view);
  if (depth_view == nullptr) return;
  ID3D11Resource* depth_resource = nullptr;
  depth_view->GetResource(&depth_resource);
  depth_view->Release();
  D3D11_TEXTURE2D_DESC depth_desc = {};
  static_cast<ID3D11Texture2D*>(depth_resource)->GetDesc(&depth_desc);
  depth_resource->Release();
  if (static_cast<float>(depth_desc.Width) != view_queue.render_width || static_cast<float>(depth_desc.Height) != view_queue.render_height
      || depth_desc.SampleDesc.Count != 1u) return;
  ID3D11Buffer* view_buffer = nullptr;
  context->VSGetConstantBuffers(1u, 1u, &view_buffer);
  if (view_buffer == nullptr) return;
  D3D11_BUFFER_DESC view_desc = {};
  view_buffer->GetDesc(&view_desc);
  // The readback reads the view-projection's first three rows.
  if (view_desc.ByteWidth < sizeof(float) * 12u) {
    view_buffer->Release();
    return;
  }
  ID3D11Buffer*& staging = view_pairing.staging[slot];
  if (staging != nullptr) {
    D3D11_BUFFER_DESC staging_desc = {};
    staging->GetDesc(&staging_desc);
    if (staging_desc.ByteWidth != view_desc.ByteWidth) {
      staging->Release();
      staging = nullptr;
    }
  }
  if (staging == nullptr) {
    ID3D11Device* device = nullptr;
    context->GetDevice(&device);
    const D3D11_BUFFER_DESC staging_desc = {.ByteWidth = view_desc.ByteWidth, .Usage = D3D11_USAGE_STAGING, .CPUAccessFlags = D3D11_CPU_ACCESS_READ};
    if (const HRESULT result = device->CreateBuffer(&staging_desc, nullptr, &staging); FAILED(result)) {
      WarnOnce("cannot create the view pairing readback buffer; the view pairing keeps its last position", result);
    }
    device->Release();
  }
  if (staging != nullptr) {
    context->CopyResource(staging, view_buffer);
    view_pairing.copied_frame[slot] = resolve_state.frame_index;
    view_pairing.resolved[slot] = false;
  }
  view_buffer->Release();
}

// Cloth simulated on the CPU uploads its vertices into default-usage vertex buffers every frame.
static bool OnUpdateBufferRegion(reshade::api::device* device, const void* data, reshade::api::resource dest, uint64_t dest_offset, uint64_t size) {
  if ((device->get_resource_desc(dest).usage & reshade::api::resource_usage::vertex_buffer) != 0) {
    resolve_state.uploaded_vertex_buffers.insert(dest.handle);
  }
  return false;
}

static bool OnDraw(reshade::api::command_list* cmd_list, uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex, uint32_t first_instance) {
  auto* context = reinterpret_cast<ID3D11DeviceContext*>(cmd_list->get_native());
  auto issue_draw = [&]() { context->DrawInstanced(vertex_count, instance_count, first_vertex, first_instance); };
  CaptureViewBuffer(context);
  if (MarkCharacter(cmd_list, {vertex_count, instance_count, first_vertex, 0u}, issue_draw)
      || RecordParticleDraw(cmd_list, false, vertex_count, instance_count, first_vertex, 0, first_instance)) return true;
  return OnDrawCall(cmd_list, issue_draw);
}

static bool OnDrawIndexed(reshade::api::command_list* cmd_list, uint32_t index_count, uint32_t instance_count, uint32_t first_index, int32_t vertex_offset, uint32_t first_instance) {
  auto* context = reinterpret_cast<ID3D11DeviceContext*>(cmd_list->get_native());
  auto issue_draw = [&]() { context->DrawIndexedInstanced(index_count, instance_count, first_index, vertex_offset, first_instance); };
  CaptureViewBuffer(context);
  if (MarkCharacter(cmd_list, {index_count, instance_count, first_index, static_cast<std::uint32_t>(vertex_offset)}, issue_draw)
      || RecordParticleDraw(cmd_list, true, index_count, instance_count, first_index, vertex_offset, first_instance)) return true;
  return OnDrawCall(cmd_list, issue_draw);
}

static void OnInitPipeline(
    reshade::api::device* device,
    reshade::api::pipeline_layout layout,
    uint32_t subobject_count,
    const reshade::api::pipeline_subobject* subobjects,
    reshade::api::pipeline pipeline) {
  // dcl_constantbuffer cb3[N], dynamicIndexed: the bone palette of skinned meshes, sized by the mesh's bone count.
  constexpr std::array<std::uint32_t, 3> BONE_PALETTE_DECLARATION = {0x04000859u, 0x00208E46u, 3u};
  for (uint32_t i = 0u; i < subobject_count; ++i) {
    if (subobjects[i].type != reshade::api::pipeline_subobject_type::vertex_shader || subobjects[i].count == 0u) continue;
    const auto* shader = static_cast<const reshade::api::shader_desc*>(subobjects[i].data);
    if (shader->code == nullptr) continue;
    const auto* tokens = static_cast<const std::uint32_t*>(shader->code);
    const std::size_t token_count = shader->code_size / sizeof(std::uint32_t);
    bool skinned = false;
    for (std::size_t t = 0u; t + BONE_PALETTE_DECLARATION.size() <= token_count && !skinned; ++t) {
      skinned = std::equal(BONE_PALETTE_DECLARATION.begin(), BONE_PALETTE_DECLARATION.end(), tokens + t);
    }
    const auto* code = static_cast<const std::uint8_t*>(shader->code);
    const std::uint32_t hash = renodx::utils::hash::ComputeCRC32(code, shader->code_size);
    const std::unique_lock lock(motion_vertex_shaders.mutex);
    motion_vertex_shaders.shaders.try_emplace(hash, MotionVertexShader{.code = {code, code + shader->code_size}, .skinned = skinned});
  }
}

static void OnInitDevice(reshade::api::device* device) {
  if (game_device != nullptr) return;
  game_device = device;

  // The game's GPU, where the host makes its devices, and whether DLSS can run there (an NVIDIA GPU and nvngx_dlss.dll
  // next to the host): read now, so the menu offers only what can run.
  IDXGIDevice* dxgi_device = nullptr;
  IDXGIAdapter* adapter = nullptr;
  DXGI_ADAPTER_DESC adapter_desc = {};
  HRESULT adapter_result = reinterpret_cast<ID3D11Device*>(device->get_native())->QueryInterface(IID_PPV_ARGS(&dxgi_device));
  if (SUCCEEDED(adapter_result)) {
    adapter_result = dxgi_device->GetAdapter(&adapter);
  }
  if (SUCCEEDED(adapter_result)) {
    adapter_result = adapter->GetDesc(&adapter_desc);
  }
  if (FAILED(adapter_result)) {
    WarnOnce("cannot read the GPU adapter; the NVIDIA check is skipped and the host may not find the game's GPU", adapter_result);
  }
  for (IUnknown* object : std::initializer_list<IUnknown*>{adapter, dxgi_device}) {
    if (object != nullptr) {
      object->Release();
    }
  }
  host_link.adapter_luid = adapter_desc.AdapterLuid;
  HMODULE addon_module = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&OnInitDevice), &addon_module);
  std::array<wchar_t, MAX_PATH> addon_path = {};
  GetModuleFileNameW(addon_module, addon_path.data(), static_cast<DWORD>(addon_path.size()));
  const std::filesystem::path addon_folder = std::filesystem::path(addon_path.data()).parent_path();
  host_link.host_path = (addon_folder / HOST_EXECUTABLE).wstring();
  constexpr UINT NVIDIA_VENDOR_ID = 0x10DEu;
  std::error_code exists_error;
  if (adapter_desc.VendorId != 0u && adapter_desc.VendorId != NVIDIA_VENDOR_ID) {
    dlss_available = false;
    reshade::log::message(reshade::log::level::info,
                          std::format("batmanac upscaling: the GPU (vendor 0x{:04X}) is not an NVIDIA GPU; FSR replaces DLAA", adapter_desc.VendorId).c_str());
  } else if (!std::filesystem::exists(addon_folder / L"nvngx_dlss.dll", exists_error)) {
    dlss_available = false;
    reshade::log::message(reshade::log::level::info, "batmanac upscaling: nvngx_dlss.dll is not next to the add-on; FSR replaces DLAA");
  }
  if (scene_view_trampoline != nullptr) return;

  auto* image = reinterpret_cast<std::byte*>(GetModuleHandleW(nullptr));
  const auto* dos_header = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
  const auto* nt_headers = reinterpret_cast<const IMAGE_NT_HEADERS*>(image + dos_header->e_lfanew);
  const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt_headers);

  // push 8; push 0x710; call appMalloc; mov esi, eax; add esp, 8; mov [esp+0x80], esi (call target masked).
  constexpr std::array<std::int16_t, 24> PATTERN = {
      0x6A, 0x08, 0x68, 0x10, 0x07, 0x00, 0x00, 0xE8, -1, -1, -1, -1,
      0x8B, 0xF0, 0x83, 0xC4, 0x08, 0x89, 0xB4, 0x24, 0x80, 0x00, 0x00, 0x00};
  std::byte* match = nullptr;
  std::uint32_t match_count = 0u;
  for (WORD section_index = 0; section_index < nt_headers->FileHeader.NumberOfSections; ++section_index) {
    const IMAGE_SECTION_HEADER& section = sections[section_index];
    if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0u) continue;
    std::byte* begin = image + section.VirtualAddress;
    const std::size_t size = section.Misc.VirtualSize;
    for (std::size_t offset = 0u; offset + PATTERN.size() <= size; ++offset) {
      bool equal = true;
      for (std::size_t i = 0u; i < PATTERN.size(); ++i) {
        if (PATTERN[i] >= 0 && begin[offset + i] != static_cast<std::byte>(PATTERN[i])) {
          equal = false;
          break;
        }
      }
      if (equal) {
        match = begin + offset;
        ++match_count;
      }
    }
  }
  if (match_count != 1u) {
    TurnOffAntiAliasing(std::format("CalcSceneView pattern found {} times; camera jitter unavailable", match_count));
    return;
  }

  scene_view_trampoline = match;
  try {
    renodx::utils::detour::Install(std::array{renodx::utils::detour::Function(&scene_view_trampoline, &SceneViewAllocationStub)});
  } catch (const std::exception& error) {
    scene_view_trampoline = nullptr;
    TurnOffAntiAliasing(std::format("cannot hook CalcSceneView ({}); camera jitter unavailable", error.what()));
    return;
  }
  reshade::log::message(reshade::log::level::info,
                        std::format("batmanac upscaling: CalcSceneView hooked at 0x{:08X}", reinterpret_cast<std::uintptr_t>(match)).c_str());
}


static void OnDestroyDevice(reshade::api::device* device) {
  if (device != game_device) return;
  game_device = nullptr;
  DestroyOutputs(device);
  if (resolve_state.prepare_pipeline.handle != 0u) {
    device->destroy_pipeline(resolve_state.prepare_pipeline);
  }
  if (resolve_state.composite_pipeline.handle != 0u) {
    device->destroy_pipeline(resolve_state.composite_pipeline);
  }
  for (IUnknown* object : std::initializer_list<IUnknown*>{resolve_state.composite_vertex_shader, resolve_state.composite_pixel_shader, resolve_state.composite_blend_state}) {
    if (object != nullptr) {
      object->Release();
    }
  }
  if (resolve_state.layout.handle != 0u) {
    device->destroy_pipeline_layout(resolve_state.layout);
  }
  ReleaseCharacterTargets();
  ClearRecordedDraws(true);
  for (ID3D11Buffer*& staging : view_pairing.staging) {
    if (staging != nullptr) {
      staging->Release();
    }
    staging = nullptr;
  }
  view_pairing = {};
  for (auto& [draw_key, captured] : resolve_state.captured_constants) {
    ReleaseCapturedConstants(&captured);
  }
  {
    const std::unique_lock lock(motion_vertex_shaders.mutex);
    for (auto& [hash, shader] : motion_vertex_shaders.shaders) {
      if (shader.stream_out_shader != nullptr) {
        shader.stream_out_shader->Release();
      }
    }
    motion_vertex_shaders.shaders.clear();
  }
  resolve_state = {};
  aa_resolving.store(false);
  dlss_available = true;
  // The last answer is read while the watcher can still free the GPU from a host that does not give it; a host ended
  // for it has exited before the watcher is stopped, so the watcher sees the exit.
  ReadHostAnswer(1000u);
  // The host is ended in any case: after a failure it may still wait for requests from this live process, and a healthy
  // one holds the names a new device's host would use. Its last signal runs first; if it never does, the watcher frees
  // the game's GPU once the host has exited.
  if (const HRESULT signal_result = WaitForFenceValue(host_link.done_value, 5000u); FAILED(signal_result)) {
    WarnOnce("the host's last fence signal was not confirmed; the host is ended anyway", signal_result);
  }
  if (host_link.host_process != nullptr) {
    TerminateProcess(host_link.host_process, 1u);
    WaitForSingleObject(host_link.host_process, INFINITE);
  }
  if (host_link.watcher != nullptr) {
    // The game's GPU may wait on a newer request than the last answer: the watcher rescues it now that the host has
    // exited, so it is stopped only once the fence reached that value (at most 15 s).
    if (const HRESULT rescue_result = WaitForFenceValue(host_link.gpu_waited_value, 15000u); FAILED(rescue_result)) {
      WarnOnce("the game's last request was not answered before the watcher stopped", rescue_result);
    }
    SetEvent(host_link.watcher_stop);
    WaitForSingleObject(host_link.watcher, INFINITE);
  }
  ReleaseSharedTextures();
  if (host_link.block != nullptr) {
    UnmapViewOfFile(host_link.block);
  }
  for (HANDLE handle : {host_link.mapping, host_link.request_event, host_link.done_event, host_link.host_process, host_link.watcher, host_link.watcher_stop}) {
    if (handle != nullptr) {
      CloseHandle(handle);
    }
  }
  const bool failed = host_link.failed;
  host_link = {};
  // A failure only held the binding off: once it is cleared, the setting's current value (a preset or Reset All may have
  // changed it) applies again on the next device.
  if (failed && renodx::utils::settings::settings != nullptr) {
    if (auto* setting = renodx::utils::settings::FindSetting(AA_SETTING_KEY); setting != nullptr) {
      setting->Write();
    }
  }
  {
    const std::unique_lock lock(upscaler_status.mutex);
    upscaler_status.text.clear();
  }
  {
    const std::unique_lock lock(host_watch.mutex);
    host_watch.awaited_value = 0u;
  }

  if (scene_view_trampoline == nullptr) return;
  try {
    renodx::utils::detour::Uninstall(std::array{renodx::utils::detour::Function(&scene_view_trampoline, &SceneViewAllocationStub)});
  } catch (const std::exception& error) {
    reshade::log::message(reshade::log::level::error, std::format("batmanac upscaling: unhook failed: {}", error.what()).c_str());
    return;
  }
  scene_view_trampoline = nullptr;
}

static void OnPresent(
    reshade::api::command_queue* queue,
    reshade::api::swapchain* swapchain,
    const reshade::api::rect* source_rect,
    const reshade::api::rect* dest_rect,
    uint32_t dirty_rect_count,
    const reshade::api::rect* dirty_rects) {
  if (swapchain->get_device() != game_device) return;
  const reshade::api::resource_desc back_buffer_desc = swapchain->get_device()->get_resource_desc(swapchain->get_current_back_buffer());
  ++resolve_state.frame_index;
  aa_resolving.store(resolve_state.last_resolve_frame != UINT64_MAX && resolve_state.last_resolve_frame + 2u >= resolve_state.frame_index);
  auto* immediate_context = reinterpret_cast<ID3D11DeviceContext*>(queue->get_immediate_command_list()->get_native());
  if (resolve_state.frame_index >= 3u) {
    const std::uint64_t checked_frame = resolve_state.frame_index - 3u;
    const std::uint32_t check_slot = static_cast<std::uint32_t>(checked_frame % ViewPairing::SLOTS);
    if (view_pairing.copied_frame[check_slot] == checked_frame && view_pairing.resolved[check_slot]) {
      D3D11_MAPPED_SUBRESOURCE mapped = {};
      const HRESULT map_result = immediate_context->Map(view_pairing.staging[check_slot], 0u, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
      if (map_result == S_OK) {
        const auto* rows = static_cast<const float*>(mapped.pData);
        auto dot3 = [&](int a, int b) { return rows[a] * rows[b] + rows[4 + a] * rows[4 + b] + rows[8 + a] * rows[8 + b]; };
        const float w_length = dot3(3, 3);
        const float rendered_x = dot3(0, 3) / w_length * view_queue.render_width * 0.5f;
        const float rendered_y = -dot3(1, 3) / w_length * view_queue.render_height * 0.5f;
        immediate_context->Unmap(view_pairing.staging[check_slot], 0u);
        std::uint32_t rendered_position = VIEW_CANDIDATES;
        for (std::uint32_t i = 0u; i < view_pairing.candidate_count[check_slot] && rendered_position == VIEW_CANDIDATES; ++i) {
          const std::array<float, 2>& jitter = view_pairing.candidate_jitter[check_slot][i];
          if (std::abs(jitter[0] - rendered_x) < 0.02f && std::abs(jitter[1] - rendered_y) < 0.02f) {
            rendered_position = i;
          }
        }
        // Another view that reuses the scene depth without jitter can draw into it first; no Halton offset comes this
        // close to (0, 0), so its readback says nothing about the pairing.
        if (!std::isfinite(rendered_x) || !std::isfinite(rendered_y)
            || (std::abs(rendered_x) < 0.005f && std::abs(rendered_y) < 0.005f)) {
          ++view_pairing.unread;
        } else {
          if (rendered_position == view_pairing.used_position[check_slot]) {
            ++view_pairing.matches;
          } else {
            ++view_pairing.mismatches;
          }
          if (rendered_position != VIEW_CANDIDATES && rendered_position != view_pairing.position) {
            view_pairing.position = rendered_position;
            ++view_pairing.position_changes;
          }
        }
      } else {
        ++view_pairing.unread;
        if (map_result != DXGI_ERROR_WAS_STILL_DRAWING) {
          WarnOnce("cannot read back the view pairing sample; the view pairing keeps its last position", map_result);
        }
      }
      view_pairing.resolved[check_slot] = false;
    }
  }
  ResetMotionBatch();
  constexpr std::array<float, 4> CLEAR_COLOR = {0.f, 0.f, 0.f, 0.f};
  for (ID3D11RenderTargetView* mask : {resolve_state.character_mask_rtv, resolve_state.character_depth_rtv}) {
    if (mask != nullptr) {
      immediate_context->ClearRenderTargetView(mask, CLEAR_COLOR.data());
    }
  }
  if (resolve_state.character_motion_rtv != nullptr) {
    constexpr std::array<float, 4> MOTION_CLEAR_COLOR = {CHARACTER_MOTION_UNKNOWN, CHARACTER_MOTION_UNKNOWN, 0.f, 0.f};
    immediate_context->ClearRenderTargetView(resolve_state.character_motion_rtv, MOTION_CLEAR_COLOR.data());
    immediate_context->ClearRenderTargetView(resolve_state.tessellated_motion_rtv, MOTION_CLEAR_COLOR.data());
    immediate_context->ClearDepthStencilView(resolve_state.tessellated_depth_dsv, D3D11_CLEAR_DEPTH, 0.f, 0u);
  }
  resolve_state.draw_occurrences.clear();
  resolve_state.older_uploaded_vertex_buffers = std::move(resolve_state.previous_uploaded_vertex_buffers);
  resolve_state.previous_uploaded_vertex_buffers = std::move(resolve_state.uploaded_vertex_buffers);
  resolve_state.uploaded_vertex_buffers.clear();
  // Draws held back in a frame without the first post pass are dropped.
  ClearRecordedDraws(false);
  // Draws not seen for two frames (meshes out of view, unloaded) give back their constant copies.
  for (auto entry = resolve_state.captured_constants.begin(); entry != resolve_state.captured_constants.end();) {
    auto& [draw_key, captured] = *entry;
    if (captured.frame == UINT64_MAX || captured.frame + 2u < resolve_state.frame_index) {
      ReleaseCapturedConstants(&captured);
      entry = resolve_state.captured_constants.erase(entry);
    } else {
      ++entry;
    }
  }
#ifndef NDEBUG
  if (aa_enabled != 0.f && resolve_state.frame_index - resolve_state.last_logged_frame >= 120u) {
    resolve_state.last_logged_frame = resolve_state.frame_index;
    reshade::log::message(reshade::log::level::info,
                          std::format("batmanac upscaling: last 120 frames: {} pass draws, {} resolves ({} after the fog), host wait {:.2f} ms per resolve (max {:.2f}), {} empty-queue reuses, {} scene skips, "
                                      "max queue {} (at present {}), {} skinned draws with motion, {} without, {} captured draws, {} particle draws replayed after the upscaler, view pairing {} match / {} mismatch / {} position changes (now {}) / {} unread",
                                      resolve_state.pass_draws, resolve_state.resolves, resolve_state.fog_resolves,
                                      resolve_state.host_wait_milliseconds / std::max(resolve_state.resolves, 1u), resolve_state.host_wait_max_milliseconds,
                                      resolve_state.empty_queue_reuses, resolve_state.scene_skips,
                                      resolve_state.max_queue_size, resolve_state.max_present_queue_size,
                                      resolve_state.motion_draws, resolve_state.motion_fallbacks, resolve_state.captured_constants.size(), resolve_state.recorded_particle_draws,
                                      view_pairing.matches, view_pairing.mismatches, view_pairing.position_changes, view_pairing.position, view_pairing.unread).c_str());
    resolve_state.pass_draws = 0u;
    resolve_state.resolves = 0u;
    resolve_state.fog_resolves = 0u;
    resolve_state.host_wait_milliseconds = 0.0;
    resolve_state.host_wait_max_milliseconds = 0.0;
    resolve_state.empty_queue_reuses = 0u;
    resolve_state.scene_skips = 0u;
    resolve_state.max_queue_size = 0u;
    view_pairing.matches = 0u;
    view_pairing.mismatches = 0u;
    view_pairing.position_changes = 0u;
    view_pairing.unread = 0u;
    resolve_state.max_present_queue_size = 0u;
    resolve_state.motion_draws = 0u;
    resolve_state.motion_fallbacks = 0u;
    resolve_state.recorded_particle_draws = 0u;
  }
#endif

  const std::unique_lock lock(view_queue.mutex);
  view_queue.render_width = static_cast<float>(back_buffer_desc.texture.width);
  view_queue.render_height = static_cast<float>(back_buffer_desc.texture.height);
  resolve_state.max_present_queue_size = std::max(resolve_state.max_present_queue_size, view_queue.pending_views.size());
  // Frames without a resolve (menus, anti-aliasing off) leave views behind; keep at most a frame of lead.
  while (view_queue.pending_views.size() > 2u) {
    view_queue.pending_views.pop_front();
  }
}

static void Use(DWORD fdw_reason, ShaderInjectData* injection) {
  switch (fdw_reason) {
    case DLL_PROCESS_ATTACH:
      shader_injection = injection;
      reshade::register_event<reshade::addon_event::init_device>(OnInitDevice);
      reshade::register_event<reshade::addon_event::destroy_device>(OnDestroyDevice);
      reshade::register_event<reshade::addon_event::init_pipeline>(OnInitPipeline);
      reshade::register_event<reshade::addon_event::init_sampler>(OnInitSampler);
      reshade::register_event<reshade::addon_event::destroy_sampler>(OnDestroySampler);
      reshade::register_event<reshade::addon_event::push_descriptors>(OnPushDescriptors);
      reshade::register_event<reshade::addon_event::draw>(OnDraw);
      reshade::register_event<reshade::addon_event::draw_indexed>(OnDrawIndexed);
      reshade::register_event<reshade::addon_event::present>(OnPresent);
      reshade::register_event<reshade::addon_event::update_buffer_region>(OnUpdateBufferRegion);
      break;
    case DLL_PROCESS_DETACH:
      reshade::unregister_event<reshade::addon_event::update_buffer_region>(OnUpdateBufferRegion);
      reshade::unregister_event<reshade::addon_event::present>(OnPresent);
      reshade::unregister_event<reshade::addon_event::draw_indexed>(OnDrawIndexed);
      reshade::unregister_event<reshade::addon_event::draw>(OnDraw);
      reshade::unregister_event<reshade::addon_event::push_descriptors>(OnPushDescriptors);
      reshade::unregister_event<reshade::addon_event::destroy_sampler>(OnDestroySampler);
      reshade::unregister_event<reshade::addon_event::init_sampler>(OnInitSampler);
      reshade::unregister_event<reshade::addon_event::init_pipeline>(OnInitPipeline);
      reshade::unregister_event<reshade::addon_event::destroy_device>(OnDestroyDevice);
      reshade::unregister_event<reshade::addon_event::init_device>(OnInitDevice);
      break;
  }
}

}  // namespace batmanac::upscaling
