#version 460
#extension GL_GOOGLE_include_directive : require
// HUD: one screen-space quad per instance, vkCmdDraw(6, quadCount, 0, 0), no vertex buffers.
// Positions are output pixels (y down), so the quad maps straight to NDC with outputSize.
#define VX_NEED_HUD
#include "include/common.glsl"

layout(location = 0) noperspective out vec2 v_Cell;   // 0..VX_HUD_CELLS_X, 0..VX_HUD_CELLS_Y across the quad
layout(location = 1) flat out uvec3 v_Data;           // colour, cells 0..31, cells 32..44

void main() {
    // Two triangles: (0,0) (1,0) (1,1) and (0,0) (1,1) (0,1).
    int k = gl_VertexIndex % 6;
    vec2 corner = vec2((k == 1 || k == 2 || k == 4) ? 1.0 : 0.0,
                       (k == 2 || k == 4 || k == 5) ? 1.0 : 0.0);
    vec4 rect = u_Hud.quads[gl_InstanceIndex].rect;
    vec2 pixel = rect.xy + corner * rect.zw;
    gl_Position = vec4(pixel * u_Frame.outputSize.zw * 2.0 - 1.0, 0.0, 1.0);
    v_Cell = corner * vec2(VX_HUD_CELLS_X, VX_HUD_CELLS_Y);
    v_Data = u_Hud.quads[gl_InstanceIndex].data.xyz;
}
