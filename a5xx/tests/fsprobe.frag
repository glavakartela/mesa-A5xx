#version 450
layout(set = 0, binding = 0) buffer Probe { uint value; } probe;
layout(location = 0) out vec4 color;
void main() {
   probe.value = 0x87654321u;
   color = vec4(1.0, 0.0, 0.0, 1.0);
}
