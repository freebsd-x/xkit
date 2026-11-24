#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D texSampler;

layout(push_constant) uniform PushConstants {
    layout(offset = 40) float opacity;   // after ui_image.vert's
} pc;

void main() {
    outColor = texture(texSampler, inUV);
    outColor.a *= pc.opacity;
}