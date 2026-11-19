/*
 * Copyright (C) 2026 ArshiaTN
 * Copyright (C) 2026 RenoDX contributors
 * SPDX-License-Identifier: MIT
 */

// Adapted from ArshiaTN's Total War DLAA helper (src/games/twattila/dlaa_helper/upscale_d3d12.hpp and gpu_completion.hpp
// on github.com/arshiatn/renodx, MIT).

#pragma once

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_2.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <limits>
#include <ratio>
#include <string>
#include <utility>

// AMD's FSR SDK (MIT): its headers at build time, its signed amd_fidelityfx_loader_dx12.dll and
// amd_fidelityfx_upscaler_dx12.dll loaded from the helper's folder at run time.
#if __has_include(<ffx_api.h>)
#include <dx12/ffx_api_dx12.h>
#include <ffx_api_loader.h>
#include <ffx_upscale.h>
#define BATMANAC_FSR_SDK 1
#endif

#include "../upscaling/host_protocol.h"

namespace batmanac::upscaling {

// AMD FSR at native resolution on a D3D12 device of the helper's own, on the add-on's shared textures and fence: its queue
// waits for the request's fence value, runs FSR and signals the value after it, also when FSR could not be recorded.
// A failure leaves `failure` and `failure_code` set; the caller then stops.
struct FsrUpscaler {
  struct Slot {
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    // own_fence's value once the GPU is done with this slot's last list.
    std::uint64_t done_value = 0u;
  };
  // The CPU's longest wait for this queue: a slot was last used SLOT_COUNT requests ago, normally long done.
  static constexpr DWORD FENCE_TIMEOUT_MILLISECONDS = 3000u;
  static constexpr std::uint32_t SLOT_COUNT = 3u;

  ID3D12Device* device = nullptr;
  ID3D12CommandQueue* queue = nullptr;
  std::array<Slot, SLOT_COUNT> slots = {};
  std::uint32_t next_slot = 0u;
  ID3D12Fence* own_fence = nullptr;
  std::uint64_t own_value = 0u;
  HANDLE own_event = nullptr;
  std::array<ID3D12Resource*, SHARED_TEXTURE_COUNT> textures = {};
  ID3D12Fence* shared_fence = nullptr;
  HMODULE library = nullptr;
#if defined(BATMANAC_FSR_SDK)
  ffxFunctions fsr = {};
  ffxContext context = nullptr;
#endif
  std::uint32_t context_width = 0u;
  std::uint32_t context_height = 0u;
  std::chrono::steady_clock::time_point last_dispatch_time;
  bool history_reset = true;
  // FSR's provider, as it names itself, once its context exists.
  std::string version;
  std::string failure;
  HRESULT failure_code = S_OK;

  bool Fail(const std::string& reason, HRESULT code) {
    failure = reason;
    failure_code = code;
    return false;
  }

  // True once the GPU passed own_fence's `value` within FENCE_TIMEOUT_MILLISECONDS (or nothing was submitted).
  [[nodiscard]] bool WaitForOwnFence(std::uint64_t value) const {
    if (own_fence == nullptr || own_fence->GetCompletedValue() >= value) return true;
    if (FAILED(own_fence->SetEventOnCompletion(value, own_event))) return false;
    // An older registration can wake the event too: only the fence's value proves completion.
    const std::uint64_t deadline = GetTickCount64() + FENCE_TIMEOUT_MILLISECONDS;
    while (own_fence->GetCompletedValue() < value) {
      const std::uint64_t now = GetTickCount64();
      if (now >= deadline || WaitForSingleObject(own_event, static_cast<DWORD>(deadline - now)) == WAIT_FAILED) return false;
    }
    return true;
  }

