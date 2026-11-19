// Marks a tessellated skinned surface in the character mask where it is still the visible surface at the end of the
// frame: its own depth, tested as SV_Depth against the final depth. Its motion comes from the tessellated motion target,
// which the motion pass filled from the untessellated triangles.

#include "../shared.h"

// The surfaces' device depth as their draws rasterized it, 1 where none.
Texture2D<float> tessellated_surface_depth : register(t0);

// A few steps of a 24-bit depth buffer: the final depth holds the surface's depth rounded to its format.
static const float DEPTH_ROUNDING = 1.f / 4194304.f;

float4 main(float4 position: SV_Position, out float2 motion: SV_Target1, out float device_depth: SV_Target2, out float depth: SV_Depth) : SV_Target0 {
  const float surface_depth = tessellated_surface_depth.Load(int3(position.xy, 0));
  if (surface_depth == 1.f) discard;
  depth = surface_depth - DEPTH_ROUNDING;
  motion = CHARACTER_MOTION_TESSELLATED;
  device_depth = 0.f;
  return 1.f;
}
