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
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <mutex>
#include <ratio>
#include <shared_mutex>
#include <string>
#include <string_view>

#include <include/reshade.hpp>

#include "./host_protocol.h"
#include "./upscaling_state.hpp"

namespace batmanac::upscaling {

// DLSS only ships for x64, so the upscaler (DLSS or FSR) runs in a 64-bit host. The add-on writes the inputs into shared textures, signals the
// host and composites its output over the scene once the GPU has it; the CPU normally finds the host's answer to the
// previous frame already there. Any failure turns anti-aliasing off until the device is recreated; from then on the scene stays
// as the game drew it (a failed evaluation can be noticed one frame later, so that frame may still show the previous
// output).
struct HostLink {
  HANDLE mapping = nullptr;
  HANDLE request_event = nullptr;
  HANDLE done_event = nullptr;
  HANDLE host_process = nullptr;
  // When the host was started: it must report ready within HOST_READY_MILLISECONDS.
  std::uint64_t host_started_tick = 0u;
  // Set when the device is created: the host next to this add-on, and the game's GPU.
  std::wstring host_path;
  LUID adapter_luid = {};
  SharedBlock* block = nullptr;
  std::array<ID3D11Texture2D*, SHARED_TEXTURE_COUNT> textures = {};
  std::array<HANDLE, SHARED_TEXTURE_COUNT> shared_handles = {};
  ID3D11Fence* fence = nullptr;
  HANDLE fence_handle = nullptr;
  ID3D11DeviceContext4* context = nullptr;
  // The value the host last signalled; a request waits for it plus one.
  std::uint64_t done_value = 0u;
  // The last value the game's GPU was given to wait for (0: none on this fence).
  std::uint64_t gpu_waited_value = 0u;
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
  // The upscaler the textures were made for.
  float upscaler = 0.f;
  bool failed = false;
};

static HostLink host_link;

// Also stops the camera jitter, which without the upscaler would shake the image.
static void TurnOffAntiAliasing(const std::string& reason) {
  host_link.failed = true;
  aa_enabled = 0.f;
  {
    const std::unique_lock lock(upscaler_status.mutex);
    upscaler_status.text = std::format("Off: {}", reason);
  }
  reshade::log::message(reshade::log::level::warning, std::format("batmanac upscaling: {}; anti-aliasing off", reason).c_str());
}

// The last request the game's GPU waits on, for the watcher thread: the value it waits for (0: none to rescue), the set's
// generation and when it was handed over.
struct HostWatch {
  std::shared_mutex mutex;
  std::uint64_t awaited_value = 0u;
  std::uint32_t generation = 0u;
  std::uint64_t requested_tick = 0u;
};
static HostWatch host_watch;

static void ReleaseSharedTextures() {
  for (IUnknown* view : std::initializer_list<IUnknown*>{host_link.color_uav, host_link.depth_uav, host_link.motion_vector_uav, host_link.bias_mask_uav, host_link.output_srv}) {
    if (view != nullptr) {
      view->Release();
    }
  }
  host_link.color_uav = nullptr;
  host_link.depth_uav = nullptr;
  host_link.motion_vector_uav = nullptr;
  host_link.bias_mask_uav = nullptr;
  host_link.output_srv = nullptr;
  for (std::uint32_t i = 0u; i < SHARED_TEXTURE_COUNT; ++i) {
    if (host_link.textures[i] != nullptr) {
      host_link.textures[i]->Release();
    }
    if (host_link.shared_handles[i] != nullptr) {
      CloseHandle(host_link.shared_handles[i]);
    }
    host_link.textures[i] = nullptr;
    host_link.shared_handles[i] = nullptr;
  }
  if (host_link.fence != nullptr) {
    host_link.fence->Release();
  }
  if (host_link.fence_handle != nullptr) {
    CloseHandle(host_link.fence_handle);
  }
  if (host_link.context != nullptr) {
    host_link.context->Release();
  }
  host_link.fence = nullptr;
  host_link.fence_handle = nullptr;
  host_link.context = nullptr;
  host_link.done_value = 0u;
  host_link.gpu_waited_value = 0u;
  host_link.steady = false;
  host_link.width = 0u;
  host_link.height = 0u;
}

// Watches the host on its own thread; its exit outranks the stop event. A host that leaves a request unanswered for 5 s
// is ended. Once the host exits after any request, a host process started with "signal" opens the fence by name and
// answers, from the CPU, each request of the game's that the GPU still waits on, retrying once a second until it
// succeeds or the watcher stops.
static DWORD WINAPI WatchHost(void* parameter) {
  constexpr DWORD POLL_MILLISECONDS = 500u;
  constexpr std::uint64_t HANG_MILLISECONDS = 5000u;
  const std::array<HANDLE, 2> handles = {host_link.host_process, host_link.watcher_stop};
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
      if (awaited_value != 0u && host_link.block->done_value < awaited_value && GetTickCount64() - requested_tick > HANG_MILLISECONDS) {
        // An answer to a newer request (a new texture generation restarts the values) is not this request's: only this
        // request still being the last one ends the host.
        const std::shared_lock lock(host_watch.mutex);
        if (host_watch.awaited_value == awaited_value && host_watch.generation == generation && host_link.block->done_value < awaited_value) {
          TerminateProcess(host_link.host_process, 1u);
        }
      }
      continue;
    }
    if (awaited_value == 0u) return 0u;
    auto signal_command_line = [&]() {
      return std::format(L"\"{}\" signal {} {} {} {} {}", host_link.host_path, GetCurrentProcessId(), generation, awaited_value,
                         host_link.block->adapter_luid_low, host_link.block->adapter_luid_high);
    };
    std::wstring command_line = signal_command_line();
    // Until one signals, the game's GPU waits for good: attempts repeat once a second until one succeeds or the watcher
    // is stopped.
    DWORD exit_code = 1u;
    while (exit_code != 0u) {
      STARTUPINFOW startup_info = {.cb = sizeof(STARTUPINFOW)};
      PROCESS_INFORMATION process_info = {};
      if (CreateProcessW(host_link.host_path.c_str(), command_line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup_info, &process_info) != FALSE) {
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
      if (exit_code != 0u && WaitForSingleObject(host_link.watcher_stop, 1000u) == WAIT_OBJECT_0) return 0u;
      if (exit_code != 0u) {
        // A retired fence (cleared once it passed every value, then replaced by a new set) needs no rescue. A newer
        // request on the same fence doesn't mean the older wait passed: the signal process answers every request up to
        // the newest.
        const std::shared_lock lock(host_watch.mutex);
        if (host_watch.awaited_value == 0u || host_watch.generation != generation) return 0u;
        if (host_watch.awaited_value != awaited_value) {
          awaited_value = host_watch.awaited_value;
          command_line = signal_command_line();
        }
      }
    }
    return 0u;
  }
}

