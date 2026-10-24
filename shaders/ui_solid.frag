#version 450

layout(location = 0) in vec2 fragLocalPos;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PushConstants {
    vec2 pos;
    vec2 size;
    vec2 screen_size;
    float border_radius;
    float shadow_blur;
    vec4 color;
    float shadow_alpha;
} pc;

// Exact SDF for a rounded rectangle.
// p: position relative to rect center; halfSize: half-extents; radius: corner radius.
float roundedBoxSDF(vec2 p, vec2 halfSize, float radius) {
    vec2 q = abs(p) - halfSize + vec2(radius);
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - radius;
}

void main() {
    vec2 halfSize = pc.size * 0.5;
    float dist = roundedBoxSDF(fragLocalPos, halfSize, pc.border_radius);

    // Box fill — smoothstepped edge for sub-pixel AA.
    float boxAlpha = 1.0 - smoothstep(-1.0, 1.0, dist);

    // Drop shadow — exponential falloff outside the rounded rect.
    // Only computed when shadow_blur > 0 and we are outside the box.
    float shadow = 0.0;
    if (pc.shadow_blur > 0.0 && dist > 0.0) {
        shadow = exp(-dist / pc.shadow_blur) * pc.shadow_alpha * (1.0 - boxAlpha);
    }

    vec4 boxColor    = vec4(pc.color.rgb, pc.color.a * boxAlpha);
    vec4 shadowColor = vec4(0.0, 0.0, 0.0, shadow);

    // Composite box over shadow using standard alpha blending.
    float fa = boxColor.a + shadowColor.a * (1.0 - boxColor.a);
    vec3  fr = (boxColor.rgb * boxColor.a + shadowColor.rgb * shadowColor.a * (1.0 - boxColor.a))
               / max(fa, 0.001);

    outColor = vec4(fr, fa);
    if (outColor.a < 0.001) discard;
}
