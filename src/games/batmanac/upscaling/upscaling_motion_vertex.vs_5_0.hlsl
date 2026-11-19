// Draws a skinned mesh's triangles from its streamed-out clip positions of this frame and the previous one, with each
// frame's camera jitter taken out. b1 / b11: the game's view buffer, as this frame and the previous one left it.

#include "./upscaling_motion.hlsli"

MotionVertex main(float4 current_clip: CURRENT, float4 previous_clip: PREVIOUS) {
  return MakeMotionVertex(current_clip, previous_clip);
}
