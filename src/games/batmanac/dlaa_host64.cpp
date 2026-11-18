/*
 * Copyright (C) 2026 RenoDX contributors
 * SPDX-License-Identifier: MIT
 */

// 64-bit helper that runs NVIDIA DLAA for the 32-bit Batman: Arkham City add-on. The add-on starts it with its process
// id, shares the inputs and the output as textures on the same adapter, ordered by a shared fence, and signals one event
// per frame.

#include <Windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_2.h>

#include <array>
#include <cwchar>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>

#include "./dlaa_shared.h"

namespace {

std::ofstream log_file;

void Log(const std::string& message) {
  log_file << message << '\n';
  log_file.flush();
}

// The adapter with this LUID, or null.
IDXGIAdapter1* FindAdapter(std::uint32_t luid_low, std::int32_t luid_high) {
  IDXGIFactory1* factory = nullptr;
  IDXGIAdapter1* found = nullptr;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return nullptr;
  IDXGIAdapter1* adapter = nullptr;
  for (UINT index = 0u; found == nullptr && factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; ++index) {
    DXGI_ADAPTER_DESC1 adapter_desc = {};
    adapter->GetDesc1(&adapter_desc);
    if (adapter_desc.AdapterLuid.LowPart == luid_low && adapter_desc.AdapterLuid.HighPart == luid_high) {
      found = adapter;
    } else {
      adapter->Release();
    }
  }
  factory->Release();
  return found;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous_instance, PWSTR command_line, int show_command) {
  namespace dlaa = batmanac::dlaa;

  std::array<wchar_t, MAX_PATH> executable_path = {};
  GetModuleFileNameW(nullptr, executable_path.data(), static_cast<DWORD>(executable_path.size()));
  const std::filesystem::path host_folder = std::filesystem::path(executable_path.data()).parent_path();
  constexpr std::wstring_view SIGNAL_COMMAND = L"signal ";
  // "signal <game process id> <generation> <awaited value> <adapter LUID low> <adapter LUID high>": started by the add-on
  // when the host stopped after a request. Opens the shared fence by name on a D3D12 device, which can signal from the CPU
  // (a D3D11 device cannot), and answers each request of the game's the fence reaches (odd values) until it passes the
  // awaited value. Never signalling ahead of the game's own request signal keeps the fence from being rewound by it.
  if (std::wstring_view(command_line).starts_with(SIGNAL_COMMAND)) {
    log_file.open(host_folder / L"renodx-batmanac-dlaa-host64.log", std::ios::app);
    wchar_t* next = nullptr;
    const auto game_process_id = static_cast<DWORD>(std::wcstoul(command_line + SIGNAL_COMMAND.size(), &next, 10));
    const auto generation = static_cast<std::uint32_t>(std::wcstoul(next, &next, 10));
    const std::uint64_t awaited_value = std::wcstoull(next, &next, 10);
    const auto luid_low = static_cast<std::uint32_t>(std::wcstoul(next, &next, 10));
    const auto luid_high = static_cast<std::int32_t>(std::wcstol(next, &next, 10));
    HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    auto* create_device = (d3d12 != nullptr ? reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(d3d12, "D3D12CreateDevice")) : nullptr);
    IDXGIAdapter1* adapter = FindAdapter(luid_low, luid_high);
    ID3D12Device* device = nullptr;
    HANDLE handle = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE changed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    bool signalled = changed != nullptr && create_device != nullptr && adapter != nullptr && SUCCEEDED(create_device(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))
                     && SUCCEEDED(device->OpenSharedHandleByName(std::format(L"{}{}_{}", batmanac::dlaa::SHARED_FENCE_NAME_PREFIX, game_process_id, generation).c_str(),
                                                                 GENERIC_ALL, &handle))
                     && SUCCEEDED(device->OpenSharedHandle(handle, IID_PPV_ARGS(&fence)));
    if (signalled) {
      const std::uint64_t deadline = GetTickCount64() + 10000u;
      std::uint64_t completed = fence->GetCompletedValue();
      while (completed < awaited_value && GetTickCount64() < deadline) {
        if ((completed & 1u) != 0u) {
          if (FAILED(fence->Signal(completed + 1u))) break;
        } else if (SUCCEEDED(fence->SetEventOnCompletion(completed + 1u, changed))) {
          // The game's next request signal has not landed yet.
          WaitForSingleObject(changed, 100u);
        }
        completed = fence->GetCompletedValue();
      }
      signalled = completed >= awaited_value;
    }
    Log(std::format("signal up to {} for process {}: {}", awaited_value, game_process_id, signalled ? "done" : "failed"));
    if (changed != nullptr) {
      CloseHandle(changed);
    }
    if (fence != nullptr) {
      fence->Release();
    }
    if (handle != nullptr) {
      CloseHandle(handle);
    }
    if (device != nullptr) {
      device->Release();
    }
    if (adapter != nullptr) {
      adapter->Release();
    }
    return signalled ? 0 : 1;
  }
  const DWORD game_process_id = std::wcstoul(command_line, nullptr, 10);
  log_file.open(host_folder / L"renodx-batmanac-dlaa-host64.log", std::ios::trunc);
  Log(std::format("DLAA host for process {}", game_process_id));

