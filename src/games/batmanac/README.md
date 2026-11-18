# Batman: Arkham City (batmanac)

HDR for the Steam GOTY release (DX11, 32-bit). Needs ReShade with add-on support installed as `dxgi.dll` next to `Binaries\Win32\BatmanAC.exe`. Use the game's DX11 renderer.

The main story was played and checked end to end; Harley Quinn's Revenge and the challenge maps had spot checks only. The add-on replaces the tonemap uber pass (`0xD160F56A`, `0x2151085D` with Motion Blur on, or `0x280F18BC` with a bokeh depth of field), the detective-mode final pass (`0xC1C294D3`, also Catwoman's thief vision), the LUT builders (`0x3207DF59`, and its variant `0xC240CBE6` that adds a texture to the table coordinates; their 256x16 table upgraded to RGBA16F), the DOF downsample (`0x46BA7BD5`, the blur targets upgraded to RGBA16F) and the bloom bright pass (`0x25455918`, its taps capped at 1 as the original blur target did) and four smoke and flame sheet materials (`0x8E23494B`, `0x2A25E549`, `0xB720ADD7`, `0xE6F50BDA`, for the flame cores below) and a lightmapped surface material (`0x63BA77AD`, its vertex color clamped to 0..1 outside Vanilla: MSAA edge pixels extrapolated it into single pixels thousands of times past white, which the float blur spread into glowing blobs), upgrades the back-buffer-sized target FXAA reads to RGBA16F (FXAA itself only blends, so both FXAA settings keep HDR), and outputs HDR10 on a borderless flip-model swap chain. HDR comes from the game's float scene before its clip at white. Tone Mapper Vanilla matches the game's shaders, apart from 8-bit rounding: the LUT's table and FXAA's input are RGBA16F (under one 8-bit step; with FXAA, a few edge pixels anti-alias slightly differently); PsychoV-30, the default, matches Vanilla within 1% below white and rolls highlights off toward Peak.

## Settings

