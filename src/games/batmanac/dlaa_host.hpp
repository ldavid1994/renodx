/*
 * Copyright (C) 2026 RenoDX contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <mutex>
#include <ratio>
#include <shared_mutex>
#include <string>

#include <include/reshade.hpp>

#include "./dlaa_shared.h"
#include "./dlaa_state.hpp"

namespace batmanac::dlaa {

// DLSS only ships for x64, so DLAA runs in a 64-bit host. The add-on writes the inputs into shared textures, signals the
// host and composites its output over the scene once the GPU has it; the CPU normally finds the host's answer to the
// previous frame already there. Any failure turns DLAA off until the device is recreated; from then on the scene stays
// as the game drew it (a failed evaluation can be noticed one frame later, so that frame may still show the previous
// output).
struct DlaaState {
  HANDLE mapping = nullptr;
  HANDLE request_event = nullptr;
  HANDLE done_event = nullptr;
  HANDLE host_process = nullptr;
  // When the host was started: it must report ready within HOST_READY_MILLISECONDS.
  std::uint64_t host_started_tick = 0u;
  std::wstring host_path;
  SharedBlock* block = nullptr;
  std::array<ID3D11Texture2D*, SHARED_TEXTURE_COUNT> textures = {};
  std::array<HANDLE, SHARED_TEXTURE_COUNT> shared_handles = {};
  ID3D11Fence* fence = nullptr;
  HANDLE fence_handle = nullptr;
  ID3D11DeviceContext4* context = nullptr;
  // The value the host last signalled; a request waits for it plus one.
  std::uint64_t done_value = 0u;
  // A request whose answer has not been read yet: read at the next request, normally long there.
  bool pending = false;
  // The last answer was an evaluated frame for the preset requested: the host's feature is ready, so the next request
  // need not wait for the answer.
  bool steady = false;
  float steady_preset = 0.f;
  // Frees the game's GPU if the host stops while it waits for the host's signal.
  HANDLE watcher = nullptr;
  HANDLE watcher_stop = nullptr;
  ID3D11UnorderedAccessView* depth_uav = nullptr;
  ID3D11UnorderedAccessView* motion_vector_uav = nullptr;
  ID3D11UnorderedAccessView* bias_mask_uav = nullptr;
  ID3D11ShaderResourceView* output_srv = nullptr;
  ID3D11UnorderedAccessView* color_uav = nullptr;
  std::uint32_t width = 0u;
  std::uint32_t height = 0u;
  bool failed = false;
  bool vendor_checked = false;
};

static DlaaState dlaa_state;

// Also stops the camera jitter, which without DLAA would shake the image.
static void FailDlaa(const std::string& reason) {
  dlaa_state.failed = true;
  dlaa_enabled = 0.f;
  reshade::log::message(reshade::log::level::warning, std::format("batmanac DLAA: {}; DLAA off", reason).c_str());
}

// The last request the game's GPU waits on, for the watcher thread: the value it waits for (0: none yet), the set's
// generation and when it was handed over.
struct HostWatch {
  std::shared_mutex mutex;
  std::uint64_t awaited_value = 0u;
  std::uint32_t generation = 0u;
  std::uint64_t requested_tick = 0u;
};
static HostWatch host_watch;

static void ReleaseDlaaTextures() {
  for (IUnknown* view : std::initializer_list<IUnknown*>{dlaa_state.color_uav, dlaa_state.depth_uav, dlaa_state.motion_vector_uav, dlaa_state.bias_mask_uav, dlaa_state.output_srv}) {
    if (view != nullptr) {
      view->Release();
    }
  }
  dlaa_state.color_uav = nullptr;
  dlaa_state.depth_uav = nullptr;
  dlaa_state.motion_vector_uav = nullptr;
  dlaa_state.bias_mask_uav = nullptr;
  dlaa_state.output_srv = nullptr;
  for (std::uint32_t i = 0u; i < SHARED_TEXTURE_COUNT; ++i) {
    if (dlaa_state.textures[i] != nullptr) {
      dlaa_state.textures[i]->Release();
    }
    if (dlaa_state.shared_handles[i] != nullptr) {
      CloseHandle(dlaa_state.shared_handles[i]);
    }
    dlaa_state.textures[i] = nullptr;
    dlaa_state.shared_handles[i] = nullptr;
  }
  if (dlaa_state.fence != nullptr) {
    dlaa_state.fence->Release();
  }
  if (dlaa_state.fence_handle != nullptr) {
    CloseHandle(dlaa_state.fence_handle);
  }
  if (dlaa_state.context != nullptr) {
    dlaa_state.context->Release();
  }
  dlaa_state.fence = nullptr;
  dlaa_state.fence_handle = nullptr;
  dlaa_state.context = nullptr;
  dlaa_state.done_value = 0u;
  dlaa_state.steady = false;
  dlaa_state.width = 0u;
  dlaa_state.height = 0u;
}

// Watches the host on its own thread; its exit outranks the stop event. A host that leaves a request unanswered for 5 s
// is ended. Once the host exits after any request, a host process started with "signal" opens the fence by name and
// answers, from the CPU, each request of the game's that the GPU still waits on, retrying once a second until it
// succeeds or the watcher stops.
static DWORD WINAPI WatchHost(void* parameter) {
  constexpr DWORD POLL_MILLISECONDS = 500u;
  constexpr std::uint64_t HANG_MILLISECONDS = 5000u;
  const std::array<HANDLE, 2> handles = {dlaa_state.host_process, dlaa_state.watcher_stop};
  for (;;) {
    const DWORD woken = WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, POLL_MILLISECONDS);
    if (woken == WAIT_FAILED) {
      WarnOnce("the host watcher's wait failed; it stops and can no longer free the game's GPU if the host dies", HRESULT_FROM_WIN32(GetLastError()));
    }
    if (woken != WAIT_TIMEOUT && woken != WAIT_OBJECT_0) return 0u;
    std::uint64_t awaited_value = 0u;
    std::uint32_t generation = 0u;
    std::uint64_t requested_tick = 0u;
    {
      const std::shared_lock lock(host_watch.mutex);
      awaited_value = host_watch.awaited_value;
      generation = host_watch.generation;
      requested_tick = host_watch.requested_tick;
    }
    if (woken == WAIT_TIMEOUT) {
      if (awaited_value != 0u && dlaa_state.block->done_value < awaited_value && GetTickCount64() - requested_tick > HANG_MILLISECONDS) {
        TerminateProcess(dlaa_state.host_process, 1u);
      }
      continue;
    }
    if (awaited_value == 0u) return 0u;
    std::wstring command_line = std::format(L"\"{}\" signal {} {} {} {} {}", dlaa_state.host_path, GetCurrentProcessId(), generation, awaited_value,
                                            dlaa_state.block->adapter_luid_low, dlaa_state.block->adapter_luid_high);
    // Until one signals, the game's GPU waits for good: attempts repeat once a second until one succeeds or the watcher
    // is stopped.
    DWORD exit_code = 1u;
    while (exit_code != 0u) {
      STARTUPINFOW startup_info = {.cb = sizeof(STARTUPINFOW)};
      PROCESS_INFORMATION process_info = {};
      if (CreateProcessW(dlaa_state.host_path.c_str(), command_line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup_info, &process_info) != FALSE) {
        // A signalling process that hangs is ended before the next attempt.
        if (WaitForSingleObject(process_info.hProcess, 15000u) != WAIT_OBJECT_0) {
          TerminateProcess(process_info.hProcess, 1u);
          WaitForSingleObject(process_info.hProcess, INFINITE);
        }
        GetExitCodeProcess(process_info.hProcess, &exit_code);
        CloseHandle(process_info.hThread);
        CloseHandle(process_info.hProcess);
      } else {
        WarnOnce("cannot start the signal helper; retrying every second", HRESULT_FROM_WIN32(GetLastError()));
      }
      if (exit_code != 0u && WaitForSingleObject(dlaa_state.watcher_stop, 1000u) == WAIT_OBJECT_0) return 0u;
    }
    return 0u;
  }
}

// The host answers before its fence signal runs on the GPU. S_OK once the fence has reached the last answer's value
// (within 5 s), else the timeout or the error that stopped the wait: until then a host that dies leaves the game's GPU
// waiting, and only the watcher can free it.
static HRESULT WaitForHostSignal() {
  if (dlaa_state.fence == nullptr || dlaa_state.fence->GetCompletedValue() >= dlaa_state.done_value) return S_OK;
  HANDLE completed_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (completed_event == nullptr) return HRESULT_FROM_WIN32(GetLastError());
  HRESULT result = dlaa_state.fence->SetEventOnCompletion(dlaa_state.done_value, completed_event);
  if (SUCCEEDED(result)) {
    switch (WaitForSingleObject(completed_event, 5000u)) {
      case WAIT_OBJECT_0:
        break;
      case WAIT_TIMEOUT:
        result = HRESULT_FROM_WIN32(WAIT_TIMEOUT);
        break;
      default:
        result = HRESULT_FROM_WIN32(GetLastError());
        break;
    }
  }
  CloseHandle(completed_event);
  return result;
}

// Reads the host's answer to the pending request (waiting at most `timeout` ms). False when the host did not answer or
// did not evaluate the frame: DLAA is then off, and a host that hangs is ended so the watcher frees the game's GPU.
static bool ReadHostAnswer(DWORD timeout) {
  if (!dlaa_state.pending) return true;
  dlaa_state.pending = false;
  const auto start = std::chrono::steady_clock::now();
  const std::array<HANDLE, 2> handles = {dlaa_state.done_event, dlaa_state.host_process};
  const bool answered = WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, timeout) == WAIT_OBJECT_0
                        && dlaa_state.block->done_value > dlaa_state.done_value;
  const double waited_milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  resolve_state.host_wait_milliseconds += waited_milliseconds;
  resolve_state.host_wait_max_milliseconds = std::max(resolve_state.host_wait_max_milliseconds, waited_milliseconds);
  if (!answered) {
    TerminateProcess(dlaa_state.host_process, 1u);
    FailDlaa("the host did not answer");
    return false;
  }
  dlaa_state.done_value = dlaa_state.block->done_value;
  // The host signals after a failed evaluation too; only a completed frame holds DLAA's result.
  if (dlaa_state.block->completed_frame != dlaa_state.block->frame) {
    FailDlaa("the host did not evaluate the frame");
    return false;
  }
  dlaa_state.steady = true;
  return true;
}

// Starts the host and (re)creates the shared textures for the scene size. False while the host is starting or after
// a failure.
static bool StartDlaa(ID3D11DeviceContext* context, std::uint32_t width, std::uint32_t height) {
  if (!dlaa_state.vendor_checked) {
    dlaa_state.vendor_checked = true;
    ID3D11Device* device = nullptr;
    context->GetDevice(&device);
    IDXGIDevice* dxgi_device = nullptr;
    IDXGIAdapter* adapter = nullptr;
    DXGI_ADAPTER_DESC adapter_desc = {};
    HRESULT adapter_result = device->QueryInterface(IID_PPV_ARGS(&dxgi_device));
    if (SUCCEEDED(adapter_result)) {
      adapter_result = dxgi_device->GetAdapter(&adapter);
    }
    if (SUCCEEDED(adapter_result)) {
      adapter_result = adapter->GetDesc(&adapter_desc);
    }
    if (FAILED(adapter_result)) {
      WarnOnce("cannot read the GPU adapter; the NVIDIA check is skipped", adapter_result);
    }
    for (IUnknown* object : std::initializer_list<IUnknown*>{adapter, dxgi_device, device}) {
      if (object != nullptr) {
        object->Release();
      }
    }
    constexpr UINT NVIDIA_VENDOR_ID = 0x10DEu;
    if (adapter_desc.VendorId != 0u && adapter_desc.VendorId != NVIDIA_VENDOR_ID) {
      dlaa_supported = false;
      FailDlaa(std::format("the GPU (vendor 0x{:04X}) is not an NVIDIA GPU; DLAA needs an NVIDIA RTX GPU", adapter_desc.VendorId));
      return false;
    }
  }
  if (dlaa_state.failed) {
    // Turned back on in the menu after a failure: stay off.
    dlaa_enabled = 0.f;
    return false;
  }

  if (dlaa_state.block == nullptr) {
    const DWORD process_id = GetCurrentProcessId();
    dlaa_state.mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0u, sizeof(SharedBlock),
                                            std::format(L"{}{}", SHARED_BLOCK_NAME, process_id).c_str());
    dlaa_state.request_event = CreateEventW(nullptr, FALSE, FALSE, std::format(L"{}{}", REQUEST_EVENT_NAME, process_id).c_str());
    dlaa_state.done_event = CreateEventW(nullptr, FALSE, FALSE, std::format(L"{}{}", DONE_EVENT_NAME, process_id).c_str());
    dlaa_state.watcher_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (dlaa_state.mapping != nullptr) {
      dlaa_state.block = static_cast<SharedBlock*>(MapViewOfFile(dlaa_state.mapping, FILE_MAP_ALL_ACCESS, 0u, 0u, sizeof(SharedBlock)));
    }
    if (dlaa_state.block == nullptr || dlaa_state.request_event == nullptr || dlaa_state.done_event == nullptr || dlaa_state.watcher_stop == nullptr) {
      FailDlaa("cannot create the shared block");
      return false;
    }
    *dlaa_state.block = {.protocol_version = PROTOCOL_VERSION};

    ID3D11Device* device = nullptr;
    context->GetDevice(&device);
    IDXGIDevice* dxgi_device = nullptr;
    IDXGIAdapter* adapter = nullptr;
    DXGI_ADAPTER_DESC adapter_desc = {};
    HRESULT adapter_result = device->QueryInterface(IID_PPV_ARGS(&dxgi_device));
    if (SUCCEEDED(adapter_result)) {
      adapter_result = dxgi_device->GetAdapter(&adapter);
    }
    if (SUCCEEDED(adapter_result)) {
      adapter_result = adapter->GetDesc(&adapter_desc);
      adapter->Release();
    }
    if (FAILED(adapter_result)) {
      WarnOnce("cannot read the GPU adapter; the host will not find the game's GPU", adapter_result);
    }
    if (dxgi_device != nullptr) {
      dxgi_device->Release();
    }
    device->Release();
    dlaa_state.block->adapter_luid_low = adapter_desc.AdapterLuid.LowPart;
    dlaa_state.block->adapter_luid_high = adapter_desc.AdapterLuid.HighPart;

    // The host sits next to this add-on.
    HMODULE addon_module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&StartDlaa), &addon_module);
    std::array<wchar_t, MAX_PATH> addon_path = {};
    GetModuleFileNameW(addon_module, addon_path.data(), static_cast<DWORD>(addon_path.size()));
    dlaa_state.host_path = (std::filesystem::path(addon_path.data()).parent_path() / HOST_EXECUTABLE).wstring();
    std::wstring command_line = std::format(L"\"{}\" {}", dlaa_state.host_path, process_id);
    STARTUPINFOW startup_info = {.cb = sizeof(STARTUPINFOW)};
    PROCESS_INFORMATION process_info = {};
    if (CreateProcessW(dlaa_state.host_path.c_str(), command_line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup_info, &process_info) == FALSE) {
      FailDlaa(std::format("cannot start {} (error {})", std::filesystem::path(dlaa_state.host_path).string(), GetLastError()));
      return false;
    }
    CloseHandle(process_info.hThread);
    dlaa_state.host_process = process_info.hProcess;
    dlaa_state.host_started_tick = GetTickCount64();
    dlaa_state.watcher = CreateThread(nullptr, 0u, &WatchHost, nullptr, 0u, nullptr);
    if (dlaa_state.watcher == nullptr) {
      TerminateProcess(dlaa_state.host_process, 1u);
      FailDlaa("cannot start the host watcher");
      return false;
    }
    reshade::log::message(reshade::log::level::info, "batmanac DLAA: host started");
  }

  if (dlaa_state.block->host_state == HostState::FAILED) {
    FailDlaa(std::format("the host stopped (0x{:08X}, see renodx-batmanac-dlaa-host64.log)", static_cast<std::uint32_t>(dlaa_state.block->host_error)));
    return false;
  }
  if (WaitForSingleObject(dlaa_state.host_process, 0u) == WAIT_OBJECT_0) {
    FailDlaa("the host exited");
    return false;
  }
  if (dlaa_state.block->host_state != HostState::READY) {
    // NGX's first initialization can take seconds; a host still not ready after 30 s hangs.
    constexpr std::uint64_t HOST_READY_MILLISECONDS = 30000u;
    if (GetTickCount64() - dlaa_state.host_started_tick > HOST_READY_MILLISECONDS) {
      TerminateProcess(dlaa_state.host_process, 1u);
      FailDlaa("the host did not become ready within 30 s");
    }
    return false;
  }

  if (dlaa_state.width == width && dlaa_state.height == height) return true;

  // The host may still read the old textures, and the old fence is retired only once the watcher no longer has to signal
  // it.
  if (!ReadHostAnswer(5000u)) return false;
  if (FAILED(WaitForHostSignal())) {
    TerminateProcess(dlaa_state.host_process, 1u);
    FailDlaa("the host's last frame did not finish");
    return false;
  }
  ReleaseDlaaTextures();
  ID3D11Device* device = nullptr;
  context->GetDevice(&device);
  constexpr std::array<DXGI_FORMAT, SHARED_TEXTURE_COUNT> FORMATS = {
      DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT};
  const std::uint32_t generation = dlaa_state.block->texture_generation + 1u;
  bool created = true;
  for (std::uint32_t i = 0u; i < SHARED_TEXTURE_COUNT && created; ++i) {
    const D3D11_TEXTURE2D_DESC desc = {
        .Width = width,
        .Height = height,
        .MipLevels = 1u,
        .ArraySize = 1u,
        .Format = FORMATS[i],
        .SampleDesc = {.Count = 1u},
        .Usage = D3D11_USAGE_DEFAULT,
        .BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
        .MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE,
    };
    IDXGIResource1* dxgi_resource = nullptr;
    const std::wstring name = std::format(L"{}{}_{}_{}", SHARED_TEXTURE_NAME_PREFIX, SHARED_TEXTURE_NAMES[i], GetCurrentProcessId(), generation);
    created = SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &dlaa_state.textures[i]))
              && SUCCEEDED(dlaa_state.textures[i]->QueryInterface(IID_PPV_ARGS(&dxgi_resource)))
              && SUCCEEDED(dxgi_resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, name.c_str(), &dlaa_state.shared_handles[i]));
    if (dxgi_resource != nullptr) {
      dxgi_resource->Release();
    }
  }
  created = created
            && SUCCEEDED(device->CreateUnorderedAccessView(dlaa_state.textures[static_cast<std::uint32_t>(SharedTexture::COLOR)], nullptr, &dlaa_state.color_uav))
            && SUCCEEDED(device->CreateUnorderedAccessView(dlaa_state.textures[static_cast<std::uint32_t>(SharedTexture::DEPTH)], nullptr, &dlaa_state.depth_uav))
            && SUCCEEDED(device->CreateUnorderedAccessView(dlaa_state.textures[static_cast<std::uint32_t>(SharedTexture::MOTION_VECTORS)], nullptr, &dlaa_state.motion_vector_uav))
            && SUCCEEDED(device->CreateUnorderedAccessView(dlaa_state.textures[static_cast<std::uint32_t>(SharedTexture::BIAS_CURRENT_COLOR_MASK)], nullptr, &dlaa_state.bias_mask_uav))
            && SUCCEEDED(device->CreateShaderResourceView(dlaa_state.textures[static_cast<std::uint32_t>(SharedTexture::OUTPUT)], nullptr, &dlaa_state.output_srv));
  // The fence, shared by name (the watcher's signalling process opens it so) and duplicated into the host.
  ID3D11Device5* device5 = nullptr;
  HANDLE host_fence_handle = nullptr;
  created = created && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device5)))
            && SUCCEEDED(device5->CreateFence(0u, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&dlaa_state.fence)))
            && SUCCEEDED(dlaa_state.fence->CreateSharedHandle(nullptr, GENERIC_ALL, std::format(L"{}{}_{}", SHARED_FENCE_NAME_PREFIX, GetCurrentProcessId(), generation).c_str(),
                                                              &dlaa_state.fence_handle))
            && DuplicateHandle(GetCurrentProcess(), dlaa_state.fence_handle, dlaa_state.host_process, &host_fence_handle, 0u, FALSE, DUPLICATE_SAME_ACCESS) != FALSE
            && SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&dlaa_state.context)));
  if (device5 != nullptr) {
    device5->Release();
  }
  device->Release();
  if (!created) {
    if (host_fence_handle != nullptr) {
      DuplicateHandle(dlaa_state.host_process, host_fence_handle, nullptr, nullptr, 0u, FALSE, DUPLICATE_CLOSE_SOURCE);
    }
    ReleaseDlaaTextures();
    FailDlaa("cannot create the shared textures");
    return false;
  }
  dlaa_state.width = width;
  dlaa_state.height = height;
  dlaa_state.block->width = width;
  dlaa_state.block->height = height;
  dlaa_state.block->fence_handle = reinterpret_cast<std::uintptr_t>(host_fence_handle);
  dlaa_state.block->texture_generation = generation;
  reshade::log::message(reshade::log::level::info, std::format("batmanac DLAA: shared textures {}x{}, generation {}", width, height, generation).c_str());
  return true;
}

// One frame: writes the inputs (prepare), hands them to the host and composites its output (composite) after the GPU
// waited for it. The answer is read at the next request; the first request after new textures or a preset change
// waits for it, since the host then creates its DLSS feature. False when the exchange failed; the scene then stays as
// the game drew it.
template <typename Prepare, typename Composite>
static bool RunDlaa(const std::array<float, 2>& jitter_pixels, bool history_valid, const Prepare& prepare, const Composite& composite) {
  constexpr DWORD ANSWER_TIMEOUT_MILLISECONDS = 5000u;
  if (!ReadHostAnswer(ANSWER_TIMEOUT_MILLISECONDS)) return false;
  const bool steady = dlaa_state.steady && dlaa_state.steady_preset == dlaa_preset;
  prepare();
  const std::uint64_t request_value = dlaa_state.done_value + 1u;
  if (FAILED(dlaa_state.context->Signal(dlaa_state.fence, request_value))) {
    FailDlaa("cannot signal the host");
    return false;
  }
  dlaa_state.context->Flush();

  // DLSS takes the jitter as the sample's offset within the pixel: the opposite of the image shift.
  dlaa_state.block->jitter_x = -jitter_pixels[0];
  dlaa_state.block->jitter_y = -jitter_pixels[1];
  dlaa_state.block->motion_vector_scale_x = 1.f;
  dlaa_state.block->motion_vector_scale_y = 1.f;
  dlaa_state.block->reset = (history_valid ? 0u : 1u);
  dlaa_state.block->preset = static_cast<std::uint32_t>(dlaa_preset);
  dlaa_state.block->request_value = request_value;
  ++dlaa_state.block->frame;
  {
    const std::unique_lock lock(host_watch.mutex);
    host_watch.awaited_value = request_value + 1u;
    host_watch.generation = dlaa_state.block->texture_generation;
    host_watch.requested_tick = GetTickCount64();
  }
  dlaa_state.pending = true;
  dlaa_state.steady_preset = dlaa_preset;
  SetEvent(dlaa_state.request_event);
  // A host that exited before the request was published may have been seen by the watcher too early: no GPU wait then.
  if (WaitForSingleObject(dlaa_state.host_process, 0u) == WAIT_OBJECT_0) {
    dlaa_state.pending = false;
    FailDlaa("the host exited");
    return false;
  }
  if (!steady && !ReadHostAnswer(ANSWER_TIMEOUT_MILLISECONDS)) return false;
  if (FAILED(dlaa_state.context->Wait(dlaa_state.fence, request_value + 1u))) {
    FailDlaa("cannot wait for the host");
    return false;
  }
  composite();
  return true;
}

}  // namespace batmanac::dlaa
