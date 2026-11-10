// =============================================================================
//  G-buffer fragment shader of dynamic objects (shared by the two variants).
// =============================================================================
//  Included by gbuffer_object_front.frag (#defines VX_OBJECT_FRONT) and
//  gbuffer_object_inside.frag after include/common.glsl with VX_NEED_TRACE.
//
//  The rasterised proxy box only selects the pixels; the fragment takes the pixel's own view
//  ray, moves it into the instance's chunk-local voxel space, clips it against the voxel
//  bounds and walks the chunk. The local direction is not renormalised, so the ray parameter
//  stays in render-space metres.
// =============================================================================
#ifndef VX_GBUFFER_OBJECT_GLSL
#define VX_GBUFFER_OBJECT_GLSL

#include "shared/shading.glsl"

layout(location = 0) flat in uint v_Instance;

layout(location = 0) out uint o_Material;
layout(location = 1) out vec4 o_Normal;
layout(location = 2) out vec2 o_Velocity;

#ifdef VX_OBJECT_FRONT
// Reverse-Z: the hit lies behind the front face, i.e. its depth value is smaller.
layout(depth_less) out float gl_FragDepth;
#endif

void main() {
    uint instance = v_Instance;
    vec2 uv = gl_FragCoord.xy * u_Frame.renderSize.zw;
    vec3 rd = VxCameraRay(uv);
    vec3 ro = u_Frame.cameraPos.xyz;

    mat4 toLocal = u_Instances.instances[instance].worldToLocal;
    vec3 lro = VxTransformPoint(toLocal, ro);
    vec3 lrd = VxSafeDirection(VxTransformVector(toLocal, rd));
    vec3 linv = vec3(1.0) / lrd;
    ivec3 bmin = u_Instances.instances[instance].boundsMinSlot.xyz;
    ivec3 bmax = u_Instances.instances[instance].boundsMaxPalette.xyz;

    float tEnter;
    float tExit;
    int entryAxis;
    if (!VxRayBox(lro, linv, vec3(bmin), vec3(bmax), tEnter, tExit, entryAxis)) {
        discard;
    }
    float tStart = max(tEnter, 0.0);
    if (tStart >= tExit) {
        discard;
    }
    if (tEnter <= 0.0) {
        entryAxis = -1;     // the camera is inside the box
    }

    VxHit hit = VxMiss();
    uint slot = uint(u_Instances.instances[instance].boundsMinSlot.w);
    if (!VxTraceChunk(slot, lro, lrd, linv, tStart, tExit, entryAxis, bmin, bmax, hit)) {
        discard;
    }

    if (u_Frame.settings.y == VX_DEBUG_STEPS) {
        o_Material = VX_GBUFFER_DEBUG;
        o_Normal = vec4(VxHeatColor(float(hit.steps) * (1.0 / 96.0)), 0.0);
    } else {
        // Normals transform with the inverse transpose of local->world, i.e. the transpose of
        // the 3x3 part of world->local.
        vec3 n = vec3(dot(toLocal[0].xyz, hit.normal), dot(toLocal[1].xyz, hit.normal), dot(toLocal[2].xyz, hit.normal));
        float len2 = dot(n, n);
        n = (len2 > 1.0e-12) ? n * inversesqrt(len2) : -rd;

        uint palette = uint(u_Instances.instances[instance].boundsMaxPalette.w);
        o_Material = VxPackMaterial(palette + hit.material, VxVoxelHash(hit.voxel));
        o_Normal = VxEncodeNormal(n);
    }

    // The same voxel-space point, placed with last frame's transform, gives the motion.
    vec3 pos = ro + rd * hit.t;
    vec3 localPos = lro + lrd * hit.t;
    vec3 prevPos = VxTransformPoint(u_Instances.instances[instance].prevLocalToWorld, localPos);
    o_Velocity = VxMotionVector(pos, prevPos);

    float depth = clamp(VxDepthFromRay(rd, hit.t), 1.0e-7, 1.0);
#ifdef VX_OBJECT_FRONT
    // Honour the depth_less promise even where rounding would break it.
    depth = min(depth, gl_FragCoord.z);
#endif
    gl_FragDepth = depth;
}

#endif // VX_GBUFFER_OBJECT_GLSL
