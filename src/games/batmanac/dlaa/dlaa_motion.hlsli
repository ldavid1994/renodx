// Shared by the motion vertex shaders: each frame's view-projection and the vertex they hand the motion pixel shader.

// The view-projection rows (row vectors) of the current frame, then of the previous frame.
cbuffer current_view : register(b1) {
  float4 view_projection_rows[4];
};

cbuffer previous_view : register(b11) {
  float4 previous_view_projection_rows[4];
};

// The jitter adds a multiple of the depth column (clip w) to the x and y columns of the view-projection; the camera's
// rotation is orthonormal, so projecting those columns onto the depth column over the rotation rows recovers it (NDC).
float2 ViewJitter(float4 rows[4]) {
  const float3 x_column = float3(rows[0].x, rows[1].x, rows[2].x);
  const float3 y_column = float3(rows[0].y, rows[1].y, rows[2].y);
  const float3 w_column = float3(rows[0].w, rows[1].w, rows[2].w);
  return float2(dot(x_column, w_column), dot(y_column, w_column)) / dot(w_column, w_column);
}

struct MotionVertex {
  float4 position : SV_Position;
  float4 current_clip : TEXCOORD0;
  float4 previous_clip : TEXCOORD1;
};

// Rasterized at the current position; both positions lose their frame's jitter.
MotionVertex MakeMotionVertex(float4 current_clip, float4 previous_clip) {
  MotionVertex output;
  output.position = current_clip;
  output.current_clip = current_clip;
  output.current_clip.xy -= ViewJitter(view_projection_rows) * current_clip.w;
  output.previous_clip = previous_clip;
  output.previous_clip.xy -= ViewJitter(previous_view_projection_rows) * previous_clip.w;
  return output;
}
