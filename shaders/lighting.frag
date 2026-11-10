#version 460
#extension GL_GOOGLE_include_directive : require
// Lighting: one sample of the direct and ambient light per pixel and frame, accumulated over
// time. Output rgb = diffuse irradiance (to be multiplied by the albedo), a = sun specular
// amount (to be multiplied by the sun radiance and the specular colour).
#define VX_NEED_TRACE
#define VX_NEED_MATERIALS
#define VX_NEED_LIGHTS
#include "include/common.glsl"
#include "shared/shading.glsl"

layout(set = 0, binding = 16) uniform usampler2D u_GMaterial;       // nearest
layout(set = 0, binding = 17) uniform sampler2D  u_GNormal;         // nearest
layout(set = 0, binding = 18) uniform sampler2D  u_GDepth;          // nearest
layout(set = 0, binding = 19) uniform sampler2D  u_GVelocity;       // nearest
layout(set = 0, binding = 20) uniform sampler2D  u_PrevLighting;    // linear
layout(set = 0, binding = 21) uniform sampler2D  u_PrevLightCount;  // linear
layout(set = 0, binding = 22) uniform sampler2D  u_PrevDepth;       // nearest
layout(set = 0, binding = 23) uniform sampler2D  u_PrevNormal;      // nearest

layout(location = 0) in vec2 v_Uv;

layout(location = 0) out vec4 o_Lighting;
layout(location = 1) out float o_Count;

const int   VX_MAX_LIGHTS        = 256;      // constant loop bound; lights beyond it are ignored
const float VX_SHADOW_DISTANCE   = 1.0e4;    // m, the traversal clips it to the streaming window
const float VX_HISTORY_DEPTH_TOL = 0.1;      // relative depth difference that rejects history
const float VX_HISTORY_NORMAL    = 0.9;      // minimum cosine between current and previous normal
const float VX_CHECKERBOARD_MIN_HISTORY = 6.0;   // frames of history before a pixel may skip
const float VX_MAX_SAMPLE        = 64.0;     // clamp of a single frame's sample (fireflies)

// Decode without the NaN a zero vector would produce.
vec3 SafeNormal(vec4 encoded) {
    vec3 v = encoded.xyz * 2.0 - 1.0;
    float len2 = dot(v, v);
    return (len2 > 1.0e-6) ? v * inversesqrt(len2) : vec3(0.0, 1.0, 0.0);
}

vec3 SkyAmbient(vec3 dir) {
    return VxSkyRadiance(dir, u_Frame.sunDirection.xyz, u_Frame.sunDirection.w, u_Frame.sunRadiance.rgb,
                         u_Frame.skyZenith.rgb, u_Frame.skyHorizon.rgb, u_Frame.skyGround.rgb, 0.0);
}

// Radiance emitted by the voxel a secondary ray hit.
vec3 HitEmission(VxHit hit) {
    uint palette = 0u;      // the world uses palette 0
    if (hit.instance != VX_INVALID) {
        palette = uint(u_Instances.instances[hit.instance].boundsMaxPalette.w);
    }
    vec4 ce = u_Materials.materials[palette + hit.material].colorEmissive;
    return ce.rgb * (max(ce.a, 0.0) * u_Frame.ambientParams.z);
}

