#version 450
layout(binding = 0, std140) uniform UBO {
   mat4 mvp;
   vec4 color;
} ubo;
layout(location = 0) out vec4 out_color;
void main() {
   out_color = ubo.color;
}
