#version 450
layout(set = 0, binding = 0) buffer Probe { uint v[4]; } probe;
layout(location = 0) in vec2 pos;
void main() {
   probe.v[gl_VertexIndex] = 1u;
   gl_Position = vec4(pos, 0.0, 1.0);
}
