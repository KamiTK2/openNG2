#version 450
// Xenos rectangle lists: three corners of a rectangle per primitive, the GPU makes the fourth.
layout(triangles) in;
layout(triangle_strip, max_vertices = 4) out;
layout(location = 0) in vec4 vi[][16];
layout(location = 0) out vec4 vo[16];
in gl_PerVertex { vec4 gl_Position; } gl_in[];
out gl_PerVertex { vec4 gl_Position; };
void Emit(int v) {
  gl_Position = gl_in[v].gl_Position;
  for (int i = 0; i < 16; ++i) vo[i] = vi[v][i];
  EmitVertex();
}
void main() {
  vec2 p0 = gl_in[0].gl_Position.xy / gl_in[0].gl_Position.w;
  vec2 p1 = gl_in[1].gl_Position.xy / gl_in[1].gl_Position.w;
  vec2 p2 = gl_in[2].gl_Position.xy / gl_in[2].gl_Position.w;
  float d01 = dot(p1 - p0, p1 - p0), d02 = dot(p2 - p0, p2 - p0), d12 = dot(p2 - p1, p2 - p1);
  // The longest edge is the diagonal; c is the corner opposite the made-up vertex.
  int a = 1, b = 2, c = 0;
  if (d01 >= d02 && d01 >= d12) { a = 0; b = 1; c = 2; }
  else if (d02 >= d12) { a = 0; b = 2; c = 1; }
  Emit(c); Emit(a); Emit(b);
  gl_Position = gl_in[a].gl_Position + gl_in[b].gl_Position - gl_in[c].gl_Position;
  for (int i = 0; i < 16; ++i) vo[i] = vi[a][i] + vi[b][i] - vi[c][i];
  EmitVertex();
  EndPrimitive();
}