- **Tone Mapper:** Vanilla (the game's picture, clipped at white), None (no roll-off), PsychoV-30 (default: ShortFuse's PsychoV tone mapper; **Compression** 2 keeps it on Vanilla below white, 0 is automatic; **Hue Clip**, PsychoV-30 only, bends highlight hues toward the game's clip, sky blue twice as far toward cyan).
- **Peak Brightness** defaults to the display's peak. **Game** and **UI Brightness** default to 203 nits.
- **Gamma Correction:** 2.2 (default) shows the game as a typical SDR display does; Off decodes it as sRGB.
- **Color Grading:** Exposure, Highlights, Shadows, Contrast and Saturation work with None and PsychoV-30. **Scene Grading** scales the game's color grade. **Bloom** and **Depth of Field** scale the game's bloom and blur (50 is the game's, 0 turns them off), also under Vanilla. Lens flares are the launcher's switch. Keep Depth of Field on in the launcher: turning it off there makes the game skip its color grade; use the slider instead.
- **Preset Off** switches to Vanilla at 203 nits. Force Borderless and Prevent Fullscreen (Advanced) stay on for HDR.

## Flame cores

The game's flame sheets mix a cool color at their thin edges toward a hot color at their dense core. In the emissive ones the cool color is authored far above white (a bright red) but the hot color at SDR white, so their brightness falls toward the core; in HDR, where nothing clips, the core showed as a flat, dim patch inside brighter flames (the Steel Mill wall fires, the Clayface floor fire, the fires on top of Wonder Tower). Outside Vanilla the replaced sheets raise a dimmer hot color, hue kept, to the cool color's luminance, as the materials authored in HDR already have, so the core's color is no longer dimmer than the edge's; a hot color already as bright is left alone. It applies only where the cool color is above white, so smoke and SDR-authored flames (the Gotham window fires) keep the game's colors.

## In-game brightness

Leave the game's own brightness slider at its default (the middle) and set brightness with Game Brightness. The game's slider is a gamma on its SDR image (baked into the color-grade LUT, and the exponent of the detective-mode pass), so it still works with every tone mapper, lifting shadows and midtones on top of Game Brightness.

## Building

Run from the repository root inside the Visual Studio developer shell for x86 (`vcvarsall.bat x86`). After adding or removing a shader file, run `cmake --preset clang-x86` again first.

| Build | Command | Output folder | Dev tools |
|---|---|---|---|
| Release | `cmake --build --preset clang-x86-release --target batmanac` | `build32/Release/` | off |
| Debug | `cmake --build --preset clang-x86-debug --target batmanac` | `build32/Debug/` | on |

Both make `renodx-batmanac.addon32`. Copy it into `Binaries\Win32` with the game closed.

The build writes each replaced shader to `build32/batmanac.include/embed/`: `0xD160F56A.h`, `0x2151085D.h`, `0x280F18BC.h`, `0xC1C294D3.h`, `0x3207DF59.h`, `0xC240CBE6.h`, `0x46BA7BD5.h`, `0x25455918.h`, `0x8E23494B.h`, `0x2A25E549.h`, `0xB720ADD7.h`, `0xE6F50BDA.h`, `0x63BA77AD.h`, and the proxy's `swap_chain_proxy_vertex_shader_dx11.h` and `swap_chain_proxy_pixel_shader_dx11.h`; `shaders.h` lists them.

## Verification

1. Start the game through its launcher with the DX11 renderer.
2. `ReShade.log` in `Binaries\Win32` shows the add-on loaded, the line `batmanac: cleared WS_EX_TOOLWINDOW on the UE3 game window before CreateSwapChain`, and `mods::swapchain::OnCreateSwapchain(swap: r8g8b8a8_unorm => r10g10b10a2_unorm ...)`. The ReShade overlay shows "RenoDX for Batman: Arkham City".
3. The game runs borderless (not exclusive fullscreen) and the picture is not washed out.
4. Tone Mapper Vanilla: an HDR screenshot peaks at Game Brightness (201.8 nits luminance at 203), and Peak Brightness doesn't change it as long as Peak is at least Game Brightness. Tone Mapper None or PsychoV-30: highlights (city lights, floodlights, neon) go above it, up to about Peak.
5. Detective mode (or Catwoman's thief vision) on: the picture changes to the detective look, with no black frame or washed-out image.
6. The settings that a tone mapper doesn't use are hidden (PsychoV-30-only) or grayed out (under Vanilla); Preset Off switches to Vanilla.
7. Flame cores, a paused frame at the Steel Mill wall fires (or the Clayface floor fire): under PsychoV-30 the flames' cores are no longer a flat patch darker than their red edges; Vanilla shows the game's clipped original. Gray smoke and the Gotham window fires match Vanilla below white.

## Known risks

- **HUD and map overlays can go above UI Brightness** under the HDR tone mappers (map text about 222 nits at UI 203): the original's 8-bit back buffer clipped them at white, and only Vanilla keeps that clip. RenoDX maintainers judged this fine.
- **Subtitles over very bright lamps** can be hard to read under PsychoV-30: they have no backing box, and lamps behind them reach several times UI Brightness where Vanilla clipped them at white. A fix would need intercepting every UI draw after the tonemap; RenoDX maintainers judged that not worth it without frame generation.
- **The flame-core fix acts on every emissive use of these materials.** An untested effect whose cool color is above white and whose hot color is dimmer would also be brightened; Vanilla shows the original for comparison. Around a brightened core the flame's edge takes on the core's paler color, where Vanilla shows a red-pink outline. Two more sprite materials with the same flat-core pattern (`0x63FAD3EF`, `0xCE5AB695`) aren't replaced, as they never appeared in testing; a flat fire core elsewhere would point to them. Hard-edged fire art (the Steel Mill walls) still ends in an abrupt edge where the core meets its orange rim: that edge is in the art, which the game's clip at white hid.
- **The bokeh uber pass `0x280F18BC` draws the game-over screen** after Batman dies (the villain's taunt, for example the Joker, over a dark background). Its depth of field is a 13x13 bokeh gather over the scene; Depth of Field scales the bokeh radius, so above 50 the blur grows only up to that 13x13 kernel.
- **MSAA allocates two unused back-buffer-sized targets** (a multisampled one and its resolve texture) that match the FXAA input upgrade. Both are upgraded to RGBA16F so their formats stay equal (upgrading only one crashes the game); the cost is extra video memory, no visual change.
- **32:9 and other non-16:9 resolutions are untested.** The DOF blur upgrade matches the blur by size, computed as back buffer / 4 + 2 (checked at 3840x2160, 2560x1440 and 1920x1080). If another aspect ratio rounds differently, the blur stays at the game's `RGBA16_UNORM` and blurred lights cap at 4× white, as in the original; nothing else breaks. The FXAA input upgrade matches the back buffer's exact size, so it follows any resolution (checked at 3840x2160 only).
