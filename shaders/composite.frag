#version 460
#extension GL_GOOGLE_include_directive : require
// Composite: sky for empty pixels, otherwise albedo x filtered lighting + emission + sun
// specular, then height fog. Liquids get an animated, sky-reflecting surface instead (no extra
// pass, no extra target: a few noise lookups on the pixels that show them). Also produces the
// debug views.
#define VX_NEED_MATERIALS
#include "include/common.glsl"
#include "shared/shading.glsl"

layout(set = 0, binding = 16) uniform usampler2D u_GMaterial;   // nearest
layout(set = 0, binding = 17) uniform sampler2D  u_GNormal;     // nearest
layout(set = 0, binding = 18) uniform sampler2D  u_GDepth;      // nearest
layout(set = 0, binding = 19) uniform sampler2D  u_Lighting;    // nearest
layout(set = 0, binding = 20) uniform sampler2D  u_LightCount;  // nearest

layout(location = 0) in vec2 v_Uv;

layout(location = 0) out vec4 o_Color;

const float VX_SKY_DISTANCE     = 5000.0;   // m, length of the fog segment towards the sky
const float VX_FILTER_SETTLED   = 12.0;     // accumulated frames after which the kernel is 3x3
const float VX_FILTER_MAX_STEP  = 3.0;      // tap spacing (px) for pixels without history
const float VX_FILTER_DEPTH_TOL = 0.08;     // relative depth difference at which a tap is dropped

vec3 SafeNormal(vec4 encoded) {
    vec3 v = encoded.xyz * 2.0 - 1.0;
    float len2 = dot(v, v);
    return (len2 > 1.0e-6) ? v * inversesqrt(len2) : vec3(0.0, 1.0, 0.0);
}

// Edge-aware 3x3 filter of the lighting buffer. Taps are weighted by how well their depth
// and normal match the centre, and their spacing grows while the pixel has little history
// (fresh disocclusions are the noisy ones), shrinking to a plain 3x3 once it has settled.
vec4 FilteredLighting(ivec2 px, float depth, vec3 n) {
    vec4 centre = texelFetch(u_Lighting, px, 0);
    if (!VxFeature(VX_FEATURE_SPATIAL_FILTER)) {
        return centre;
    }

    float count = texelFetch(u_LightCount, px, 0).r * 255.0;
    float settled = clamp(count / VX_FILTER_SETTLED, 0.0, 1.0);
    int spacing = int(floor(mix(VX_FILTER_MAX_STEP, 1.0, settled) + 0.5));
    ivec2 maxPx = ivec2(u_Frame.renderSize.xy) - ivec2(1);

    vec4 sum = centre;
    float weightSum = 1.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            if (x == 0 && y == 0) continue;
            ivec2 tap = px + ivec2(x, y) * spacing;
            if (tap.x < 0 || tap.y < 0 || tap.x > maxPx.x || tap.y > maxPx.y) continue;

            float tapDepth = texelFetch(u_GDepth, tap, 0).r;
            if (tapDepth <= 0.0) continue;      // sky has no lighting
            vec3 tapNormal = SafeNormal(texelFetch(u_GNormal, tap, 0));

            float depthDiff = abs(tapDepth - depth) / max(max(tapDepth, depth), 1.0e-9);
            float wDepth = clamp(1.0 - depthDiff / VX_FILTER_DEPTH_TOL, 0.0, 1.0);
            float c = max(dot(tapNormal, n), 0.0);
            float c4 = c * c * c * c;
            float wNormal = c4 * c4 * c4 * c4;
            float wKernel = (x == 0 || y == 0) ? 0.5 : 0.25;
            float w = wKernel * wDepth * wNormal;

            sum += texelFetch(u_Lighting, tap, 0) * w;
            weightSum += w;
        }
    }
    return sum / weightSum;
}

// ---- Liquid surface --------------------------------------------------------------------------
// Water is an ordinary opaque voxel material: the G-buffer holds a flat, static surface, so the
// lighting history, the checkerboard and the spatial filter treat it like stone. Only its
// shading differs: the normal is replaced by an animated procedural one and the colour is a
// Fresnel blend of the water body and the reflected sky.

