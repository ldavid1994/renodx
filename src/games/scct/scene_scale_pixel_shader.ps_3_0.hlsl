// Scene scale (ps_3_0): the end of the scene, drawn by the add-on from a copy of the back buffer clone,
// pixel for pixel, just before the first HUD draw (or at the present). Until here the scene has white
// at 1.0, as the game drew it: the composite, night vision, thermal and EMF. This decodes it as the
// swap chain pass will (sRGB, or 2.2 with Gamma Correction 2.2), scales it by Game / UI Brightness (the
// swap chain pass scales the whole image by UI Brightness) and encodes it again, so the HUD still
// blends on encoded values and stays at UI Brightness.

#include "./shared.h"

sampler2D scene_copy : register(s0);

float4 main(float2 uv : TEXCOORD0) : COLOR {
  const float4 scene = tex2D(scene_copy, uv);
  const float3 color = renodx::draw::DecodeColor(scene.rgb, CUSTOM_SCENE_ENCODING) * CUSTOM_SCENE_SCALE;
  return float4(renodx::draw::EncodeColor(color, CUSTOM_SCENE_ENCODING), scene.a);
}
