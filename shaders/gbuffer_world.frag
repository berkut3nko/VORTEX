#version 460
#extension GL_GOOGLE_include_directive : require
// G-buffer, streaming world: one primary ray per pixel through the chunk grid.
#define VX_NEED_TRACE
#include "include/common.glsl"
#include "shared/shading.glsl"

layout(location = 0) in vec2 v_Uv;

layout(location = 0) out uint o_Material;
layout(location = 1) out vec4 o_Normal;
layout(location = 2) out vec2 o_Velocity;

void main() {
    vec3 rd = VxCameraRay(v_Uv);

    VxHit hit = VxMiss();
    // perf.md G1: past the edge-fog radius composite shows SkyColor anyway; such a ray is not traced there.
    // jitter.w is the radius R in metres or 0 (edge fog off); cameraPosVoxel.w is the voxel size.
    float tMax = VxEdgeFogTMax(rd, u_Frame.jitter.w, u_Frame.cameraPosVoxel.w);
    if (!VxTraceWorld(u_Frame.cameraPosVoxel.xyz, rd, 0.0, tMax, hit)) {
        discard;
    }

    // The world is traced in voxel units; everything downstream works in metres.
    float t = hit.t * u_Frame.cameraPosVoxel.w;
    vec3 pos = u_Frame.cameraPos.xyz + rd * t;

    if (hit.material == 0u) {
        // VX_GRID_PENDING: a chunk that is not loaded yet. It shows the sky / fog colour (the
        // composite pass draws "sky" for it) but keeps its depth, so nothing behind it shows.
        o_Material = VX_GBUFFER_SKY;
        o_Normal = VX_NORMAL_UNLIT;       // filters must not take its (zero) lighting
    } else if (u_Frame.settings.y == VX_DEBUG_STEPS) {
        o_Material = VX_GBUFFER_DEBUG;
        o_Normal = vec4(VxHeatColor(float(hit.steps) * (1.0 / 96.0)), 0.0);
    } else {
        // Variation is keyed on the absolute voxel so it does not change when the window moves.
        ivec3 absVoxel = hit.voxel + u_Frame.gridMinChunk.xyz * VX_CHUNK_SIZE;
        o_Material = VxPackMaterial(hit.material, VxVoxelHash(absVoxel));
        o_Normal = VxEncodeNormal(hit.normal);
    }

    // Static geometry: it was at the same render-space position last frame.
    o_Velocity = VxMotionVector(pos, pos);
    // Never 0 (that means sky) and never above the near plane value.
    gl_FragDepth = clamp(VxDepthFromRay(rd, t), 1.0e-7, 1.0);
}
