#version 450

layout(set = 0, binding = 0) uniform sampler2D materialTexture;
layout(location = 0) in vec3 worldNormal;
layout(location = 1) in vec2 materialUv;
layout(location = 0) out vec4 color;

void main() {
    vec4 material = texture(materialTexture, materialUv);
    if (material.a == 0.0) {
        discard;
    }
    vec3 n = worldNormal * inversesqrt(max(dot(worldNormal, worldNormal), 1e-8));
    float light = 0.27 + 0.73 * max(dot(n, normalize(vec3(0.4, 0.85, 0.52))), 0.0);
    color = vec4(material.rgb * light, 1.0);
}
