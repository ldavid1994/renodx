// DLAA inputs from the jittered scene: color, device depth, motion in pixels (current to previous), and the mask that
// biases DLSS toward the current frame. Static surfaces get the camera's motion; characters get their own motion where
// it was captured, else the camera's motion and full bias.

#include "./dlaa_common.hlsli"

// 1 where a skinned mesh (characters, the cape) is the visible surface.
Texture2D<float> character_mask_texture : register(t1);
// Skinned draws: previous minus current NDC; CHARACTER_MOTION_UNKNOWN where none was captured.
Texture2D<float2> character_motion_texture : register(t2);
// Device depth of skinned draws with motion, 0 elsewhere: their materials write other data into the scene alpha.
Texture2D<float> character_depth_texture : register(t3);
// Tessellated skinned draws: previous minus current NDC of their untessellated triangles (nearest first), then the
// device depth of those triangles (0 where none).
Texture2D<float2> tessellated_motion_texture : register(t4);
Texture2D<float> tessellated_depth_texture : register(t5);
RWTexture2D<float> depth_output : register(u0);
RWTexture2D<float2> motion_vector_output : register(u1);
RWTexture2D<float> bias_mask_output : register(u2);
RWTexture2D<float4> color_output : register(u3);
// Replaces the DLAA output on screen while a debug view is selected.
RWTexture2D<float4> debug_output : register(u4);

// Debug views: hue is the direction, brightness the length (full at full_length pixels).
float3 MotionColor(float2 pixels, float full_length) {
  const float angle = atan2(pixels.y, pixels.x) / (2.f * 3.14159265f) + 0.5f;
  const float3 hue = saturate(abs(frac(angle + float3(0.f, 2.f / 3.f, 1.f / 3.f)) * 6.f - 3.f) - 1.f);
  return hue * saturate(length(pixels) / full_length);
}

[numthreads(8, 8, 1)]
void main(uint3 dispatch_thread_id: SV_DispatchThreadID) {
  uint width, height;
  scene_texture.GetDimensions(width, height);
  if (dispatch_thread_id.x >= width || dispatch_thread_id.y >= height) return;

  const int2 pixel = int2(dispatch_thread_id.xy);
  const float2 screen_size = float2(width, height);
  const float2 uv = (float2(pixel) + 0.5f) / screen_size;
  const float4 scene = scene_texture.Load(int3(pixel, 0));
  const bool is_character = character_mask_valid != 0.f && character_mask_texture.Load(int3(pixel, 0)) > 0.5f;

  // DLSS turns non-finite or negative input into black blotches around very bright effects.
  const float3 scene_color = (isfinite(scene.rgb) ? max(0.f, scene.rgb) : 0.f);
  color_output[pixel] = float4(scene_color, 1.f);

  // Skinned draws' own motion and device depth; a tessellated draw's come from its untessellated triangles.
  float2 character_ndc_motion = character_motion_texture.Load(int3(pixel, 0));
  float character_depth = character_depth_texture.Load(int3(pixel, 0));
  if (is_character && character_ndc_motion.x == CHARACTER_MOTION_TESSELLATED) {
    character_ndc_motion = tessellated_motion_texture.Load(int3(pixel, 0));
    character_depth = tessellated_depth_texture.Load(int3(pixel, 0));
    // The tessellated surface bulges past its untessellated triangles: its rim takes the motion and depth of a covered
    // pixel found in rings of 1 to 32 pixels.
    [loop]
    for (int radius = 1; radius <= 32 && abs(character_ndc_motion.x) >= CHARACTER_MOTION_UNKNOWN * 0.5f; radius *= 2) {
      [unroll]
      for (int i = 0; i < 8; ++i) {
        const int2 offset = int2(round(float2(cos(i * 0.785398f), sin(i * 0.785398f)) * radius));
        const int3 neighbor_pixel = int3(clamp(pixel + offset, int2(0, 0), int2(width, height) - 1), 0);
        const float2 neighbor = tessellated_motion_texture.Load(neighbor_pixel);
        if (abs(neighbor.x) < CHARACTER_MOTION_UNKNOWN * 0.5f) {
          character_ndc_motion = neighbor;
          character_depth = tessellated_depth_texture.Load(neighbor_pixel);
          break;
        }
      }
    }
  }

  // 1 - device depth, as the scene alpha stores it: from the motion pass where a skinned draw wrote its depth.
  const float surface_alpha = (character_depth > 0.f ? 1.f - character_depth : scene.a);
  depth_output[pixel] = 1.f - surface_alpha;

  // The view point is homogeneous, (x, y, 1, 1 / view distance): distant surfaces keep their precision, and the sky
  // becomes a point at infinity.
  const float2 ndc = float2(uv.x * 2.f - 1.f, 1.f - uv.y * 2.f);
  const float4 view_point = float4(ndc * projection_terms.xy, 1.f, (projection_terms.z - surface_alpha) * projection_terms.w);
  const float4 previous_clip = mul(view_point, view_to_previous_clip);
  const float2 previous_uv = float2(previous_clip.x, -previous_clip.y) / previous_clip.w * 0.5f + 0.5f;
  // A point behind the previous camera has no previous position on screen.
  float2 camera_motion = (previous_uv - uv) * screen_size;
  if (!(previous_clip.w > 0.f) || !all(isfinite(camera_motion))) {
    camera_motion = 0.f;
  }
  float2 motion = camera_motion;
  bool has_character_motion = false;
  if (is_character) {
    has_character_motion = abs(character_ndc_motion.x) < CHARACTER_MOTION_UNKNOWN * 0.5f;
    // NDC y points up; the motion pass already took each frame's jitter out. Without captured motion the camera's
    // motion (as if static) is closer than none.
    motion = (has_character_motion ? character_ndc_motion * float2(0.5f, -0.5f) * screen_size : camera_motion);
  }
  motion_vector_output[pixel] = motion;
  bias_mask_output[pixel] = ((is_character && !has_character_motion) ? 1.f : 0.f);
  if (CUSTOM_DLAA_DEBUG_VIEW == 0.f) return;

  const float3 dimmed_scene = scene_color * 0.1f;
  float3 output_color = dimmed_scene;
  if (CUSTOM_DLAA_DEBUG_VIEW == 1.f) {
    // Motion handed to DLSS, full at 16 pixels.
    output_color = lerp(dimmed_scene, MotionColor(motion, 1.f), saturate(length(motion) / 16.f));
  } else if (CUSTOM_DLAA_DEBUG_VIEW == 2.f) {
    // Characters' own motion against the camera's, full at 4 pixels: black when they stand still; red without motion.
    if (has_character_motion) {
      output_color = MotionColor(motion - camera_motion, 4.f);
    } else if (is_character) {
      output_color = float3(1.f, 0.f, 0.f);
    }
  }
  debug_output[pixel] = float4(output_color, scene.a);
}