  // The device on the game's adapter and FSR's loader from `folder`.
  bool Start(IDXGIAdapter1* adapter, const std::filesystem::path& folder) {
#if defined(BATMANAC_FSR_SDK)
    // Loaded once per process: FSR's restarts reuse it.
    static HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    auto* create_device = (d3d12 != nullptr ? reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(d3d12, "D3D12CreateDevice")) : nullptr);
    if (create_device == nullptr) return Fail("cannot load d3d12.dll", HRESULT_FROM_WIN32(GetLastError()));
    if (const HRESULT result = create_device(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)); FAILED(result)) {
      return Fail("cannot create the D3D12 device", result);
    }
    const D3D12_COMMAND_QUEUE_DESC queue_desc = {.Type = D3D12_COMMAND_LIST_TYPE_DIRECT};
    HRESULT result = device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue));
    if (SUCCEEDED(result)) {
      result = device->CreateFence(0u, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&own_fence));
    }
    for (Slot& slot : slots) {
      if (SUCCEEDED(result)) {
        result = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator));
      }
      if (SUCCEEDED(result)) {
        result = device->CreateCommandList(0u, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator, nullptr, IID_PPV_ARGS(&slot.list));
      }
      if (SUCCEEDED(result)) {
        result = slot.list->Close();
      }
    }
    if (FAILED(result)) return Fail("cannot create the D3D12 queue", result);
    own_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (own_event == nullptr) return Fail("cannot create the D3D12 completion event", HRESULT_FROM_WIN32(GetLastError()));
    library = LoadLibraryExW((folder / L"amd_fidelityfx_loader_dx12.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (library == nullptr) return Fail("amd_fidelityfx_loader_dx12.dll not found next to the helper", HRESULT_FROM_WIN32(GetLastError()));
    ffxLoadFunctions(&fsr, library);
    if (fsr.CreateContext == nullptr || fsr.DestroyContext == nullptr || fsr.Query == nullptr || fsr.Dispatch == nullptr) {
      return Fail("amd_fidelityfx_loader_dx12.dll is not the expected one", E_NOINTERFACE);
    }
    return true;
#else
    return Fail("FSR is not in this helper's build", E_NOTIMPL);
#endif
  }

  // False when work submitted earlier did not finish in time: nothing it uses may be released then.
  [[nodiscard]] bool WaitIdle() const { return WaitForOwnFence(own_value); }

  // Lets go of a generation's textures and fence; only once WaitIdle succeeded.
  void CloseShared() {
    for (ID3D12Resource*& texture : textures) {
      if (texture != nullptr) {
        texture->Release();
      }
      texture = nullptr;
    }
    if (shared_fence != nullptr) {
      shared_fence->Release();
    }
    shared_fence = nullptr;
  }

  // Opens a generation's textures and fence by name and makes FSR's context for its size; the previous ones must be idle.
  bool Open(DWORD game_process_id, const SharedBlock& block) {
#if defined(BATMANAC_FSR_SDK)
    CloseShared();
    auto open = [&](const std::wstring& name, auto** object) {
      HANDLE handle = nullptr;
      HRESULT result = device->OpenSharedHandleByName(name.c_str(), GENERIC_ALL, &handle);
      if (SUCCEEDED(result)) {
        result = device->OpenSharedHandle(handle, IID_PPV_ARGS(object));
        CloseHandle(handle);
      }
      return result;
    };
    for (std::uint32_t i = 0u; i < SHARED_TEXTURE_COUNT; ++i) {
      const std::wstring name = std::format(L"{}{}_{}_{}", SHARED_TEXTURE_NAME_PREFIX, SHARED_TEXTURE_NAMES[i], game_process_id, block.texture_generation);
      if (const HRESULT result = open(name, &textures[i]); FAILED(result)) return Fail("cannot open the shared textures on D3D12", result);
    }
    const std::wstring fence_name = std::format(L"{}{}_{}", SHARED_FENCE_NAME_PREFIX, game_process_id, block.texture_generation);
    if (const HRESULT result = open(fence_name, &shared_fence); FAILED(result)) return Fail("cannot open the shared fence on D3D12", result);

    if (context == nullptr || context_width != block.width || context_height != block.height) {
      if (context != nullptr) {
        fsr.DestroyContext(&context, nullptr);
      }
      context = nullptr;
      ffxCreateBackendDX12Desc backend_desc = {.header = {.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12}, .device = device};
      ffxCreateContextDescUpscaleVersion version_desc = {
          .header = {.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION, .pNext = &backend_desc.header},
          .version = FFX_UPSCALER_VERSION,
      };
      ffxCreateContextDescUpscale create_desc = {
          .header = {.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, .pNext = &version_desc.header},
          // Auto exposure: FSR needs an exposure (its own, or a texture), and its output undoes it.
          // The depth runs from 0 at the near plane toward 0.999 at infinity (the projection's z, not reversed).
          .flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_DEPTH_INFINITE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE,
          .maxRenderSize = {.width = block.width, .height = block.height},
          .maxUpscaleSize = {.width = block.width, .height = block.height},
      };
      if (const ffxReturnCode_t created = fsr.CreateContext(&context, &create_desc.header, nullptr); created != FFX_API_RETURN_OK) {
        context = nullptr;
        return Fail("cannot create FSR's context", static_cast<HRESULT>(created));
      }
      context_width = block.width;
      context_height = block.height;
      ffxQueryGetProviderVersion provider = {.header = {.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION}};
      fsr.Query(&context, &provider.header);
      version = (provider.versionName != nullptr ? provider.versionName : "unknown");
    }
    history_reset = true;
    return true;
#else
    return Fail("FSR is not in this helper's build", E_NOTIMPL);
#endif
  }

  // One request: `evaluated` says whether FSR ran. False when the add-on's value could not be waited for or signalled
  // (no answer); otherwise it is signalled, also when FSR did not run.
  bool Evaluate(const SharedBlock& block, bool* evaluated) {
    *evaluated = false;
#if defined(BATMANAC_FSR_SDK)
    Slot& slot = slots[next_slot];
    next_slot = (next_slot + 1u) % SLOT_COUNT;
    bool recorded = false;
    if (!WaitForOwnFence(slot.done_value)) {
      Fail("FSR's earlier frame did not finish", HRESULT_FROM_WIN32(WAIT_TIMEOUT));
    } else if (const HRESULT allocator_result = slot.allocator->Reset(); FAILED(allocator_result)) {
      Fail("cannot reset FSR's command allocator", allocator_result);
    } else if (const HRESULT list_result = slot.list->Reset(slot.allocator, nullptr); FAILED(list_result)) {
      Fail("cannot reset FSR's command list", list_result);
    } else {
      // D3D11's shared textures are in the common state between the two devices.
      constexpr std::array<SharedTexture, 4> USED = {SharedTexture::COLOR, SharedTexture::DEPTH, SharedTexture::MOTION_VECTORS, SharedTexture::OUTPUT};
      std::array<D3D12_RESOURCE_BARRIER, USED.size()> barriers = {};
      for (std::uint32_t i = 0u; i < USED.size(); ++i) {
        barriers[i] = {
            .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
            .Transition = {
                .pResource = textures[static_cast<std::uint32_t>(USED[i])],
                .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                .StateBefore = D3D12_RESOURCE_STATE_COMMON,
                .StateAfter = (USED[i] == SharedTexture::OUTPUT ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            },
        };
      }
      slot.list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
      const auto now = std::chrono::steady_clock::now();
      const ffxDispatchDescUpscale dispatch_desc = {
          .header = {.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE},
          .commandList = slot.list,
          .color = ffxApiGetResourceDX12(textures[static_cast<std::uint32_t>(SharedTexture::COLOR)], FFX_API_RESOURCE_STATE_COMPUTE_READ),
          .depth = ffxApiGetResourceDX12(textures[static_cast<std::uint32_t>(SharedTexture::DEPTH)], FFX_API_RESOURCE_STATE_COMPUTE_READ),
          .motionVectors = ffxApiGetResourceDX12(textures[static_cast<std::uint32_t>(SharedTexture::MOTION_VECTORS)], FFX_API_RESOURCE_STATE_COMPUTE_READ),
          .output = ffxApiGetResourceDX12(textures[static_cast<std::uint32_t>(SharedTexture::OUTPUT)], FFX_API_RESOURCE_STATE_UNORDERED_ACCESS),
          // FSR takes the image's shift as it is.
          .jitterOffset = {.x = block.jitter_x, .y = block.jitter_y},
          .motionVectorScale = {.x = block.motion_vector_scale_x, .y = block.motion_vector_scale_y},
          .renderSize = {.width = block.width, .height = block.height},
          .upscaleSize = {.width = block.width, .height = block.height},
          .enableSharpening = block.sharpening != 0u,
          .sharpness = block.sharpness,
          .frameTimeDelta = (history_reset ? 1000.f / 60.f : std::chrono::duration<float, std::milli>(now - last_dispatch_time).count()),
          .preExposure = 1.f,
          .reset = (history_reset || block.reset != 0u),
          .cameraNear = block.camera_near,
          .cameraFar = std::numeric_limits<float>::max(),
          .cameraFovAngleVertical = block.camera_vertical_fov,
          // Unreal units are centimeters.
          .viewSpaceToMetersFactor = 0.01f,
      };
      last_dispatch_time = now;
      if (const ffxReturnCode_t dispatched = fsr.Dispatch(&context, &dispatch_desc.header); dispatched != FFX_API_RETURN_OK) {
        Fail("FSR's dispatch failed", static_cast<HRESULT>(dispatched));
      }
      for (D3D12_RESOURCE_BARRIER& barrier : barriers) {
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
      }
      slot.list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
      if (const HRESULT close_result = slot.list->Close(); FAILED(close_result)) {
        Fail("cannot close FSR's command list", close_result);
      } else {
        recorded = failure.empty();
        history_reset = false;
      }
    }
    // A failed wait or signal leaves the game's GPU to the add-on's watcher once the helper exits.
    HRESULT result = queue->Wait(shared_fence, block.request_value);
    if (SUCCEEDED(result) && recorded) {
      ID3D12CommandList* list = slot.list;
      queue->ExecuteCommandLists(1u, &list);
      // Before the add-on's signal, so the list's completion is tracked even if that signal fails; a failed signal here
      // leaves the slot waiting for good, and the helper then ends without cleanup.
      slot.done_value = ++own_value;
      result = queue->Signal(own_fence, own_value);
    }
    if (SUCCEEDED(result)) {
      result = queue->Signal(shared_fence, block.request_value + 1u);
    }
    if (FAILED(result)) return Fail("cannot wait for or signal the add-on on D3D12", result);
    *evaluated = recorded;
    return true;
#else
    return Fail("FSR is not in this helper's build", E_NOTIMPL);
#endif
  }

  // Frees everything, only once WaitIdle succeeded; Start can then begin again.
  void Release() {
    CloseShared();
#if defined(BATMANAC_FSR_SDK)
    if (context != nullptr) {
      fsr.DestroyContext(&context, nullptr);
    }
#endif
    for (Slot& slot : slots) {
      if (slot.list != nullptr) {
        slot.list->Release();
      }
      if (slot.allocator != nullptr) {
        slot.allocator->Release();
      }
    }
    for (IUnknown* object : std::initializer_list<IUnknown*>{own_fence, queue, device}) {
      if (object != nullptr) {
        object->Release();
      }
    }
    if (own_event != nullptr) {
      CloseHandle(own_event);
    }
    if (library != nullptr) {
      FreeLibrary(library);
    }
    *this = {};
  }
};

}  // namespace batmanac::upscaling
