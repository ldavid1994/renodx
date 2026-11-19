# Batman: Arkham City (batmanac)

HDR for the Steam GOTY release (DX11, 32-bit). Needs ReShade with add-on support installed as `dxgi.dll` next to `Binaries\Win32\BatmanAC.exe`. Use the game's DX11 renderer.

The main story was played and checked end to end; Harley Quinn's Revenge and the challenge maps had spot checks only. The add-on replaces the tonemap uber pass (`0xD160F56A`, `0x2151085D` with Motion Blur on, or `0x280F18BC` with a bokeh depth of field), the detective-mode final pass (`0xC1C294D3`, also Catwoman's thief vision), the LUT builders (`0x3207DF59`, and its variant `0xC240CBE6` that adds a texture to the table coordinates; their 256x16 table upgraded to RGBA16F), the DOF downsample (`0x46BA7BD5`, the blur targets upgraded to RGBA16F) and the bloom bright pass (`0x25455918`, its taps capped at 1 as the original blur target did) and four smoke and flame sheet materials (`0x8E23494B`, `0x2A25E549`, `0xB720ADD7`, `0xE6F50BDA`, for the flame cores below), upgrades the back-buffer-sized target FXAA reads to RGBA16F (FXAA itself only blends, so both FXAA settings keep HDR), and outputs HDR10 on a borderless flip-model swap chain. HDR comes from the game's float scene before its clip at white. Tone Mapper Vanilla matches the game's shaders, apart from 8-bit rounding: the LUT's table and FXAA's input are RGBA16F (under one 8-bit step; with FXAA, a few edge pixels anti-alias slightly differently); PsychoV-30, the default, matches Vanilla within 1% below white and rolls highlights off toward Peak.

## Settings

