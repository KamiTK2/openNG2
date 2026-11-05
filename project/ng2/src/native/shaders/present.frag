#version 450
// Final output: display gamma ramp, then optional FXAA (same constants as the SDK's swap FXAA).
// Rebuild: glslangValidator -V --vn kPresentFrag -o present_frag.h present.frag
layout(set = 0, binding = 0) uniform sampler2D image;
layout(set = 0, binding = 1) uniform sampler2D ramp;
layout(push_constant) uniform Constants {
  vec2 texel;
  float edge_threshold;
  float edge_threshold_min;
  float span_max;
  int fxaa;
} pc;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
const vec3 kLumaWeights = vec3(0.299, 0.587, 0.114);
const float kDirReduceMul = 0.125;
const float kDirReduceMin = 1.0 / 128.0;
float Ramp(float v, int c) { return texture(ramp, vec2((v * 255.0 + 0.5) / 256.0, 0.5))[c]; }
vec3 Fetch(vec2 p) {
  vec3 c = textureLod(image, p, 0.0).rgb;
  return vec3(Ramp(c.r, 0), Ramp(c.g, 1), Ramp(c.b, 2));
}
float Luma(vec2 p) { return dot(Fetch(p), kLumaWeights); }
void main() {
  vec3 rgbm = Fetch(uv);
  if (pc.fxaa == 0) {
    color = vec4(rgbm, 1.0);
    return;
  }
  vec2 texel = pc.texel;
  float lumaM = dot(rgbm, kLumaWeights);
  float lumaNW = Luma(uv + vec2(-texel.x, -texel.y));
  float lumaNE = Luma(uv + vec2(texel.x, -texel.y));
  float lumaSW = Luma(uv + vec2(-texel.x, texel.y));
  float lumaSE = Luma(uv + vec2(texel.x, texel.y));
  float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
  float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));
  vec3 result = rgbm;
  if (lumaMax - lumaMin >= max(pc.edge_threshold_min, lumaMax * pc.edge_threshold)) {
    vec2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y = ((lumaNW + lumaSW) - (lumaNE + lumaSE));
    float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 * kDirReduceMul), kDirReduceMin);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = clamp(dir * rcpDirMin, vec2(-pc.span_max), vec2(pc.span_max)) * texel;
    vec3 rgbA = 0.5 * (Fetch(uv + dir * (1.0 / 3.0 - 0.5)) + Fetch(uv + dir * (2.0 / 3.0 - 0.5)));
    vec3 rgbB = rgbA * 0.5 + 0.25 * (Fetch(uv + dir * -0.5) + Fetch(uv + dir * 0.5));
    float lumaB = dot(rgbB, kLumaWeights);
    result = (lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB;
  }
  color = vec4(result, 1.0);
}