// The host answers before its fence signal runs on the GPU. S_OK once the fence has reached `value` (within `timeout`
// ms), else the timeout or the error that stopped the wait: until then a host that dies leaves the game's GPU waiting,
// and only the watcher can free it.
static HRESULT WaitForFenceValue(std::uint64_t value, DWORD timeout) {
  if (host_link.fence == nullptr || host_link.fence->GetCompletedValue() >= value) return S_OK;
  HANDLE completed_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (completed_event == nullptr) return HRESULT_FROM_WIN32(GetLastError());
  HRESULT result = host_link.fence->SetEventOnCompletion(value, completed_event);
  if (SUCCEEDED(result)) {
    switch (WaitForSingleObject(completed_event, timeout)) {
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

// The host's status: what runs and its version, or why it failed.
static std::string_view HostStatusText() {
  return {host_link.block->upscaler_status, strnlen(host_link.block->upscaler_status, sizeof(host_link.block->upscaler_status))};
}

// The host's failure and its reason when it wrote one, else `otherwise`.
static std::string HostFailure(std::string_view otherwise) {
  if (host_link.block->host_state != HostState::FAILED) return std::string(otherwise);
  return std::format("the host stopped: {} (0x{:08X})", HostStatusText(), static_cast<std::uint32_t>(host_link.block->host_error));
}

// Reads the host's answer to the pending request (waiting at most `timeout` ms). False when the host did not answer or
// did not evaluate the frame: anti-aliasing is then off, and a host that hangs is ended so the watcher frees the game's GPU.
static bool ReadHostAnswer(DWORD timeout) {
  if (!host_link.pending) return true;
  host_link.pending = false;
  const auto start = std::chrono::steady_clock::now();
  const std::array<HANDLE, 2> handles = {host_link.done_event, host_link.host_process};
  const bool answered = WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, timeout) == WAIT_OBJECT_0
                        && host_link.block->done_value > host_link.done_value;
  const double waited_milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  resolve_state.host_wait_milliseconds += waited_milliseconds;
  resolve_state.host_wait_max_milliseconds = std::max(resolve_state.host_wait_max_milliseconds, waited_milliseconds);
  if (!answered) {
    TerminateProcess(host_link.host_process, 1u);
    TurnOffAntiAliasing(HostFailure("the host did not answer"));
    return false;
  }
  host_link.done_value = host_link.block->done_value;
  const std::string_view host_status = HostStatusText();
  {
    const std::unique_lock lock(upscaler_status.mutex);
    if (upscaler_status.text != host_status) {
      upscaler_status.text = host_status;
    }
  }
  // The host signals after a failed evaluation too; only a completed frame holds the upscaler's result. A frame DLSS could not
  // evaluate is no failure: the next StartHost switches to FSR.
  if (host_link.block->completed_frame != host_link.block->frame) {
    if (host_link.upscaler != static_cast<float>(Upscaler::DLSS) || host_link.block->dlss_available != 0u) {
      TurnOffAntiAliasing(std::format("the host did not evaluate the frame ({})", host_status));
    }
    return false;
  }
  host_link.steady = true;
  return true;
}

// Starts the host and (re)creates the shared textures for the scene size. False while the host is starting or after
// a failure.
static bool StartHost(ID3D11DeviceContext* context, std::uint32_t width, std::uint32_t height) {
  if (host_link.block == nullptr) {
    const DWORD process_id = GetCurrentProcessId();
    host_link.mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0u, sizeof(SharedBlock),
                                           std::format(L"{}{}", SHARED_BLOCK_NAME, process_id).c_str());
    host_link.request_event = CreateEventW(nullptr, FALSE, FALSE, std::format(L"{}{}", REQUEST_EVENT_NAME, process_id).c_str());
    host_link.done_event = CreateEventW(nullptr, FALSE, FALSE, std::format(L"{}{}", DONE_EVENT_NAME, process_id).c_str());
    host_link.watcher_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (host_link.mapping != nullptr) {
      host_link.block = static_cast<SharedBlock*>(MapViewOfFile(host_link.mapping, FILE_MAP_ALL_ACCESS, 0u, 0u, sizeof(SharedBlock)));
    }
    if (host_link.block == nullptr || host_link.request_event == nullptr || host_link.done_event == nullptr || host_link.watcher_stop == nullptr) {
      TurnOffAntiAliasing("cannot create the shared block");
      return false;
    }
    *host_link.block = {
        .protocol_version = PROTOCOL_VERSION,
        .adapter_luid_low = host_link.adapter_luid.LowPart,
        .adapter_luid_high = host_link.adapter_luid.HighPart,
    };

    std::wstring command_line = std::format(L"\"{}\" {}", host_link.host_path, process_id);
    STARTUPINFOW startup_info = {.cb = sizeof(STARTUPINFOW)};
    PROCESS_INFORMATION process_info = {};
    if (CreateProcessW(host_link.host_path.c_str(), command_line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup_info, &process_info) == FALSE) {
      TurnOffAntiAliasing(std::format("cannot start {} (error {})", std::filesystem::path(host_link.host_path).string(), GetLastError()));
      return false;
    }
    CloseHandle(process_info.hThread);
    host_link.host_process = process_info.hProcess;
    host_link.host_started_tick = GetTickCount64();
    host_link.watcher = CreateThread(nullptr, 0u, &WatchHost, nullptr, 0u, nullptr);
    if (host_link.watcher == nullptr) {
      TerminateProcess(host_link.host_process, 1u);
      TurnOffAntiAliasing("cannot start the host watcher");
      return false;
    }
    reshade::log::message(reshade::log::level::info, "batmanac upscaling: host started");
  }

  if (host_link.block->host_state == HostState::FAILED || WaitForSingleObject(host_link.host_process, 0u) == WAIT_OBJECT_0) {
    TurnOffAntiAliasing(HostFailure("the host exited"));
    return false;
  }
  if (host_link.block->host_state != HostState::READY) {
    // NGX's first initialization can take seconds; a host still not ready after 30 s hangs.
    constexpr std::uint64_t HOST_READY_MILLISECONDS = 30000u;
    if (GetTickCount64() - host_link.host_started_tick > HOST_READY_MILLISECONDS) {
      TerminateProcess(host_link.host_process, 1u);
      TurnOffAntiAliasing("the host did not become ready within 30 s");
    }
    return false;
  }
  if (dlss_available && host_link.block->dlss_available == 0u) {
    dlss_available = false;
    reshade::log::message(reshade::log::level::info, "batmanac upscaling: DLSS cannot run (see renodx-batmanac-dlaa-host64.log); FSR replaces it");
  }
  // The player's choice stays as saved (DLAA may run again on a later device); where DLSS cannot run, FSR is used.
  const float used_upscaler = (dlss_available ? upscaler : static_cast<float>(Upscaler::FSR));

  if (host_link.width == width && host_link.height == height && host_link.upscaler == used_upscaler) return true;

  // The host may still read the old textures, and the old fence is retired only once the watcher no longer has to signal
  // it.
  if (!ReadHostAnswer(5000u)) return false;
  if (FAILED(WaitForFenceValue(host_link.done_value, 5000u))) {
    TerminateProcess(host_link.host_process, 1u);
    TurnOffAntiAliasing("the host's last frame did not finish");
    return false;
  }
  {
    // The old fence passed every value the game waits on: nothing to rescue if the host exits now.
    const std::unique_lock lock(host_watch.mutex);
    host_watch.awaited_value = 0u;
  }
  ReleaseSharedTextures();
  ID3D11Device* device = nullptr;
  context->GetDevice(&device);
  constexpr std::array<DXGI_FORMAT, SHARED_TEXTURE_COUNT> FORMATS = {
      DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT};
  const std::uint32_t generation = host_link.block->texture_generation + 1u;
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
    created = SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &host_link.textures[i]))
              && SUCCEEDED(host_link.textures[i]->QueryInterface(IID_PPV_ARGS(&dxgi_resource)))
              && SUCCEEDED(dxgi_resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, name.c_str(), &host_link.shared_handles[i]));
    if (dxgi_resource != nullptr) {
      dxgi_resource->Release();
    }
  }
  created = created
            && SUCCEEDED(device->CreateUnorderedAccessView(host_link.textures[static_cast<std::uint32_t>(SharedTexture::COLOR)], nullptr, &host_link.color_uav))
            && SUCCEEDED(device->CreateUnorderedAccessView(host_link.textures[static_cast<std::uint32_t>(SharedTexture::DEPTH)], nullptr, &host_link.depth_uav))
            && SUCCEEDED(device->CreateUnorderedAccessView(host_link.textures[static_cast<std::uint32_t>(SharedTexture::MOTION_VECTORS)], nullptr, &host_link.motion_vector_uav))
            && SUCCEEDED(device->CreateUnorderedAccessView(host_link.textures[static_cast<std::uint32_t>(SharedTexture::BIAS_CURRENT_COLOR_MASK)], nullptr, &host_link.bias_mask_uav))
            && SUCCEEDED(device->CreateShaderResourceView(host_link.textures[static_cast<std::uint32_t>(SharedTexture::OUTPUT)], nullptr, &host_link.output_srv));
  // The fence, shared by name (the watcher's signalling process opens it so) and duplicated into the host.
  ID3D11Device5* device5 = nullptr;
  HANDLE host_fence_handle = nullptr;
  created = created && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device5)))
            && SUCCEEDED(device5->CreateFence(0u, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&host_link.fence)))
            && SUCCEEDED(host_link.fence->CreateSharedHandle(nullptr, GENERIC_ALL, std::format(L"{}{}_{}", SHARED_FENCE_NAME_PREFIX, GetCurrentProcessId(), generation).c_str(),
                                                             &host_link.fence_handle))
            && DuplicateHandle(GetCurrentProcess(), host_link.fence_handle, host_link.host_process, &host_fence_handle, 0u, FALSE, DUPLICATE_SAME_ACCESS) != FALSE
            && SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&host_link.context)));
  if (device5 != nullptr) {
    device5->Release();
  }
  device->Release();
  if (!created) {
    if (host_fence_handle != nullptr) {
      DuplicateHandle(host_link.host_process, host_fence_handle, nullptr, nullptr, 0u, FALSE, DUPLICATE_CLOSE_SOURCE);
    }
    ReleaseSharedTextures();
    TurnOffAntiAliasing("cannot create the shared textures");
    return false;
  }
  host_link.width = width;
  host_link.height = height;
  host_link.upscaler = used_upscaler;
  host_link.block->width = width;
  host_link.block->height = height;
  host_link.block->upscaler = static_cast<Upscaler>(used_upscaler);
  host_link.block->fence_handle = reinterpret_cast<std::uintptr_t>(host_fence_handle);
  host_link.block->texture_generation = generation;
  reshade::log::message(reshade::log::level::info, std::format("batmanac upscaling: shared textures {}x{} for upscaler {}, generation {}", width, height, used_upscaler, generation).c_str());
  return true;
}

