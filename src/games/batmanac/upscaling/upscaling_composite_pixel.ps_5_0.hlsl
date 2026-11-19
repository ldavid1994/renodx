// The upscaler's output drawn over the scene. The blend state writes color only, so the scene's alpha (device depth, read by
// the later passes) stays as it is.

Texture2D<float4> upscaler_output_texture : register(t0);

float4 main(float4 position: SV_Position) : SV_Target {
  return float4(max(0.f, upscaler_output_texture.Load(int3(position.xy, 0)).rgb), 1.f);
}
