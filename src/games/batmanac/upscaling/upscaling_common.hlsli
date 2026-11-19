// Shared by the upscaler's input and composite passes: the scene and the per-frame constants.

#include "../shared.h"

// rgb: jittered scene color, a: 1 - device depth (the CPU projection's z), where the scene stores depth.
Texture2D<float4> scene_texture : register(t0);

cbuffer upscaling_constants : register(b0) {
  // A camera-relative point of the current view to the previous frame's clip space, row vectors.
  row_major float4x4 view_to_previous_clip;
  // 1/P[0][0], 1/P[1][1], 1 - P[2][2], 1/P[3][2] of the unjittered projection.
  float4 projection_terms;
  // 1 when the character mask exists.
  float character_mask_valid;
};
