#version 460
#extension GL_GOOGLE_include_directive : require
// Composite: sky for empty pixels, otherwise albedo x filtered lighting + emission + sun
// specular, then height fog. Also produces the debug views.
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

    float metallic = clamp(params.y, 0.0, 1.0);
    vec3 diffuseColor = albedo * (1.0 - metallic);
    vec3 specularColor = mix(vec3(0.04), albedo, metallic);
    vec3 emission = albedo * (max(colorEmissive.a, 0.0) * u_Frame.ambientParams.z);

    vec3 color = diffuseColor * lighting.rgb
               + emission
               + specularColor * u_Frame.sunRadiance.rgb * lighting.a;
    color = ApplyFog(color, rd, t);

    // ---- Debug views -----------------------------------------------------------------------
    if (debugView == VX_DEBUG_ALBEDO) {
        color = albedo;
    } else if (debugView == VX_DEBUG_NORMAL) {
        color = n * 0.5 + 0.5;
    } else if (debugView == VX_DEBUG_DEPTH) {
        // Reverse-Z depth is near / distance: the fourth root spreads it over the visible range.
        color = vec3(sqrt(sqrt(clamp(depth, 0.0, 1.0))));
    } else if (debugView == VX_DEBUG_LIGHTING) {
        color = lighting.rgb;
    } else if (debugView == VX_DEBUG_STEPS) {
        // Geometry whose G-buffer pass did not write a step count.
        color = vec3(0.0);
    } else if (debugView == VX_DEBUG_MOTION) {
        // The velocity target is not an input of this pass: show the motion the camera
        // induces on this point. taa.frag replaces it with the stored motion vector, which
        // also contains object movement.
        vec3 pos = u_Frame.cameraPos.xyz + rd * t;
        vec2 motion = VxMotionVector(pos, pos);
        color = vec3(clamp(abs(motion) * 50.0, vec2(0.0), vec2(1.0)), 0.0);
    } else if (debugView == VX_DEBUG_MATERIAL) {
        uint h = VxHashPcg(materialIndex);
        color = vec3(float(h & 0xFFu), float((h >> 8u) & 0xFFu), float((h >> 16u) & 0xFFu)) * (1.0 / 255.0);
    }

    o_Color = vec4(color, 1.0);
}
