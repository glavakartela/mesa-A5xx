#version 450
layout(binding = 0, std140) uniform UBO {
   vec4 pos[3];
} ubo;
void main() {
   gl_Position = vec4(ubo.pos[gl_VertexIndex].xy, 0.0, 1.0);
}
