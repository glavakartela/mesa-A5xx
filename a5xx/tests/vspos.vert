#version 450
layout(set = 0, binding = 0) buffer Probe { uint v[16]; } probe;
layout(location = 0) in vec2 pos;
void main() {
   uint i = gl_VertexIndex;
   gl_Position = vec4(pos, 0.0, 1.0);
   probe.v[i * 4 + 0] = floatBitsToUint(pos.x);
   probe.v[i * 4 + 1] = floatBitsToUint(pos.y);
   probe.v[i * 4 + 2] = floatBitsToUint(gl_Position.x);
   probe.v[i * 4 + 3] = floatBitsToUint(gl_Position.y);
}
