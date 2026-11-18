// Marks the pixels of a tessellated skinned draw in the character mask; their motion comes from the tessellated motion
// target, which the motion pass filled from the untessellated triangles.

#include "../shared.h"

float4 main(out float2 motion: SV_Target1, out float device_depth: SV_Target2) : SV_Target0 {
  motion = CHARACTER_MOTION_TESSELLATED;
  device_depth = 0.f;
  return 1.f;
}
