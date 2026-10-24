#version 450

layout(location = 0) out vec2 outUV;

layout(push_constant) uniform PushConstants {
    vec2 pos;
    vec2 size;
    vec2 screen_size;
    // Two vec2s, NOT a vec4: std430 aligns a vec4 to 16 bytes, which would pad
    // uv_rect to offset 32 while the C++ side writes the UVs at offset 24. Two
    // vec2s stay 8-byte aligned and line up exactly with the pushed constants.
    vec2 uv0;  // u0, v0
    vec2 uv1;  // u1, v1 — allows Cover / Contain / Center crop
} pc;

void main() {
    // Generate quad vertices based on gl_VertexIndex (0..3)
    vec2 t = vec2(
        (gl_VertexIndex == 1 || gl_VertexIndex == 3) ? 1.0 : 0.0,
        (gl_VertexIndex == 2 || gl_VertexIndex == 3) ? 1.0 : 0.0
    );
    outUV = pc.uv0 + t * (pc.uv1 - pc.uv0);

    vec2 pixel_pos = pc.pos + t * pc.size;
    vec2 ndc = (pixel_pos / pc.screen_size) * 2.0 - 1.0;

    gl_Position = vec4(ndc, 0.0, 1.0);
}