  HANDLE game_process = OpenProcess(SYNCHRONIZE, FALSE, game_process_id);
  HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, std::format(L"{}{}", dlaa::SHARED_BLOCK_NAME, game_process_id).c_str());
  HANDLE request_event = OpenEventW(SYNCHRONIZE, FALSE, std::format(L"{}{}", dlaa::REQUEST_EVENT_NAME, game_process_id).c_str());
  HANDLE done_event = OpenEventW(EVENT_MODIFY_STATE, FALSE, std::format(L"{}{}", dlaa::DONE_EVENT_NAME, game_process_id).c_str());
  if (game_process == nullptr || mapping == nullptr || request_event == nullptr || done_event == nullptr) {
    Log(std::format("cannot open the game's objects (error {})", GetLastError()));
    return 1;
  }
  auto* block = static_cast<dlaa::SharedBlock*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(dlaa::SharedBlock)));
  if (block == nullptr || block->protocol_version != dlaa::PROTOCOL_VERSION) {
    Log("shared block missing or from another version");
    return 1;
  }
  auto fail = [&](const std::string& reason, int error) {
    Log(std::format("{} (0x{:08X})", reason, static_cast<unsigned int>(error)));
    block->host_error = error;
    block->host_state = dlaa::HostState::FAILED;
  };

  // A device on the game's adapter, so the shared textures stay on the same GPU.
  IDXGIAdapter1* game_adapter = FindAdapter(block->adapter_luid_low, block->adapter_luid_high);
  if (game_adapter == nullptr) {
    fail("the game's adapter was not found", 0);
    return 1;
  }
  DXGI_ADAPTER_DESC1 game_adapter_desc = {};
  game_adapter->GetDesc1(&game_adapter_desc);
  Log(std::format("adapter {}", std::filesystem::path(game_adapter_desc.Description).string()));
  ID3D11Device* base_device = nullptr;
  ID3D11DeviceContext* context = nullptr;
  constexpr std::array<D3D_FEATURE_LEVEL, 2> FEATURE_LEVELS = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  HRESULT device_result = D3D11CreateDevice(game_adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0u, FEATURE_LEVELS.data(),
                                            static_cast<UINT>(FEATURE_LEVELS.size()), D3D11_SDK_VERSION, &base_device, nullptr, &context);
  game_adapter->Release();
  ID3D11Device5* device = nullptr;
  ID3D11DeviceContext4* context4 = nullptr;
  if (SUCCEEDED(device_result)) {
    device_result = base_device->QueryInterface(IID_PPV_ARGS(&device));
  }
  if (SUCCEEDED(device_result)) {
    device_result = context->QueryInterface(IID_PPV_ARGS(&context4));
  }
  if (FAILED(device_result)) {
    fail("cannot create the D3D11 device", device_result);
    return 1;
  }
  base_device->Release();

  // NGX looks for nvngx_dlss.dll in this executable's folder.
  NVSDK_NGX_Result ngx_result = NVSDK_NGX_D3D11_Init_with_ProjectID(
      "b5a1c6e2-7d3f-4e8a-9c21-6f0e4d2b8a17", NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", host_folder.c_str(), device);
  if (NVSDK_NGX_FAILED(ngx_result)) {
    fail("NGX initialization failed", ngx_result);
    return 1;
  }
  NVSDK_NGX_Parameter* parameters = nullptr;
  ngx_result = NVSDK_NGX_D3D11_GetCapabilityParameters(&parameters);
  int dlss_available = 0;
  if (NVSDK_NGX_SUCCEED(ngx_result)) {
    ngx_result = parameters->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &dlss_available);
  }
  if (dlss_available == 0) {
    fail("DLSS is not available on this GPU or driver", ngx_result);
    if (parameters != nullptr) {
      NVSDK_NGX_D3D11_DestroyParameters(parameters);
    }
    NVSDK_NGX_D3D11_Shutdown1(device);
    return 1;
  }
  // The game's exposure, handed to DLSS in place of its auto exposure: a scene value of 1 is the game's white, with no
  // eye adaptation. 0x3C00 is 1 in half precision.
  constexpr std::uint16_t EXPOSURE_ONE = 0x3C00u;
  const D3D11_TEXTURE2D_DESC exposure_desc = {
      .Width = 1u, .Height = 1u, .MipLevels = 1u, .ArraySize = 1u, .Format = DXGI_FORMAT_R16_FLOAT,
      .SampleDesc = {.Count = 1u}, .Usage = D3D11_USAGE_IMMUTABLE, .BindFlags = D3D11_BIND_SHADER_RESOURCE};
  const D3D11_SUBRESOURCE_DATA exposure_data = {.pSysMem = &EXPOSURE_ONE, .SysMemPitch = sizeof(EXPOSURE_ONE)};
  ID3D11Texture2D* exposure_texture = nullptr;
  if (const HRESULT result = device->CreateTexture2D(&exposure_desc, &exposure_data, &exposure_texture); FAILED(result)) {
    fail("cannot create the exposure texture", result);
    NVSDK_NGX_D3D11_DestroyParameters(parameters);
    NVSDK_NGX_D3D11_Shutdown1(device);
    return 1;
  }

  block->host_state = dlaa::HostState::READY;
  Log("NGX ready");

  std::array<ID3D11Texture2D*, dlaa::SHARED_TEXTURE_COUNT> textures = {};
  ID3D11Fence* fence = nullptr;
  NVSDK_NGX_Handle* feature = nullptr;
  std::uint32_t opened_generation = 0u;
  std::uint32_t created_preset = 0u;
  auto release_shared = [&]() {
    if (feature != nullptr) {
      NVSDK_NGX_D3D11_ReleaseFeature(feature);
    }
    feature = nullptr;
    for (ID3D11Texture2D*& texture : textures) {
      if (texture != nullptr) {
        texture->Release();
      }
      texture = nullptr;
    }
  };
  // Signals the value after the request's, failed or not, so the game's GPU never waits for a frame that will not come.
  // False when the signal failed: the host then stops and the add-on frees its GPU.
  auto answer = [&]() {
    const std::uint64_t done_value = block->request_value + 1u;
    if (const HRESULT result = context4->Signal(fence, done_value); FAILED(result)) {
      fail("cannot signal the add-on", result);
      return false;
    }
    context->Flush();
    block->done_value = done_value;
    SetEvent(done_event);
    return true;
  };

  while (block->host_state == dlaa::HostState::READY) {
    const std::array<HANDLE, 2> waits = {game_process, request_event};
    if (WaitForMultipleObjects(static_cast<DWORD>(waits.size()), waits.data(), FALSE, 1000u) == WAIT_OBJECT_0) break;
    if (block->texture_generation == 0u || block->frame == block->completed_frame) continue;

    // The previous evaluation may still run on the GPU: DLSS's feature and the old textures are released only once it
    // finished. At most 3 s, so a stall is reported before the add-on's 5 s wait for the answer ends this host.
    if (feature != nullptr && (block->texture_generation != opened_generation || block->preset != created_preset)) {
      const D3D11_QUERY_DESC idle_desc = {.Query = D3D11_QUERY_EVENT};
      ID3D11Query* idle_query = nullptr;
      HRESULT idle_result = device->CreateQuery(&idle_desc, &idle_query);
      if (SUCCEEDED(idle_result)) {
        context->End(idle_query);
        const std::uint64_t idle_deadline = GetTickCount64() + 3000u;
        BOOL idle = FALSE;
        idle_result = context->GetData(idle_query, &idle, sizeof(idle), 0u);
        while (idle_result == S_FALSE && GetTickCount64() < idle_deadline) {
          Sleep(0u);
          idle_result = context->GetData(idle_query, &idle, sizeof(idle), 0u);
        }
        idle_query->Release();
      }
      if (idle_result != S_OK) {
        // Releasing the feature now could fault in the driver: the host ends without cleanup, and the add-on's watcher
        // answers the game's requests.
        fail("the previous frame did not finish", (idle_result == S_FALSE ? HRESULT_FROM_WIN32(WAIT_TIMEOUT) : idle_result));
        return 1;
      }
    }
    if (block->texture_generation != opened_generation) {
      if (fence != nullptr) {
        fence->Release();
      }
      fence = nullptr;
      // The add-on duplicated the new set's fence handle into this process.
      if (const HRESULT result = device->OpenSharedFence(reinterpret_cast<HANDLE>(block->fence_handle), IID_PPV_ARGS(&fence)); FAILED(result)) {
        fail("cannot open the shared fence", result);
        break;
      }
      CloseHandle(reinterpret_cast<HANDLE>(block->fence_handle));
    }
    // The add-on's inputs are written once its value is signalled; every answer after this is ordered behind it. A failed
    // wait gives no answer: the host stops and the add-on's watcher answers in order.
    if (const HRESULT result = context4->Wait(fence, block->request_value); FAILED(result)) {
      fail("cannot wait for the add-on", result);
      break;
    }
    if (block->texture_generation != opened_generation || block->preset != created_preset) {
      release_shared();
      HRESULT open_result = S_OK;
      for (std::uint32_t i = 0u; i < dlaa::SHARED_TEXTURE_COUNT && SUCCEEDED(open_result); ++i) {
        const std::wstring name = std::format(L"{}{}_{}_{}", dlaa::SHARED_TEXTURE_NAME_PREFIX, dlaa::SHARED_TEXTURE_NAMES[i],
                                              game_process_id, block->texture_generation);
        open_result = device->OpenSharedResourceByName(name.c_str(), DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, IID_PPV_ARGS(&textures[i]));
      }
      if (FAILED(open_result)) {
        fail("cannot open the shared textures", open_result);
        answer();
        break;
      }
      NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, block->preset);
      NVSDK_NGX_DLSS_Create_Params create_params = {
          .Feature = {
              .InWidth = block->width,
              .InHeight = block->height,
              .InTargetWidth = block->width,
              .InTargetHeight = block->height,
              .InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_DLAA,
          },
          .InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes,
      };
      ngx_result = NGX_D3D11_CREATE_DLSS_EXT(context, &feature, parameters, &create_params);
      if (NVSDK_NGX_FAILED(ngx_result)) {
        fail("cannot create the DLAA feature", ngx_result);
        answer();
        break;
      }
      opened_generation = block->texture_generation;
      created_preset = block->preset;
      Log(std::format("DLAA {}x{}, preset {}, generation {}", block->width, block->height, block->preset, opened_generation));
    }

    NVSDK_NGX_D3D11_DLSS_Eval_Params evaluate_params = {
        .Feature = {
            .pInColor = textures[static_cast<std::uint32_t>(dlaa::SharedTexture::COLOR)],
            .pInOutput = textures[static_cast<std::uint32_t>(dlaa::SharedTexture::OUTPUT)],
        },
        .pInDepth = textures[static_cast<std::uint32_t>(dlaa::SharedTexture::DEPTH)],
        .pInMotionVectors = textures[static_cast<std::uint32_t>(dlaa::SharedTexture::MOTION_VECTORS)],
        .InJitterOffsetX = block->jitter_x,
        .InJitterOffsetY = block->jitter_y,
        .InRenderSubrectDimensions = {.Width = block->width, .Height = block->height},
        .InReset = static_cast<int>(block->reset),
        .InMVScaleX = block->motion_vector_scale_x,
        .InMVScaleY = block->motion_vector_scale_y,
        .pInExposureTexture = exposure_texture,
        .pInBiasCurrentColorMask = textures[static_cast<std::uint32_t>(dlaa::SharedTexture::BIAS_CURRENT_COLOR_MASK)],
        .InPreExposure = 1.f,
        .InExposureScale = 1.f,
    };
    ngx_result = NGX_D3D11_EVALUATE_DLSS_EXT(context, feature, parameters, &evaluate_params);
    // Published before the answer: the add-on keeps DLAA on only for a frame marked completed.
    const bool evaluated = !NVSDK_NGX_FAILED(ngx_result);
    if (evaluated) {
      block->completed_frame = block->frame;
    } else {
      fail("DLAA evaluation failed", ngx_result);
    }
    if (!answer()) break;
    if (!evaluated) break;
  }

  release_shared();
  if (fence != nullptr) {
    fence->Release();
  }
  exposure_texture->Release();
  NVSDK_NGX_D3D11_DestroyParameters(parameters);
  NVSDK_NGX_D3D11_Shutdown1(device);
  context4->Release();
  context->Release();
  device->Release();
  UnmapViewOfFile(block);
  CloseHandle(request_event);
  CloseHandle(done_event);
  CloseHandle(mapping);
  CloseHandle(game_process);
  Log("DLAA host stopped");
  return 0;
}
