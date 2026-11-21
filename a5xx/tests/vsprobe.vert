#version 450
layout(set = 0, binding = 0) buffer Probe { uint value; } probe;
layout(location = 0) in vec2 pos;
void main() {
   probe.value = 0x12345678u;
   gl_Position = vec4(pos, 0.0, 1.0);
}
