// DLAA's output with the scene's alpha (device depth, read by the later passes), into a texture copied back over the
// scene.

#include "./dlaa_common.hlsli"

Texture2D<float4> dlaa_output_texture : register(t1);
RWTexture2D<float4> composite_output : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 dispatch_thread_id: SV_DispatchThreadID) {
  uint width, height;
  scene_texture.GetDimensions(width, height);
  if (dispatch_thread_id.x >= width || dispatch_thread_id.y >= height) return;

  const int2 pixel = int2(dispatch_thread_id.xy);
  composite_output[pixel] = float4(max(0.f, dlaa_output_texture.Load(int3(pixel, 0)).rgb), scene_texture.Load(int3(pixel, 0)).a);
}