void main() {
    ivec2 px = ivec2(gl_FragCoord.xy);
    uint packed = texelFetch(u_GMaterial, px, 0).r;
    float depth = texelFetch(u_GDepth, px, 0).r;

    // Sky and unlit debug pixels carry no lighting and no history.
    if (packed == VX_GBUFFER_SKY || packed == VX_GBUFFER_DEBUG || depth <= 0.0) {
        o_Lighting = vec4(0.0);
        o_Count = 0.0;
        return;
    }

    vec3 n = SafeNormal(texelFetch(u_GNormal, px, 0));
    vec3 rd = VxCameraRay(v_Uv);
    float t = VxRayDistanceFromDepth(rd, depth);
    vec3 pos = u_Frame.cameraPos.xyz + rd * t;
    float roughness = u_Materials.materials[VxMaterialIndex(packed)].params.x;

    // Secondary rays start slightly off the surface: far enough to clear the voxel they come
    // from at any distance (reconstruction error grows with t), close enough not to skip
    // neighbouring voxels.
    float voxelSize = u_Frame.cameraPosVoxel.w;
    vec3 origin = pos + n * (voxelSize * 0.02 + t * 2.0e-4);

    uint rng = VxRandomSeed(uint(px.x), uint(px.y), uint(u_Frame.counts.w));

    // ---- Temporal accumulation -----------------------------------------------------------------
    // Reproject into the previous frame and accept its history only if it shows the same
    // surface: on screen, similar depth, similar normal.
    float count = 0.0;
    vec4 history = vec4(0.0);
    if (u_Frame.temporalParams.z > 0.5) {
        vec2 prevUv = v_Uv - texelFetch(u_GVelocity, px, 0).rg;
        vec4 prevClip = u_Frame.prevViewProjNoJitter * vec4(pos, 1.0);
        if (prevUv.x > 0.0 && prevUv.x < 1.0 && prevUv.y > 0.0 && prevUv.y < 1.0 && prevClip.w > 1.0e-6) {
            float expectedDepth = prevClip.z / prevClip.w;
            // Bilinear footprint with every tap validated on its own, so that lighting of
            // another surface (or the empty sky) next to a silhouette never leaks in.
            vec2 texel = prevUv * u_Frame.renderSize.xy - 0.5;
            ivec2 base = ivec2(floor(texel));
            vec2 f = texel - vec2(base);
            ivec2 maxPx = ivec2(u_Frame.renderSize.xy) - ivec2(1);
            vec4 historySum = vec4(0.0);
            float countSum = 0.0;
            float weightSum = 0.0;
            for (int i = 0; i < 4; ++i) {
                ivec2 offset = ivec2(i & 1, i >> 1);
                ivec2 tap = clamp(base + offset, ivec2(0), maxPx);
                float prevDepth = texelFetch(u_PrevDepth, tap, 0).r;
                vec3 prevNormal = SafeNormal(texelFetch(u_PrevNormal, tap, 0));
                bool sameDepth = prevDepth > 0.0 &&
                                 abs(prevDepth - expectedDepth) <= VX_HISTORY_DEPTH_TOL * max(prevDepth, expectedDepth);
                bool sameNormal = dot(prevNormal, n) >= VX_HISTORY_NORMAL;
                float w = ((offset.x == 1) ? f.x : 1.0 - f.x) * ((offset.y == 1) ? f.y : 1.0 - f.y);
                w = (sameDepth && sameNormal) ? w : 0.0;
                historySum += max(texelFetch(u_PrevLighting, tap, 0), vec4(0.0)) * w;
                countSum += texelFetch(u_PrevLightCount, tap, 0).r * w;
                weightSum += w;
            }
            if (weightSum > 1.0e-3) {
                history = historySum / weightSum;
                count = floor(countSum / weightSum * 255.0 + 0.5);
            }
        }
    }


    // ---- Checkerboard ---------------------------------------------------------------------
    // With an established history only half of the screen traces new rays each frame (the
    // pattern alternates), the other half keeps its history. The pattern is made of 8x8 pixel
    // tiles rather than single pixels: GPUs shade fragments in SIMD groups, and a group in
    // which only some lanes skip still pays for the full trace.
    if (VxFeature(VX_FEATURE_CHECKERBOARD) && count >= VX_CHECKERBOARD_MIN_HISTORY &&
        (((px.x >> 3) + (px.y >> 3) + u_Frame.counts.w) & 1) == 1) {
        o_Lighting = history;
        o_Count = count * (1.0 / 255.0);
        return;
    }

    vec3 irradiance = vec3(0.0);
    float specular = 0.0;

    // ---- Sun: one jittered shadow ray inside the sun's cone ---------------------------------
    vec3 sunDir = u_Frame.sunDirection.xyz;
    float sunU1 = VxRandom(rng);
    float sunU2 = VxRandom(rng);
    float ndl = dot(n, sunDir);
    if (ndl > 0.0) {
        float visibility = 1.0;
        if (VxFeature(VX_FEATURE_SUN_SHADOWS)) {
            vec3 dir = VxConeDirection(sunDir, u_Frame.sunDirection.w, sunU1, sunU2);
            VxHit hit = VxMiss();
            if (VxTraceScene(origin, dir, 0.0, VX_SHADOW_DISTANCE, true, hit)) {
                visibility = 0.0;
            }
        }
        irradiance += u_Frame.sunRadiance.rgb * (ndl * visibility);
        specular = VxSpecular(n, -rd, sunDir, roughness) * visibility;
    }

    // ---- Ambient: one cosine-weighted ray -----------------------------------------------------
    // A miss sees the sky; a hit is occluded (ambient occlusion) and, if the voxel glows,
    // brings its emission back - which is how emissive voxels light their surroundings.
    {
        float u1 = VxRandom(rng);
        float u2 = VxRandom(rng);
        vec3 dir = VxCosineHemisphere(n, u1, u2);
        vec3 sky = SkyAmbient(dir) * u_Frame.ambientParams.x;
        vec3 ambient = sky;

        bool occlusion = VxFeature(VX_FEATURE_AMBIENT_OCCLUSION);
        bool emissive = VxFeature(VX_FEATURE_EMISSIVE_LIGHT);
        float reach = u_Frame.ambientParams.y;
        if ((occlusion || emissive) && reach > 0.0) {
            VxHit hit = VxMiss();
            // Emission needs the closest voxel; plain occlusion is happy with any.
            if (VxTraceScene(origin, dir, 0.0, reach, !emissive, hit)) {
                ambient = occlusion ? vec3(0.0) : sky;
                if (emissive) {
                    ambient += HitEmission(hit);
                }
            }
        }
        irradiance += ambient;
    }

    // ---- Point lights: all in range, one stochastically chosen shadow ray ------------------
    {
        int lightCount = min(u_Frame.counts.z, VX_MAX_LIGHTS);
        vec3 unshadowed = vec3(0.0);
        float weightSum = 0.0;
        int chosen = -1;
        vec3 chosenValue = vec3(0.0);
        float chosenWeight = 0.0;

        for (int i = 0; i < VX_MAX_LIGHTS; ++i) {
            if (i >= lightCount) break;
            vec4 positionRadius = u_Lights.lights[i].positionRadius;
            vec3 toLight = positionRadius.xyz - pos;
            float dist2 = dot(toLight, toLight);
            float radius = positionRadius.w;
            if (dist2 >= radius * radius) continue;

            float dist = sqrt(dist2);
            vec3 l = toLight / max(dist, 1.0e-4);
            float cosine = dot(n, l);
            if (cosine <= 0.0) continue;

            vec3 value = max(u_Lights.lights[i].colorSize.rgb, vec3(0.0)) * (VxLightFalloff(dist, radius) * cosine);
            float weight = VxLuminance(value);
            if (weight <= 0.0) continue;

            unshadowed += value;
            // Weighted reservoir of size one: light i replaces the choice with probability
            // weight / weightSum, which selects each light proportionally to its contribution.
            weightSum += weight;
            if (VxRandom(rng) * weightSum < weight) {
                chosen = i;
                chosenValue = value;
                chosenWeight = weight;
            }
        }

        if (chosen >= 0 && VxFeature(VX_FEATURE_LIGHT_SHADOWS)) {
            vec4 positionRadius = u_Lights.lights[chosen].positionRadius;
            float sourceRadius = max(u_Lights.lights[chosen].colorSize.a, 0.0);
            vec3 toLight = positionRadius.xyz - origin;
            float dist = max(length(toLight), 1.0e-4);
            vec3 l = toLight / dist;
            float u1 = VxRandom(rng);
            float u2 = VxRandom(rng);
            vec3 dir = VxConeDirection(l, min(sourceRadius / dist, 1.0), u1, u2);
            float reach = dist - sourceRadius;

            float visibility = 1.0;
            if (reach > 0.0) {
                VxHit hit = VxMiss();
                if (VxTraceScene(origin, dir, 0.0, reach, true, hit)) {
                    visibility = 0.0;
                }
            }
            // Unbiased estimate of the shadowed sum from the one light that was tested.
            irradiance += chosenValue * (visibility * weightSum / chosenWeight);
        } else {
            irradiance += unshadowed;
        }
    }

    vec4 current = clamp(vec4(irradiance, specular), vec4(0.0), vec4(VX_MAX_SAMPLE));

    float maxHistory = clamp(u_Frame.temporalParams.x, 1.0, 255.0);
    float samples = min(count + 1.0, maxHistory);
    o_Lighting = mix(history, current, 1.0 / samples);
    o_Count = samples * (1.0 / 255.0);
}
