// Draws a tessellated skinned mesh's untessellated triangles from the world positions its vertex shader streamed out
// this frame and the previous one, projected with each frame's camera. b1 / b11: the domain shader's view buffer.

#include "./dlaa_motion.hlsli"

float4 ToClip(float3 world, float4 rows[4]) {
  return world.x * rows[0] + world.y * rows[1] + world.z * rows[2] + rows[3];
}

MotionVertex main(float4 current_world: CURRENT, float4 previous_world: PREVIOUS) {
  return MakeMotionVertex(ToClip(current_world.xyz, view_projection_rows), ToClip(previous_world.xyz, previous_view_projection_rows));
}