const float VX_WATER_F0            = 0.02;    // reflectance of water at normal incidence
const float VX_WATER_DEEP          = 0.16;    // body brightness at grazing view angles (looks deep)
const float VX_WATER_SHALLOW       = 0.60;    // body brightness looking straight down
const float VX_WATER_RIPPLE_SHADE  = 3.0;     // how strongly ripples facing the sun brighten the body
const float VX_WATER_SHADOW_MIRROR = 0.25;    // part of the sky reflection a cast shadow removes (grounds objects)
const float VX_WATER_GLINT_ROUGH   = 0.16;    // lobe of the sun glint: wide enough not to alias
const int   VX_WATER_ORIGIN_MASK   = 1023;    // chunks: the noise domain repeats every 1024 chunks
const float VX_WATER_CHUNK_VOXELS  = 32.0;    // VX_CHUNK_SIZE of shared/trace.glsl (not included here; a contract test compares them)

// Hash without sine (Hoskins): stable across GPUs, input may be a few thousand units large.
float WaterHash(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

// Value noise with its analytic gradient (in units per noise cell).
vec2 WaterNoiseGradient(vec2 p) {
    vec2 i = floor(p);
    vec2 f = p - i;
    vec2 u = f * f * (3.0 - 2.0 * f);
    vec2 du = 6.0 * f * (1.0 - f);
    float a = WaterHash(i);
    float b = WaterHash(i + vec2(1.0, 0.0));
    float c = WaterHash(i + vec2(0.0, 1.0));
    float d = WaterHash(i + vec2(1.0, 1.0));
    float k = a - b - c + d;
    return du * (vec2(b - a, c - a) + k * u.yx);
}

// Slope (dh/dx, dh/dz) of the animated surface at a world-space position: one octave of value
// noise for the large, irregular swell plus four directional sine ripples whose phase the noise
// bends, so they never line up into a regular pattern. Every component fades out before its
// wavelength reaches the size of a pixel (`footprint`, metres), so the distance shows a calm
// mirror instead of shimmering noise.
// Kept this small on purpose: on Intel a second noise octave made the whole pass slower for
// every pixel, liquid or not (measured, UHD 620).
vec2 WaterSlope(vec2 worldXZ, float footprint) {
    float time = u_Frame.temporalParams.w;

    // Swell: 0.45 cells per metre, drifting slowly.
    vec2 g = WaterNoiseGradient(worldXZ * 0.45 + vec2(0.11, 0.07) * time);
    vec2 slope = g * (0.085 * clamp(1.0 - footprint * 0.9, 0.0, 1.0));

    // Ripples: direction, wave number k (rad/m), angular speed (rad/s), slope amplitude.
    // A wave of wave number k is gone when the footprint reaches half its wavelength (pi / k).
    const float invPi = 0.3183099;
    vec2 bend = g * 3.0;
    slope += vec2(0.80, 0.60) * (cos(dot(vec2(0.80, 0.60), worldXZ) * 2.1 + 1.3 * time + bend.x) *
                                 0.040 * clamp(1.0 - footprint * 2.1 * invPi, 0.0, 1.0));
    slope += vec2(-0.39, 0.92) * (cos(dot(vec2(-0.39, 0.92), worldXZ) * 3.4 - 1.9 * time + bend.y) *
                                  0.034 * clamp(1.0 - footprint * 3.4 * invPi, 0.0, 1.0));
    slope += vec2(0.96, -0.28) * (cos(dot(vec2(0.96, -0.28), worldXZ) * 5.9 + 2.6 * time - bend.y) *
                                  0.028 * clamp(1.0 - footprint * 5.9 * invPi, 0.0, 1.0));
    slope += vec2(-0.71, -0.71) * (cos(dot(vec2(-0.71, -0.71), worldXZ) * 9.7 - 3.4 * time - bend.x) *
                                   0.022 * clamp(1.0 - footprint * 9.7 * invPi, 0.0, 1.0));
    return slope;
}

// Colour of a liquid pixel before emission and fog.
//   albedo        body colour of the liquid
//   lighting      rgb = accumulated diffuse lighting of the flat surface (sun x shadow + ambient
//                 + lights), a = accumulated sun visibility (what the lighting pass stores for
//                 liquids instead of the sun specular)
//   n             G-buffer normal (flat), rd = view ray, t = distance along it
// Written so that few values are alive at once (inputs are reduced to what the end needs before
// the waves are evaluated): the Intel compiler's 16-wide variant of this pass has no registers
// to spare, and losing it costs every pixel of the screen, not just the liquid ones.
vec3 ShadeLiquid(vec3 albedo, vec4 lighting, float roughness, vec3 n, vec3 rd, float t) {
    vec3 sunDir = u_Frame.sunDirection.xyz;
    vec3 irradiance = lighting.rgb;

    // Whether the sun reaches this spot (for the glint) comes straight from the shadow rays: no
    // other light can switch the glint on. How much of the sky it sees (for the reflection) is
    // what is left of the lighting once the sun's share - known exactly, it was accumulated from
    // the same rays - is taken away, relative to an open surface. A cosine-weighted hemisphere
    // of the sky gradient averages to mix(horizon, zenith, 0.8). Point lights and glowing voxels
    // are part of that rest and cannot be told from sky light here.
    float sunVisible = clamp(lighting.a, 0.0, 1.0);
    float openAmbient = VxLuminance(mix(u_Frame.skyHorizon.rgb, u_Frame.skyZenith.rgb, 0.8)) * u_Frame.ambientParams.x;
    float sunFlat = VxLuminance(u_Frame.sunRadiance.rgb) * max(dot(n, sunDir), 0.0);
    float skyVisible = clamp((VxLuminance(irradiance) - sunFlat * sunVisible) / max(openAmbient, 1.0e-4), 0.0, 1.0);

    // The body: there is no depth to darken with (the liquid is opaque), so the view angle
    // stands in for it - dark looking along the surface, lighter looking down into it.
    float down = clamp(dot(n, -rd), 0.0, 1.0);
    vec3 body = albedo * irradiance * mix(VX_WATER_DEEP, VX_WATER_SHALLOW, down * down);

    // Animated normal on faces that look up. Render space jumps by whole chunks when the
    // streaming window moves; adding the window's chunk coordinate (wrapped, to keep float
    // precision) gives a coordinate that stays put in the world.
    vec3 wn = n;
    if (n.y > 0.5) {
        float chunkMetres = VX_WATER_CHUNK_VOXELS * u_Frame.cameraPosVoxel.w;
        vec2 worldXZ = u_Frame.cameraPos.xz + rd.xz * t
                     + vec2(u_Frame.gridMinChunk.xz & ivec2(VX_WATER_ORIGIN_MASK)) * chunkMetres;
        // Size on the surface of one pixel, along the view direction (the long axis).
        float pixelAngle = 2.0 * length(u_Frame.cameraUp.xyz) * u_Frame.renderSize.w;
        float footprint = t * pixelAngle / max(-rd.y, 0.03);
        vec2 slope = WaterSlope(worldXZ, footprint);
        wn = normalize(vec3(-slope.x, 1.0, -slope.y));
        // Ripples tilted towards the sun are a little brighter: it makes the waves readable
        // where the view is too steep for the reflection to show them.
        body *= max(1.0 + VX_WATER_RIPPLE_SHADE * dot(wn - n, sunDir) * sunVisible, 0.0);
    }

    float f = 1.0 - clamp(dot(wn, -rd), 0.0, 1.0);
    float f2 = f * f;
    float fresnel = VX_WATER_F0 + (1.0 - VX_WATER_F0) * f2 * f2 * f;
    float smoothness = 1.0 - clamp(roughness, 0.0, 1.0);
    float mirror = fresnel * smoothness * smoothness;

    // Sun glint, then the reflected sky (gradient only: the sun is the glint). A ray reflected
    // into the surface is folded back out instead of showing the ground colour.
    float glint = VxSpecular(wn, -rd, sunDir, max(roughness, VX_WATER_GLINT_ROUGH)) * sunVisible;
    vec3 r = reflect(rd, wn);
    r -= 2.0 * min(dot(r, n), 0.0) * n;
    vec3 sky = VxSkyRadiance(r, sunDir, u_Frame.sunDirection.w, u_Frame.sunRadiance.rgb,
                             u_Frame.skyZenith.rgb, u_Frame.skyHorizon.rgb, u_Frame.skyGround.rgb, 0.0);
    float skyAmount = skyVisible * (1.0 - VX_WATER_SHADOW_MIRROR * (1.0 - sunVisible));

    return body * (1.0 - mirror) + (sky * skyAmount + u_Frame.sunRadiance.rgb * glint) * mirror;
}

vec3 ApplyFog(vec3 color, vec3 rd, float dist) {
    if (!VxFeature(VX_FEATURE_FOG)) {
        return color;
    }
    float amount = VxFogAmount(u_Frame.cameraPos.xyz, rd, dist, u_Frame.fogColor.w, u_Frame.fogParams.x,
                               u_Frame.fogParams.y, u_Frame.fogParams.z, u_Frame.fogParams.w);
    return mix(color, u_Frame.fogColor.rgb, amount);
}

void main() {
    ivec2 px = ivec2(gl_FragCoord.xy);
    uint packed = texelFetch(u_GMaterial, px, 0).r;
    float depth = texelFetch(u_GDepth, px, 0).r;
    int debugView = u_Frame.settings.y;
    vec3 rd = VxCameraRay(v_Uv);

    // Unlit debug colour written by the G-buffer pass (traversal cost view).
    if (packed == VX_GBUFFER_DEBUG) {
        o_Color = vec4(texelFetch(u_GNormal, px, 0).rgb, 1.0);
        return;
    }

    // ---- Sky -------------------------------------------------------------------------------
    if (packed == VX_GBUFFER_SKY || depth <= 0.0) {
        vec3 sky = vec3(0.0);
        if (debugView == VX_DEBUG_NONE) {
            sky = VxSkyRadiance(rd, u_Frame.sunDirection.xyz, u_Frame.sunDirection.w, u_Frame.sunRadiance.rgb,
                                u_Frame.skyZenith.rgb, u_Frame.skyHorizon.rgb, u_Frame.skyGround.rgb, 1.0);
            sky = ApplyFog(sky, rd, VX_SKY_DISTANCE);
        }
        o_Color = vec4(sky, 1.0);
        return;
    }

    // ---- Surface ---------------------------------------------------------------------------
    uint materialIndex = VxMaterialIndex(packed);
    vec4 colorEmissive = u_Materials.materials[materialIndex].colorEmissive;
    vec4 params = u_Materials.materials[materialIndex].params;
    vec3 n = SafeNormal(texelFetch(u_GNormal, px, 0));
    float t = VxRayDistanceFromDepth(rd, depth);

    vec3 albedo = VxAlbedo(colorEmissive.rgb, params.z, VxMaterialVariation(packed));
    vec4 lighting = max(FilteredLighting(px, depth, n), vec4(0.0));

    // ---- Debug views -----------------------------------------------------------------------
    if (debugView != VX_DEBUG_NONE) {
        vec3 debugColor = vec3(0.0);    // VX_DEBUG_STEPS: geometry whose G-buffer pass wrote no step count
        if (debugView == VX_DEBUG_ALBEDO) {
            debugColor = albedo;
        } else if (debugView == VX_DEBUG_NORMAL) {
            debugColor = n * 0.5 + 0.5;
        } else if (debugView == VX_DEBUG_DEPTH) {
            // Reverse-Z depth is near / distance: the fourth root spreads it over the visible range.
            debugColor = vec3(sqrt(sqrt(clamp(depth, 0.0, 1.0))));
        } else if (debugView == VX_DEBUG_LIGHTING) {
            debugColor = lighting.rgb;
        } else if (debugView == VX_DEBUG_MOTION) {
            // The velocity target is not an input of this pass: show the motion the camera
            // induces on this point. taa.frag replaces it with the stored motion vector, which
            // also contains object movement.
            vec3 pos = u_Frame.cameraPos.xyz + rd * t;
            vec2 motion = VxMotionVector(pos, pos);
            debugColor = vec3(clamp(abs(motion) * 50.0, vec2(0.0), vec2(1.0)), 0.0);
        } else if (debugView == VX_DEBUG_MATERIAL) {
            uint h = VxHashPcg(materialIndex);
            debugColor = vec3(float(h & 0xFFu), float((h >> 8u) & 0xFFu), float((h >> 16u) & 0xFFu)) * (1.0 / 255.0);
        }
        o_Color = vec4(debugColor, 1.0);
        return;
    }

    float emissive = max(colorEmissive.a, 0.0) * u_Frame.ambientParams.z;
    vec3 color;
    if (VxMaterialIsLiquid(params)) {
        color = ShadeLiquid(albedo, lighting, params.x, n, rd, t) + albedo * emissive;
    } else {
        float metallic = clamp(params.y, 0.0, 1.0);
        // There are no reflections yet, so a physically correct metal (no diffuse at all) would
        // render black. Until reflections exist metals keep most of their albedo as diffuse.
        vec3 diffuseColor = albedo * (1.0 - 0.35 * metallic);
        vec3 specularColor = mix(vec3(0.04), albedo, metallic);
        color = diffuseColor * lighting.rgb
              + albedo * emissive
              + specularColor * u_Frame.sunRadiance.rgb * lighting.a;
    }
    color = ApplyFog(color, rd, t);

    o_Color = vec4(color, 1.0);
}
