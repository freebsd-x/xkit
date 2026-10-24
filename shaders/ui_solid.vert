#version 450

layout(location = 0) out vec2 fragLocalPos; // position relative to rect center (pixels)

layout(push_constant) uniform PushConstants {
    vec2 pos;
    vec2 size;
    vec2 screen_size;
    float border_radius;
    float shadow_blur;
    vec4 color;
    float shadow_alpha;
} pc;

void main() {
    vec2 uv = vec2(
        (gl_VertexIndex == 1 || gl_VertexIndex == 3) ? 1.0 : 0.0,
        (gl_VertexIndex == 2 || gl_VertexIndex == 3) ? 1.0 : 0.0
    );

    // Expand the drawn quad beyond rect bounds to allow shadow bleed.
    // When shadow_blur == 0, expand == 0 and the quad is unchanged.
    float expand = pc.shadow_blur * 1.5;
    vec2 quad_pos  = pc.pos  - vec2(expand);
    vec2 quad_size = pc.size + vec2(expand * 2.0);

    vec2 pixel_pos = quad_pos + uv * quad_size;

    // Local position relative to the RECT center (not the expanded quad).
    vec2 rect_center = pc.pos + pc.size * 0.5;
    fragLocalPos = pixel_pos - rect_center;

    vec2 ndc = (pixel_pos / pc.screen_size) * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
}
