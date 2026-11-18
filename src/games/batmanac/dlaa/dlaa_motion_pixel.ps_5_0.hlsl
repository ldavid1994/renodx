// Marks a skinned mesh in the character mask and writes its motion (previous minus current NDC) and device depth.

float4 main(float4 position: SV_Position, float4 current_clip: TEXCOORD0, float4 previous_clip: TEXCOORD1,
            out float2 motion: SV_Target1, out float device_depth: SV_Target2) : SV_Target0 {
  motion = previous_clip.xy / previous_clip.w - current_clip.xy / current_clip.w;
  device_depth = position.z;
  return 1.f;
}
