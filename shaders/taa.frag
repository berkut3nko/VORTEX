#version 460
#extension GL_GOOGLE_include_directive : require
// Temporal anti-aliasing: reproject the resolved history with the motion of the closest
// surface in a 3x3 neighbourhood, clamp it to the neighbourhood's colour box and blend.
#include "include/common.glsl"

layout(set = 0, binding = 16) uniform sampler2D u_Color;        // nearest
layout(set = 0, binding = 17) uniform sampler2D u_History;      // linear
layout(set = 0, binding = 18) uniform sampler2D u_GVelocity;    // nearest
layout(set = 0, binding = 19) uniform sampler2D u_GDepth;       // nearest

layout(location = 0) in vec2 v_Uv;

layout(location = 0) out vec4 o_Color;

const float VX_TAA_MAX_HISTORY_WEIGHT = 0.98;

// Motion of the sky (nothing was drawn, so the velocity target holds no value for it): a
// point at infinity in the view direction, which only the camera's rotation moves.
vec2 SkyMotion(vec2 uv) {
    // Built without the jitter, like every other motion vector: a still camera gives zero.
    vec2 ndc = uv * 2.0 - 1.0;
    vec3 dir = u_Frame.cameraForward.xyz + ndc.x * u_Frame.cameraRight.xyz - ndc.y * u_Frame.cameraUp.xyz;
    vec4 p = u_Frame.prevViewProjNoJitter * vec4(dir, 0.0);
    if (p.w <= 1.0e-6) {
        return vec2(0.0);
    }
    return (ndc - p.xy / p.w) * 0.5;
}

void main() {
    ivec2 px = ivec2(gl_FragCoord.xy);
    ivec2 maxPx = ivec2(u_Frame.renderSize.xy) - ivec2(1);
    vec4 current = texelFetch(u_Color, px, 0);

    // Motion debug view: show the stored motion vectors (camera and object movement).
    if (u_Frame.settings.y == VX_DEBUG_MOTION) {
        float depth = texelFetch(u_GDepth, px, 0).r;
        vec2 motion = (depth > 0.0) ? texelFetch(u_GVelocity, px, 0).rg : vec2(0.0);
        o_Color = vec4(clamp(abs(motion) * 50.0, vec2(0.0), vec2(1.0)), 0.0, 1.0);
        return;
    }

    if (!VxFeature(VX_FEATURE_TAA) || u_Frame.temporalParams.z < 0.5) {
        o_Color = current;
        return;
    }

    // One pass over the 3x3 neighbourhood: colour bounds and the closest surface
    // (reverse-Z: the largest depth value), whose motion keeps moving edges crisp.
    vec3 boxMin = current.rgb;
    vec3 boxMax = current.rgb;
    float closestDepth = texelFetch(u_GDepth, px, 0).r;
    ivec2 closestPx = px;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            if (x == 0 && y == 0) continue;
            ivec2 tap = clamp(px + ivec2(x, y), ivec2(0), maxPx);
            vec3 c = texelFetch(u_Color, tap, 0).rgb;
            boxMin = min(boxMin, c);
            boxMax = max(boxMax, c);
            float d = texelFetch(u_GDepth, tap, 0).r;
            if (d > closestDepth) {
                closestDepth = d;
                closestPx = tap;
            }
        }
    }

    vec2 motion = (closestDepth > 0.0) ? texelFetch(u_GVelocity, closestPx, 0).rg : SkyMotion(v_Uv);
    vec2 prevUv = v_Uv - motion;
    if (prevUv.x <= 0.0 || prevUv.x >= 1.0 || prevUv.y <= 0.0 || prevUv.y >= 1.0) {
        o_Color = current;      // the history of this pixel was off screen
        return;
    }

    vec3 history = clamp(texture(u_History, prevUv).rgb, boxMin, boxMax);
    float weight = clamp(u_Frame.temporalParams.y, 0.0, VX_TAA_MAX_HISTORY_WEIGHT);
    o_Color = vec4(mix(current.rgb, history, weight), current.a);
}
