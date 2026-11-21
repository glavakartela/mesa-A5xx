#version 450
layout(binding = 0, std140) uniform UBO {
   mat4 mvp;
   vec4 color;
} ubo;
layout(location = 0) in vec2 in_pos;
void main() {
   gl_Position = ubo.mvp * vec4(in_pos, 0.0, 1.0);
}
