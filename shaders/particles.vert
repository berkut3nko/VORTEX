#version 460
#extension GL_GOOGLE_include_directive : require
// Particles: one camera-facing quad per instance, vkCmdDraw(6, particleCount, 0, 0).
// Lighting is evaluated once per particle here; the fragment shader only shapes the sprite.
#define VX_NEED_PARTICLES
#include "include/common.glsl"
#include "shared/shading.glsl"

layout(location = 0) out vec2 v_Local;              // quad coordinate, [-1, 1]^2
layout(location = 1) flat out vec4 v_Color;         // rgb = premultiplied radiance at full coverage, a = opacity
layout(location = 2) flat out uint v_Particle;

const float VX_PARTICLE_SUN_WRAP = 0.5;     // share of the sun a small unoriented particle receives

vec3 SafeNormalize(vec3 v, vec3 fallback) {
    float len2 = dot(v, v);
    return (len2 > 1.0e-12) ? v * inversesqrt(len2) : fallback;
}

void main() {
    // Two triangles: (-1,-1) (1,-1) (1,1) and (-1,-1) (1,1) (-1,1).
    int k = gl_VertexIndex % 6;
    vec2 corner = vec2((k == 1 || k == 2 || k == 4) ? 1.0 : -1.0,
                       (k == 2 || k == 4 || k == 5) ? 1.0 : -1.0);

    vec4 positionSize = u_Particles.particles[gl_InstanceIndex].positionSize;
    vec4 color = u_Particles.particles[gl_InstanceIndex].color;
    vec4 params = u_Particles.particles[gl_InstanceIndex].params;

    // cameraRight / cameraUp are scaled by the field of view; only their direction is wanted.
    vec3 right = SafeNormalize(u_Frame.cameraRight.xyz, vec3(1.0, 0.0, 0.0));
    vec3 up = SafeNormalize(u_Frame.cameraUp.xyz, vec3(0.0, 1.0, 0.0));
    vec3 center = positionSize.xyz;
    vec3 pos = center + (right * corner.x + up * corner.y) * max(positionSize.w, 0.0);

    // Simple lighting: a share of the sun plus the average sky, no shadows.
    vec3 base = max(color.rgb, vec3(0.0));
    vec3 lit = base;
    if (params.z < 0.5) {
        vec3 sky = (u_Frame.skyZenith.rgb + 2.0 * u_Frame.skyHorizon.rgb + u_Frame.skyGround.rgb) * 0.25;
        float sunUp = clamp(u_Frame.sunDirection.y * 4.0 + 0.5, 0.0, 1.0);
        lit = base * (u_Frame.sunRadiance.rgb * (VX_PARTICLE_SUN_WRAP * sunUp) + sky * u_Frame.ambientParams.x);
    }

    // Premultiplied: the lit part is covered by the opacity, the emitted part is pure light
    // (additive particles have opacity 0 and consist of the emitted part only).
    float opacity = clamp(color.a, 0.0, 1.0);
    vec3 emitted = base * max(params.x, 0.0);
    vec4 premultiplied = vec4(lit * opacity + emitted, opacity);

    // Fog between the camera and the particle dims its light and tints its opaque part.
    if (VxFeature(VX_FEATURE_FOG)) {
        vec3 toParticle = center - u_Frame.cameraPos.xyz;
        float dist = length(toParticle);
        vec3 dir = toParticle / max(dist, 1.0e-4);
        float fog = VxFogAmount(u_Frame.cameraPos.xyz, dir, dist, u_Frame.fogColor.w, u_Frame.fogParams.x,
                                u_Frame.fogParams.y, u_Frame.fogParams.z, u_Frame.fogParams.w);
        premultiplied.rgb = premultiplied.rgb * (1.0 - fog) + u_Frame.fogColor.rgb * (fog * opacity);
    }

    v_Local = corner;
    v_Color = premultiplied;
    v_Particle = uint(gl_InstanceIndex);
    gl_Position = u_Frame.viewProj * vec4(pos, 1.0);
}
