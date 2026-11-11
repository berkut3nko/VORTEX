#version 460
#extension GL_GOOGLE_include_directive : require
// Present: bilinear upscale to the output resolution, exposure, tone mapping, sRGB curve and
// a small dither against banding. The backbuffer is UNORM, so the curve is applied here.
#include "include/common.glsl"
#include "shared/shading.glsl"

layout(set = 0, binding = 16) uniform sampler2D u_Color;    // linear

layout(location = 0) in vec2 v_Uv;

layout(location = 0) out vec4 o_Color;

// Interleaved gradient noise (Jimenez): cheap, well distributed screen-space noise in [0, 1).
float Dither(vec2 pixel) {
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

void main() {
    vec3 color = max(texture(u_Color, v_Uv).rgb, vec3(0.0));

    // Normals, depth, traversal cost, motion and material ids are data, not light: they are
    // shown as stored. Everything else goes through the display transform.
    int debugView = u_Frame.settings.y;
    bool data = debugView == VX_DEBUG_NORMAL || debugView == VX_DEBUG_DEPTH || debugView == VX_DEBUG_STEPS ||
                debugView == VX_DEBUG_MOTION || debugView == VX_DEBUG_MATERIAL;
    if (data) {
        color = clamp(color, vec3(0.0), vec3(1.0));
    } else {
        color = VxTonemapAces(color * max(u_Frame.ambientParams.w, 0.0));
        color = VxLinearToSrgb(color);
    }

    // Backdrop of a HUD menu: darkening here costs nothing, a full-screen blended quad would not.
    color *= 1.0 - float(clamp(u_Frame.settings.w, 0, 255)) * (1.0 / 255.0);

    // +- half an 8-bit step.
    color += (Dither(gl_FragCoord.xy) - 0.5) * (1.0 / 255.0);
    o_Color = vec4(clamp(color, vec3(0.0), vec3(1.0)), 1.0);
}
