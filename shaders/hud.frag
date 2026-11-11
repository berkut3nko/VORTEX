#version 460
#extension GL_GOOGLE_include_directive : require
// HUD: a cell of the quad's 5 x 9 grid is drawn when its bit is set (solid rectangles have every
// bit set, glyphs their bitmap). Display-space colour, premultiplied alpha blending, no discard:
// an unset cell writes 0, which leaves the backbuffer unchanged.
#include "include/common.glsl"

layout(location = 0) noperspective in vec2 v_Cell;
layout(location = 1) flat in uvec3 v_Data;

layout(location = 0) out vec4 o_Color;

void main() {
    ivec2 cell = clamp(ivec2(floor(v_Cell)), ivec2(0), ivec2(VX_HUD_CELLS_X - 1, VX_HUD_CELLS_Y - 1));
    uint bit = uint(cell.y * VX_HUD_CELLS_X + cell.x);
    uint word = bit < 32u ? v_Data.y : v_Data.z;
    float on = float((word >> (bit & 31u)) & 1u);
    vec4 color = unpackUnorm4x8(v_Data.x);
    o_Color = vec4(color.rgb * color.a, color.a) * on;
}
