// Scene scale vertex shader (vs_3_0): scene_scale_pixel_shader is ps_3_0, which only runs with a
// vs_3_0. The add-on sends the full-screen quad already in clip space, half a pixel early so each
// pixel samples its own texel.

struct VertexOutput {
  float4 position : POSITION;
  float2 uv : TEXCOORD0;
};

VertexOutput main(float4 position : POSITION, float2 uv : TEXCOORD0) {
  VertexOutput output;
  output.position = position;
  output.uv = uv;
  return output;
}
