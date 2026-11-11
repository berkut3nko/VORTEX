#version 460
#extension GL_GOOGLE_include_directive : require
// Full-screen triangle for every screen-space pass: vkCmdDraw(3, 1, 0, 0), no vertex buffers.
#include "include/common.glsl"

layout(location = 0) out vec2 v_Uv;     // (0,0) = top-left of the image

void main() {
    // Vertices 0,1,2 -> uv (0,0) (2,0) (0,2): one triangle that covers the whole viewport.
    vec2 uv = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    v_Uv = uv;
    // Vulkan clip space has y pointing down, so uv maps to NDC without a flip.
    // z = 0 is the far plane in reverse-Z; passes that need depth write gl_FragDepth.
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
