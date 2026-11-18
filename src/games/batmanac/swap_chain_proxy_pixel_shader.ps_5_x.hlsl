#include "./shared.h"

Texture2D t0 : register(t0);
SamplerState s0 : register(s0);
float4 main(float4 vpos: SV_POSITION, float2 uv: TEXCOORD0)
    : SV_TARGET {
  float4 color = t0.Sample(s0, uv);

  // Vanilla: the original R8G8B8A8_UNORM back buffer clipped the HUD drawn over the scene at white.
  [branch]
  if (RENODX_TONE_MAP_TYPE == renodx::draw::TONE_MAP_TYPE_VANILLA) {
    float3 white = renodx::draw::EncodeColor(max(1.f, RENODX_INTERMEDIATE_SCALING), RENODX_SWAP_CHAIN_DECODING);
    color.rgb = clamp(color.rgb, 0.f, white);
  }

  return renodx::draw::SwapChainPass(color);
}