// One frame: writes the inputs (prepare), hands them to the host and composites its output (composite) after the GPU
// waited for it. The answer is read at the next request; the first request after new textures or a preset change
// waits for it, since the host then creates its DLSS feature. False when the exchange failed; the scene then stays as
// the game drew it.
template <typename Prepare, typename Composite>
static bool RunUpscaler(const ViewMatrices& view, bool history_valid, const Prepare& prepare, const Composite& composite) {
  constexpr DWORD ANSWER_TIMEOUT_MILLISECONDS = 5000u;
  if (!ReadHostAnswer(ANSWER_TIMEOUT_MILLISECONDS)) return false;
  const bool steady = host_link.steady && host_link.steady_preset == dlss_preset;
  prepare();
  const std::uint64_t request_value = host_link.done_value + 1u;
  if (FAILED(host_link.context->Signal(host_link.fence, request_value))) {
    TurnOffAntiAliasing("cannot signal the host");
    return false;
  }
  host_link.context->Flush();

  host_link.block->jitter_x = view.jitter_pixels[0];
  host_link.block->jitter_y = view.jitter_pixels[1];
  // The projection's z, P[2][2] + P[3][2] / w, is 0 at the near plane.
  host_link.block->camera_near = -view.projection_matrix.m[14] / view.projection_matrix.m[10];
  host_link.block->camera_vertical_fov = 2.f * std::atan(1.f / view.projection_matrix.m[5]);
  host_link.block->sharpening = (fsr_sharpening != 0.f ? 1u : 0u);
  host_link.block->sharpness = fsr_sharpness;
  host_link.block->motion_vector_scale_x = 1.f;
  host_link.block->motion_vector_scale_y = 1.f;
  host_link.block->reset = (history_valid ? 0u : 1u);
  host_link.block->preset = static_cast<std::uint32_t>(dlss_preset);
  host_link.block->request_value = request_value;
  ++host_link.block->frame;
  {
    const std::unique_lock lock(host_watch.mutex);
    host_watch.awaited_value = request_value + 1u;
    host_watch.generation = host_link.block->texture_generation;
    host_watch.requested_tick = GetTickCount64();
  }
  host_link.pending = true;
  host_link.steady_preset = dlss_preset;
  SetEvent(host_link.request_event);
  // A host that exited before the request was published may have been seen by the watcher too early: no GPU wait then.
  if (WaitForSingleObject(host_link.host_process, 0u) == WAIT_OBJECT_0) {
    host_link.pending = false;
    TurnOffAntiAliasing(HostFailure("the host exited"));
    return false;
  }
  if (!steady && !ReadHostAnswer(ANSWER_TIMEOUT_MILLISECONDS)) return false;
  if (FAILED(host_link.context->Wait(host_link.fence, request_value + 1u))) {
    TurnOffAntiAliasing("cannot wait for the host");
    return false;
  }
  host_link.gpu_waited_value = request_value + 1u;
  composite();
  return true;
}

}  // namespace batmanac::upscaling
