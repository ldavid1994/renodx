// Marks the pixels of a skinned draw in the character mask; with the motion targets bound, also marks their motion
// unknown and their depth not captured, so an earlier character's data underneath is not reused.

#include "../shared.h"

float4 main(out float2 motion: SV_Target1, out float device_depth: SV_Target2) : SV_Target0 {
  motion = CHARACTER_MOTION_UNKNOWN;
  device_depth = 0.f;
  return 1.f;
}
