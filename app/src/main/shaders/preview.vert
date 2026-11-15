#version 450

layout(push_constant) uniform Preview {
    vec2 scale;
} preview;

layout(location = 0) out vec2 uv;

void main() {
    uv = vec2(gl_VertexIndex & 1, (gl_VertexIndex >> 1) & 1);
    gl_Position = vec4((2.0 * uv - 1.0) * preview.scale, 0.0, 1.0);
}
