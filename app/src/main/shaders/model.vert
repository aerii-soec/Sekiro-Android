#version 450

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec2 uv;

layout(push_constant) uniform Mesh {
    mat4 viewProjection;
    vec2 uvScale;
} mesh;

layout(location = 0) out vec3 worldNormal;
layout(location = 1) out vec2 materialUv;

void main() {
    gl_Position = mesh.viewProjection * vec4(position, 1.0);
    worldNormal = normal;
    materialUv = uv * mesh.uvScale;
}
