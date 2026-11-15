#version 450

layout(set = 0, binding = 0) uniform sampler2D previewTexture;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

void main() {
    vec4 texel = texture(previewTexture, uv);
    if (texel.a == 0.0) {
        discard;
    }
    color = texel;
}
