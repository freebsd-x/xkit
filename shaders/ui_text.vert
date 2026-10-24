#version 450

layout(location = 0) out vec2 outUV;

// color is first so the vec4 sits at offset 0 (16-byte aligned in std430). If it
// followed the vec2s it would force 8 bytes of padding before it, shifting every
// later member off the tightly-packed C++ push-constant array and corrupting UV
// (and pushing uv_size out of the 56-byte range) — which renders text invisible.
layout(push_constant) uniform PushConstants {
    vec4 color;
    vec2 pos;
    vec2 size;
    vec2 screen_size;
    vec2 uv_pos;
    vec2 uv_size;
} pc;

void main() {
    // Generate quad vertices based on gl_VertexIndex (0..3)
    vec2 uv = vec2(
        (gl_VertexIndex == 1 || gl_VertexIndex == 3) ? 1.0 : 0.0,
        (gl_VertexIndex == 2 || gl_VertexIndex == 3) ? 1.0 : 0.0
    );
    outUV = pc.uv_pos + uv * pc.uv_size;
    
    vec2 pixel_pos = pc.pos + uv * pc.size;
    vec2 ndc = (pixel_pos / pc.screen_size) * 2.0 - 1.0;
    
    gl_Position = vec4(ndc, 0.0, 1.0);
}