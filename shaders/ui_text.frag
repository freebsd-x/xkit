#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D sdfSampler;

// Must match ui_text.vert exactly: color first so the vec4 is 16-byte aligned
// at offset 0 and the following vec2s stay tightly packed (no std430 padding).
layout(push_constant) uniform PushConstants {
    vec4 color;
    vec2 pos;
    vec2 size;
    vec2 screen_size;
    vec2 uv_pos;
    vec2 uv_size;
} pc;

void main() {
    float dist = texture(sdfSampler, inUV).r - 0.5;
    // Anti-alias over about one screen pixel, whatever the glyph's scale. A
    // fixed ±0.05 band was far narrower than a pixel at small sizes (the atlas
    // is 48 px; 13 px text samples it at ~1/4), so a 1 px stroke — the upright
    // of a "+" — vanished depending on where it fell between pixels.
    float w = max(fwidth(dist) * 0.75, 1e-4);
    float alpha = smoothstep(-w, w, dist);
    if (alpha < 0.01) discard;
    outColor = vec4(pc.color.rgb, pc.color.a * alpha);
}