- **Tone Mapper:** Vanilla (the game's picture, clipped at white), None (no roll-off), PsychoV-30 (default: ShortFuse's PsychoV tone mapper; **Compression** 2 keeps it on Vanilla below white, 0 is automatic; **Hue Clip**, PsychoV-30 only, bends highlight hues toward the game's clip, sky blue twice as far toward cyan).
- **Peak Brightness** defaults to the display's peak. **Game** and **UI Brightness** default to 203 nits.
- **Gamma Correction:** 2.2 (default) shows the game as a typical SDR display does; Off decodes it as sRGB.
- **Color Grading:** Exposure, Highlights, Shadows, Contrast and Saturation work with None and PsychoV-30. **Scene Grading** scales the game's color grade. **Bloom** and **Depth of Field** scale the game's bloom and blur (50 is the game's, 0 turns them off), also under Vanilla. Lens flares are the launcher's switch. Keep Depth of Field on in the launcher: turning it off there makes the game skip its color grade; use the slider instead.
- **Preset Off** switches to Vanilla at 203 nits. Force Borderless and Prevent Fullscreen (Advanced) stay on for HDR.
- **Anti-Aliasing** (off by default) and **Upscaling Method**: DLAA or FSR at native resolution. Where DLAA can't run, the method reads "FSR" without a choice. **DLAA Preset** (DLAA only): K (default) is the steadiest on railings and grates; L keeps falling snow and animated lights, with more shimmer on thin geometry. **FSR Sharpening** (default On) and **FSR Sharpness** (0-100, default 80; FSR only): FSR's own sharpening pass. A grey line under these settings names the upscaler running and its DLL's version, or why anti-aliasing is off. See Anti-aliasing below.

## Flame cores

The game's flame sheets mix a cool color at their thin edges toward a hot color at their dense core. In the emissive ones the cool color is authored far above white (a bright red) but the hot color at SDR white, so their brightness falls toward the core; in HDR, where nothing clips, the core showed as a flat, dim patch inside brighter flames (the Steel Mill wall fires, the Clayface floor fire, the fires on top of Wonder Tower). Outside Vanilla the replaced sheets raise a dimmer hot color, hue kept, to the cool color's luminance, as the materials authored in HDR already have, so the core's color is no longer dimmer than the edge's; a hot color already as bright is left alone. It applies only where the cool color is above white, so smoke and SDR-authored flames (the Gotham window fires) keep the game's colors.

## In-game brightness

Leave the in-game brightness slider at its default (the middle) and set brightness with Game Brightness. The game's slider is a gamma on its SDR image (baked into the color-grade LUT, and the exponent of the detective-mode pass), so it still works with every tone mapper, lifting shadows and midtones on top of Game Brightness.

## Anti-aliasing (DLAA and FSR)

Temporal anti-aliasing at native resolution: NVIDIA DLAA (DLSS at native resolution) or AMD FSR (FSR 4 on AMD GPUs that support it, FSR 3.1 elsewhere, both from the same DLL). Both need anti-aliasing off in the game's launcher (no MSAA, no FXAA).

- **Files in `Binaries\Win32`:** the helper `renodx-batmanac-dlaa-host64.exe` next to the add-on. For FSR, `amd_fidelityfx_loader_dx12.dll` and `amd_fidelityfx_upscaler_dx12.dll` from AMD's FidelityFX SDK v2.4.0 (included), with AMD's license as `amd_fidelityfx_license.md`: AMD allows redistributing these binaries with its notice. For DLAA, `nvngx_dlss.dll` (DLSS 310 or newer, not included: NVIDIA's license doesn't allow it in this release).
- **Which upscaler:** DLAA needs an NVIDIA RTX GPU and `nvngx_dlss.dll`. On another GPU, without the DLL, or when DLSS fails to start or to evaluate, FSR replaces it and the method reads "FSR"; the saved choice stays for when DLAA can run again.
- **Helper process:** DLSS and FSR ship only for 64-bit programs, so the helper runs them next to the 32-bit game. The add-on starts it when anti-aliasing is turned on. They share the textures and hand frames over through a shared fence: after the first frame the game's CPU doesn't wait for the helper, and a watcher thread signals the fence if the helper dies so the game's GPU can't hang. DLSS runs on the helper's D3D11 device; FSR on a D3D12 device the helper makes only when FSR is chosen and frees after 10 s without an FSR frame. The design follows ATN's DLAA helper for Total War: Attila; the FSR path is adapted from its D3D12 upscaler.
- **Where it runs:** after the scene fog, on the linear HDR scene, before the translucents, depth of field, bloom, motion blur and the tonemap. Particles, weather and the menu overlay are drawn after it.
- **Inputs:** the camera is jittered over 32 Halton phases and textures get a -1 LOD bias while anti-aliasing runs. Motion vectors come from the camera (jitter removed), plus each moving object's own motion: skinned characters, tessellated meshes (Clayface, Poison Ivy's pods), CPU-simulated cloth (capes, coats) and PhysX meshes (papers, debris). Their vertex positions are streamed out each frame and the previous frame's are replayed. Depth is the projection's z (0 at the near plane, 1 at infinity), from the same surface as the motion.
- **Turning itself off:** with MSAA on, or when the helper can't start or stops, anti-aliasing turns off and the game's image stays as it is; the switch is greyed out and the status line says why. It stays off until the game recreates its device or restarts. `ReShade.log` gives the reason (`batmanac upscaling: ...; anti-aliasing off`); the helper writes `renodx-batmanac-dlaa-host64.log`.
- **DLAA vs FSR:** with FSR Sharpening on, FSR looks sharper than DLAA (DLSS has no sharpening of its own); FSR 3.1 shimmers more on thin geometry. Bright thin lights such as neon tubes get a slightly brighter bloom halo with FSR: the game's thresholded bloom amplifies FSR's slightly brighter cores (the two match with Bloom 0).
- **Cost:** on an RTX 5090 at 3840x2160 with preset K, DLAA costs about 1.4 ms per frame without GPU PhysX (smelting chamber 2.87 → 4.25 ms, courthouse 3.12 → 4.51 ms). With GPU PhysX High it costs about 1–2 ms, depending on the spot: where PhysX already holds the frame back, part of DLAA's work fits into the gaps (courthouse 7.22 → 8.16 ms); where PhysX particles keep the GPU busy, PhysX's GPU work waits behind DLAA's, about 0.5 ms more (smelting chamber 4.35 → 6.29 ms, open world 6.11 → 8.00 ms). PhysX High by itself costs about 1.5 ms in the smelting chamber. On slower GPUs, PhysX Normal or a dedicated PhysX GPU helps. Close programs that draw on the GPU in the background, such as a browser: an idle browser took about 12% of the GPU's 3D engine in one measurement, which PhysX and DLAA then wait behind. FSR costs the same as DLAA: in the museum with GPU PhysX on, both take the frame from 5.2 to 7.6 ms.

## Building

Run from the repository root inside the Visual Studio developer shell for x86 (`vcvarsall.bat x86`). After adding or removing a shader file, run `cmake --preset clang-x86` again first.

| Build | Command | Output folder | Dev tools |
|---|---|---|---|
| Release | `cmake --build --preset clang-x86-release --target batmanac` | `build32/Release/` | off |
| Debug | `cmake --build --preset clang-x86-debug --target batmanac` | `build32/Debug/` | on |

Both make `renodx-batmanac.addon32`. Copy it into `Binaries\Win32` with the game closed.

The helper is a 64-bit program (source in `helper/`). For FSR, put AMD's FidelityFX SDK v2.4.0 headers, signed DLLs and license in `build/downloads/fsr/sdk` **before** configuring (from PowerShell or cmd; Git Bash rewrites the leading slashes):

```
git clone --depth 1 --filter=blob:none --sparse -b v2.4.0 https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git build/downloads/fsr/sdk
git -C build/downloads/fsr/sdk sparse-checkout set --no-cone /Kits/FidelityFX/api/include/ /Kits/FidelityFX/upscalers/include/ /Kits/FidelityFX/signedbin/amd_fidelityfx_loader_dx12.dll /Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_dx12.dll /Kits/FidelityFX/docs/license.md
```

Without it the helper still builds, with DLAA only (choosing FSR then reports "FSR is not in this helper's build"). Then, in the developer shell for x64 (`vcvarsall.bat x64`), run `cmake --preset clang-x64` once, then `cmake --build --preset clang-x64-release --target batmanac_dlaa_host64`. It writes `build/Release/renodx-batmanac-dlaa-host64.exe` and copies the two FSR DLLs next to it. Copy all three next to the add-on, with `build/downloads/fsr/sdk/Kits/FidelityFX/docs/license.md` as `amd_fidelityfx_license.md`.

The build writes each replaced shader to `build32/batmanac.include/embed/`: `0xD160F56A.h`, `0x2151085D.h`, `0x280F18BC.h`, `0xC1C294D3.h`, `0x3207DF59.h`, `0xC240CBE6.h`, `0x46BA7BD5.h`, `0x25455918.h`, `0x8E23494B.h`, `0x2A25E549.h`, `0xB720ADD7.h`, `0xE6F50BDA.h`, and the proxy's `swap_chain_proxy_vertex_shader_dx11.h` and `swap_chain_proxy_pixel_shader_dx11.h`; `shaders.h` lists them.

## Verification

1. Start the game through its launcher with the DX11 renderer.
2. `ReShade.log` in `Binaries\Win32` shows the add-on loaded, the line `batmanac: cleared WS_EX_TOOLWINDOW on the UE3 game window before CreateSwapChain`, and `mods::swapchain::OnCreateSwapchain(swap: r8g8b8a8_unorm => r10g10b10a2_unorm ...)`. The ReShade overlay shows "RenoDX for Batman: Arkham City".
3. The game runs borderless (not exclusive fullscreen) and the picture is not washed out.
4. Tone Mapper Vanilla: an HDR screenshot peaks at Game Brightness (201.8 nits luminance at 203), and Peak Brightness doesn't change it as long as Peak is at least Game Brightness. Tone Mapper None or PsychoV-30: highlights (city lights, floodlights, neon) go above it, up to about Peak.
5. Detective mode (or Catwoman's thief vision) on: the picture changes to the detective look, with no black frame or washed-out image.
6. The settings that a tone mapper doesn't use are hidden (PsychoV-30-only) or grayed out (under Vanilla); Preset Off switches to Vanilla.
7. Flame cores, a paused frame at the Steel Mill wall fires (or the Clayface floor fire): under PsychoV-30 the flames' cores are no longer a flat patch darker than their red edges; Vanilla shows the game's clipped original. Gray smoke and the Gotham window fires match Vanilla below white.
8. Anti-aliasing (MSAA and FXAA off in the launcher, the helper and its DLLs in `Binaries\Win32`): turning it on starts `renodx-batmanac-dlaa-host64.exe`; the status line reads `DLAA preset K (nvngx_dlss.dll ...)` with DLAA and `FSR 3.1.x (amd_fidelityfx_upscaler_dx12.dll ...)` with FSR on GPUs without FSR 4. Edges are smooth, and a camera pan leaves no smearing on scenery, Batman's cape or the enemies. Switching between DLAA and FSR works both ways, and `renodx-batmanac-dlaa-host64.log` shows `FSR unused for 10 s: released` 10 s after leaving FSR. Without `nvngx_dlss.dll` the method reads "FSR" and FSR runs. With MSAA on, `ReShade.log` shows `batmanac upscaling: the scene is multisampled; ...; anti-aliasing off`.

## Known risks

- **HUD and map overlays can go above UI Brightness** under the HDR tone mappers (map text about 222 nits at UI 203): the original's 8-bit back buffer clipped them at white, and only Vanilla keeps that clip. RenoDX maintainers judged this fine.
- **Subtitles over very bright lamps** can be hard to read under PsychoV-30: they have no backing box, and lamps behind them reach several times UI Brightness where Vanilla clipped them at white. A fix would need intercepting every UI draw after the tonemap; RenoDX maintainers judged that not worth it without frame generation.
- **The flame-core fix acts on every emissive use of these materials.** An untested effect whose cool color is above white and whose hot color is dimmer would also be brightened; Vanilla shows the original for comparison. Around a brightened core the flame's edge takes on the core's paler color, where Vanilla shows a red-pink outline. Two more sprite materials with the same flat-core pattern (`0x63FAD3EF`, `0xCE5AB695`) aren't replaced, as they never appeared in testing; a flat fire core elsewhere would point to them. Hard-edged fire art (the Steel Mill walls) still ends in an abrupt edge where the core meets its orange rim: that edge is in the art, which the game's clip at white hid.
- **The bokeh uber pass `0x280F18BC` draws the game-over screen** after Batman dies (the villain's taunt, for example the Joker, over a dark background). Its depth of field is a 13x13 bokeh gather over the scene; Depth of Field scales the bokeh radius, so above 50 the blur grows only up to that 13x13 kernel.
- **MSAA is not recommended.** With MSAA, edge pixels of the lightmapped surface material `0x63BA77AD` extrapolate its vertex color into single pixels thousands of times past white; Vanilla clips them to a dotted line, and the HDR tone mappers and the float blur turn them into glowing blobs. Use FXAA or DLAA instead.
- **Anti-aliasing's limits.** A skinned mesh coming back into view has no previous frame, so its first frame uses camera motion; plants near Poison Ivy can flash for a frame when the camera's view pairing switches. Translucent effects (smoke, glows, flames) carry no depth or motion of their own, so the upscalers see the surface behind them.
- **FSR 4 is untested** (it needs an AMD GPU that supports it): its first start can take longer than the add-on waits for the helper's first answer, which would turn anti-aliasing off; on Windows 10 it may need Microsoft's Agility SDK, which the helper doesn't ship, and then runs as FSR 3.1.
- **MSAA allocates two unused back-buffer-sized targets** (a multisampled one and its resolve texture) that match the FXAA input upgrade. Both are upgraded to RGBA16F so their formats stay equal (upgrading only one crashes the game); the cost is extra video memory, no visual change.
- **32:9 and other non-16:9 resolutions are untested.** The DOF blur upgrade matches the blur by size, computed as back buffer / 4 + 2 (checked at 3840x2160, 2560x1440 and 1920x1080). If another aspect ratio rounds differently, the blur stays at the game's `RGBA16_UNORM` and blurred lights cap at 4× white, as in the original; nothing else breaks. The FXAA input upgrade matches the back buffer's exact size, so it follows any resolution (checked at 3840x2160 only).
