// Tonemap uber pass 0x280F18BC: a 13x13 bokeh gather over the scene replaces the blurred-image lerp; the bloom is t1 and
// the LUT t2. Depth of Field scales the bokeh radius.

#include "./tonemap.hlsli"

cbuffer cb0 : register(b0) {
  float4 cb0[10];
}

// Depth buffer value (scene alpha) to view distance (cb2[1].zw).
float ViewDistance(float2 uv) {
  return 1 / (t0.SampleLevel(s0_s, uv, 0).w * cb2[1].z - cb2[1].w);
}

// Bokeh radius in taps: distance from the focus plane (cb0[8].x) scaled and raised to a power (cb0[8].yz), capped by
// the near or far maximum (cb0[9].xy), plus a floor that keeps the center tap.
float BokehRadius(float view_distance) {
  float focus_distance = view_distance - cb0[8].x;
  float blur = min((focus_distance < 0) ? cb0[9].x : cb0[9].y, pow(cb0[8].y * abs(focus_distance), cb0[8].z));
  return blur * CUSTOM_DOF * 6.26f + 0.37f;
}

void main(
    float4 v0: TEXCOORD0,
    float2 v1: TEXCOORD1,
    out float4 o0: SV_Target0) {
  float center_distance = ViewDistance(v0.zw);
  float center_radius = BokehRadius(center_distance);
  float4 sum = float4(0.f, 0.f, 0.f, 0.00001f);
  [loop]
  for (int y = -6; y <= 6; ++y) {
    [loop]
    for (int x = -6; x <= 6; ++x) {
      float2 uv = cb0[9].zw * float2(x, y) + v0.zw;
      float tap_distance = ViewDistance(uv);
      float tap_radius = BokehRadius(tap_distance);
      // A tap behind this pixel with a larger blur takes this pixel's radius, so blurred background doesn't spread over it.
      float radius = (center_distance < tap_distance && center_radius < tap_radius) ? center_radius : tap_radius;
      float weight = saturate(radius - length(float2(x, y) + 0.25f)) / (radius * radius);
      sum += float4(DropGarbage(t0.Sample(s0_s, uv).rgb), 1.f) * weight;
    }
  }
  o0 = GradeAndRender(sum.rgb / sum.a, t1.Sample(s1_s, v1).rgb, t2, s2_s);
}
