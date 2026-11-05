#version 450
layout(set = 0, binding = 0) uniform sampler2D image;
layout(set = 0, binding = 1) uniform sampler2D ramp;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
float Ramp(float v, int c) { return texture(ramp, vec2((v * 255.0 + 0.5) / 256.0, 0.5))[c]; }
void main() {
  vec3 c = texture(image, uv).rgb;
  color = vec4(Ramp(c.r, 0), Ramp(c.g, 1), Ramp(c.b, 2), 1.0);
}
