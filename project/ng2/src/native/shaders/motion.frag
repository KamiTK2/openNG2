#version 450
// Writes where each opaque scene pixel was last frame, relative to now, in NDC. Both clip positions
// carry that frame's jitter, so the difference is taken out to leave true motion.
// Build: glslangValidator -V --vn kMotionFrag motion.frag -o motion_frag.h
layout(location = 16) in vec4 previous;
layout(location = 17) in vec4 current;
layout(set = 1, binding = 0, std140) uniform System {
  layout(offset = 336) vec2 jitter_delta;  // jitter now minus jitter last frame
  layout(offset = 344) float debug;        // above 0: show magnified absolute motion
};
layout(location = 0) out vec2 motion;

void main() {
  vec2 v = previous.xy / previous.w - current.xy / current.w + jitter_delta;
  motion = debug > 0.0 ? abs(v) * debug : v;
}
