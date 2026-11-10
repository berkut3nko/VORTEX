#version 460
#extension GL_GOOGLE_include_directive : require
// Particles: sprite shape and premultiplied output (blend: src ONE, dst ONE_MINUS_SRC_ALPHA).
#define VX_NEED_PARTICLES
#include "include/common.glsl"

layout(location = 0) in vec2 v_Local;               // quad coordinate, [-1, 1]^2
layout(location = 1) flat in vec4 v_Color;          // rgb = premultiplied radiance at full coverage, a = opacity
layout(location = 2) flat in uint v_Particle;

layout(location = 0) out vec4 o_Color;

void main() {
    // Shape: 0 = square (full coverage), 1 = soft disc (smooth falloff to the quad's edge).
    float shape = u_Particles.particles[v_Particle].params.y;
    float falloff = clamp(1.0 - dot(v_Local, v_Local), 0.0, 1.0);
    float coverage = (shape > 0.5) ? falloff * falloff : 1.0;
    if (coverage <= 0.0) {
        discard;
    }
    o_Color = v_Color * coverage;
}
